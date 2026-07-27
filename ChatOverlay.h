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
	std::string GetAutocompleteSuggestion() const;

	void Render(LPDIRECT3DDEVICE9 pDevice);

	void SetMsgCallback(ChatMessageCallback_t callback);
	void AddCommand(const std::string &name, const std::string &desc, ChatCommandCallback_t callback,
		ChatCommandListing listing = ChatCommandListing::Always);
};

extern ChatOverlay g_ChatOverlay;
