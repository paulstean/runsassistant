#include "CurveMap.h"

#include <cmath>

namespace runsp
{

double curveMap (double p, double curveStrength)
{
    // S5.5: endpoints exact; k = 1 -> exactly linear (returned bit-exact).
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1.0;
    if (curveStrength <= 0.0) return p;
    const double k = curveExponent (curveStrength);
    const double r = (1.0 - p) / p;
    return 1.0 / (1.0 + std::pow (r, k));
}

} // namespace runsp
