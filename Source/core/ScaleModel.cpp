#include "ScaleModel.h"

namespace runsp
{

namespace
{
// S9 mode list v1; matches PluginProcessor.cpp kModeList exactly.
const ModeInfo kModes[] =
{
    { "Major (Ionian)",            7, { 0, 2, 4, 5, 7, 9, 11 } },
    { "Dorian",                    7, { 0, 2, 3, 5, 7, 9, 10 } },
    { "Phrygian",                  7, { 0, 1, 3, 5, 7, 8, 10 } },
    { "Lydian",                    7, { 0, 2, 4, 6, 7, 9, 11 } },
    { "Mixolydian",                7, { 0, 2, 4, 5, 7, 9, 10 } },
    { "Aeolian (Natural Minor)",   7, { 0, 2, 3, 5, 7, 8, 10 } },
    { "Locrian",                   7, { 0, 1, 3, 5, 6, 8, 10 } },
    { "Harmonic Minor",            7, { 0, 2, 3, 5, 7, 8, 11 } },
    { "Melodic Minor (ascending)", 7, { 0, 2, 3, 5, 7, 9, 11 } },
    { "Whole Tone",                6, { 0, 2, 4, 6, 8, 10 } },
    { "Octatonic (whole-half)",    8, { 0, 2, 3, 5, 6, 8, 9, 11 } },
    { "Octatonic (half-whole)",    8, { 0, 1, 3, 4, 6, 7, 9, 10 } },
    { "Major Pentatonic",          5, { 0, 2, 4, 7, 9 } },
    { "Minor Pentatonic",          5, { 0, 3, 5, 7, 10 } },
    { "Blues",                     6, { 0, 3, 5, 6, 7, 10 } },
    { "Chromatic",                12, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
};

constexpr int kNumModeEntries = 16;
constexpr uint16_t kAllClasses = 0x0FFF;

inline int wrap (int pc) { int r = pc % 12; return r < 0 ? r + 12 : r; }

inline bool bitSet (uint16_t mask, int bit)
{
    if (bit < 0 || bit > 11) return false;
    return (mask >> bit) & 1u;
}
} // namespace

const ModeInfo& modeInfo (int index)
{
    if (index < 0 || index >= kNumModeEntries)
        index = 0;
    return kModes[index];
}

const char* modeName (int index)
{
    if (index == kCustomMode) return "Custom";
    return modeInfo (index).name;
}

ScaleModel::ScaleModel (int tonicIn, int modeIn, uint16_t customOffsets)
{
    tonic_ = tonicIn >= 0 && tonicIn < 12 ? tonicIn : 0;
    mode_ = modeIn >= 0 && modeIn <= kCustomMode ? modeIn : 0;
    customOffsets_ = customOffsets;
}

bool ScaleModel::inScale (int pitchClass) const
{
    if (pitchClass < 0 || pitchClass > 127) return false;
    const int rel = wrap (pitchClass - tonic_);
    if (mode_ == kCustomMode)
        return bitSet (customOffsets_, rel);
    const ModeInfo& m = kModes[mode_];
    for (int i = 0; i < m.numOffsets; ++i)
        if ((int) m.offsets[i] == rel) return true;
    return false;
}

// S3.2: degenerate detection operates on the snapped span.
bool ScaleModel::inScale (const int* pitches, int count) const
{
    for (int i = 0; i < count; ++i)
        if (! inScale (pitches[i])) return false;
    return true;
}

int ScaleModel::snapClassDown (int pitchClass) const
{
    // S5.3: snapped inward; nearest scale class at or below pc (circular down).
    const int pc = wrap (pitchClass);
    for (int d = 0; d < 12; ++d)
    {
        const int cand = wrap (pc - d);
        if (inScale (cand)) return cand;
    }
    return pc; // unreachable for any sane scale
}

int ScaleModel::snapClassUp (int pitchClass) const
{
    const int pc = wrap (pitchClass);
    for (int d = 0; d < 12; ++d)
    {
        const int cand = wrap (pc + d);
        if (inScale (cand)) return cand;
    }
    return pc;
}

int ScaleModel::snapPitchDown (int pitch) const
{
    if (pitch < 0 || pitch > 127) return pitch;
    const int s = snapClassDown (pitch % 12);
    return pitch - (int) ((unsigned) (pitch % 12 - s + 12) % 12u);
}

int ScaleModel::snapPitchUp (int pitch) const
{
    if (pitch < 0 || pitch > 127) return pitch;
    const int s = snapClassUp (pitch % 12);
    return pitch + (int) ((unsigned) (s - pitch % 12 + 12) % 12u);
}

int ScaleModel::stepsUp (int classLo, int classHi) const
{
    // S5.4: D = number of scale-tone steps upward; distinct pitches on the
    // walk = D + 1 (inclusive of both ends).
    const int lo = wrap (classLo);
    const int hi = wrap (classHi);
    const int span = (hi - lo + 12) % 12;
    if (span == 0) return 0;
    int steps = 0;
    for (int k = 1; k <= span; ++k)
        if (inScale (wrap (lo + k))) ++steps;
    return steps;
}

bool ScaleModel::snapSpan (int pitchLo, int pitchHi, int* snappedLo, int* snappedHi) const
{
    // S5.3: sort, snap each endpoint inward; S3.2: degenerate = equal pair
    // (equal pitches, S3.2) or equal pitches after snap; does not fire.
    if (pitchLo > pitchHi) { const int t = pitchLo; pitchLo = pitchHi; pitchHi = t; }
    const int lo = snapPitchDown (pitchLo);
    const int hi = snapPitchUp (pitchHi);
    *snappedLo = lo;
    *snappedHi = hi;
    return pitchLo == pitchHi || lo == hi; // S3.2: degenerate pair
}

int ScaleModel::buildSpan (int lo, int hi, int* out, int cap) const
{
    if (lo > hi) { const int t = lo; lo = hi; hi = t; }
    int count = 0;
    for (int p = lo; p <= hi && count < cap; ++p)
        if (inScale (p % 12)) out[count++] = p;
    return count;
}

} // namespace runsp
