/*
	NetworkMessages.h
	Game-specific network message definitions.
	These are the messages exchanged between the Hobbit multiplayer client and server.
*/

#pragma once

#include "yojimbo.h"
#include <cstdint>
#include <vector>
#include "GameTypes.h"
#include "SkinSystem.h"
#include <unordered_map>

using namespace yojimbo;

// ---------------------------------------------------------------------------
// Message Type Enum
// ---------------------------------------------------------------------------

enum GameMessageType
{
	POSITION_UPDATE,
	ENEMIES_UPDATE,

	HOISTABLE_ACQUIRE,
	HOISTABLE_RELEASE,
	HOISTABLE_UPDATE,

	PUSHBLOCK_ACQUIRE,
	PUSHBLOCK_RELEASE,
	PUSHBLOCK_UPDATE,

	WEB_WALL_BREAK,

	GUID_ASSIGN,
	SKIN_ANNOUNCE,
	SKIN_FILE_TRANSFER,
	SKIN_CLEAR,
	NICKNAME_UPDATE,
	STATUS_UPDATE,
	CHAT_MESSAGE,
	STONE_THROW,
	CHEST_OPEN,
	PICKUP_COLLECT,
	TRIGGER_ONPRESSB,
	TRIGGER_ONUSE,
	SWITCH_TOGGLE,
	ANIM_SYNC,
	RING_SYNC,
	SPAWN_OBJECT,
	SPAWN_FX,
	CINEMA_SYNC,
	ANIM_SYNC_REQUEST,
	NUM_GAME_MESSAGE_TYPES
};

// uint64_t must be serialized as two 32-bit halves
#define serialize_GUID(stream, guid64) \
	do { \
		uint32_t guid_low = static_cast<uint32_t>(guid64); \
		uint32_t guid_high = static_cast<uint32_t>(guid64 >> 32); \
		serialize_bits(stream, guid_low, 32); \
		serialize_bits(stream, guid_high, 32); \
		guid64 = (static_cast<uint64_t>(guid_high) << 32) | guid_low; \
	} while(0)

struct WebWallBreakMessage : public Message
{
    uint64_t wallGuid = 0;
    float    breakX = 0.0f;
    float    breakY = 0.0f;
    float    breakZ = 0.0f;
    uint32_t nowLevel = 0;

    template <typename Stream>
    bool Serialize(Stream& stream)
    {
        serialize_GUID(stream, wallGuid);
        serialize_float(stream, breakX);
        serialize_float(stream, breakY);
        serialize_float(stream, breakZ);
        serialize_bits(stream, nowLevel, 32);

        if (!stream.IsWriting)
        {
            nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
        }

        return true;
    }

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};



// both HOISTABLE_ACQUIRE & HOISTABLE_RELEASE
struct HoistableAcquireReleaseMessage : public Message
{
	uint64_t hoistableGuid = 0;
	uint64_t playerGuid = 0;
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, hoistableGuid);
		serialize_GUID(stream, playerGuid);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct HoistableStateMessage : public Message
{
	float    x = 0.0f;
	float    y = 0.0f;
	float    z = 0.0f;
	float    rotationY = 0.0f;
	uint64_t hoistableGuid = 0;
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_float(stream, x);
		serialize_float(stream, y);
		serialize_float(stream, z);
		serialize_float(stream, rotationY);
		serialize_GUID(stream, hoistableGuid);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			x = NetworkClamp::sanitizePosition(x);
			y = NetworkClamp::sanitizePosition(y);
			z = NetworkClamp::sanitizePosition(z);
			rotationY = NetworkClamp::sanitizeRotationRadians(rotationY);
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};


// ---------------------------------------------------------------------------
// PositionMessage — Player state update (position, rotation, animation)
// ---------------------------------------------------------------------------

struct PositionMessage : public Message
{
	float    x = 0.0f;
	float    y = 0.0f;
	float    z = 0.0f;
	float    rotationY = 0.0f;
	uint32_t animation = 0;
	float    animFrame = 0.0f;
	float    lastAnimFrame = 0.0f;
	uint64_t playerGuid = 0;

	uint32_t bilboWeapon = 0;
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_float(stream, x);
		serialize_float(stream, y);
		serialize_float(stream, z);
		serialize_float(stream, rotationY);
		serialize_bits(stream, animation, 32);
		serialize_float(stream, animFrame);
		serialize_float(stream, lastAnimFrame);

		serialize_GUID(stream, playerGuid);
		serialize_bits(stream, bilboWeapon, 32);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			x = NetworkClamp::sanitizePosition(x);
			y = NetworkClamp::sanitizePosition(y);
			z = NetworkClamp::sanitizePosition(z);
			rotationY = NetworkClamp::sanitizeRotationRadians(rotationY);
			animation = NetworkClamp::sanitizeAnimation(animation);
			animFrame = NetworkClamp::sanitizeAnimationFrame(animFrame);
			lastAnimFrame = NetworkClamp::sanitizeAnimationFrame(lastAnimFrame);
			bilboWeapon = NetworkClamp::sanitizeWeapon(bilboWeapon);
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

// ---------------------------------------------------------------------------
// GuidAssignMessage — Server -> Client: your assigned NPC GUID
// ---------------------------------------------------------------------------

struct GuidAssignMessage : public Message
{
	uint64_t guid = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, guid);
		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct SkinAnnouncementMessage : public Message
{
	uint64_t playerGuid = 0;
	char     textureName[SkinSync::MaxTextureNameLength] = {};

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, playerGuid);
		serialize_string(stream, textureName, sizeof(textureName));
		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct SkinFileTransferMessage : public BlockMessage
{
	uint64_t playerGuid = 0;
	char     textureName[SkinSync::MaxTextureNameLength] = {};
	char     fileName[SkinSync::MaxFileNameLength] = {};

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, playerGuid);
		serialize_string(stream, textureName, sizeof(textureName));
		serialize_string(stream, fileName, sizeof(fileName));
		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct SkinClearMessage : public Message
{
	uint64_t playerGuid = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, playerGuid);
		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};


// ---------------------------------------
// ---------------------------------------



struct EnemiesStateMessage : public Message
{
	std::unordered_map<uint64_t, Enemy> enemies;

	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		if (stream.IsWriting)
		{
			// Serialize the number of enemies
			uint32_t numEnemies = static_cast<uint32_t>(std::min(enemies.size(), NetworkClamp::MaxEnemyUpdatesPerMessage));
			serialize_bits(stream, numEnemies, 16);  // assuming max 65535 enemies

			// Serialize each enemy key-value pair
			size_t count = 0;
			for (const auto& pair : enemies)
			{
				if (count >= NetworkClamp::MaxEnemyUpdatesPerMessage)
					break;

				uint64_t guid = pair.first;
				serialize_GUID(stream, guid);

				Enemy e = NetworkClamp::sanitizeEnemy(pair.second);
				serialize_float(stream, e.x);
				serialize_float(stream, e.y);
				serialize_float(stream, e.z);
				serialize_float(stream, e.rot);
				serialize_bits(stream, e.anim, 32);
				serialize_float(stream, e.health);
				serialize_bool(stream, e.shieldIntact);

				++count;
			}

			serialize_bits(stream, nowLevel, 32);
		}
		else  // reading
		{
			// Deserialize the number of enemies
			uint32_t numEnemies;
			serialize_bits(stream, numEnemies, 16);
			if (numEnemies > NetworkClamp::MaxEnemyUpdatesPerMessage)
				return false;

			enemies.clear();

			// Deserialize each enemy
			for (uint32_t i = 0; i < numEnemies; ++i)
			{
				uint64_t guid = 0;
				serialize_GUID(stream, guid);

				Enemy e;
				serialize_float(stream, e.x);
				serialize_float(stream, e.y);
				serialize_float(stream, e.z);
				serialize_float(stream, e.rot);
				serialize_bits(stream, e.anim, 32);
				serialize_float(stream, e.health);
				serialize_bool(stream, e.shieldIntact);

				if (guid != 0)
					enemies[guid] = NetworkClamp::sanitizeEnemy(e);
			}

			serialize_bits(stream, nowLevel, 32);
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct NicknameUpdateMessage : public Message
{
	uint64_t player_guid = 0;
	char new_name[32];

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, player_guid);
		serialize_string(stream, new_name, sizeof(new_name));
		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct StatusUpdateMessage : public Message
{
	uint64_t player_guid = 0;
	char new_status[64];

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, player_guid);
		serialize_string(stream, new_status, sizeof(new_status));
		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct ChatMsgMessage : public Message
{
	uint64_t player_guid = 0; // sender GUID
	char msg[128];

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, player_guid);
		serialize_string(stream, msg, sizeof(msg));

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

// ---------------------------------------------------------------------------
// StoneThrowMessage — a player threw a stone from one point to another
// ---------------------------------------------------------------------------

struct StoneThrowMessage : public Message
{
	uint64_t playerGuid = 0;   // who threw it
	float    fromX = 0.0f;     // spawn point (hand)
	float    fromY = 0.0f;
	float    fromZ = 0.0f;
	float    toX = 0.0f;       // destination / aim point
	float    toY = 0.0f;
	float    toZ = 0.0f;
	uint8_t  stoneType = 0x19; // 0x19=normal, 0x1A=fire, 0x1B=explosive, 0x1C=freeze
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, playerGuid);
		serialize_float(stream, fromX);
		serialize_float(stream, fromY);
		serialize_float(stream, fromZ);
		serialize_float(stream, toX);
		serialize_float(stream, toY);
		serialize_float(stream, toZ);
		serialize_bits(stream, stoneType, 8);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			fromX = NetworkClamp::sanitizePosition(fromX);
			fromY = NetworkClamp::sanitizePosition(fromY);
			fromZ = NetworkClamp::sanitizePosition(fromZ);
			toX = NetworkClamp::sanitizePosition(toX);
			toY = NetworkClamp::sanitizePosition(toY);
			toZ = NetworkClamp::sanitizePosition(toZ);
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct ChestOpenMessage : public Message
{
	uint64_t chestGuid = 0;
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, chestGuid);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct PickupCollectMessage : public Message
{
	uint64_t pickupGuid = 0;
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, pickupGuid);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct TriggerOnPressBMessage : public Message
{
	uint64_t triggerGuid = 0;
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, triggerGuid);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct TriggerOnUseMessage : public Message
{
	uint64_t triggerGuid = 0;
	uint32_t nowLevel = 0;
	int32_t  itemCount = 0;
	int32_t  itemIds[4] = { -1, -1, -1, -1 };

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, triggerGuid);
		serialize_bits(stream, nowLevel, 32);
		serialize_int(stream, itemCount, 0, 4);

		for (int i = 0; i < 4; i++)
		{
			serialize_int(stream, itemIds[i], -1, 1023);
		}

		if (!stream.IsWriting)
		{
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

struct SwitchToggleMessage : public Message
{
	uint64_t switchGuid = 0;
	uint8_t  switchedOn = 0;
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, switchGuid);
		serialize_bits(stream, switchedOn, 8);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

// ---------------------------------------------------------------------------
// AnimSyncRequestMessage — a non-host client just (re)activated part of its
// world (level entry, or one of ITS OWN load triggers fired) and wants the
// host's looping-animation frames. Carries only the requester's level so the
// host can ignore requests from a different level. The host answers with the
// normal AnimSyncMessage broadcast; clients that didn't ask re-apply frames
// their loops are already at, which is visually a no-op.
// ---------------------------------------------------------------------------

struct AnimSyncRequestMessage : public Message
{
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_bits(stream, nowLevel, 32);
		if (stream.IsReading)
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

// ---------------------------------------------------------------------------
// AnimSyncMessage — one-shot snapshot of the current animation frame of every
// LOOPING animated rigid_instance on the level, keyed by object GUID (one-shot
// anims - doors, levers - are excluded; they belong to the trigger sync). Sent by a
// peer; each receiver snaps its matching objects to those frames. Because the
// engine's rigid_instance animations loop at a fixed rate, a single alignment
// keeps them in sync from that point on. (Same variable-length map shape as
// EnemiesStateMessage.)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// RingSyncMessage — a player toggled the One Ring on/off. Receivers apply the
// ring's stealth effect to that player's fake-bilbo NPC, tiered by the NPC's
// team: team 2 => invisible, otherwise => half-transparent (matching bilbo's
// own ring look). Only the equipped state travels; the target's team is read
// from the NPC locally.
// ---------------------------------------------------------------------------

struct RingSyncMessage : public Message
{
	uint64_t playerGuid = 0;    // whose ring toggled (== their fake-bilbo NPC GUID)
	uint8_t  ringEquipped = 0;  // 1 = ring on, 0 = ring off
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, playerGuid);
		serialize_bits(stream, ringEquipped, 8);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

// ---------------------------------------------------------------------------
// CinemaSyncMessage — a cinema (cutscene) started on one player's machine and
// it is one of the GUIDs listed in SYNCED_CINEMAS.txt, so everyone else plays
// it too. Typically the "you were spotted" cutscene on stealth levels, but the
// list is arbitrary, so any cinema can be shared this way.
//
// Only the GUID travels. Cinema objects are authored into the level, so every
// client already has the same object under the same GUID; receivers just call
// cinema::Start on their own copy.
// ---------------------------------------------------------------------------

struct CinemaSyncMessage : public Message
{
	uint64_t cinemaGuid = 0;   // the cinema object's GUID
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, cinemaGuid);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

// ---------------------------------------------------------------------------
// SpawnObjectMessage — a player spawned a RigidInstance. Receivers create the
// same object with the SAME GUID (so it stays cross-referenceable) at the same
// world position, optionally configured from a shared template file. Mirrors
// Kingjoyer's spawn recipe: obj_mgr::CreateObject("RigidInstance", guid) ->
// object::OnImport(template) -> object::Move(pos).
// ---------------------------------------------------------------------------

struct SpawnObjectMessage : public Message
{
	uint64_t objectGuid = 0;      // shared GUID (assigned by the spawner's engine)
	float    x = 0.0f;
	float    y = 0.0f;
	float    z = 0.0f;
	char     templateName[64] = {}; // *.export in ./Templates/ (empty = bare object)
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_GUID(stream, objectGuid);
		serialize_float(stream, x);
		serialize_float(stream, y);
		serialize_float(stream, z);
		serialize_string(stream, templateName, sizeof(templateName));
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			x = NetworkClamp::sanitizePosition(x);
			y = NetworkClamp::sanitizePosition(y);
			z = NetworkClamp::sanitizePosition(z);
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

// ---------------------------------------------------------------------------
// SpawnFxMessage — play a one-shot FX effect at a world position on every peer.
// Maps to fx_object::FireAndForget(name, pos, rot, scale): the effect plays then
// auto-destroys, so no GUID/lifetime sync is needed — each client just plays it.
// ---------------------------------------------------------------------------

struct SpawnFxMessage : public Message
{
	char     fxName[64] = {};    // e.g. "fx_fire"
	float    x = 0.0f, y = 0.0f, z = 0.0f;          // world position
	float    pitch = 0.0f, yaw = 0.0f, roll = 0.0f; // rotation (radians)
	float    scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		serialize_string(stream, fxName, sizeof(fxName));
		serialize_float(stream, x);
		serialize_float(stream, y);
		serialize_float(stream, z);
		serialize_float(stream, pitch);
		serialize_float(stream, yaw);
		serialize_float(stream, roll);
		serialize_float(stream, scaleX);
		serialize_float(stream, scaleY);
		serialize_float(stream, scaleZ);
		serialize_bits(stream, nowLevel, 32);

		if (!stream.IsWriting)
		{
			x = NetworkClamp::sanitizePosition(x);
			y = NetworkClamp::sanitizePosition(y);
			z = NetworkClamp::sanitizePosition(z);
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};

// Cap objects per message so a crowded level can't blow the packet budget.
static const size_t MaxAnimSyncPerMessage = 512;

struct AnimSyncMessage : public Message
{
	std::unordered_map<uint64_t, float> frames; // rigid_instance GUID -> anim frame
	uint32_t nowLevel = 0;

	template <typename Stream>
	bool Serialize(Stream& stream)
	{
		if (stream.IsWriting)
		{
			uint32_t numObjs = static_cast<uint32_t>(std::min(frames.size(), MaxAnimSyncPerMessage));
			serialize_bits(stream, numObjs, 16);

			size_t count = 0;
			for (const auto& pair : frames)
			{
				if (count >= MaxAnimSyncPerMessage)
					break;

				uint64_t guid = pair.first;
				float    frame = NetworkClamp::sanitizeAnimationFrame(pair.second);
				serialize_GUID(stream, guid);
				serialize_float(stream, frame);
				++count;
			}

			serialize_bits(stream, nowLevel, 32);
		}
		else // reading
		{
			uint32_t numObjs = 0;
			serialize_bits(stream, numObjs, 16);
			if (numObjs > MaxAnimSyncPerMessage)
				return false;

			frames.clear();
			for (uint32_t i = 0; i < numObjs; ++i)
			{
				uint64_t guid = 0;
				float    frame = 0.0f;
				serialize_GUID(stream, guid);
				serialize_float(stream, frame);

				if (guid != 0)
					frames[guid] = NetworkClamp::sanitizeAnimationFrame(frame);
			}

			serialize_bits(stream, nowLevel, 32);
			nowLevel = NetworkClamp::sanitizeLevel(nowLevel);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};
