#include "ChatOverlay.h"

#include <Windows.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <string>
#include <vector>

#include "kiero.h"
#include "minhook/MinHook.h"

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
	if (vk >= 'A' && vk <= 'Z') return shift ? vk : (vk + 32);
	if (vk >= '0' && vk <= '9') return vk;

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
				g_ChatOverlay.m_ChatBuffer.clear();
			}
			g_ChatOverlay.m_KeyState[VK_OEM_2] = slashPressed;
		}

		// Typing mode
		if (g_ChatOverlay.m_ChatOpen)
		{
			for (int vk = VK_SPACE; vk <= VK_OEM_7; vk++)
			{
				SHORT state = oGetAsyncKeyState(vk);
				bool pressed = (state & 0x8000) != 0;
				bool wasPressed = g_ChatOverlay.m_KeyState[vk];
				if (pressed && !wasPressed)
				{
					char c = MapVkToChar(vk, shift);
					if (c && g_ChatOverlay.m_ChatBuffer.size() < 128)
						g_ChatOverlay.m_ChatBuffer += c;
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
					if (c && g_ChatOverlay.m_ChatBuffer.size() < 128)
						g_ChatOverlay.m_ChatBuffer += c;
				}
				g_ChatOverlay.m_KeyState[vk] = pressed;
			}

			// Backspace
			{
				SHORT state = oGetAsyncKeyState(VK_BACK);
				bool pressed = (state & 0x8000) != 0;
				bool wasPressed = g_ChatOverlay.m_KeyState[VK_BACK];
				if (pressed && !wasPressed && !g_ChatOverlay.m_ChatBuffer.empty())
					g_ChatOverlay.m_ChatBuffer.pop_back();
				g_ChatOverlay.m_KeyState[VK_BACK] = pressed;
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
					g_ChatOverlay.m_ChatBuffer.clear();
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
			g_ChatOverlay.m_ChatBuffer.clear();
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
			char c = (char)wParam;
			if (c >= 32 && c <= 126 && g_ChatOverlay.m_ChatBuffer.size() < 128)
				g_ChatOverlay.m_ChatBuffer += c;
			return 0;
		}

		case WM_KEYUP:
			return CallWindowProc(g_ChatOverlay.m_OriginalWndProc, hwnd, msg, wParam, lParam);

		case WM_KEYDOWN:
		{
			switch (wParam)
			{
			case VK_BACK:
				if (!g_ChatOverlay.m_ChatBuffer.empty())
					g_ChatOverlay.m_ChatBuffer.pop_back();
				return 0;

			case VK_RETURN:
				g_ChatOverlay.ProcessChatSend();
				return 0;

			case VK_ESCAPE:
				g_ChatOverlay.m_ChatBuffer.clear();
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

static void HelpCommand(const std::string&)
{
	g_ChatOverlay.AddSystemMessage("[System] Available commands:");
	for(size_t i = 0; i < g_ChatOverlay.m_Commands.size(); i++) {
		g_ChatOverlay.AddSystemMessage("  " + g_ChatOverlay.m_Commands[i].cmd + " " + g_ChatOverlay.m_Commands[i].desc);
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
	if (msg.empty())
		return;

	m_ChatHistory.push_back({msg, GetTickCount()});
	m_LastMessageTime = GetTickCount();
	if (m_ChatHistory.size() > 6)
		m_ChatHistory.erase(m_ChatHistory.begin());
}

void ChatOverlay::AddSystemMessage(const std::string& text)
{
	m_ChatHistory.push_back({text, GetTickCount()});
	m_LastMessageTime = GetTickCount();
	if (m_ChatHistory.size() > 6)
		m_ChatHistory.erase(m_ChatHistory.begin());
}

void ChatOverlay::ProcessChatSend()
{
	if (m_ChatBuffer.empty())
	{
		m_ChatOpen = false;
		return;
	}

	if (m_ChatBuffer[0] == '/')
	{
		std::string command = m_ChatBuffer;
		size_t space = command.find(' ');
		if (space != std::string::npos)
			command = command.substr(0, space);

		for(size_t i = 0; i < m_Commands.size(); i++) {
			if(m_Commands[i].cmd == command) {
				std::string params = (space != std::string::npos)
					? m_ChatBuffer.substr(space + 1) : "";
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
			m_MsgCallback(m_ChatBuffer);
	}

	found:
	m_ChatBuffer.clear();
	m_ChatOpen = false;
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

	int histSize = (int)m_ChatHistory.size();
	int showCount = m_ChatOpen ? histSize : (histSize < 6 ? histSize : 6);
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

		std::string text = "> " + m_ChatBuffer;
		if ((GetTickCount64() / 500) % 2)
			text += "_";

		DrawTextSimple(m_Font, text, 25, 655,
			D3DCOLOR_XRGB(255, 255, 255));
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

void ChatOverlay::AddCommand(const std::string &name, const std::string &desc, ChatCommandCallback_t callback)
{
	m_Commands.push_back({name,desc,callback});
}