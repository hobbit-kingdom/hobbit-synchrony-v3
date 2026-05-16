#pragma once

#include <Windows.h>
#include <string>
#include <vector>

void ChatOverlay_Init(HMODULE hModule);
void ChatOverlay_Shutdown();

void SendChatMessage(const std::string& msg);

typedef void (*ChatNicknameCallback_t)(const std::string& name);
void ChatOverlay_SetNicknameCallback(ChatNicknameCallback_t cb);

struct ChatMessage
{
	std::string text;
	DWORD       timestamp;
};

extern bool         g_ChatOpen;
extern std::string  g_ChatBuffer;
extern std::vector<ChatMessage> g_ChatHistory;
