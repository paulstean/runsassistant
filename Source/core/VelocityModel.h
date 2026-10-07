#pragma once

// VelocityModel: S5.3/S5.7 velocity stack. Plain C++17, no JUCE.
//
// vel = clamp(round(base * arc * accent), 1, 127), integers only (S5.3).
//   base   = v_start + (v_end - v_start) * p_i        (linear by note index)
//   arc    = 1 + arcSlider * p_i                      (arc in -1..+1 -> 0..2)
//   accent = 1 + accentStrength * weight * falloff_i
//     falloff_i = max(0, 1 - 2 * d_i), d_i = beats to nearest integer beat
//     weight = 1.0 on a bar line, 0.75 otherwise (S5.7; caller supplies
//              the weight from beat %% timeSigNumerator).

namespace runsp
{

// S5.7: falloff reaches zero at half a beat from the grid line.
inline double accentFalloff (double beatDistance)
{
    const double f = 1.0 - 2.0 * beatDistance;
    return f > 0.0 ? f : 0.0;
}

// S5.7 beat weights (v1 tunables): downbeat 1.0, mid-bar beat 0.75. These are
// the RunParams defaults; the Settings values travel in RunParams (the engine
// never reads Settings itself).
constexpr double kDownbeatWeight = 1.0;
constexpr double kMidBarWeight = 0.75;

// Full velocity stack; returns the clamped integer velocity 1..127 (S5.3).
int computeVelocity (double vStart, double vEnd, double pIndex, double arc,
                     double accentStrength, double weight, double falloff);

} // namespace runsp
