#include "OfflineRun.h"

#include <algorithm>

namespace runsp
{

namespace
{
// Drain target: every onset sits at startBeat + beats at the latest, and an
// overlap off can reach past it, so pump with a large finite horizon (the
// conformance tests use the same trick).
constexpr double kDrainHorizon = 1.0e9;

// Time order first; on a beat tie the note-off precedes the note-on (S5.8 /
// S6.1 ordering), which is also what a retrigger of the same pitch needs.
struct ByBeat
{
    bool operator() (const RunEvent& a, const RunEvent& b) const
    {
        if (a.beat != b.beat) return a.beat < b.beat;
        return a.kind > b.kind;
    }
};
} // namespace

OfflineRunResult renderRunOffline (const PairTrigger& t, const RunParams& p,
                                   double tailBeats, RunEvent* events, int cap)
{
    OfflineRunResult r;
    if (events == nullptr || cap < 2)
    {
        r.error = OfflineRunResult::BadBuffer;
        return r;
    }

    RunEngine e;
    RunEventSink sink;
    sink.data = events;
    sink.cap = cap;

    if (! e.startRun (t, p))
    {
        r.error = OfflineRunResult::Degenerate; // S3.2: refused, no run
        return r;
    }
    r.startBeat = e.startBeat();

    e.pumpUpTo (kDrainHorizon, sink);

    // S5.6 tail-end steady: the final note has no gate, so the caller's tail
    // length is what closes it.
    const double tail = tailBeats > 0.0 ? tailBeats : 0.0;
    e.cut (r.startBeat + (double) p.beats + tail, sink);

    if (sink.overflow)
    {
        r.error = OfflineRunResult::Overflow;
        r.eventCount = sink.count;
        return r;
    }

    std::stable_sort (events, events + sink.count, ByBeat {});

    r.eventCount = sink.count;
    r.endBeat = sink.count > 0 ? events[sink.count - 1].beat : r.startBeat;
    for (int i = 0; i < sink.count; ++i)
        if (events[i].kind == RunEvent::NoteOn) ++r.noteCount;
    return r;
}

} // namespace runsp
