#pragma once

// CurveMap: S5.5 shape (power-S map). Plain C++17, no JUCE.

namespace runsp
{

// curve_map(p) = 1 / (1 + ((1 - p) / p)^k), k = 1 + 9 * curveStrength.
// Guards: y(0) = 0, y(1) = 1 exactly (S5.5 endpoints exact).
// S5.5: curveStrength 0 -> k = 1 -> y = p, returned bit-exact (linear path).
double curveMap (double p, double curveStrength);

// k from S5.5, exposed for tests/debug.
inline double curveExponent (double curveStrength) { return 1.0 + 9.0 * curveStrength; }

} // namespace runsp
