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

class Marker
{
public:
	// --- Construction & Initialization ---

	explicit Marker(HobbitProcessAnalyzer* analyzer);

	/// Look up the NPC in game memory by its GUID and cache all relevant pointers.
	void initializeByGuid(uint64_t guid);
	void initializeByAddress(uint32_t guid);

	/// Whether the NPC was found in game memory (objectAddress is valid).
	bool isValid() const { return objectAddress_ != 0; }

	// --- Getters ---

	uint32_t getObjectPtr() const { return objectAddress_; }
	uint64_t getGUID() const;

	// --- Position ---
	void setPosition(float x, float y, float z);
	Vector3 getPosition() const;


	// --- Raw overloads (write uint32_t bit patterns) ---
	void setGUID(uint32_t newGUID);

	void setText(const char *new_text);

private:
	HobbitProcessAnalyzer* analyzer_ = nullptr;

	// Cached game-memory addresses
	uint32_t objectAddress_ = 0;
	uint32_t objectPointer_ = 0;
	uint64_t guid_ = 0;

	std::vector<uint32_t> positionXAddresses_;

	// --- Internal pointer resolution ---

	void resolveObjectPtr(uint64_t guid);
	void resolvePositionPtrs();

	/// Null/process safety check.
	bool isAnalyzerReady() const;
};
