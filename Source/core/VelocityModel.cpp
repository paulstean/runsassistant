#include "VelocityModel.h"

#include <cmath>

namespace runsp
{

int computeVelocity (double vStart, double vEnd, double pIndex, double arc,
                     double accentStrength, double weight, double falloff)
{
    // S5.3: base fade by note index, arc multiplier, accent emphasis;
    // integers only: round, then clamp 1..127.
    const double base = vStart + (vEnd - vStart) * pIndex;
    const double arcMul = 1.0 + arc * pIndex;
    const double accentMul = 1.0 + accentStrength * weight * falloff;
    long v = std::lround (base * arcMul * accentMul);
    if (v < 1) v = 1;
    if (v > 127) v = 127;
    return (int) v;
}

} // namespace runsp
