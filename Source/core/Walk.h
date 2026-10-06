#pragma once

// Pitch walk per specification.md S5.4: Fold (bounce at boundaries) and
// Zig-zag (monotonic forward with periodic single backward steps).
// Plain C++17, no JUCE, no allocation on the call path.

#include "ScaleModel.h"

namespace runsp
{

enum class WalkMode { Fold, ZigZag };

struct WalkResult
{
    int count = 0;       // number of emitted pitches (n, possibly n-1: S5.4 parity drop)
    bool parityDrop = false; // true when a note was dropped (S5.4 odd S - D)
};

// Builds the walk pitches for a span of L distinct scale pitches (list from
// ScaleModel::buildSpan, ascending). dirUp: start at spanLo (index 0) and end
// at spanHi; dirDown mirrored (start at spanHi, end at spanLo), per S5.3.
// n = requested notes; S = n - 1 steps. Endpoint exactness guaranteed
// (S5.4: pitch_0 = run start, pitch_{n-1} = run target, always exact).
// out must have capacity >= the returned count (<= n).
WalkResult buildWalk (const int* spanPitches, int spanLength, bool dirUp, int n,
                      WalkMode mode, int* out);

} // namespace runsp
