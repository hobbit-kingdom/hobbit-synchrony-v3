#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <d3d9.h>
#include <d3dx9.h>

typedef void (*ChatMessageCallback_t)(const std::string& msg);
typedef void (*ChatCommandCallback_t)(const std::string& parameters);

struct ChatMessage
{
	std::string text;
	DWORD       timestamp;
};

/// Whether /help mentions a command outside a debug session. This controls the
/// LISTING only - every command stays typeable either way, so anyone who knows
/// /damage or /syncanim can still use them in a release build.
enum class ChatCommandListing
{
	Always,      ///< part of the normal player-facing set
	DebugOnly,   ///< development / troubleshooting, listed when debug=1
};

struct ChatCommand
{
	std::string cmd;
	std::string desc;
	ChatCommandCallback_t callback;
	ChatCommandListing listing = ChatCommandListing::Always;
};

/// How many messages are remembered, and how many are drawn when the chat box is
/// closed. The history used to hold six, which silently ate the first half of any
/// longer burst - /help among them. Twelve drawn lines is what fits between the
/// first line (y=400) and the input box (y=650) at 20px spacing.
constexpr size_t MaxChatHistory = 16;
constexpr int    MaxVisibleChatLines = 12;

/// How many sent lines Up/Down can walk back through.
constexpr size_t MaxInputHistory = 32;

class ChatOverlay
{
	public:
	bool m_ChatOpen;
	std::string m_ChatBuffer;
	std::vector<ChatMessage> m_ChatHistory;
	DWORD m_LastMessageTime;

	/// Set by a command that printed something worth reading: the box then stays
	/// open after it runs, which keeps the text at full alpha instead of letting
	/// it fade out a few seconds later.
	bool m_KeepChatOpen = false;

	/// Insertion point in m_ChatBuffer, 0..size(). Every path that replaces the
	/// buffer wholesale must move this with it - use SetInputBuffer, which does
	/// both, rather than assigning m_ChatBuffer directly.
	size_t m_CaretPos = 0;

	// --- Input history (Up / Down, like a terminal) ---

	/// Lines that were actually sent, oldest first.
	std::vector<std::string> m_InputHistory;
	/// Position in that list while browsing; -1 means "editing a fresh line".
	int m_HistoryCursor = -1;
	/// What was half-typed when browsing started, so Down can hand it back.
	std::string m_DraftBuffer;

	public:
	std::vector<ChatCommand> m_Commands;
	ChatMessageCallback_t m_MsgCallback;

	public:
	bool m_KeyState[256];

	public:
	// --- Window ---
	HWND m_GameWindow;
	WNDPROC m_OriginalWndProc;

	// --- Low-level keyboard hook ---
	HHOOK m_hLLKeyboardHook;

	private:
	ID3DXFont* m_Font;

	public:

	void Init();
	void Shutdown();

	void SendChatMessage(const std::string& msg);
	void AddSystemMessage(const std::string& text);
	void ProcessChatSend();
	void AppendInputChar(char ch);
	void AutocompleteCommand();

	/// Replace the whole input line and park the caret at its end.
	void SetInputBuffer(const std::string& text);
	void ClearInputBuffer();

	// --- Caret movement / editing (Left, Right, Home, End, Backspace, Delete) ---
	void MoveCaret(int delta);
	void MoveCaretToStart();
	void MoveCaretToEnd();
	void DeleteBeforeCaret();
	void DeleteAtCaret();

	/// Terminal-style recall: Up walks towards older lines, Down back towards the
	/// line that was being typed. RememberInput records a sent line; ResetHistory
	/// Browsing puts the cursor back on a fresh line without touching the list.
	void RecallPreviousInput();
	void RecallNextInput();
	void RememberInput(const std::string& line);
	void ResetHistoryBrowsing();
	std::string GetAutocompleteSuggestion() const;

	void Render(LPDIRECT3DDEVICE9 pDevice);

	void SetMsgCallback(ChatMessageCallback_t callback);
	void AddCommand(const std::string &name, const std::string &desc, ChatCommandCallback_t callback,
		ChatCommandListing listing = ChatCommandListing::Always);
};

extern ChatOverlay g_ChatOverlay;
