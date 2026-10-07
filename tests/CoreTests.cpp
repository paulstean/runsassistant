// Core conformance vectors: CurveMap (S5.5), ScaleModel (S5.3/S5.4/S3.2/S9),
// VelocityModel (S5.3/S5.7).

#include <cmath>

#include "TestFramework.h"
#include "core/CurveMap.h"
#include "core/ScaleModel.h"
#include "core/VelocityModel.h"
#include "core/Walk.h"

using namespace runsp;

// ------------------------------------------------------------------ CurveMap

TEST_CASE (curve_linear_at_zero_bit_exact)
{
    for (int i = 0; i <= 100; ++i)
    {
        const double p = (double) i / 100.0;
        const double y = curveMap (p, 0.0);
        if (i == 0) CHECK_EQ (y, 0.0);
        else if (i == 100) CHECK_EQ (y, 1.0);
        else CHECK_EQ (y, p); // S5.5: k = 1 -> y = p exactly (bit-exact)
    }
}

TEST_CASE (curve_endpoints_and_mid_anchor)
{
    for (double s = 0.0; s <= 1.0; s += 0.1)
    {
        CHECK_EQ (curveMap (0.0, s), 0.0); // S5.5: y(0) = 0
        CHECK_EQ (curveMap (1.0, s), 1.0); // S5.5: y(1) = 1
        CHECK_NEAR (curveMap (0.5, s), 0.5, 1e-12); // symmetric mid anchor
    }
}

TEST_CASE (curve_monotonic_0_to_1)
{
    for (double s : { 0.0, 0.1, 0.5, 0.9, 1.0 })
    {
        double prev = curveMap (0.0, s);
        for (int i = 1; i <= 1000; ++i)
        {
            const double p = (double) i / 1000.0;
            const double y = curveMap (p, s);
            CHECK (y >= prev); // S5.5: monotonic
            prev = y;
        }
    }
}

TEST_CASE (curve_symmetric)
{
    // S5.5: y(1 - p) = 1 - y(p).
    for (double s : { 0.2, 0.5, 1.0 })
        for (int i = 1; i < 1000; ++i)
        {
            const double p = (double) i / 1000.0;
            CHECK_NEAR (curveMap (1.0 - p, s), 1.0 - curveMap (p, s), 1e-12);
        }
}

TEST_CASE (curve_slow_quick_slow_direction)
{
    // S5.5 (revised, S13.11): strength > 0 widens the onset spacing at both
    // rims and rushes the middle (slow, quick, slow). For a uniform note
    // index, the first quarter of the run must span MORE time than linear
    // and the symmetric end quarter spans less of its rim distance:
    // y(p) > p for p < 0.5 and y(p) < p for p > 0.5.
    for (double s : { 0.05, 0.2, 0.5, 0.9, 1.0 })
    {
        CHECK (curveMap (0.1, s) > 0.1);
        CHECK (curveMap (0.25, s) > 0.25);
        CHECK (curveMap (0.75, s) < 0.75);
        CHECK (curveMap (0.9, s) < 0.9);
        // strength increases -> stronger S in the same direction
        CHECK (curveMap (0.25, 1.0) > curveMap (0.25, 0.2));
        CHECK (curveMap (0.75, 1.0) < curveMap (0.75, 0.2));
    }
}

// ---------------------------------------------------------------- ScaleModel

TEST_CASE (scale_tables_match_processor_list)
{
    // Table contract with PluginProcessor.cpp kModeList.
    CHECK_EQ (modeInfo (0).numOffsets, 7);
    CHECK_EQ ((int) modeInfo (0).offsets[0], 0);
    CHECK_EQ ((int) modeInfo (9).numOffsets, 6);   // Whole Tone
    CHECK_EQ ((int) modeInfo (9).offsets[5], 10);
    CHECK_EQ ((int) modeInfo (10).numOffsets, 8);  // Octatonic whole-half
    CHECK_EQ ((int) modeInfo (12).numOffsets, 5);  // Major Pentatonic
    CHECK_EQ ((int) modeInfo (15).numOffsets, 12); // Chromatic
    for (int i = 0; i < kNumNamedModes; ++i)
        CHECK (modeName (i) != nullptr && modeName (i)[0] != '\0');
    CHECK (std::strcmp (modeName (kCustomMode), "Custom") == 0);
}

TEST_CASE (scale_membership)
{
    ScaleModel c (0, 0, 0); // C major
    CHECK (c.inScale (0) && c.inScale (2) && c.inScale (7));
    CHECK (! c.inScale (1) && ! c.inScale (10));

    ScaleModel dLyd (2, 3, 0); // D Lydian: D E F# G# A B C#
    CHECK (dLyd.inScale (6));
    CHECK (! dLyd.inScale (7)); // G natural not in D Lydian

    ScaleModel custom (0, kCustomMode, 0x0AB5); // Major tick set, relative to tonic
    CHECK (custom.inScale (4));
    CHECK (! custom.inScale (1));
}

TEST_CASE (snap_inward_and_degenerate)
{
    ScaleModel c (0, 0, 0); // C major
    int lo = 0, hi = 0;
    // S5.3: snap inward, span never grows: 61/64 (C#/E) -> 60/64.
    CHECK (! c.snapSpan (61, 64, &lo, &hi));
    CHECK_EQ (lo, 60);
    CHECK_EQ (hi, 64);
    // Low endpoint snaps down even when its upper neighbor is nearest
    // (inward rule wins): 61 -> 60, never 62.
    CHECK_EQ (c.snapPitchDown (61), 60);
    CHECK_EQ (c.snapPitchUp (61), 62);
    // Pentatonic: between-tone classes snap to nearest inward neighbor.
    ScaleModel pent (0, 12, 0); // C Major Pentatonic {0,2,4,7,9}
    CHECK_EQ (pent.snapPitchDown (63), 62); // class 3 -> 2 (down)
    CHECK_EQ (pent.snapPitchUp (63), 64);   // class 3 -> 4 (up)
    // S3.2: degenerate pair (equal pitches after snap) detected.
    CHECK (c.snapSpan (61, 61, &lo, &hi));
    CHECK_EQ (lo, 60);
}

TEST_CASE (degree_steps_known_spans)
{
    ScaleModel c (0, 0, 0); // C major
    // S5.4: D = scale-tone steps upward; distinct pitches = D + 1.
    CHECK_EQ (c.stepsUp (0, 7), 4);   // C D E F G
    CHECK_EQ (c.stepsUp (0, 11), 6);
    CHECK_EQ (c.stepsUp (0, 0), 0);
    CHECK_EQ (c.stepsUp (1, 1), 0);   // same class
    // Whole tone: C to D is one scale step; neighboring scale classes are 2 semitones.
    ScaleModel wt (0, 9, 0);
    CHECK_EQ (wt.stepsUp (0, 2), 1);  // C -> D
    CHECK_EQ (wt.stepsUp (2, 4), 1);  // D -> E
    CHECK_EQ (wt.stepsUp (0, 10), 5);
    // Chromatic counts every semitone.
    ScaleModel chroma (0, 15, 0);
    CHECK_EQ (chroma.stepsUp (0, 11), 11);
}

TEST_CASE (build_span_array)
{
    ScaleModel pent (0, 12, 0);
    int pitches[64];
    const int n = pent.buildSpan (60, 72, pitches, 64);
    // C Major Pentatonic across one octave: (60,62,64,67,69,72).
    CHECK_EQ (n, 6);
    const int expect[6] = { 60, 62, 64, 67, 69, 72 };
    for (int i = 0; i < n; ++i)
        CHECK_EQ (pitches[i], expect[i]);
}

// ------------------------------------------------------------- VelocityModel

TEST_CASE (velocity_base_fade_exact)
{
    // S5.3: base = v_start + (v_end - v_start) * p; arc 0, accent 0.
    CHECK_EQ (computeVelocity (100.0, 20.0, 0.0, 0.0, 0.0, 1.0, 0.0), 100);
    CHECK_EQ (computeVelocity (100.0, 20.0, 1.0, 0.0, 0.0, 1.0, 0.0), 20);
    CHECK_EQ (computeVelocity (100.0, 20.0, 0.5, 0.0, 0.0, 1.0, 0.0), 60);
}

TEST_CASE (velocity_arc_multiplier)
{
    // S5.3: arc -1..+1 -> multiplier 1 + arc * p (0..2).
    CHECK_EQ (computeVelocity (100.0, 100.0, 0.5, 1.0, 0.0, 1.0, 0.0), 127);
    CHECK_EQ (computeVelocity (100.0, 100.0, 0.5, -1.0, 0.0, 1.0, 0.0), 50);
}

TEST_CASE (velocity_accent_falloff)
{
    // S5.7: d = 0 (exact beat line) -> full accent; d = 0.5 -> zero accent.
    CHECK_EQ (computeVelocity (100.0, 100.0, 0.3, 0.0, 1.0, 1.0, 1.0), 127);
    CHECK_EQ (computeVelocity (100.0, 100.0, 0.3, 0.0, 0.25, 1.0, 1.0), 125);
    CHECK_EQ (computeVelocity (100.0, 100.0, 0.3, 0.0, 1.0, 1.0, 0.0), 100);
    CHECK_NEAR (accentFalloff (0.0), 1.0, 1e-12);
    CHECK_NEAR (accentFalloff (0.5), 0.0, 1e-12);
    CHECK_NEAR (accentFalloff (0.25), 0.5, 1e-12);
}

TEST_CASE (velocity_clamps)
{
    // S5.3: clamp(round(...), 1, 127); integers only.
    CHECK_EQ (computeVelocity (1.0, 1.0, 0.0, -1.0, 0.0, 0.75, 0.0), 1);  // round(0.25) -> clamp 1
    CHECK_EQ (computeVelocity (0.1, 0.1, 0.0, 0.0, 0.0, 1.0, 0.0), 1);
    CHECK_EQ (computeVelocity (1000.0, 1000.0, 0.0, 1.0, 1.0, 1.0, 1.0), 127);
    CHECK_EQ (computeVelocity (127.0, 127.0, 0.0, 1.0, 0.0, 1.0, 1.0), 127);
    for (double p = 0.0; p <= 1.0; p += 0.05)
    {
        const int v = computeVelocity (1.0, 127.0, p, 1.0, 1.0, 1.0, 1.0);
        CHECK (v >= 1 && v <= 127);
    }
}
