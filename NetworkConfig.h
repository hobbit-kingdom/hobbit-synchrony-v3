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
YOJIMBO_DECLARE_MESSAGE_TYPE(HOISTABLE_UPDATE, HoistableStateMessage);
YOJIMBO_DECLARE_MESSAGE_TYPE(GUID_ASSIGN, GuidAssignMessage);
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
