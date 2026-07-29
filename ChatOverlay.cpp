#include "ChatOverlay.h"

#include <Windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "kiero.h"
#include "minhook/MinHook.h"
#include "DebugLog.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dx9.lib")

// ===========================================================================
//  Externs
// ===========================================================================

extern HMODULE moduleInstance;

// ===========================================================================
//  Globals
// ===========================================================================

typedef HRESULT(__stdcall* EndScene_t)(LPDIRECT3DDEVICE9 pDevice);
static EndScene_t oEndScene = nullptr;

// --- Win32 API hooks ---
typedef SHORT (WINAPI *GetAsyncKeyState_t)(int);
static GetAsyncKeyState_t oGetAsyncKeyState = nullptr;

typedef BOOL (WINAPI *PeekMessageA_t)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessageA_t oPeekMessageA = nullptr;

typedef BOOL (WINAPI *PeekMessageW_t)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessageW_t oPeekMessageW = nullptr;

typedef BOOL (WINAPI *GetMessageA_t)(LPMSG, HWND, UINT, UINT);
static GetMessageA_t oGetMessageA = nullptr;

typedef BOOL (WINAPI *GetMessageW_t)(LPMSG, HWND, UINT, UINT);
static GetMessageW_t oGetMessageW = nullptr;

ChatOverlay g_ChatOverlay;

static constexpr size_t MaxChatInputLength = 127;
static constexpr size_t MaxChatDisplayLength = 256;

static bool IsSafeChatChar(unsigned char ch)
{
	return ch >= 32 && ch <= 126;
}

static std::string SanitizeChatText(const std::string& text, size_t maxLength)
{
	std::string result;
	result.reserve(text.size() < maxLength ? text.size() : maxLength);
	for (unsigned char ch : text)
	{
		if (IsSafeChatChar(ch))
		{
			if (result.size() >= maxLength)
				break;
			result.push_back(static_cast<char>(ch));
		}
	}
	return result;
}

static bool StartsWithCaseInsensitive(const std::string& text, const std::string& prefix)
{
	if (prefix.size() > text.size())
		return false;

	for (size_t i = 0; i < prefix.size(); ++i)
	{
		unsigned char lhs = static_cast<unsigned char>(text[i]);
		unsigned char rhs = static_cast<unsigned char>(prefix[i]);
		if (std::tolower(lhs) != std::tolower(rhs))
			return false;
	}
	return true;
}

static int MeasureTextWidth(ID3DXFont* pFont, const std::string& text)
{
	if (!pFont || text.empty())
		return 0;

	std::string measured = text;
	int trailingSpaces = 0;
	while (!measured.empty() && measured.back() == ' ')
	{
		measured.pop_back();
		++trailingSpaces;
	}

	RECT rect = { 0, 0, 0, 0 };
	if (!measured.empty())
		pFont->DrawTextA(NULL, measured.c_str(), -1, &rect, DT_CALCRECT | DT_NOCLIP, D3DCOLOR_XRGB(255, 255, 255));

	RECT charRect = { 0, 0, 0, 0 };
	pFont->DrawTextA(NULL, "X", -1, &charRect, DT_CALCRECT | DT_NOCLIP, D3DCOLOR_XRGB(255, 255, 255));
	return (rect.right - rect.left) + trailingSpaces * (charRect.right - charRect.left);
}

// ===========================================================================
//  Vertex / drawing helpers
// ===========================================================================

struct Vertex
{
	float x, y, z, rhw;
	D3DCOLOR color;
};

#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static void DrawFilledRect(LPDIRECT3DDEVICE9 device, int x, int y, int w, int h, D3DCOLOR color)
{
	Vertex vertices[4] =
	{
		{(float)x,       (float)(y + h), 0.0f, 1.0f, color},
		{(float)x,       (float)y,       0.0f, 1.0f, color},
		{(float)(x + w), (float)(y + h), 0.0f, 1.0f, color},
		{(float)(x + w), (float)y,       0.0f, 1.0f, color}
	};

	device->SetTexture(0, NULL);
	device->SetPixelShader(NULL);
	device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	device->SetFVF(FVF);
	device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices, sizeof(Vertex));
}

static void DrawTextSimple(ID3DXFont* pFont, const std::string& text, int x, int y, D3DCOLOR color)
{
	if (!pFont)
		return;

	RECT rect;
	SetRect(&rect, x, y, x + 1000, y + 20);
	pFont->DrawTextA(NULL, text.c_str(), -1, &rect, DT_NOCLIP, color);
}

// ===========================================================================
//  EndScene hook & rendering
// ===========================================================================

static HRESULT __stdcall hkEndScene(LPDIRECT3DDEVICE9 pDevice)
{
	g_ChatOverlay.Render(pDevice);
	return oEndScene(pDevice);
}

static void HookEndScene()
{
	if (kiero::init(kiero::RenderType::Auto) == kiero::Status::Success)
	{
		kiero::bind(42, (void**)&oEndScene, hkEndScene);
	}
}

// ===========================================================================
//  Message-queue hooks (block keyboard input while chat is open)
// ===========================================================================

static void NullifyKeyboardMessage(LPMSG lpMsg)
{
	if (lpMsg->message == WM_KEYUP)
		return; // Allow key releases so held keys don't get stuck

	if ((lpMsg->message >= WM_KEYFIRST && lpMsg->message <= WM_KEYLAST)
		|| lpMsg->message == WM_INPUT)
	{
		lpMsg->message = WM_NULL;
		lpMsg->wParam = 0;
		lpMsg->lParam = 0;
	}
}

static BOOL WINAPI hkPeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin,
	UINT wMsgFilterMax, UINT wRemoveMsg)
{
	BOOL result = oPeekMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
	if (g_ChatOverlay.m_ChatOpen && result && lpMsg)
		NullifyKeyboardMessage(lpMsg);
	return result;
}

static BOOL WINAPI hkPeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin,
	UINT wMsgFilterMax, UINT wRemoveMsg)
{
	BOOL result = oPeekMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax, wRemoveMsg);
	if (g_ChatOverlay.m_ChatOpen && result && lpMsg)
		NullifyKeyboardMessage(lpMsg);
	return result;
}

static BOOL WINAPI hkGetMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
	BOOL result = oGetMessageA(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
	if (g_ChatOverlay.m_ChatOpen && result != -1 && lpMsg)
		NullifyKeyboardMessage(lpMsg);
	return result;
}

static BOOL WINAPI hkGetMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax)
{
	BOOL result = oGetMessageW(lpMsg, hWnd, wMsgFilterMin, wMsgFilterMax);
	if (g_ChatOverlay.m_ChatOpen && result != -1 && lpMsg)
		NullifyKeyboardMessage(lpMsg);
	return result;
}

// ===========================================================================
//  GetAsyncKeyState hook
// ===========================================================================

static SHORT WINAPI hkGetAsyncKeyState(int nVirtKey)
{
	if (g_ChatOverlay.m_ChatOpen)
		return 0;
	return oGetAsyncKeyState(nVirtKey);
}

// ===========================================================================
//  Low-level keyboard hook (WH_KEYBOARD_LL)
// ===========================================================================

static LRESULT CALLBACK hkLowLevelKeyboard(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode >= 0 && g_ChatOverlay.m_ChatOpen)
	{
		bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
		if (isKeyDown)
		{
			HWND fg = GetForegroundWindow();
			if (fg == g_ChatOverlay.m_GameWindow)
				return 1;
		}
	}
	return CallNextHookEx(g_ChatOverlay.m_hLLKeyboardHook, nCode, wParam, lParam);
}

static DWORD WINAPI KeyboardHookThread(LPVOID)
{
	g_ChatOverlay.m_hLLKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, hkLowLevelKeyboard,
		moduleInstance, 0);
	if (!g_ChatOverlay.m_hLLKeyboardHook)
		return 1;

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	UnhookWindowsHookEx(g_ChatOverlay.m_hLLKeyboardHook);
	return 0;
}

// ===========================================================================
//  Chat input thread (polls original GetAsyncKeyState)
// ===========================================================================

static char MapVkToChar(int vk, bool shift)
{
	if (vk >= 'A' && vk <= 'Z') return static_cast<char>(shift ? vk : (vk + 32));
	if (vk >= '0' && vk <= '9') return static_cast<char>(vk);

	if (!shift)
	{
		switch (vk)
		{
		case VK_SPACE:        return ' ';
		case VK_OEM_MINUS:    return '-';
		case VK_OEM_PLUS:     return '=';
		case VK_OEM_1:        return ';';
		case VK_OEM_2:        return '/';
		case VK_OEM_3:        return '`';
		case VK_OEM_4:        return '[';
		case VK_OEM_5:        return '\\';
		case VK_OEM_6:        return ']';
		case VK_OEM_7:        return '\'';
		case VK_OEM_COMMA:    return ',';
		case VK_OEM_PERIOD:   return '.';
		}
	}
	else
	{
		switch (vk)
		{
		case VK_SPACE:        return ' ';
		case VK_OEM_MINUS:    return '_';
		case VK_OEM_PLUS:     return '+';
		case VK_OEM_1:        return ':';
		case VK_OEM_2:        return '?';
		case VK_OEM_3:        return '~';
		case VK_OEM_4:        return '{';
		case VK_OEM_5:        return '|';
		case VK_OEM_6:        return '}';
		case VK_OEM_7:        return '"';
		case VK_OEM_COMMA:    return '<';
		case VK_OEM_PERIOD:   return '>';
		case '1': return '!';
		case '2': return '@';
		case '3': return '#';
		case '4': return '$';
		case '5': return '%';
		case '6': return '^';
		case '7': return '&';
		case '8': return '*';
		case '9': return '(';
		case '0': return ')';
		}
	}
	return 0;
}

static DWORD WINAPI ChatInputThread(LPVOID)
{
	Sleep(2000); // let game start up

	while (true)
	{
		bool shift = (oGetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

		// Open chat with /
		if (!g_ChatOverlay.m_ChatOpen)
		{
			SHORT slashState = oGetAsyncKeyState(VK_OEM_2);
			bool slashPressed = (slashState & 0x8000) != 0;
			bool slashWasPressed = g_ChatOverlay.m_KeyState[VK_OEM_2];

			if (slashPressed && !slashWasPressed)
			{
				g_ChatOverlay.m_ChatOpen = true;
				g_ChatOverlay.SetInputBuffer("/");
				g_ChatOverlay.ResetHistoryBrowsing();
			}
			g_ChatOverlay.m_KeyState[VK_OEM_2] = slashPressed;
		}

		// Typing mode
		if (g_ChatOverlay.m_ChatOpen)
		{
			// Editing keys: history (Up/Down), caret (Left/Right/Home/End) and
			// forward delete.
			//
			// These have to be read BEFORE the character sweep below. That loop
			// runs VK_SPACE..VK_OEM_7, a range which swallows every key here -
			// VK_END 0x23, VK_HOME 0x24, VK_LEFT 0x25, VK_UP 0x26, VK_RIGHT 0x27,
			// VK_DOWN 0x28, VK_DELETE 0x2E - and it stamps m_KeyState for every
			// key it touches. Reading the edge afterwards would always see
			// "already held" and nothing would ever fire. (MapVkToChar returns 0
			// for all of them, so the sweep itself does no harm.)
			auto editKeyPressed = [](int vk) -> bool
			{
				const bool pressed = (oGetAsyncKeyState(vk) & 0x8000) != 0;
				const bool wasPressed = g_ChatOverlay.m_KeyState[vk];
				g_ChatOverlay.m_KeyState[vk] = pressed;
				return pressed && !wasPressed;
			};

			if (editKeyPressed(VK_UP))     g_ChatOverlay.RecallPreviousInput();
			if (editKeyPressed(VK_DOWN))   g_ChatOverlay.RecallNextInput();
			if (editKeyPressed(VK_LEFT))   g_ChatOverlay.MoveCaret(-1);
			if (editKeyPressed(VK_RIGHT))  g_ChatOverlay.MoveCaret(1);
			if (editKeyPressed(VK_HOME))   g_ChatOverlay.MoveCaretToStart();
			if (editKeyPressed(VK_END))    g_ChatOverlay.MoveCaretToEnd();
			if (editKeyPressed(VK_DELETE)) g_ChatOverlay.DeleteAtCaret();

			for (int vk = VK_SPACE; vk <= VK_OEM_7; vk++)
			{
				SHORT state = oGetAsyncKeyState(vk);
				bool pressed = (state & 0x8000) != 0;
				bool wasPressed = g_ChatOverlay.m_KeyState[vk];
				if (pressed && !wasPressed)
				{
					char c = MapVkToChar(vk, shift);
					if (c)
						g_ChatOverlay.AppendInputChar(c);
				}
				g_ChatOverlay.m_KeyState[vk] = pressed;
			}
			for (int vk = '0'; vk <= 'Z'; vk++)
			{
				SHORT state = oGetAsyncKeyState(vk);
				bool pressed = (state & 0x8000) != 0;
				bool wasPressed = g_ChatOverlay.m_KeyState[vk];
				if (pressed && !wasPressed)
				{
					char c = MapVkToChar(vk, shift);
					if (c)
						g_ChatOverlay.AppendInputChar(c);
				}
				g_ChatOverlay.m_KeyState[vk] = pressed;
			}

			// Backspace
			{
				SHORT state = oGetAsyncKeyState(VK_BACK);
				bool pressed = (state & 0x8000) != 0;
				bool wasPressed = g_ChatOverlay.m_KeyState[VK_BACK];
				if (pressed && !wasPressed)
					g_ChatOverlay.DeleteBeforeCaret();
				g_ChatOverlay.m_KeyState[VK_BACK] = pressed;
			}

			// Tab = autocomplete command
			{
				SHORT state = oGetAsyncKeyState(VK_TAB);
				bool pressed = (state & 0x8000) != 0;
				bool wasPressed = g_ChatOverlay.m_KeyState[VK_TAB];
				if (pressed && !wasPressed)
					g_ChatOverlay.AutocompleteCommand();
				g_ChatOverlay.m_KeyState[VK_TAB] = pressed;
			}

			// Enter = send
			{
				SHORT state = oGetAsyncKeyState(VK_RETURN);
				bool pressed = (state & 0x8000) != 0;
				bool wasPressed = g_ChatOverlay.m_KeyState[VK_RETURN];
				if (pressed && !wasPressed)
				{
					g_ChatOverlay.ProcessChatSend();
				}
				g_ChatOverlay.m_KeyState[VK_RETURN] = pressed;
			}

			// Escape = cancel
			{
				SHORT state = oGetAsyncKeyState(VK_ESCAPE);
				bool pressed = (state & 0x8000) != 0;
				bool wasPressed = g_ChatOverlay.m_KeyState[VK_ESCAPE];
				if (pressed && !wasPressed)
				{
					g_ChatOverlay.ClearInputBuffer();
					g_ChatOverlay.ResetHistoryBrowsing();
					g_ChatOverlay.m_ChatOpen = false;
				}
				g_ChatOverlay.m_KeyState[VK_ESCAPE] = pressed;
			}
		}
		else
		{
			for (int i = 0; i < 256; i++)
				g_ChatOverlay.m_KeyState[i] = false;
		}

		Sleep(10);
	}

	return 0;
}

// ===========================================================================
//  WndProc hook
// ===========================================================================

static LRESULT CALLBACK hkWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// Open chat with /
	if (!g_ChatOverlay.m_ChatOpen)
	{
		if (msg == WM_KEYDOWN && wParam == VK_OEM_2)
		{
			g_ChatOverlay.m_ChatOpen = true;
			g_ChatOverlay.SetInputBuffer("/");
			g_ChatOverlay.ResetHistoryBrowsing();
			return 0;
		}
	}

	// Chat input mode
	if (g_ChatOverlay.m_ChatOpen)
	{
		switch (msg)
		{
		case WM_CHAR:
		{
			if (wParam <= 0x7F)
				g_ChatOverlay.AppendInputChar(static_cast<char>(wParam));
			return 0;
		}

		case WM_KEYUP:
			return CallWindowProc(g_ChatOverlay.m_OriginalWndProc, hwnd, msg, wParam, lParam);

		case WM_KEYDOWN:
		{
			switch (wParam)
			{
			case VK_BACK:
				g_ChatOverlay.DeleteBeforeCaret();
				return 0;

			case VK_TAB:
				g_ChatOverlay.AutocompleteCommand();
				return 0;

			case VK_UP:
				g_ChatOverlay.RecallPreviousInput();
				return 0;

			case VK_DOWN:
				g_ChatOverlay.RecallNextInput();
				return 0;

			case VK_LEFT:
				g_ChatOverlay.MoveCaret(-1);
				return 0;

			case VK_RIGHT:
				g_ChatOverlay.MoveCaret(1);
				return 0;

			case VK_HOME:
				g_ChatOverlay.MoveCaretToStart();
				return 0;

			case VK_END:
				g_ChatOverlay.MoveCaretToEnd();
				return 0;

			case VK_DELETE:
				g_ChatOverlay.DeleteAtCaret();
				return 0;

			case VK_RETURN:
				g_ChatOverlay.ProcessChatSend();
				return 0;

			case VK_ESCAPE:
				g_ChatOverlay.ClearInputBuffer();
				g_ChatOverlay.ResetHistoryBrowsing();
				g_ChatOverlay.m_ChatOpen = false;
				return 0;
			}
			return 0;
		}
		}

		return 0;
	}

	return CallWindowProc(g_ChatOverlay.m_OriginalWndProc, hwnd, msg, wParam, lParam);
}

// ===========================================================================
//  Find game window
// ===========================================================================

static HWND FindMainWindow()
{
	HWND hwnd = nullptr;
	while (!hwnd)
	{
		hwnd = GetForegroundWindow();
		if (hwnd)
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			if (pid == GetCurrentProcessId())
				return hwnd;
		}
		Sleep(100);
	}
	return hwnd;
}

// ===========================================================================
//  Init / Shutdown
// ===========================================================================

// Lists the player-facing commands, plus the development ones when debug=1.
// Keeps the chat box open so the whole list stays readable at full alpha instead
// of fading out; Enter on an empty line closes it as usual.
static void HelpCommand(const std::string&)
{
	g_ChatOverlay.m_KeepChatOpen = true;

	g_ChatOverlay.AddSystemMessage("[System] Available commands:");
	for (size_t i = 0; i < g_ChatOverlay.m_Commands.size(); i++)
	{
		const ChatCommand& command = g_ChatOverlay.m_Commands[i];
		if (command.listing == ChatCommandListing::DebugOnly && !g_debugLogging)
			continue;

		g_ChatOverlay.AddSystemMessage("  " + command.cmd + " " + command.desc);
	}
}

void ChatOverlay::Init()
{
	m_GameWindow = FindMainWindow();
	m_OriginalWndProc = (WNDPROC)SetWindowLongPtr(
		m_GameWindow, GWLP_WNDPROC, (LONG_PTR)hkWndProc);

	HookEndScene();

	// Hook GetAsyncKeyState
	if (MH_CreateHook(&GetAsyncKeyState, &hkGetAsyncKeyState,
		reinterpret_cast<LPVOID*>(&oGetAsyncKeyState)) == MH_OK)
	{
		MH_EnableHook(&GetAsyncKeyState);
	}

	// Hook message queue APIs
	HMODULE hUser32 = GetModuleHandleA("user32.dll");
	if (hUser32)
	{
		void* pPeekA = GetProcAddress(hUser32, "PeekMessageA");
		if (pPeekA)
			MH_CreateHook(pPeekA, hkPeekMessageA, (LPVOID*)&oPeekMessageA),
			MH_EnableHook(pPeekA);

		void* pPeekW = GetProcAddress(hUser32, "PeekMessageW");
		if (pPeekW)
			MH_CreateHook(pPeekW, hkPeekMessageW, (LPVOID*)&oPeekMessageW),
			MH_EnableHook(pPeekW);

		void* pGetA = GetProcAddress(hUser32, "GetMessageA");
		if (pGetA)
			MH_CreateHook(pGetA, hkGetMessageA, (LPVOID*)&oGetMessageA),
			MH_EnableHook(pGetA);

		void* pGetW = GetProcAddress(hUser32, "GetMessageW");
		if (pGetW)
			MH_CreateHook(pGetW, hkGetMessageW, (LPVOID*)&oGetMessageW),
			MH_EnableHook(pGetW);
	}

	// Start low-level keyboard hook thread
	CloseHandle(CreateThread(NULL, 0, KeyboardHookThread, NULL, 0, NULL));

	// Start chat input polling thread
	CloseHandle(CreateThread(NULL, 0, ChatInputThread, NULL, 0, NULL));

	AddCommand("/help", "- List available commands", HelpCommand);
}

void ChatOverlay::Shutdown()
{
	if (m_OriginalWndProc && m_GameWindow)
	{
		SetWindowLongPtr(m_GameWindow, GWLP_WNDPROC,
			(LONG_PTR)m_OriginalWndProc);
	}

	if (m_Font)
		m_Font->Release();

	m_Commands.clear();
}

void ChatOverlay::SendChatMessage(const std::string& msg)
{
	std::string clean = SanitizeChatText(msg, MaxChatDisplayLength);
	if (clean.empty())
		return;

	m_ChatHistory.push_back({clean, GetTickCount()});
	m_LastMessageTime = GetTickCount();
	if (m_ChatHistory.size() > MaxChatHistory)
		m_ChatHistory.erase(m_ChatHistory.begin());
}

void ChatOverlay::AddSystemMessage(const std::string& text)
{
	m_ChatHistory.push_back({SanitizeChatText(text, MaxChatDisplayLength), GetTickCount()});
	m_LastMessageTime = GetTickCount();
	if (m_ChatHistory.size() > MaxChatHistory)
		m_ChatHistory.erase(m_ChatHistory.begin());
}

// ===========================================================================
//  Input history (Up / Down)
// ===========================================================================

void ChatOverlay::ResetHistoryBrowsing()
{
	m_HistoryCursor = -1;
	m_DraftBuffer.clear();
}

void ChatOverlay::RememberInput(const std::string& line)
{
	// Consecutive repeats are not worth a slot - same as a shell with
	// ignoredups, and it keeps a spammed command from filling the list.
	if (!line.empty() && (m_InputHistory.empty() || m_InputHistory.back() != line))
	{
		m_InputHistory.push_back(line);
		if (m_InputHistory.size() > MaxInputHistory)
			m_InputHistory.erase(m_InputHistory.begin());
	}

	ResetHistoryBrowsing();
}

void ChatOverlay::RecallPreviousInput()
{
	if (m_InputHistory.empty())
		return;

	if (m_HistoryCursor < 0)
	{
		// Starting to browse: keep whatever was already typed so Down restores it.
		m_DraftBuffer = m_ChatBuffer;
		m_HistoryCursor = static_cast<int>(m_InputHistory.size()) - 1;
	}
	else if (m_HistoryCursor > 0)
	{
		--m_HistoryCursor;
	}
	else
	{
		return;   // already on the oldest line
	}

	SetInputBuffer(m_InputHistory[m_HistoryCursor]);
}

void ChatOverlay::RecallNextInput()
{
	if (m_HistoryCursor < 0)
		return;   // not browsing, nothing newer to go to

	if (m_HistoryCursor + 1 < static_cast<int>(m_InputHistory.size()))
	{
		++m_HistoryCursor;
		SetInputBuffer(m_InputHistory[m_HistoryCursor]);
		return;
	}

	// Past the newest entry: back to the line that was being typed.
	SetInputBuffer(m_DraftBuffer);
	ResetHistoryBrowsing();
}

// ===========================================================================
//  Input line editing (caret)
// ===========================================================================

void ChatOverlay::SetInputBuffer(const std::string& text)
{
	m_ChatBuffer = text;
	m_CaretPos = m_ChatBuffer.size();
}

void ChatOverlay::ClearInputBuffer()
{
	m_ChatBuffer.clear();
	m_CaretPos = 0;
}

void ChatOverlay::MoveCaret(int delta)
{
	if (delta < 0)
	{
		const size_t step = static_cast<size_t>(-delta);
		m_CaretPos = (m_CaretPos > step) ? (m_CaretPos - step) : 0;
		return;
	}

	m_CaretPos += static_cast<size_t>(delta);
	if (m_CaretPos > m_ChatBuffer.size())
		m_CaretPos = m_ChatBuffer.size();
}

void ChatOverlay::MoveCaretToStart()
{
	m_CaretPos = 0;
}

void ChatOverlay::MoveCaretToEnd()
{
	m_CaretPos = m_ChatBuffer.size();
}

void ChatOverlay::DeleteBeforeCaret()
{
	if (m_CaretPos == 0 || m_ChatBuffer.empty())
		return;

	if (m_CaretPos > m_ChatBuffer.size())
		m_CaretPos = m_ChatBuffer.size();

	m_ChatBuffer.erase(m_CaretPos - 1, 1);
	--m_CaretPos;
}

void ChatOverlay::DeleteAtCaret()
{
	if (m_CaretPos >= m_ChatBuffer.size())
		return;

	m_ChatBuffer.erase(m_CaretPos, 1);
}

void ChatOverlay::AppendInputChar(char ch)
{
	if (m_ChatBuffer == "/" && ch == '/')
		return;

	if (!IsSafeChatChar(static_cast<unsigned char>(ch)) || m_ChatBuffer.size() >= MaxChatInputLength)
		return;

	// Typed text lands at the caret, not the end, so a line can be fixed in the
	// middle instead of being retyped.
	if (m_CaretPos > m_ChatBuffer.size())
		m_CaretPos = m_ChatBuffer.size();

	m_ChatBuffer.insert(m_CaretPos, 1, ch);
	++m_CaretPos;
}

std::string ChatOverlay::GetAutocompleteSuggestion() const
{
	if (m_ChatBuffer.empty() || m_ChatBuffer[0] != '/' || m_ChatBuffer.find(' ') != std::string::npos)
		return std::string();

	for (const ChatCommand& command : m_Commands)
	{
		if (StartsWithCaseInsensitive(command.cmd, m_ChatBuffer) && command.cmd.size() > m_ChatBuffer.size())
			return command.cmd.substr(m_ChatBuffer.size());
	}

	return std::string();
}

void ChatOverlay::AutocompleteCommand()
{
	std::string suggestion = GetAutocompleteSuggestion();
	if (!suggestion.empty())
		SetInputBuffer(m_ChatBuffer + suggestion);
}

void ChatOverlay::ProcessChatSend()
{
	std::string input = SanitizeChatText(m_ChatBuffer, MaxChatInputLength);
	if (input.empty())
	{
		// Enter on an empty line just closes the box - but it can be reached
		// while browsing (recall a line, clear it, press Enter), so the cursor
		// has to be dropped here too or the next Up would resume mid-list.
		ResetHistoryBrowsing();
		m_ChatOpen = false;
		return;
	}

	// A command may ask to keep the box open (see HelpCommand); default is the
	// old behaviour of closing as soon as the line is handled.
	m_KeepChatOpen = false;

	// Recorded before dispatch so a command that fails or closes the box still
	// leaves its line available on Up.
	RememberInput(input);

	if (input[0] == '/')
	{
		std::string command = input;
		size_t space = command.find(' ');
		if (space != std::string::npos)
			command = command.substr(0, space);

		for(size_t i = 0; i < m_Commands.size(); i++) {
			if(StartsWithCaseInsensitive(m_Commands[i].cmd, command) && m_Commands[i].cmd.size() == command.size()) {
				std::string params = (space != std::string::npos)
					? input.substr(space + 1) : "";
				m_Commands[i].callback(params);
				goto found;
			}
		}

		AddSystemMessage("[System] Unknown command. Write /help to see all commands.");
	}
	else
	{
		// DONT add message themselves! Wait for echo from server (fot connection test)
		// AddSystemMessage("[Chat] " + m_ChatBuffer);

		if(m_MsgCallback)
			m_MsgCallback(input);
	}

	found:
	ClearInputBuffer();
	m_ChatOpen = m_KeepChatOpen;
	m_KeepChatOpen = false;
}

void ChatOverlay::Render(LPDIRECT3DDEVICE9 pDevice)
{
	if (!m_Font)
	{
		D3DXCreateFontA(pDevice, 18, 0, FW_BOLD, 1, FALSE,
			DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
			DEFAULT_PITCH | FF_DONTCARE, "Consolas", &m_Font);
	}

	IDirect3DStateBlock9* stateBlock = nullptr;
	pDevice->CreateStateBlock(D3DSBT_ALL, &stateBlock);

	// Enable alpha blending for chat text fade
	pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

	// Draw chat history
	int startY = 400;
	DWORD now = GetTickCount();

	int alpha = 255;
	if (!m_ChatOpen && m_LastMessageTime > 0)
	{
		DWORD elapsed = now - m_LastMessageTime;
		if (elapsed > 6000)
		{
			DWORD fade = elapsed - 6000;
			if (fade >= 2000)
				alpha = 0; // fully invisible
			else
				alpha = 255 - (int)(fade * 255 / 2000); // fade to 0 over 2s
		}
	}

	// Open or closed, never draw more lines than fit above the input box.
	int histSize = (int)m_ChatHistory.size();
	int showCount = (histSize < MaxVisibleChatLines) ? histSize : MaxVisibleChatLines;
	int startIdx = (int)m_ChatHistory.size() - showCount;
	D3DCOLOR color = D3DCOLOR_ARGB((BYTE)alpha, 255, 255, 255);
	for (int i = 0; i < showCount; i++)
	{
		DrawTextSimple(m_Font, m_ChatHistory[startIdx + i].text, 20, startY + i * 20, color);
	}

	// Draw input box
	if (m_ChatOpen)
	{
		DrawFilledRect(pDevice, 15, 650, 500, 30,
			D3DCOLOR_ARGB(180, 0, 0, 0));

		std::string text = "> " + SanitizeChatText(m_ChatBuffer, MaxChatInputLength);
		std::string suggestion = GetAutocompleteSuggestion();

		DrawTextSimple(m_Font, text, 25, 655,
			D3DCOLOR_XRGB(255, 255, 255));

		int typedWidth = MeasureTextWidth(m_Font, text);
		if (!suggestion.empty())
		{
			DrawTextSimple(m_Font, suggestion, 25 + typedWidth, 655,
				D3DCOLOR_ARGB(120, 255, 255, 255));
		}

		// Caret sits at the insertion point rather than always at the end, so
		// Left/Right/Home/End are visible. Measured over the same string that
		// was drawn ("> " + the sanitized line), clamped in case sanitising
		// dropped a character the caret index still counted.
		if ((GetTickCount64() / 500) % 2)
		{
			const size_t typedChars = (text.size() >= 2) ? (text.size() - 2) : 0;
			const size_t caretChars = (m_CaretPos < typedChars) ? m_CaretPos : typedChars;
			const int caretX = (caretChars == typedChars)
				? typedWidth   // already measured for the suggestion
				: MeasureTextWidth(m_Font, text.substr(0, 2 + caretChars));

			DrawTextSimple(m_Font, "_", 25 + caretX, 655,
				D3DCOLOR_XRGB(255, 255, 255));
		}
	}

	if (stateBlock)
	{
		stateBlock->Apply();
		stateBlock->Release();
	}

	return;
}

void ChatOverlay::SetMsgCallback(ChatMessageCallback_t callback)
{
	m_MsgCallback = callback;
}

void ChatOverlay::AddCommand(const std::string &name, const std::string &desc, ChatCommandCallback_t callback,
	ChatCommandListing listing)
{
	m_Commands.push_back({name,desc,callback,listing});
}
