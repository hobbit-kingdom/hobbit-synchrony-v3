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

	GUID_ASSIGN,
	SKIN_ANNOUNCE,
	SKIN_FILE_TRANSFER,
	SKIN_CLEAR,
	NICKNAME_UPDATE,
	STATUS_UPDATE,
	CHAT_MESSAGE,
	STONE_THROW,
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