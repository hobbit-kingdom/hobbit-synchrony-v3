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

// These animations sit too low on the fake bilbo, so the character is lifted while
// one of them is playing. Purely a display offset — the networked position is left
// untouched, so interpolation never sees it and the lift can't accumulate.
static constexpr float ANIM_Y_LIFT = 100.0f;

static bool animNeedsYLift(uint32_t anim)
{
	switch (anim)
	{
	case 21: case 22:
	case 36: case 37: case 38: case 39:
		return true;
	default:
		return false;
	}
}

// Animations that must play exactly once and then stay on their final frame,
// instead of the engine's default looping. 17 = death (fall down and lie there).
static bool animPlaysOnce(uint32_t anim)
{
	return anim == 17;
}

// How many frames short of the observed end to park a play-once animation.
// Parking exactly on the last frame lets the engine's next advance tip past the
// end and wrap to frame 0, which renders as a flicker between the first and last
// frame. Sitting a few frames back keeps every advance safely inside the clip.
// (Measured: the death animation peaks at frame 39, so this parks it at 35.)
static constexpr float ANIM_HOLD_BACKOFF = 4.0f;

// Clip length (last frame) per one-shot animation id, shared by every player.
//
// Pre-seeded with the measured values so we park BEFORE the animation reaches its
// end — it never wraps, so the first frame is never shown, not even on the first
// death of the session. The engine's own "end frame" field is useless here: it
// reads 0.000 for these clips, which is why this is measured rather than queried.
//
// Anything not listed is learned automatically the first time it wraps (costing one
// visible restart), then behaves like a seeded entry.
//
// Game-thread only (tickLerp runs inside the OnAdvanceLogic hook), so no locking.
static std::unordered_map<uint32_t, float> g_learnedAnimPeak = {
	{ 17, 39.0f },   // death: falls and lies down; measured 39 frames
};

// Health every remote player's fake bilbo is held at (NPCObject+0x290).
static constexpr float FAKE_BILBO_HEALTH = 1000.0f;

void Player::tickLerp(float t)
{
	if (!npc || !npc->isValid())
		return;

	// --- Position ---
	x = MathUtils::lerp(prevX, targetX, t);
	y = MathUtils::lerp(prevY, targetY, t);
	z = MathUtils::lerp(prevZ, targetZ, t);

	// Lift only what we hand to the game; x/y/z stay the true networked values.
	const float renderY = y + (animNeedsYLift(animation) ? ANIM_Y_LIFT : 0.0f);
	npc->setPosition(x, renderY, z);

	// --- Rotation (direct) ---
	rotationY = targetRotationY;
	npc->setRotationY(rotationY);

	// --- Health ---
	// Pinned every frame rather than set once: a remote player's real health lives
	// on their own client, so the local AI chipping this copy down would only ever
	// desync it - and at 0 the engine would kill the fake bilbo out from under them.
	npc->setHealth(FAKE_BILBO_HEALTH);

	// --- Animation frames ---
	//animFrame = MathUtils::lerp(prevAnimFrame, targetAnimFrame, t);
	//lastAnimFrame = targetLastAnimFrame;

	// --- Play-once animations (death): run to the end, then hold there ---
	// The engine loops animations, so the death anim restarts and the character
	// keeps falling over until they revive. We can't ask the engine not to loop,
	// so we watch the playback frame: when it jumps backwards the animation has
	// completed a pass, and from then on we pin it to the final frame until a
	// different animation arrives.
	// We deliberately do NOT trust the "end frame" field here — its units didn't
	// match the playback frame in practice (comparing against it froze the anim on
	// frame one). Instead we only use values we actually observe: remember the
	// highest frame reached, and treat the frame jumping backwards as "one full
	// pass completed", then pin it to that observed peak.
	if (animPlaysOnce(animation))
	{
		const float cur = npc->getAnimFrame();

		if (!animHoldActive)
		{
			const auto learned = g_learnedAnimPeak.find(animation);
			const float holdAt = (learned != g_learnedAnimPeak.end())
				? (learned->second - ANIM_HOLD_BACKOFF) : 0.0f;

			if (learned != g_learnedAnimPeak.end() && cur >= holdAt)
			{
				// Known clip: park before it ever reaches the end, so the engine
				// never wraps and the first frame is never shown.
				animHoldActive = true;
				animHoldPeakFrame = (holdAt > 0.0f) ? holdAt : 0.0f;
			}
			else if (cur + 0.001f < animHoldPeakFrame)
			{
				// Unknown clip: it just wrapped, so now we know its length.
				// Remember it — every later playback parks cleanly. Seed this
				// value into g_learnedAnimPeak above to avoid the one restart.
				g_learnedAnimPeak[animation] = animHoldPeakFrame;
				printf("learned anim %u ends at frame %.3f\n",
					animation, animHoldPeakFrame);

				animHoldActive = true;
				animHoldPeakFrame -= ANIM_HOLD_BACKOFF;
				if (animHoldPeakFrame < 0.0f)
					animHoldPeakFrame = 0.0f;
			}
			else
			{
				animHoldPeakFrame = cur; // still playing forward
			}
		}

		if (animHoldActive)
			npc->setAnimFrame(animHoldPeakFrame);
	}
	else if (animHoldActive || animHoldPeakFrame != 0.0f)
	{
		animHoldActive = false;          // different animation - release the hold
		animHoldPeakFrame = 0.0f;
	}

	// --- Marker(s) ---  (follow the lifted body so labels stay above the head)
	if (nickname_marker)
		nickname_marker->setPosition(x, renderY + 110.f, z);
	if (status_marker)
		status_marker->setPosition(x, renderY + 103.f, z);

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

void Player::setPlayerAnim(int anim)
{
	if (npc && npc->isValid()) {
		npc->setNPCAnim(anim);
		animation = anim;
	}
}

void Player::setTeam(int teamId)
{
	if (!npc || !npc->isValid())
		return;

	npc->setTeam(teamId);
}
