/*
	Player.h
	Represents a remote player seen by the local client.
	Holds current state, lerp targets, and an NPC handle for rendering in-game.
*/

#pragma once

#include <cstdint>
#include "NPC.h"
#include "MathUtils.h"

class Player
{
public:
	// --- Identity ---
	int      clientIndex = -1;
	uint64_t npcGuid = 0;

	// --- Current state ---
	float    x = 0.0f, y = 0.0f, z = 0.0f;
	float    rotationY = 0.0f;
	uint32_t animation = 0;
	float    animFrame = 0.0f;
	float    lastAnimFrame = 0.0f;

	uint32_t bilboWeapon = 0;
	uint32_t nowLevel = 0;

	// --- In-game representation ---
	NPC* npc = nullptr;

	// --- Interpolation: position ---
	float prevX = 0.0f, prevY = 0.0f, prevZ = 0.0f;
	float targetX = 0.0f, targetY = 0.0f, targetZ = 0.0f;

	// --- Interpolation: rotation ---
	float targetRotationY = 0.0f;

	// --- Interpolation: animation ---
	float    prevAnimFrame = 0.0f;
	float    targetAnimFrame = 0.0f;
	float    prevLastAnimFrame = 0.0f;
	float    targetLastAnimFrame = 0.0f;
	uint32_t targetAnimation = 0;

	// --- Interpolation: timing ---
	double lerpStartTime = 0.0;

	// ----- Methods -----

	void setPosition(float px, float py, float pz);
	void setRotationY(float ry);
	void setAnimation(uint32_t anim, float frame, float lastFrame);
	void setNpcGuid(uint64_t guid);
	void setClientIndex(int index);
	void setWeapon(int index);

	/// Initialize all lerp fields to match the current state (no movement on first frame).
	void initializeLerp(double currentTime);

	/// Advance interpolation and write the result to the NPC.
	/// @param t  Interpolation factor clamped to [0, 1].
	void tickLerp(float t);
};
