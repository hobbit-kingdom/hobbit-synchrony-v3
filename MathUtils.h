/*
    MathUtils.h
    General-purpose math utilities for interpolation.
*/

#pragma once

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace MathUtils
{
    /// Standard linear interpolation.
    inline float lerp(float from, float to, float t)
    {
        return from + (to - from) * t;
    }

    /// Angle-aware lerp that handles wrapping around -PI to PI.
    inline float lerpAngle(float from, float to, float t)
    {
        float diff = to - from;
        while (diff >  static_cast<float>(M_PI)) diff -= 2.0f * static_cast<float>(M_PI);
        while (diff < -static_cast<float>(M_PI)) diff += 2.0f * static_cast<float>(M_PI);
        return from + diff * t;
    }

    /// Clamp a value to [minVal, maxVal].
    inline float clamp(float value, float minVal, float maxVal)
    {
        if (value < minVal) return minVal;
        if (value > maxVal) return maxVal;
        return value;
    }
}
