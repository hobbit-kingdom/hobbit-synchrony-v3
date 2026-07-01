/*
	GameTypes.h
	Common types, constants, and utility functions shared across client and server.
*/

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// Math / Geometry
// ---------------------------------------------------------------------------

struct Vector3
{
	float x = 0.0f;
	float y = 0.0f;
	float z = 0.0f;
};

struct Enemy
{
	float x, y, z;        // Vector3 position
	float rot;            // rotationY
	uint32_t anim;        // animation
	float health;
};

// ---------------------------------------------------------------------------
// Networking Defaults
// ---------------------------------------------------------------------------

namespace NetDefaults
{
	constexpr int    MAX_CLIENTS = 64;
	constexpr double TICK_RATE = 15.0;          // updates per second
	constexpr double SEND_INTERVAL = 1.0 / TICK_RATE;
	constexpr double DELTA_TIME = 0.01;
	constexpr double LERP_DURATION = 1.0 / TICK_RATE;
	constexpr double INITIAL_TIME = 100.0;
	constexpr const char* DEFAULT_IP = "127.0.0.1";
	constexpr const char* CONFIG_FILE = "config.txt";
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

/// Parse a GUID string like "18B5CCD1_211A0401" into a uint64_t.
inline uint64_t guidFromString(const std::string& s)
{
	std::string clean = s;
	clean.erase(std::remove(clean.begin(), clean.end(), '_'), clean.end());
	return std::stoull(clean, nullptr, 16);
}

namespace NetworkClamp
{
	constexpr float PositionAbsMax = 100000.0f;
	constexpr float HealthMin = 0.0f;
	constexpr float HealthMax = 10000.0f;
	constexpr float AnimationFrameMin = 0.0f;
	constexpr float AnimationFrameMax = 1024.0f;

	constexpr uint32_t AnimationMax = 200;
	constexpr uint32_t WeaponDefault = 2;
	constexpr uint32_t LevelMax = 11;
	constexpr uint32_t InvalidLevel = LevelMax + 1;

	constexpr size_t MaxEnemyUpdatesPerMessage = 256;

	inline bool isFinite(float value)
	{
		return std::isfinite(value);
	}

	inline float clampFloat(float value, float minValue, float maxValue, float fallback = 0.0f)
	{
		if (!isFinite(value))
			return fallback;

		if (value < minValue)
			return minValue;

		if (value > maxValue)
			return maxValue;

		return value;
	}

	inline float sanitizePosition(float value)
	{
		return clampFloat(value, -PositionAbsMax, PositionAbsMax);
	}

	inline float sanitizeRotationRadians(float value)
	{
		if (!isFinite(value))
			return 0.0f;

		constexpr float Pi = 3.14159265358979323846f;
		constexpr float TwoPi = Pi * 2.0f;

		value = std::fmod(value, TwoPi);
		if (value > Pi)
			value -= TwoPi;
		else if (value < -Pi)
			value += TwoPi;

		return value;
	}

	inline uint32_t sanitizeAnimation(uint32_t value)
	{
		return value <= AnimationMax ? value : 1u;
	}

	inline float sanitizeAnimationFrame(float value)
	{
		return clampFloat(value, AnimationFrameMin, AnimationFrameMax);
	}

	inline uint32_t sanitizeWeapon(uint32_t value)
	{
		switch (value)
		{
		case 0u:
		case 1u:
		case 2u:
		case 3u:
			return value;

		default:
			return WeaponDefault;
		}
	}

	inline uint32_t sanitizeLevel(uint32_t value)
	{
		return value <= LevelMax ? value : InvalidLevel;
	}

	inline float sanitizeHealth(float value)
	{
		return clampFloat(value, HealthMin, HealthMax);
	}

	inline Enemy sanitizeEnemy(const Enemy& enemy)
	{
		Enemy sanitized = enemy;
		sanitized.x = sanitizePosition(enemy.x);
		sanitized.y = sanitizePosition(enemy.y);
		sanitized.z = sanitizePosition(enemy.z);
		sanitized.rot = sanitizeRotationRadians(enemy.rot);
		sanitized.anim = sanitizeAnimation(enemy.anim);
		sanitized.health = sanitizeHealth(enemy.health);
		return sanitized;
	}
}
