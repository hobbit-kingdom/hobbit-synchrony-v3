/*
	GameTypes.h
	Common types, constants, and utility functions shared across client and server.
*/

#pragma once

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
