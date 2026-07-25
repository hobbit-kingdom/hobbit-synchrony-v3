/*
	NPC.h
	Interface for controlling an NPC entity in the Hobbit game process.
	Reads/writes game memory to move, rotate, and animate the NPC.
*/

#pragma once

#include <cstdint>
#include <vector>
#include <unordered_map>

#ifdef _WIN32
#include <winsock2.h>    // Must come before <windows.h> to avoid winsock.h conflicts
#include <windows.h>
#endif

#include "./HobbitGameManager/HobbitProcessAnalyzer.h"

#include "GameTypes.h"

// Global animation-data map shared between client reading and NPC writing.
// Maps animation ID → animation data pointer value.
extern std::unordered_map<uint32_t, uint32_t> animDataMap;

// Engine object layout, taken from the obj_mgr class registry (the descriptor
// constructor at 0x0054B5E0 indexes a table by these tags).
//   object + 0x7C : type tag  -> 0x1C = NPC, 0x12 = Bilbo, 0x05 = RigidInstance, ...
//   object + 0x1A4: team      -> 0 = neutral, 1 / 2 = the two fighting sides
// NPCObject's constructor defaults the team to 2; level scripts move NPCs between
// teams while the level is running.
static constexpr uint8_t  OBJ_TYPE_NPC = 0x1C;
static constexpr uint32_t OBJ_TYPE_OFFSET = 0x7C;
static constexpr uint32_t OBJ_TEAM_OFFSET = 0x1A4;

/// The two teams whose members are driven by the host. Everything else (team 0)
/// keeps running its own local AI on every client.
inline bool isSyncedTeam(int team) { return team == 1 || team == 2; }

class NPC
{
public:
	// --- Construction & Initialization ---

	explicit NPC(HobbitProcessAnalyzer* analyzer);

	/// Look up the NPC in game memory by its GUID and cache all relevant pointers.
	void initializeByGuid(uint64_t guid);
	void initializeByAddress(uint32_t guid);

	/// Whether the NPC was found in game memory (objectAddress is valid).
	bool isValid() const { return objectAddress_ != 0; }

	// --- Getters ---

	uint32_t getObjectPtr() const { return objectAddress_; }
	uint64_t getGUID() const;
	float    getHealth() const;
	uint32_t getAnimation() const;

	// --- Position ---

	void setPositionX(float value);
	void setPositionY(float value);
	void setPositionZ(float value);
	void setPosition(float x, float y, float z);

	Vector3 getPosition() const;
	float getRotationY() const;

	// --- Rotation ---

	void setRotationY(float value);

	// --- Health ---

	void setHealth(float value);

	// --- Animation ---

	void setNPCAnim(int anim);
	void setAnimation(uint32_t animId);
	void setAnimFrames(float frame, float lastFrame);

	/// Current playback frame of the active animation.
	float getAnimFrame() const;
	/// Final frame (length) of the active animation; 0 if unknown.
	float getAnimEndFrame() const;
	/// Overwrite just the playback frame, leaving the animation itself alone.
	void  setAnimFrame(float frame);

	// --- Weapon ---

	void setWeapon(uint32_t weaponId);

	void setGUID(uint32_t newGUID);
	void setAIMode(int mode);
	/// Put the AI flag byte back to whatever it was when this NPC was discovered.
	void restoreAIState();
	void setTeam(int teamId);

	/// Current team: 0 = neutral, 1 / 2 = the two fighting sides.
	/// Scripted level events can change this at any time, so never cache it.
	int  getTeam() const;

	/// Whether the object still carries the NPC type tag. Objects can be destroyed
	/// mid-level and their slot reused, so cached addresses are re-checked here
	/// before being trusted (same convention as the pickup/switch/trigger caches).
	bool isNpcType() const;

	bool isActivated() const;

private:
	HobbitProcessAnalyzer* analyzer_ = nullptr;

	// Cached game-memory addresses
	uint32_t objectAddress_ = 0;
	uint32_t objectPointer_ = 0;
	uint32_t rotationYAddr_ = 0;
	uint32_t animationAddr_ = 0;
	float    lastAnimFrame_ = 0.0f;
	uint64_t guid_ = 0;

	uint8_t initialAIState = 0;

	std::vector<uint32_t> positionXAddresses_;

	// --- Internal pointer resolution ---

	void resolveObjectPtr(uint64_t guid);
	void resolvePositionPtrs();
	void resolveRotationPtr();
	void resolveAnimationPtr();

	/// Helper: read the animation address by following the pointer chain.
	uint32_t followAnimationPtrChain() const;

	/// Null/process safety check.
	bool isAnalyzerReady() const;
};
