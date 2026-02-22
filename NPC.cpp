/*
	NPC.cpp
	Implementation for controlling NPC entities via game-process memory.
*/

#include "NPC.h"
#include <iostream>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

NPC::NPC(HobbitProcessAnalyzer* analyzer)
	: analyzer_(analyzer)
{
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void NPC::initializeByGuid(uint64_t guid)
{
	guid_ = guid;
	printf("Creating NPC with GUID %llu\n", guid);

	resolveObjectPtr(guid);

	if (objectAddress_ == 0)
	{
		printf("Warning: NPC object not found for GUID %llu\n", guid);
		return;
	}

	resolvePositionPtrs();
	resolveRotationPtr();
	resolveAnimationPtr();

	// Read initial health as a sanity check
	float hp = getHealth();
	printf("NPC initialized — health: %.1f\n", hp);
}

void NPC::initializeByAddress(uint32_t address)
{

	objectAddress_ = address;
	objectPointer_ = address;


	if (objectAddress_ == 0)
	{
		printf("Warning: NPC object not found for address %u\n", address);
		return;
	}

	resolvePositionPtrs();
	resolveRotationPtr();
	resolveAnimationPtr();

	guid_ = analyzer_->readData<uint64_t>(objectAddress_ + 0x8);

	// Read initial health as a sanity check
	float hp = getHealth();
	printf("NPC initialized — health: %.1f\n", hp);
}

// ---------------------------------------------------------------------------
// Safety
// ---------------------------------------------------------------------------

bool NPC::isAnalyzerReady() const
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

// ---------------------------------------------------------------------------
// GUID
// ---------------------------------------------------------------------------

void NPC::setGUID(uint32_t newGUID)
{
	analyzer_->writeData(objectAddress_, newGUID);
}

uint64_t NPC::getGUID() const
{
	return guid_;
}

// ---------------------------------------------------------------------------
// Position — float
// ---------------------------------------------------------------------------

void NPC::setPositionX(float value)
{
	for (uint32_t addr : positionXAddresses_)
		analyzer_->writeData(addr, value);
}

void NPC::setPositionY(float value)
{
	for (uint32_t addr : positionXAddresses_)
		analyzer_->writeData(addr + 0x4, value);
}

void NPC::setPositionZ(float value)
{
	for (uint32_t addr : positionXAddresses_)
		analyzer_->writeData(addr + 0x8, value);
}

void NPC::setPosition(float x, float y, float z)
{
	setPositionX(x);
	setPositionY(y);
	setPositionZ(z);
}


Vector3 NPC::getPosition() const
{
	for (uint32_t addr : positionXAddresses_)
		return { analyzer_->readData<float>(addr), analyzer_->readData<float>(addr + 0x4) , analyzer_->readData<float>(addr + 0x8) };
}

// ---------------------------------------------------------------------------
// Position — raw uint32_t
// ---------------------------------------------------------------------------

void NPC::setPositionX(uint32_t value)
{
	for (uint32_t addr : positionXAddresses_)
		analyzer_->writeData(addr, value);
}

void NPC::setPositionY(uint32_t value)
{
	for (uint32_t addr : positionXAddresses_)
		analyzer_->writeData(addr + 0x4, value);
}

void NPC::setPositionZ(uint32_t value)
{
	for (uint32_t addr : positionXAddresses_)
		analyzer_->writeData(addr + 0x8, value);
}

void NPC::setPosition(uint32_t x, uint32_t y, uint32_t z)
{
	setPositionX(x);
	setPositionY(y);
	setPositionZ(z);
}

// ---------------------------------------------------------------------------
// Rotation
// ---------------------------------------------------------------------------

void NPC::setRotationY(float value)
{
	analyzer_->writeData(rotationYAddr_, value);
}

void NPC::setRotationY(uint32_t value)
{
	analyzer_->writeData(rotationYAddr_, value);
}

float NPC::getRotationY() const
{
	return  analyzer_->readData<float>(rotationYAddr_);
}

// ---------------------------------------------------------------------------
// Health
// ---------------------------------------------------------------------------

void NPC::setHealth(float value)
{
	analyzer_->writeData(objectAddress_ + 0x290, value);
}

float NPC::getHealth() const
{
	return analyzer_->readData<float>(objectAddress_ + 0x290);
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------

class anim_track_controller
{
	int dummy;
};
typedef void(__thiscall anim_track_controller::* SetAnimPROCPTR)(int anim_id, float blend_time);

void NPC::setNPCAnim(int anim)
{

	uint32_t animAdd1 = analyzer_->readData<uint32_t>(0x304 + objectAddress_);
	uint32_t animAdd2 = analyzer_->readData<uint32_t>(0x50 + animAdd1);
	uint32_t animAdd4 = analyzer_->readData<uint32_t>(0x10C + animAdd2);
	if (animAdd4 == 0)
	{
		//strcpy(anim_result, "ERROR");
	}
	else {
		anim_track_controller* pController = (anim_track_controller*)animAdd4;

		uint32_t _SetAnimPTR = 0x5434B0;
		SetAnimPROCPTR SetAnimPTR;
		memcpy(&SetAnimPTR, &_SetAnimPTR, 4);

		(pController->*SetAnimPTR)(anim, 0.15f);

		/*
		uint32_t animationAddress = 0x8 + animAdd4;
		int *pI = (int*)animationAddress;

		*pI = anim;
		*/

		//strcpy(anim_result, "ANIM OK");
	}
}


void NPC::setAnimation(uint32_t animId)
{
	if (animId == getAnimation())
		return;   // No change

	if (animId <= 0 || animId >= 200)
		return;

	analyzer_->writeData(animationAddr_, animId);

	// Write animation data if we have a cached value
	if (animDataMap.count(animId))
		analyzer_->writeData(animationAddr_ + 0x4, animDataMap[animId]);
}

uint32_t NPC::getAnimation() const
{
	return analyzer_->readData<uint32_t>(animationAddr_);
}

void NPC::setAnimFrames(float frame, float lastFrame)
{
	analyzer_->writeData(animationAddr_ + 0x8, frame);
	analyzer_->writeData(animationAddr_ + 0x14, lastFrame);
	lastAnimFrame_ = frame;
}

// ---------------------------------------------------------------------------
// Weapon
// ---------------------------------------------------------------------------

void NPC::setWeapon(uint32_t weaponId)
{
	if (!isAnalyzerReady() || !isValid())
		return;

	uint64_t guidNone = 0x0;
	if (weaponId != -1)
	{
		//NONE
		if (weaponId == 2)
			guidNone = 0x0D8AD910E885100D;
		//STING
		else if (weaponId == 0)
			guidNone = 0x0D8AD910E885100B;
		//STAFF
		else if (weaponId == 1)
			guidNone = 0x0D8AD910E885100A;
		//STONE
		else if (weaponId == 3)
			guidNone = 0x0D8AD910E885100C;

		uint32_t addrsGuidNone = analyzer_->findGameObjByGUID(guidNone);
		if (addrsGuidNone)
			weaponId = analyzer_->readData<uint32_t>(addrsGuidNone + 0x260);
		uint32_t ObjectPtr = getObjectPtr();
		analyzer_->writeData(ObjectPtr + 0x260, weaponId);
	}
}

// ---------------------------------------------------------------------------
// Internal: Pointer resolution
// ---------------------------------------------------------------------------

uint32_t NPC::followAnimationPtrChain() const
{
	uint32_t step1 = analyzer_->readData<uint32_t>(0x304 + objectAddress_);
	uint32_t step2 = analyzer_->readData<uint32_t>(0x50 + step1);
	uint32_t step3 = analyzer_->readData<uint32_t>(0x10C + step2);
	return 0x8 + step3;
}

void NPC::resolveObjectPtr(uint64_t guid)
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
		printf("Exception resolving NPC GUID %llu\n", guid);
	}

	printf("Object address: 0x%08X\n", objectAddress_);
}

void NPC::resolvePositionPtrs()
{
	if (!isAnalyzerReady())
		return;

	positionXAddresses_.clear();

	uint32_t objPtr = objectAddress_;

	// Current position
	positionXAddresses_.push_back(objPtr + 0x14);   // 0xC + 0x8

	// Root position
	positionXAddresses_.push_back(objPtr + 0x20);   // 0x18 + 0x8

	// Animation-driven position (derived from the animation pointer chain)
	animationAddr_ = followAnimationPtrChain();
	positionXAddresses_.push_back(animationAddr_ - 0xC4);
}

void NPC::resolveRotationPtr()
{
	rotationYAddr_ = objectAddress_ + 0x6C;   // 0x64 + 0x8
}

void NPC::resolveAnimationPtr()
{
	animationAddr_ = followAnimationPtrChain();
	printf("Animation address: 0x%08X\n", animationAddr_);
}