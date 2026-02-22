/*
    shared.h
    Convenience header — includes all game networking headers at once.

    Individual headers:
      - GameTypes.h       : Vector3, NetDefaults, guidFromString()
      - MathUtils.h       : lerp, lerpAngle, clamp
      - NetworkMessages.h : PositionMessage, GuidAssignMessage
      - NetworkConfig.h   : Protocol config, message factory, adapter
*/

#ifndef SHARED_H
#define SHARED_H

#include "GameTypes.h"
#include "MathUtils.h"
#include "NetworkMessages.h"
#include "NetworkConfig.h"

#endif // SHARED_H
