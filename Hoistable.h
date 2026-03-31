#pragma once

#include "./HobbitGameManager/HobbitProcessAnalyzer.h"

#include "GameTypes.h"



class Hoistable
{
public:
	explicit Hoistable(HobbitProcessAnalyzer* analyzer);

	void initializeByGuid(uint64_t guid);
	void initializeByAddress(uint32_t guid);

	bool isValid() const { return objectAddress_ != 0; }

	uint32_t getObjectPtr() const { return objectAddress_; }
	uint64_t getGUID() const;

	void setPositionX(float value);
	void setPositionY(float value);
	void setPositionZ(float value);
	void setPosition(float x, float y, float z);

	Vector3 getPosition() const;
	float getRotationY() const;

	void setRotationY(float value);


private:
	HobbitProcessAnalyzer* analyzer_ = nullptr;

	uint32_t objectAddress_ = 0;
	uint32_t objectPointer_ = 0;
	uint32_t rotationYAddr_ = 0;

	uint64_t guid_ = 0;

	std::vector<uint32_t> positionXAddresses_;

	void resolveObjectPtr(uint64_t guid);
	void resolvePositionPtrs();
	void resolveRotationPtr();

	bool isAnalyzerReady() const;
};

