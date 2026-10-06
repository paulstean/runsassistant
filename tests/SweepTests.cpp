// Parameterized sweeps, specification.md S10: global invariants across beats x
// density x spans x directions x walk modes. Fast (< 2s total).

#include <vector>

#include "TestFramework.h"
#include "core/RunEngine.h"
#include "core/ScaleModel.h"

using namespace runsp;

namespace
{
bool collectRun (RunEngine& e, const PairTrigger& t, const RunParams& p,
                 std::vector<int>* onP, std::vector<int>* offP,
                 std::vector<double>* onBeats)
{
    e.cancel();
    if (! e.startRun (t, p)) return false;
    static RunEvent buf[8192];
    RunEventSink sink;
    sink.data = buf;
    sink.cap = 8192;
    e.pumpUpTo (100000.0, sink);
    e.cut (100000.0, sink); // release the held final note (tail-end steady)
    for (int i = 0; i < sink.count; ++i)
    {
        if (buf[i].kind == RunEvent::NoteOn)
        {
            if (onP != nullptr) onP->push_back (buf[i].pitch);
            if (onBeats != nullptr) onBeats->push_back (buf[i].beat);
        }
        else if (offP != nullptr)
        {
            offP->push_back (buf[i].pitch);
        }
    }
    return true;
}
} // namespace

TEST_CASE (sweep_global_invariants)
{
    RunEngine e;
    ScaleModel cMajor (0, 0, 0);
    const double densities[] = { 0.5, 1.25, 3.5, 6.25, 10.0 };
    for (int beats = 1; beats <= 4; ++beats)
        for (double density : densities)
            for (int spanSemis = 1; spanSemis <= 24; ++spanSemis)
                for (int d = 0; d < 2; ++d)
                    for (int wm = 0; wm < 2; ++wm)
                    {
                        RunParams p;
                        p.beats = beats;
                        p.density = density;
                        p.curveStrength = 0.35;
                        p.accentStrength = 0.4;
                        p.arc = 0.5;
                        p.tonic = 0;
                        p.mode = 0;
                        p.walk = wm == 0 ? WalkMode::Fold : WalkMode::ZigZag;
                        p.gateFraction = 0.6;

                        const int lo = 60, hi = 60 + spanSemis;
                        int snapLo = 0, snapHi = 0;
                        const bool degenerate = cMajor.snapSpan (lo, hi, &snapLo, &snapHi);

                        PairTrigger t;
                        t.pitchLo = lo;
                        t.pitchHi = hi;
                        t.velLo = 100;
                        t.velHi = 60;
                        t.direction = d == 0 ? Direction::Up : Direction::Down;
                        t.channel = 0;
                        t.triggerBeat = 0.5;

                        std::vector<int> onP;
                        std::vector<int> offP;
                        std::vector<double> onBeats;
                        const bool fired = collectRun (e, t, p, &onP, &offP, &onBeats);

                        if (degenerate)
                        {
                            CHECK (! fired); // S3.2: degenerate pairs do not fire
                            continue;
                        }
                        CHECK (fired);

                        // Onsets strictly increasing, endpoints inclusive in
                        // [startBeat, startBeat + beats] (S5.3); a single-note
                        // run (n == 1) only defines onset 0.
                        for (size_t i = 1; i < onBeats.size(); ++i)
                            CHECK (onBeats[i] > onBeats[i - 1]); // monotonic curve map (S5.5)
                        CHECK_NEAR (onBeats.front(), e.startBeat(), 1e-9);
                        if (e.noteCount() > 1)
                            CHECK_NEAR (onBeats.back(), e.startBeat() + (double) beats, 1e-9);

                        // Pitch walk: endpoints exact, scale-only, no repeats.
                        CHECK (onP.size() >= 1);
                        for (size_t i = 1; i < onP.size(); ++i)
                            CHECK (onP[i] != onP[i - 1]); // no consecutive repeats (S5.4)
                        ScaleModel sc (0, 0, 0);
                        for (int pitch : onP)
                            CHECK (sc.inScale (pitch % 12));

                        // Velocities integer in 1..127 (S5.3).
                        for (int i = 0; i < e.noteCount(); ++i)
                        {
                            const int v = e.velocityOf (i);
                            CHECK (v >= 1 && v <= 127);
                        }

                        (void) offP;
                        e.cancel();
                    }
}

TEST_CASE (sweep_zigzag_parity_accounting)
{
    // S5.4/S10: for odd S - D the run has one fewer note than asked and the
    // parity-drop counter increments once.
    RunEngine e;
    ScaleModel cMajor (0, 0, 0);
    int spanBuf[130];
    for (int spanSemis = 1; spanSemis <= 24; ++spanSemis)
    {
        int snapLo = 0, snapHi = 0;
        cMajor.snapSpan (60, 60 + spanSemis, &snapLo, &snapHi);
        if (snapLo == snapHi) continue; // degenerate
        const int L = cMajor.buildSpan (snapLo, snapHi, spanBuf, 130);
        const int D = L - 1;
        for (int n = 2; n <= 7; ++n)
        {
            const int S = n - 1;
            const bool odd = S > D && ((S - D) % 2) != 0;
            RunParams p;
            p.beats = 16;
            p.density = (double) n / 16.0; // n = 2..7 notes
            p.walk = WalkMode::ZigZag;
            PairTrigger t;
            t.pitchLo = snapLo;
            t.pitchHi = snapHi;
            t.velLo = t.velHi = 100;
            t.direction = Direction::Up;
            t.triggerBeat = 0.0;
            std::vector<int> onP;
            const int dropsBefore = e.counters().parityDrops;
            CHECK (collectRun (e, t, p, &onP, nullptr, nullptr));
            CHECK_EQ ((int) onP.size(), odd ? n - 1 : n); // parity drop (S5.4)
            CHECK_EQ (e.counters().parityDrops - dropsBefore, odd ? 1 : 0);
            CHECK_EQ (onP.back(), snapHi); // endpoints exact even after drop
            e.cancel();
        }
    }
}

TEST_CASE (sweep_fold_landing_exact_all_spans)
{
    // S5.4: fold lands exactly on target for every span and direction.
    RunEngine e;
    ScaleModel aeolian (5, 5, 0); // F Aeolian
    int spanBuf[130];
    for (int spanSemis = 3; spanSemis <= 40; ++spanSemis)
    {
        int snapLo = 0, snapHi = 0;
        aeolian.snapSpan (65, 65 + spanSemis, &snapLo, &snapHi);
        if (snapLo == snapHi) continue; // degenerate
        const int L = aeolian.buildSpan (snapLo, snapHi, spanBuf, 130);
        if (L < 2) continue;
        for (int wm = 0; wm < 2; ++wm)
            for (int d = 0; d < 2; ++d)
            {
                RunParams p;
                p.beats = 8;
                p.density = 5.0; // n = 40: over-dense for every span here
                p.tonic = 5;
                p.mode = 5;
                p.walk = wm == 0 ? WalkMode::Fold : WalkMode::ZigZag;
                PairTrigger t;
                t.pitchLo = snapLo;
                t.pitchHi = snapHi;
                t.velLo = t.velHi = 90;
                t.direction = d == 0 ? Direction::Up : Direction::Down;
                t.triggerBeat = 0.0;
                std::vector<int> onP;
                CHECK (collectRun (e, t, p, &onP, nullptr, nullptr));
                const int expectStart = d == 0 ? snapLo : snapHi;
                const int expectEnd = d == 0 ? snapHi : snapLo;
                CHECK_EQ (onP.front(), expectStart); // first pitch = start (S5.4)
                CHECK_EQ (onP.back(), expectEnd);    // landing exact (S5.4)
                for (size_t i = 1; i < onP.size(); ++i)
                    CHECK (onP[i] != onP[i - 1]); // no consecutive repeats
                for (int pitch : onP)
                    CHECK (aeolian.inScale (pitch % 12));
                e.cancel();
            }
    }
}
