/*
	NetworkConfig.h
	Yojimbo networking configuration: protocol ID, ports, message factory, and adapter.
*/

#pragma once

#include "yojimbo.h"
#include "NetworkMessages.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cinttypes>
#include <ctime>

using namespace yojimbo;

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------

const uint64_t ProtocolId = 0x11223344556677ULL;

const int ClientPort = 30000;
const int ServerPort = 40000;

// ---------------------------------------------------------------------------
// Message Factory
// ---------------------------------------------------------------------------

YOJIMBO_MESSAGE_FACTORY_START(GameMessageFactory, NUM_GAME_MESSAGE_TYPES);
YOJIMBO_DECLARE_MESSAGE_TYPE(POSITION_UPDATE, PositionMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(ENEMIES_UPDATE, EnemiesStateMessage);

YOJIMBO_DECLARE_MESSAGE_TYPE(HOISTABLE_ACQUIRE, HoistableAcquireReleaseMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(HOISTABLE_RELEASE, HoistableAcquireReleaseMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(HOISTABLE_UPDATE, HoistableStateMessage);

YOJIMBO_DECLARE_MESSAGE_TYPE(PUSHBLOCK_ACQUIRE, HoistableAcquireReleaseMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(PUSHBLOCK_RELEASE, HoistableAcquireReleaseMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(PUSHBLOCK_UPDATE, HoistableStateMessage);

YOJIMBO_DECLARE_MESSAGE_TYPE(GUID_ASSIGN, GuidAssignMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(SKIN_ANNOUNCE, SkinAnnouncementMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(SKIN_FILE_TRANSFER, SkinFileTransferMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(SKIN_CLEAR, SkinClearMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(NICKNAME_UPDATE, NicknameUpdateMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(STATUS_UPDATE, StatusUpdateMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(CHAT_MESSAGE, ChatMsgMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(STONE_THROW, StoneThrowMessage);
YOJIMBO_MESSAGE_FACTORY_FINISH();

// ---------------------------------------------------------------------------
// Adapter
// ---------------------------------------------------------------------------

class GameAdapter : public Adapter
{
public:
	MessageFactory* CreateMessageFactory(Allocator& allocator)
	{
		return YOJIMBO_NEW(allocator, GameMessageFactory, allocator);
	}
};

static GameAdapter gameAdapter;

inline void ConfigureGameNetworking(ClientServerConfig& config)
{
	config.numChannels = 2;
	config.channel[channels::Gameplay].type = CHANNEL_TYPE_RELIABLE_ORDERED;
	config.channel[channels::Skin].type = CHANNEL_TYPE_RELIABLE_ORDERED;
	config.channel[channels::Skin].maxBlockSize = SkinSync::MaxSkinFileBytes;
}
