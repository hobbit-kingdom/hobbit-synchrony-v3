/*
	NPC.cpp
	Implementation for controlling NPC entities via game-process memory.
*/

#include "Marker.h"
#include "meridian.hpp"
#include <iostream>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Marker::Marker(HobbitProcessAnalyzer* analyzer)
	: analyzer_(analyzer)
{
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void Marker::initializeByGuid(uint64_t guid)
{
	guid_ = guid;
	//printf("Creating Marker with GUID %llu\n", guid);

	resolveObjectPtr(guid);

	if (objectAddress_ == 0)
	{
		//printf("Warning: Marker object not found for GUID %llu\n", guid);
		return;
	}

	resolvePositionPtrs();
}

void Marker::initializeByAddress(uint32_t address)
{

	objectAddress_ = address;
	objectPointer_ = address;


	if (objectAddress_ == 0)
	{
		//printf("Warning: Marker object not found for address %u\n", address);
		return;
	}

	resolvePositionPtrs();

	guid_ = analyzer_->readData<uint64_t>(objectAddress_ + 0x8);
}

// ---------------------------------------------------------------------------
// GUID
// ---------------------------------------------------------------------------

void Marker::setGUID(uint32_t newGUID)
{
	analyzer_->writeData(objectAddress_, newGUID);
}

uint64_t Marker::getGUID() const
{
	return guid_;
}

// ---------------------------------------------------------------------------
// Position — float
// ---------------------------------------------------------------------------

void Marker::setPosition(float x, float y, float z)
{
	if(objectAddress_ != 0) {
		object* theObject = (object*)objectAddress_;
		vector3 ve = { x,y,z };

		theObject->Move(ve, false);
	}
}


Vector3 Marker::getPosition() const
{
	for (uint32_t addr : positionXAddresses_)
		return { analyzer_->readData<float>(addr), analyzer_->readData<float>(addr + 0x4) , analyzer_->readData<float>(addr + 0x8) };
}

void Marker::resolveObjectPtr(uint64_t guid)
{
	if (!isAnalyzerReady())
	{
		objectAddress_ = 0;
		objectPointer_ = 0;
		return;
	}

	__try
	{
		objectAddress_ = analyzer_->findGameObjByGUID(guid);
		objectPointer_ = analyzer_->findGameObjStackByPtrGUID(guid);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		objectAddress_ = 0;
		objectPointer_ = 0;
		printf("Exception resolving Marker GUID %llu\n", guid);
	}

	//printf("Object address: 0x%08X\n", objectAddress_);
}

void Marker::resolvePositionPtrs()
{
	if (!isAnalyzerReady())
		return;

	positionXAddresses_.clear();

	uint32_t objPtr = objectAddress_;

	// Current position
	positionXAddresses_.push_back(objPtr + 0x14);   // 0xC + 0x8

	// Root position
	positionXAddresses_.push_back(objPtr + 0x20);   // 0x18 + 0x8
}

void Marker::setText(const char *new_text)
{
	if(objectAddress_ != 0) {
		char *pDst = (char*)(objectAddress_ + 0xC0);
		strncpy(pDst, new_text, 32);
		pDst[31] = '\0';
	}
}

// ---------------------------------------------------------------------------
// Safety
// ---------------------------------------------------------------------------

bool Marker::isAnalyzerReady() const
{
	if (!analyzer_)
	{
		printf("Error: HobbitProcessAnalyzer is null\n");
		return false;
	}
	if (!analyzer_->isProcessSet())
	{
		printf("Error: game process not attached\n");
		return false;
	}
	return true;
}