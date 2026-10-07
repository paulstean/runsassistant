#pragma once

// CurveMap: S5.5 shape (power-S map). Plain C++17, no JUCE.

namespace runsp
{

// S5.5 (revised, see specification.md S13.11): the power-S exponent is
// INVERTED against the original k = 1 + 9 x curveStrength. The original
// k > 1 map bent the onset curve the wrong way for ear purposes: onsets
// crowded at the rims (fast start, slow middle, fast end) and only
// strengths up to ~5 percent were usable (REAPER pass). Shipped shape:
//
// curve_map(p) = 1 / (1 + ((1 - p) / p)^e), e = 1 - 0.9 * curveStrength
//
// curveStrength 0 -> e = 1 -> y = p exactly (linear, bit-exact path kept);
// larger strengths decrease e (10% strength steps the exponent down 0.9),
// which widens the onset spacing at both rims (deliberate first steps,
// settling target note) and rushes the middle: slow, quick, slow (S1).
// Symmetric: y(1 - p) = 1 - y(p); mid anchor y(0.5) = 0.5; monotonic;
// endpoints exact; cheap evaluation; no inversion needed.
double curveMap (double p, double curveStrength);

// e from S5.5, exposed for tests/debug.
inline double curveExponent (double curveStrength) { return 1.0 - 0.9 * curveStrength; }

} // namespace runsp
