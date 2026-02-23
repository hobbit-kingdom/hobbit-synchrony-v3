/*
	Player.cpp
	Player state management and interpolation logic.
*/

#include "Player.h"
#include <cmath>

void Player::setPosition(float px, float py, float pz)
{
	x = px;
	y = py;
	z = pz;
}

void Player::setWeapon(int index)
{
	if (npc) npc->setWeapon(index);
	bilboWeapon = index;
}


void Player::setRotationY(float ry)
{
	rotationY = ry;
}

void Player::setAnimation(uint32_t anim, float frame, float lastFrame)
{
	animation = anim;
	animFrame = frame;
	lastAnimFrame = lastFrame;
}

void Player::setNpcGuid(uint64_t guid)
{
	npcGuid = guid;
}

void Player::setClientIndex(int index)
{
	clientIndex = index;
}

void Player::initializeLerp(double currentTime)
{
	// Position
	prevX = x;     prevY = y;     prevZ = z;
	targetX = x;     targetY = y;     targetZ = z;

	// Rotation
	targetRotationY = rotationY;

	// Animation
	prevAnimFrame = animFrame;
	targetAnimFrame = animFrame;
	prevLastAnimFrame = lastAnimFrame;
	targetLastAnimFrame = lastAnimFrame;
	targetAnimation = animation;

	lerpStartTime = currentTime;
}

void Player::tickLerp(float t)
{
	if (!npc || !npc->isValid())
		return;

	// --- Position ---
	x = MathUtils::lerp(prevX, targetX, t);
	y = MathUtils::lerp(prevY, targetY, t);
	z = MathUtils::lerp(prevZ, targetZ, t);
	npc->setPosition(x, y, z);

	// --- Rotation (direct) ---
	rotationY = targetRotationY;
	npc->setRotationY(rotationY);

	// --- Animation frames ---
	//animFrame = MathUtils::lerp(prevAnimFrame, targetAnimFrame, t);
	//lastAnimFrame = targetLastAnimFrame;

	if (animation > 0 && animation <= 200)
	{
		float wrappedFrame = animFrame;
		if (lastAnimFrame > 0)
		{
			wrappedFrame = fmod(animFrame, lastAnimFrame);
			if (wrappedFrame < 0)
				wrappedFrame += lastAnimFrame;
		}
		//npc->setAnimFrames(wrappedFrame, lastAnimFrame);
	}
}
