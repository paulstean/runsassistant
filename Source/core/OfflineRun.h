#pragma once

// OfflineRun: complete-run rendering for the export path (drag a rendered
// .mid into the DAW arrange window). Plain C++17, NO JUCE, no allocation -
// the caller owns the event buffer. Same drain pattern the conformance tests
// use (start -> pump everything -> cut the held tail note, S5.6 tail-end
// steady), plus a final sort by beat so the caller can write events strictly
// in time order: a single drain reorders interior note-offs in overlap mode
// (an off scheduled past the next note-on is emitted before it), which
// realtime block pumping never exposes.

#include "RunEngine.h"

namespace runsp
{

struct OfflineRunResult
{
    enum Error
    {
        Ok = 0,
        Degenerate = 1, // startRun refused: equal after snap (S3.2) / empty
        Overflow = 2,   // caller buffer too small for the whole run
        BadBuffer = 3
    };
    int error = Ok;
    int eventCount = 0; // events written to the caller's buffer
    int noteCount = 0;  // note-ons among them
    double startBeat = 0.0; // aligned start beat of the run (S5.1)
    double endBeat = 0.0;   // beat of the last event (the tail note-off)
    bool ok() const { return error == Ok; }
};

// Renders one whole run for explicit trigger pitches/velocities into
// `events[0..cap)`. tailBeats is how long the held final note rings past the
// run's last onset before the closing note-off. On failure nothing is
// guaranteed about the buffer contents.
OfflineRunResult renderRunOffline (const PairTrigger& t, const RunParams& p,
                                   double tailBeats, RunEvent* events, int cap);

} // namespace runsp
