/*
	Player.h
	Represents a remote player seen by the local client.
	Holds current state, lerp targets, and an NPC handle for rendering in-game.
*/

#pragma once

#include <cstdint>
#include <string>
#include "NPC.h"
#include "Marker.h"
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
	std::string textureName;
	std::string textureFilePath;

	// --- In-game representation ---
	NPC* npc = nullptr;
	Marker* nickname_marker = nullptr;
	Marker* status_marker = nullptr;

	// Half-transparent stand-in shown while this player wears the ring on team 1.
	// A level-authored NPC clone with the see-through model, GUID derived like the
	// marker GUIDs: first half replaced with 0D8AD913 (name = ..11, status = ..12).
	// Purely cosmetic: tickLerp mirrors position/rotation/anim onto it while
	// ghostActive; the ORIGINAL npc stays the real body (senses, pain, sync) and is
	// simply unrendered. ghostActive is owned by applyRingVisual (game thread).
	NPC* ghostNpc = nullptr;
	bool ghostActive = false;

	// set nickname
	std::string nickname = "Username";
	std::string status = "Status";

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

	// --- One-shot animation hold (death) ---
	// The engine loops animations, so a death anim would replay forever. We track
	// the highest playback frame actually observed; when the frame jumps backwards
	// the animation has completed a pass, and we pin it to that peak until a
	// different animation starts.
	bool  animHoldActive = false;
	float animHoldPeakFrame = 0.0f;

	// ----- Methods -----

	void setPosition(float px, float py, float pz);
	void setRotationY(float ry);
	void setAnimation(uint32_t anim, float frame, float lastFrame);
	void setNpcGuid(uint64_t guid);
	void setClientIndex(int index);
	void setWeapon(int index);
	void setTeam(int teamId);

	/// Initialize all lerp fields to match the current state (no movement on first frame).
	void initializeLerp(double currentTime);

	/// Advance interpolation and write the result to the NPC.
	/// @param t  Interpolation factor clamped to [0, 1].
	void tickLerp(float t);

	void setPlayerAnim(int anim);
};
