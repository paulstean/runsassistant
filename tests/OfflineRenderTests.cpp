// Offline export conformance (drag-to-DAW path, Source/core/OfflineRun.h):
// a complete run rendered for explicit endpoints must come back time-sorted,
// fully paired, closed by the tail note-off, with the S5.7 accent weights
// taken from RunParams (the Settings tunables now reach the engine).

#include <vector>

#include "TestFramework.h"
#include "core/OfflineRun.h"
#include "core/ScaleModel.h"

using namespace runsp;

namespace
{
RunParams exportParams()
{
    RunParams p;
    p.beats = 4;
    p.density = 5.0;
    p.curveStrength = 0.22;
    p.accentStrength = 0.41;
    p.arc = 0.15;
    p.tonic = 0;
    p.mode = 0; // C major
    p.walk = WalkMode::ZigZag;
    p.noteOverlap = false;
    p.barNumerator = 4;
    p.hasBarOrigin = false;
    p.alignToGrid = false; // export runs start at beat 0
    return p;
}

PairTrigger trigger (int lo, int hi, bool up, int vLo = 100, int vHi = 90)
{
    PairTrigger t;
    t.pitchLo = lo;
    t.pitchHi = hi;
    t.velLo = vLo;
    t.velHi = vHi;
    t.direction = up ? Direction::Up : Direction::Down;
    t.channel = 0;
    t.triggerBeat = 0.0;
    return t;
}

// Beats must never go backwards, and on a beat tie the note-off leads.
bool isTimeSorted (const RunEvent* e, int n)
{
    for (int i = 1; i < n; ++i)
    {
        if (e[i].beat < e[i - 1].beat) return false;
        if (e[i].beat == e[i - 1].beat && e[i - 1].kind == RunEvent::NoteOn
            && e[i].kind == RunEvent::NoteOff)
            return false;
    }
    return true;
}

// Every note-on is closed exactly once, per pitch, in list order.
bool isFullyPaired (const RunEvent* e, int n)
{
    int open[128] = {};
    for (int i = 0; i < n; ++i)
    {
        if (e[i].pitch < 0 || e[i].pitch > 127) return false;
        if (e[i].kind == RunEvent::NoteOn)
        {
            if (open[e[i].pitch]) return false; // retrigger before the off
            open[e[i].pitch] = 1;
        }
        else
        {
            if (! open[e[i].pitch]) return false;
            open[e[i].pitch] = 0;
        }
    }
    for (int p = 0; p < 128; ++p)
        if (open[p]) return false; // a note never closed
    return true;
}
} // namespace

TEST_CASE (offline_render_endpoints_and_close)
{
    RunEvent buf[8192];
    const RunParams p = exportParams();
    const OfflineRunResult r =
        renderRunOffline (trigger (60, 72, true), p, 1.0, buf, 8192);

    CHECK (r.ok());
    CHECK_EQ (r.noteCount, 20); // round(5 x 4)
    CHECK_EQ (r.eventCount, 40); // one off per note
    CHECK_NEAR (r.startBeat, 0.0, 1e-9);
    CHECK (isTimeSorted (buf, r.eventCount));
    CHECK (isFullyPaired (buf, r.eventCount));
    CHECK_EQ ((int) buf[r.eventCount - 1].kind, (int) RunEvent::NoteOff);

    // First note-on is the Start pitch, last is the Target (S5.4 endpoint
    // exactness, engine Up).
    int firstOn = -1, lastOn = -1, ons = 0;
    for (int i = 0; i < r.eventCount; ++i)
        if (buf[i].kind == RunEvent::NoteOn)
        {
            if (firstOn < 0) firstOn = buf[i].pitch;
            lastOn = buf[i].pitch;
            ++ons;
        }
    CHECK_EQ (ons, 20);
    CHECK_EQ (firstOn, 60);
    CHECK_EQ (lastOn, 72);

    // Tail: the closing off sits one beat past the run's last onset.
    CHECK_NEAR (r.endBeat, 5.0, 1e-9);
}

TEST_CASE (offline_render_down_direction_reverses_endpoints)
{
    RunEvent buf[8192];
    const OfflineRunResult r =
        renderRunOffline (trigger (60, 72, false), exportParams(), 1.0,
                          buf, 8192);
    CHECK (r.ok());
    int firstOn = -1, lastOn = -1;
    for (int i = 0; i < r.eventCount; ++i)
        if (buf[i].kind == RunEvent::NoteOn)
        {
            if (firstOn < 0) firstOn = buf[i].pitch;
            lastOn = buf[i].pitch;
        }
    CHECK_EQ (firstOn, 72); // Down runs from the high end
    CHECK_EQ (lastOn, 60);
}

// Overlap mode is the whole reason the drain gets re-sorted: the interior
// note-off lands past the next note-on, which a one-shot drain emits the
// wrong way round. Two consecutive note-ons after the sort are the proof.
TEST_CASE (offline_render_overlap_is_resorted)
{
    RunEvent buf[8192];
    RunParams p = exportParams();
    p.noteOverlap = true;
    p.density = 8.0;
    p.curveStrength = 0.8;

    const OfflineRunResult r = renderRunOffline (
        trigger (60, 84, true), p, 1.0, buf, 8192);

    CHECK (r.ok());
    CHECK (r.noteCount >= 4);
    CHECK (isTimeSorted (buf, r.eventCount));
    CHECK (isFullyPaired (buf, r.eventCount));
    CHECK_EQ ((int) buf[r.eventCount - 1].kind, (int) RunEvent::NoteOff);
    CHECK (buf[0].kind == RunEvent::NoteOn
           && buf[1].kind == RunEvent::NoteOn); // simultaneous notes, sorted
}

// S5.7: the accent beat weights come from RunParams (Settings), not from
// hard-coded constants. 3 notes on beats 0 / 2 / 4 of a 4/4 run: beat 0 and
// 4 are bar lines, beat 2 is mid-bar.
TEST_CASE (offline_render_accent_weights_from_params)
{
    RunParams p = exportParams();
    p.density = 0.75;      // n = round(0.75 x 4) = 3
    p.curveStrength = 0.0; // uniform: onsets 0, 2, 4
    p.accentStrength = 1.0;
    p.arc = 0.0;
    p.walk = WalkMode::Fold;
    p.noteOverlap = false;

    auto velocities = [&] (double down, double mid)
    {
        p.downWeight = down;
        p.midBarWeight = mid;
        RunEvent buf[64];
        const OfflineRunResult r =
            renderRunOffline (trigger (60, 72, true, 50, 50), p, 1.0, buf, 64);
        CHECK (r.ok());
        std::vector<int> v;
        for (int i = 0; i < r.eventCount; ++i)
            if (buf[i].kind == RunEvent::NoteOn) v.push_back (buf[i].velocity);
        return v;
    };

    // Spec defaults (S5.7): downbeat 1.0, mid-bar 0.75 -> 100 / 88 / 100.
    const std::vector<int> def = velocities (1.0, 0.75);
    CHECK_EQ ((int) def.size(), 3);
    CHECK_EQ (def[0], 100);
    CHECK_EQ (def[1], 88);
    CHECK_EQ (def[2], 100);

    // Tuned weights must actually change the render (they used to be inert).
    const std::vector<int> tuned = velocities (0.5, 0.0);
    CHECK_EQ ((int) tuned.size(), 3);
    CHECK_EQ (tuned[0], 75);
    CHECK_EQ (tuned[1], 50);
    CHECK_EQ (tuned[2], 75);
}

TEST_CASE (offline_render_rejects_bad_input)
{
    RunEvent buf[64];
    const RunParams p = exportParams();

    // S3.2: an equal pair does not fire.
    const OfflineRunResult same =
        renderRunOffline (trigger (60, 60, true), p, 1.0, buf, 64);
    CHECK_EQ (same.error, (int) OfflineRunResult::Degenerate);
    CHECK (! same.ok());

    const OfflineRunResult noBuf =
        renderRunOffline (trigger (60, 72, true), p, 1.0, nullptr, 64);
    CHECK_EQ (noBuf.error, (int) OfflineRunResult::BadBuffer);

    const OfflineRunResult small =
        renderRunOffline (trigger (60, 72, true), p, 1.0, buf, 4);
    CHECK_EQ (small.error, (int) OfflineRunResult::Overflow);
}
