#include "Hoistable.h"
#include "meridian.hpp"

Hoistable::Hoistable(HobbitProcessAnalyzer* analyzer)
	: analyzer_(analyzer)
{
}


void Hoistable::initializeByGuid(uint64_t guid)
{
	guid_ = guid;
	printf("Creating Hoistable with GUID %llu\n", guid);

	resolveObjectPtr(guid);

	std::cout << "HOISTABLE ADDRESS: " << objectAddress_ << "\n";

	if (objectAddress_ == 0)
	{
		printf("Warning: Hoistable object not found for GUID %llu\n", guid);
		return;
	}

	resolvePositionPtrs();
	resolveRotationPtr();


	printf("Hoistable initialized\n");
}

void Hoistable::initializeByAddress(uint32_t address)
{

	objectAddress_ = address;
	objectPointer_ = address;

	std::cout << "HOISTABLE ADDRESS: " << objectAddress_ << "\n";

	if (objectAddress_ == 0)
	{
		printf("Warning: Hoistable object not found for address %u\n", address);
		return;
	}

	resolvePositionPtrs();
	resolveRotationPtr();

	guid_ = analyzer_->readData<uint64_t>(objectAddress_ + 0x8);

}

bool Hoistable::isAnalyzerReady() const
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

uint64_t Hoistable::getGUID() const
{
	return guid_;
}

void Hoistable::setPositionX(float value)
{
	for (uint32_t addr : positionXAddresses_)
		analyzer_->writeData(addr, value);
}

void Hoistable::setPositionY(float value)
{
	for (uint32_t addr : positionXAddresses_)
		analyzer_->writeData(addr + 0x4, value);
}

void Hoistable::setPositionZ(float value)
{
	for (uint32_t addr : positionXAddresses_)
		analyzer_->writeData(addr + 0x8, value);
}

void Hoistable::setPosition(float x, float y, float z)
{
	//setPositionX(x);
	//setPositionY(y);
	//setPositionZ(z);

	object* theObject = (object*)objectAddress_;
	vector3 ve = { x,y,z };

	theObject->Move(ve, false);


}

Vector3 Hoistable::getPosition() const
{
	for (uint32_t addr : positionXAddresses_)
		return { analyzer_->readData<float>(addr), analyzer_->readData<float>(addr + 0x4) , analyzer_->readData<float>(addr + 0x8) };
}

void Hoistable::setRotationY(float value)
{
	analyzer_->writeData(rotationYAddr_, value);
}

float Hoistable::getRotationY() const
{
	return  analyzer_->readData<float>(rotationYAddr_);
}


void Hoistable::resolveObjectPtr(uint64_t guid)
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

void Hoistable::resolvePositionPtrs()
{
	if (!isAnalyzerReady())
		return;

	positionXAddresses_.clear();

	uint32_t objPtr = objectAddress_;

	// Current position
	positionXAddresses_.push_back(objPtr + 0x14);   // 0xC + 0x8

}

void Hoistable::resolveRotationPtr()
{
	rotationYAddr_ = objectAddress_ + 0x6C;   // 0x64 + 0x8
}
