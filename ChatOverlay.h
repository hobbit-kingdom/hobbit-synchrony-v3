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

struct ChatCommand
{
	std::string cmd;
	std::string desc;
	ChatCommandCallback_t callback;
};

class ChatOverlay
{
	public:
	bool m_ChatOpen;
	std::string m_ChatBuffer;
	std::vector<ChatMessage> m_ChatHistory;
	DWORD m_LastMessageTime;

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
	void AddCommand(const std::string &name, const std::string &desc, ChatCommandCallback_t callback);
};

extern ChatOverlay g_ChatOverlay;
