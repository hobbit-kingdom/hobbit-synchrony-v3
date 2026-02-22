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

	// --- Weapon ---

	void setWeapon(uint32_t weaponId);

	// --- Raw overloads (write uint32_t bit patterns) ---

	void setPositionX(uint32_t value);
	void setPositionY(uint32_t value);
	void setPositionZ(uint32_t value);
	void setPosition(uint32_t x, uint32_t y, uint32_t z);
	void setRotationY(uint32_t value);
	void setGUID(uint32_t newGUID);

private:
	HobbitProcessAnalyzer* analyzer_ = nullptr;

	// Cached game-memory addresses
	uint32_t objectAddress_ = 0;
	uint32_t objectPointer_ = 0;
	uint32_t rotationYAddr_ = 0;
	uint32_t animationAddr_ = 0;
	float    lastAnimFrame_ = 0.0f;
	uint64_t guid_ = 0;

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
