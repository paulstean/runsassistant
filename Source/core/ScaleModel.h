#pragma once

// ScaleModel: tonic + mode tables + endpoint snapping + degree distance.
// Normative behavior: specification.md S5.3 (endpoint snap), S5.4 (degree
// distance D), S3.2 (degenerate pair detection), S9 (mode list).
// Plain C++17, no JUCE, no allocation, no locks.

#include <cstdint>

namespace runsp
{

constexpr int kNumNamedModes = 16; // indices 0..15 named; index 16 = Custom (S9)
constexpr int kCustomMode = 16;

struct ModeInfo
{
    const char* name;
    int numOffsets;
    uint8_t offsets[12];
};

// Tables match PluginProcessor.cpp kModeList exactly (P1 contract).
const ModeInfo& modeInfo (int index);      // 0..15; assert-free, clamps to 0
const char* modeName (int index);          // 0..16 (16 = "Custom")

class ScaleModel
{
public:
    ScaleModel() = default;
    // mode: 0..kCustomMode. For Custom, customOffsets is the tick-set stored
    // relative to the shown tonic label (S9): bit c => pitch class (tonic+c)%12.
    ScaleModel (int tonicIn, int modeIn, uint16_t customOffsets);

    int tonic() const { return tonic_; }
    int mode() const { return mode_; }

    bool inScale (int pitchClass) const;
    bool inScale (const int* pitches, int count) const; // helper: all in scale

    // S5.3/S5.4: snap each span endpoint to the nearest scale pitch class,
    // snapped INWARD so the span never grows. Ties resolve toward the other
    // endpoint by definition here: "down" snapping for the low endpoint only
    // considers classes at or below it, "up" snapping only at or above, so no
    // circular tie can occur without breaking the inward rule.
    int snapClassDown (int pitchClass) const;
    int snapClassUp (int pitchClass) const;
    int snapPitchDown (int pitch) const; // clamps result >= 0 (S5.3 inward)
    int snapPitchUp (int pitch) const;   // clamps result <= 127 (S5.3 inward)

    // S5.4: degree distance D = number of scale-tone steps going up from
    // classLo to classHi (same class => 0). Distinct scale pitches along the
    // walk = D + 1.
    int stepsUp (int classLo, int classHi) const;

    // S5.3: snapped span; returns true when degenerate (equal pitches after
    // snap, per S3.2 the pair must not fire).
    bool snapSpan (int pitchLo, int pitchHi, int* snappedLo, int* snappedHi) const;

    // Ascending scale pitches in [lo, hi]; returns count, capped at cap.
    int buildSpan (int lo, int hi, int* out, int cap) const;

private:
    int tonic_ = 0;
    int mode_ = 0;
    uint16_t customOffsets_ = 0x0AB5; // Major offsets {0,2,4,5,7,9,11} (unused unless Custom)
    // Note: default custom mask is only used when mode_ == kCustomMode.
};

} // namespace runsp
