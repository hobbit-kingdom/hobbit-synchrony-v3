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
	HOISTABLE_UPDATE,
	GUID_ASSIGN,
	SKIN_ANNOUNCE,
	SKIN_FILE_TRANSFER,
	SKIN_CLEAR,
	NUM_GAME_MESSAGE_TYPES
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
		if (stream.IsWriting)
		{


			serialize_float(stream, x);
			serialize_float(stream, y);
			serialize_float(stream, z);
			serialize_float(stream, rotationY);

			// uint64_t must be serialized as two 32-bit halves
			uint32_t guid_low = static_cast<uint32_t>(hoistableGuid);
			uint32_t guid_high = static_cast<uint32_t>(hoistableGuid >> 32);
			serialize_bits(stream, guid_low, 32);
			serialize_bits(stream, guid_high, 32);
			hoistableGuid = (static_cast<uint64_t>(guid_high) << 32) | guid_low;

			serialize_bits(stream, nowLevel, 32);
		}

		else  // reading
		{
			serialize_float(stream, x);
			serialize_float(stream, y);
			serialize_float(stream, z);

			serialize_float(stream, rotationY);

			uint32_t guid_low;
			uint32_t guid_high;
			serialize_bits(stream, guid_low, 32);
			serialize_bits(stream, guid_high, 32);
			hoistableGuid = (static_cast<uint64_t>(guid_high) << 32) | guid_low;

			serialize_bits(stream, nowLevel, 32);
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

		// uint64_t must be serialized as two 32-bit halves
		uint32_t guid_low = static_cast<uint32_t>(playerGuid);
		uint32_t guid_high = static_cast<uint32_t>(playerGuid >> 32);
		serialize_bits(stream, guid_low, 32);
		serialize_bits(stream, guid_high, 32);
		playerGuid = (static_cast<uint64_t>(guid_high) << 32) | guid_low;

		serialize_bits(stream, bilboWeapon, 32);
		serialize_bits(stream, nowLevel, 32);

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
		uint32_t guid_low = static_cast<uint32_t>(guid);
		uint32_t guid_high = static_cast<uint32_t>(guid >> 32);
		serialize_bits(stream, guid_low, 32);
		serialize_bits(stream, guid_high, 32);
		guid = (static_cast<uint64_t>(guid_high) << 32) | guid_low;

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
		uint32_t guid_low = static_cast<uint32_t>(playerGuid);
		uint32_t guid_high = static_cast<uint32_t>(playerGuid >> 32);
		serialize_bits(stream, guid_low, 32);
		serialize_bits(stream, guid_high, 32);
		playerGuid = (static_cast<uint64_t>(guid_high) << 32) | guid_low;

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
		uint32_t guid_low = static_cast<uint32_t>(playerGuid);
		uint32_t guid_high = static_cast<uint32_t>(playerGuid >> 32);
		serialize_bits(stream, guid_low, 32);
		serialize_bits(stream, guid_high, 32);
		playerGuid = (static_cast<uint64_t>(guid_high) << 32) | guid_low;

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
		uint32_t guid_low = static_cast<uint32_t>(playerGuid);
		uint32_t guid_high = static_cast<uint32_t>(playerGuid >> 32);
		serialize_bits(stream, guid_low, 32);
		serialize_bits(stream, guid_high, 32);
		playerGuid = (static_cast<uint64_t>(guid_high) << 32) | guid_low;

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
			uint32_t numEnemies = static_cast<uint32_t>(enemies.size());
			serialize_bits(stream, numEnemies, 16);  // assuming max 65535 enemies

			// Serialize each enemy key-value pair
			for (const auto& pair : enemies)
			{
				uint64_t guid = pair.first;
				// uint64_t must be serialized as two 32-bit halves
				uint32_t guid_low = static_cast<uint32_t>(guid);
				uint32_t guid_high = static_cast<uint32_t>(guid >> 32);
				serialize_bits(stream, guid_low, 32);
				serialize_bits(stream, guid_high, 32);

				Enemy e = pair.second;  // copy to non-const
				serialize_float(stream, e.x);
				serialize_float(stream, e.y);
				serialize_float(stream, e.z);
				serialize_float(stream, e.rot);
				serialize_bits(stream, e.anim, 32);
				serialize_float(stream, e.health);
			}

			serialize_bits(stream, nowLevel, 32);
		}
		else  // reading
		{
			// Deserialize the number of enemies
			uint32_t numEnemies;
			serialize_bits(stream, numEnemies, 16);

			enemies.clear();

			// Deserialize each enemy
			for (uint32_t i = 0; i < numEnemies; ++i)
			{

				uint32_t guid_low;
				uint32_t guid_high;
				serialize_bits(stream, guid_low, 32);
				serialize_bits(stream, guid_high, 32);
				uint64_t guid = (static_cast<uint64_t>(guid_high) << 32) | guid_low;

				Enemy e;
				serialize_float(stream, e.x);
				serialize_float(stream, e.y);
				serialize_float(stream, e.z);
				serialize_float(stream, e.rot);
				serialize_bits(stream, e.anim, 32);
				serialize_float(stream, e.health);

				enemies[guid] = e;
			}

			serialize_bits(stream, nowLevel, 32);
		}

		return true;
	}

	YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS();
};
