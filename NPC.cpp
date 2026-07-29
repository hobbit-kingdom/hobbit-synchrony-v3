/*
	NPC.cpp
	Implementation for controlling NPC entities via game-process memory.
*/

#include "NPC.h"
#include <iostream>

#include "meridian.hpp"
#include "DebugLog.h"

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
	dprintf("Creating NPC with GUID %llu\n", guid);

	resolveObjectPtr(guid);

	if (objectAddress_ == 0)
	{
		dprintf("Warning: NPC object not found for GUID %llu\n", guid);
		return;
	}

	resolvePositionPtrs();
	resolveRotationPtr();
	resolveAnimationPtr();

	// Read initial health as a sanity check
	float hp = getHealth();
	dprintf("NPC initialized — health: %.1f\n", hp);
}

void NPC::initializeByAddress(uint32_t address)
{

	objectAddress_ = address;
	objectPointer_ = address;


	if (objectAddress_ == 0)
	{
		dprintf("Warning: NPC object not found for address %u\n", address);
		return;
	}

	resolvePositionPtrs();
	resolveRotationPtr();
	resolveAnimationPtr();

	guid_ = analyzer_->readData<uint64_t>(objectAddress_ + 0x8);

	// Read initial health as a sanity check
	float hp = getHealth();
	dprintf("NPC initialized — health: %.1f\n", hp);

	initialAIState = analyzer_->readData<uint8_t>(objectAddress_ + 0x268);

}

// ---------------------------------------------------------------------------
// Safety
// ---------------------------------------------------------------------------

bool NPC::isAnalyzerReady() const
{
	if (!analyzer_)
	{
		dprintf("Error: HobbitProcessAnalyzer is null\n");
		return false;
	}
	if (!analyzer_->isProcessSet())
	{
		dprintf("Error: game process not attached\n");
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

	/*
		object* theObject = (object*)objectAddress_;
		if(theObject) {
			vector3 ve = { x,y,z };
			theObject->Move(ve, false);
		}
	*/
}


Vector3 NPC::getPosition() const
{
	for (uint32_t addr : positionXAddresses_)
		return { analyzer_->readData<float>(addr), analyzer_->readData<float>(addr + 0x4) , analyzer_->readData<float>(addr + 0x8) };
}

// ---------------------------------------------------------------------------
// Rotation
// ---------------------------------------------------------------------------

void NPC::setRotationY(float value)
{
	analyzer_->writeData(rotationYAddr_, value);
	/*
		object* theObject = (object*)objectAddress_;
		if(theObject) {
			radian3 ve = { 0.f,value,0.f };
			theObject->SetRotation(ve);
		}
	*/
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

	//    dcout() << analyzer_->readData<uint32_t>(animAdd4) << "\n";

	if (animAdd4 == 0)
	{
		//strcpy(anim_result, "ERROR");
	}
	else if (analyzer_->readData<uint32_t>(animAdd4) == 7289932)  // 7289932
	{
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
	else dcout() << "bad anim" << "\n";
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

// animationAddr_ + 0x8  = current playback frame
// animationAddr_ + 0x14 = final frame / length of the animation
float NPC::getAnimFrame() const
{
	if (!analyzer_ || animationAddr_ == 0)
		return 0.0f;
	return analyzer_->readData<float>(animationAddr_ + 0x8);
}

float NPC::getAnimEndFrame() const
{
	if (!analyzer_ || animationAddr_ == 0)
		return 0.0f;
	return analyzer_->readData<float>(animationAddr_ + 0x14);
}

void NPC::setAnimFrame(float frame)
{
	if (!analyzer_ || animationAddr_ == 0)
		return;
	analyzer_->writeData(animationAddr_ + 0x8, frame);
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
// Shield
// ---------------------------------------------------------------------------
//
// A shield is a normal level object (so its GUID is the same on every machine)
// that the NPC points at through its prop set:
//
//   NPCObject + 0x260          -> prop set          (the same field setWeapon writes)
//   propSet   + 0x10 (uint64)  -> ShieldGuid        (0 once the shield is gone)
//
// NPCObject::ShatterShield @0x004A40D0 is the engine's own break: it hides the
// shield object via special_surfaces::SetHidden, zeroes that GUID, then calls
// vtable slot 0x11C (0x004A0BA0) to recompute which moves the NPC can still
// perform now that it cannot block.
//
// Only a charged sting/stick hit (PainData::ePainType 4 or 5) reaches the AI
// block reaction @0x005BEA00 that calls it, and only on the machine actually
// running that NPC's AI. Clients suppress AI on synced-team NPCs, so a break has
// to be replicated - see the shieldIntact field on Enemy.
//
// Both accessors dereference game memory directly: we are injected into the game
// process, so there is no reason to go through the analyzer for two loads.

static constexpr uint32_t NPC_PROPS_OFF = 0x260;             // NPCObject+0x260: prop set
static constexpr uint32_t NPC_PROPS_SHIELD_GUID_OFF = 0x10;  // propSet+0x10: shield object GUID
static constexpr uint32_t NPC_SHATTERSHIELD_ADDR = 0x004A40D0;

// public: void __thiscall NPCObject::ShatterShield(void)
typedef void(__fastcall* ShatterShield_t)(void* self, void* edx);

bool NPC::hasShield() const
{
	if (objectAddress_ == 0)
		return false;

	const uint32_t props = *reinterpret_cast<const uint32_t*>(objectAddress_ + NPC_PROPS_OFF);
	if (props == 0)
		return false;

	return *reinterpret_cast<const uint64_t*>(props + NPC_PROPS_SHIELD_GUID_OFF) != 0;
}

void NPC::shatterShield()
{
	if (objectAddress_ == 0)
		return;

	// Object slots get recycled when something is destroyed mid-level, so re-check
	// the type tag before handing a cached address to an NPCObject member.
	if (*reinterpret_cast<const uint8_t*>(objectAddress_ + OBJ_TYPE_OFFSET) != OBJ_TYPE_NPC)
		return;

	reinterpret_cast<ShatterShield_t>(NPC_SHATTERSHIELD_ADDR)(
		reinterpret_cast<void*>(objectAddress_), nullptr);
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
		dprintf("Exception resolving NPC GUID %llu\n", guid);
	}

	dprintf("Object address: 0x%08X\n", objectAddress_);
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
	dprintf("Animation address: 0x%08X\n", animationAddr_);
}

// ---------------------------------------------------------------------------
// Dostate + Senses: AI
// ---------------------------------------------------------------------------

void NPC::setAIMode(int mode)
{
	if (!isAnalyzerReady() || !isValid())
		return;

	uint32_t ObjectPtr = getObjectPtr();


	char* pointerAI = (char*)ObjectPtr + 0x268;


	if (*pointerAI <= 127) // && *pointerAI >= 87
	{
		if (mode == 0) // dostate and senses off
		{
			*pointerAI = 87;
		}
		else if (mode == 1) // everything on
		{
			*pointerAI = 127;
		}
		else if (mode == 2) // only senses off
		{
			*pointerAI = 95;
		}
		else if (mode == 3) // only state off
		{
			*pointerAI = 119;
		}
	}
	else if (*pointerAI <= 255) // *pointerAI >= 215 && 
	{
		if (mode == 0) // dostate and senses off
		{
			*pointerAI = 215;
		}
		else if (mode == 1) // everything on
		{
			*pointerAI = 255;
		}
		else if (mode == 2) // only senses off
		{
			*pointerAI = 223;
		}
		else if (mode == 3) // only state off
		{
			*pointerAI = 247;
		}
	}
}

void NPC::restoreAIState()
{
	if (!isAnalyzerReady() || !isValid())
		return;

	*((uint8_t*)getObjectPtr() + 0x268) = initialAIState;
}

void NPC::setTeam(int teamId)
{
	if (!isAnalyzerReady() || !isValid())
		return;

	uint32_t ObjectPtr = getObjectPtr();

	char* pointerTeamId = (char*)ObjectPtr + 0x1a4;

	*pointerTeamId = teamId;

}

int NPC::getTeam() const
{
	if (!analyzer_ || objectAddress_ == 0)
		return 0;

	return (int)analyzer_->readData<uint32_t>(objectAddress_ + OBJ_TEAM_OFFSET);
}

bool NPC::isNpcType() const
{
	if (!analyzer_ || objectAddress_ == 0)
		return false;

	return analyzer_->readData<uint8_t>(objectAddress_ + OBJ_TYPE_OFFSET) == OBJ_TYPE_NPC;
}

bool NPC::isActivated() const
{
	const char* theObject = (const char*)objectAddress_;
	if (theObject) {
		return !!(theObject[0x7F] & 0x10);
	}
	return false;
}