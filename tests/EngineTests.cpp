// Engine conformance vectors, specification.md S10 + S3.2/S5.1-S5.8:
// pair detection matrix, epsilon alignment, endpoint snapping, walk modes,
// gate math, cut semantics, velocity stack, tail-end steady.

#include <cmath>
#include <vector>

#include "TestFramework.h"
#include "core/PairTracker.h"
#include "core/RunEngine.h"

using namespace runsp;

namespace
{
struct EvLog
{
    std::vector<RunEvent> v;
    RunEventSink sink;
    EvLog()
    {
        static RunEvent buf[8192];
        sink.data = buf;
        sink.cap = 8192;
    }
    const std::vector<RunEvent>& events()
    {
        v.assign (sink.data, sink.data + sink.count);
        return v;
    }
    void clear() { sink.count = 0; }
};

RunParams basicParams (double density, int beats = 4)
{
    RunParams p;
    p.beats = beats;
    p.density = density;
    p.curveStrength = 0.0; // linear by default in tests
    p.accentStrength = 0.0;
    p.arc = 0.0;
    p.tonic = 0;
    p.mode = 0; // C major
    p.walk = WalkMode::Fold;
    return p;
}

PairTrigger trig (int lo, int hi, double at, Direction dir = Direction::Up,
                  int vLo = 100, int vHi = 100, int channel = 0)
{
    PairTrigger t;
    t.pitchLo = lo;
    t.pitchHi = hi;
    t.velLo = vLo;
    t.velHi = vHi;
    t.direction = dir;
    t.channel = channel;
    t.triggerBeat = at;
    return t;
}

std::vector<int> onPitches (const std::vector<RunEvent>& ev)
{
    std::vector<int> out;
    for (const auto& e : ev)
        if (e.kind == RunEvent::NoteOn) out.push_back (e.pitch);
    return out;
}
int lastOnoffPitch (const std::vector<RunEvent>& ev)
{
    for (int i = (int) ev.size() - 1; i >= 0; --i)
        if (ev[i].kind == RunEvent::NoteOff) return ev[i].pitch;
    return -1;
}
} // namespace

// --------------------------------------------------------------- PairTracker

TEST_CASE (pair_staggered_forms)
{
    PairTracker t;
    auto a = t.noteOn (0, 60, 100, 0.2);
    CHECK_EQ ((int) a.kind, (int) PairOutcome::Nothing);
    auto b = t.noteOn (0, 67, 40, 0.4);
    CHECK_EQ ((int) b.kind, (int) PairOutcome::Pair);
    CHECK (b.hasPair);
    CHECK_EQ (b.pitchLo, 60);
    CHECK_EQ (b.pitchHi, 67);
    CHECK_EQ (b.velLo, 100); // velocities pair with their pitches (S3.2)
    CHECK_EQ (b.velHi, 40);
    CHECK_NEAR (b.pendingBeat, 0.2, 1e-12); // latched offset (S5.2)

    // Note-offs of consumed notes are consumed silently (S5.2).
    CHECK_EQ ((int) t.noteOff (0, 60).kind, (int) PairOutcome::Nothing);
    CHECK_EQ ((int) t.noteOff (0, 67).kind, (int) PairOutcome::Nothing);
    // Channel free again: next note buffers as pending.
    CHECK_EQ ((int) t.noteOn (0, 60, 90, 1.0).kind, (int) PairOutcome::Nothing);
}

TEST_CASE (pair_jab_late_publish)
{
    PairTracker t;
    auto a = t.noteOn (0, 60, 100, 0.2);
    CHECK_EQ ((int) a.kind, (int) PairOutcome::Nothing);
    auto off = t.noteOff (0, 60);
    // S5.2: jab -> late publish: note-on followed immediately by note-off.
    CHECK_EQ ((int) off.kind, (int) PairOutcome::LateOnOff);
    CHECK_EQ (off.a.pitch, 60);
    CHECK_EQ (off.a.velocity, 100);
    CHECK (off.a.isOn && ! off.b.isOn);
    CHECK_EQ (t.latePublishes(), 1);
    // Channel free: next note pairs normally.
    CHECK_EQ ((int) t.noteOn (0, 62, 90, 1.0).kind, (int) PairOutcome::Nothing);
    CHECK (t.noteOn (0, 64, 90, 1.1).hasPair);
}

TEST_CASE (pair_same_pitch_publish_and_pass)
{
    PairTracker t;
    CHECK_EQ ((int) t.noteOn (0, 60, 100, 0.2).kind, (int) PairOutcome::Nothing);
    auto r = t.noteOn (0, 60, 80, 0.5);
    // S5.2: same-pitch second note: publish pending + passthrough, no pair.
    CHECK_EQ ((int) r.kind, (int) PairOutcome::PublishAndPass);
    CHECK_EQ (r.a.pitch, 60);
    CHECK_EQ (r.a.velocity, 100);
    CHECK_EQ (r.b.velocity, 80);
    CHECK (! r.hasPair);
    // Both held notes' offs pass through.
    CHECK_EQ ((int) t.noteOff (0, 60).kind, (int) PairOutcome::One);
    CHECK_EQ ((int) t.noteOff (0, 60).kind, (int) PairOutcome::One);
    // Channel free: a fresh note rebuffers.
    CHECK_EQ ((int) t.noteOn (0, 62, 90, 2.0).kind, (int) PairOutcome::Nothing);
}

TEST_CASE (pair_three_note_chord_third_passes)
{
    PairTracker t;
    CHECK_EQ ((int) t.noteOn (0, 60, 100, 0.0).kind, (int) PairOutcome::Nothing);
    CHECK (t.noteOn (0, 64, 100, 0.0).hasPair); // S3.2: two-note chord fires
    auto third = t.noteOn (0, 67, 100, 0.0);
    // S3.2 edge: third chord note passes through (channel occupied).
    CHECK_EQ ((int) third.kind, (int) PairOutcome::One);
    CHECK_EQ (third.a.pitch, 67);
}

TEST_CASE (pair_channels_independent)
{
    PairTracker t;
    CHECK_EQ ((int) t.noteOn (0, 60, 100, 0.0).kind, (int) PairOutcome::Nothing);
    CHECK_EQ ((int) t.noteOn (1, 62, 100, 0.0).kind, (int) PairOutcome::Nothing);
    auto b = t.noteOn (1, 66, 50, 0.3);
    CHECK (b.hasPair);          // pairs within its own channel (S5.2 per-channel)
    CHECK_EQ (b.pitchLo, 62);
    auto a = t.noteOn (0, 70, 60, 0.4);
    CHECK (a.hasPair);          // ch0 still has its own pending
    CHECK_EQ (a.pitchLo, 60);
    CHECK_EQ (a.velLo, 100);
    CHECK_EQ (a.velHi, 60);
}

TEST_CASE (pair_flush_on_engine_off)
{
    PairTracker t;
    CHECK_EQ ((int) t.noteOn (3, 60, 100, 0.2).kind, (int) PairOutcome::Nothing);
    CHECK_EQ ((int) t.noteOn (1, 62, 90, 0.3).kind, (int) PairOutcome::Nothing);
    PairOutcome outs[16];
    int published = 0;
    t.flushAll (outs, 16, &published); // S5.2: engine Off flushes all channels
    CHECK_EQ (published, 2);
    for (int i = 0; i < published; ++i)
    {
        CHECK_EQ ((int) outs[i].kind, (int) PairOutcome::LateOnOff);
        CHECK (outs[i].a.isOn && ! outs[i].b.isOn);
    }
    CHECK_EQ (outs[0].a.pitch, 62); // channel order: 1 then 3
    CHECK_EQ (outs[1].a.pitch, 60);
    // After flush, a note buffers as pending again.
    CHECK_EQ ((int) t.noteOn (3, 64, 100, 5.0).kind, (int) PairOutcome::Nothing);
}

// ------------------------------------------------------------------ RunEngine

TEST_CASE (engine_epsilon_alignment)
{
    RunEngine e;
    // S5.1: start = next integer beat at/after trigger; a trigger within
    // epsilon after a line counts on that line; beyond epsilon counts next.
    const double cases[][3] =
    {
        { 3.8,  0.05, 4.0 },  // ceil
        { 3.95, 0.05, 4.0 },  // before the line anyway: next integer
        { 4.02, 0.05, 4.0 },  // within epsilon after 4 -> counts on 4
        { 4.10, 0.05, 5.0 },  // beyond epsilon after 4 -> next line
        { 4.0,  0.05, 4.0 },  // exactly on the line
    };
    for (const auto& c : cases)
    {
        e.cancel();
        RunParams p = basicParams (2.0, 2);
        p.epsilonBeats = c[1];
        PairTrigger t = trig (60, 67, c[0]);
        CHECK (e.startRun (t, p));
        EvLog log;
        e.pumpUpTo (c[1] + c[2], log.sink); // pump to the aligned start
        const auto& ev = log.events();
        CHECK_EQ ((int) ev.size(), 1);
        if (! ev.empty())
            CHECK_NEAR (ev[0].beat, c[2], 1e-9);
        e.cancel();
    }
}

TEST_CASE (engine_endpoint_snapping)
{
    RunEngine e;
    RunParams p = basicParams (2.0, 2);
    // S5.3: span 61..64 (C#/E) snaps inward to 60..64 in C major; run fires on
    // the snapped span and the first emitted pitch is the snapped start.
    CHECK (e.startRun (trig (61, 64, 0.0), p));
    EvLog log;
    e.pumpUpTo (2.0, log.sink);
    const auto& ev = log.events();
    // Fold plan for snapped 60..64 (n=4, S=3, D=2): the bounce pull gets
    // trimmed so the penultimate note does not double the target:
    // pitches 60,62,60,64 with interleaved gate offs = 7 events.
    CHECK_EQ ((int) ev.size(), 7);
    const auto ons = onPitches (ev);
    CHECK_EQ (ons.size(), (size_t) 4);
    if (ons.size() == 4)
    {
        CHECK_EQ (ons[0], 60); // snapped inward: C# -> C
        CHECK_EQ (ons[1], 62);
        CHECK_EQ (ons[2], 60);
        CHECK_EQ (ons[3], 64); // E already in scale; lands on snapped hi
    }
    e.cancel();
}

TEST_CASE (engine_simple_scale_run_both_modes_same)
{
    RunEngine e;
    RunParams p = basicParams (2.0, 2); // beats 2, density 2 -> n = 4, S = 3
    int span[16];
    ScaleModel c (0, 0, 0);
    const int L = c.buildSpan (60, 67, span, 16);
    CHECK_EQ (L, 5);
    CHECK (L - 1 >= 3 - 1); // S <= D... (S = 3, D = 4 actually)
    // S4 <= D? S=3 <= D=4: both walk modes produce the same plain run (S5.4).
    EvLog foldLog, zigLog;
    p.walk = WalkMode::Fold;
    CHECK (e.startRun (trig (60, 67, 0.0), p));
    e.pumpUpTo (100.0, foldLog.sink);
    e.cancel();
    p.walk = WalkMode::ZigZag;
    CHECK (e.startRun (trig (60, 67, 0.0), p));
    e.pumpUpTo (100.0, zigLog.sink);
    e.cancel();
    auto pf = onPitches (foldLog.events());
    auto pz = onPitches (zigLog.events());
    CHECK_EQ ((int) pf.size(), 4);
    CHECK_EQ (pf, pz);
    // Endpoint exactness (S5.4) + under-filled late snap to target: the run
    // must end exactly on 67.
    CHECK_EQ (pf.back(), 67);
}

TEST_CASE (engine_fold_overdense)
{
    RunEngine e;
    RunParams p = basicParams (4.0, 4); // n = 16, S = 15; D = 7 (60..72)
    p.walk = WalkMode::Fold;
    CHECK (e.startRun (trig (60, 72, 0.0), p));
    EvLog log;
    e.pumpUpTo (100.0, log.sink);
    auto pf = onPitches (log.events());
    CHECK_EQ ((int) pf.size(), 16);
    CHECK_EQ (pf.front(), 60); // first pitch = start (S5.4)
    CHECK_EQ (pf.back(), 72);  // landing exact on target
    for (size_t i = 1; i < pf.size(); ++i)
        CHECK (pf[i] != pf[i - 1]); // consecutive pitches never repeat (S5.4)
    ScaleModel c (0, 0, 0);
    for (int pitch : pf)
        CHECK (c.inScale (pitch % 12)); // run notes are scale notes
    e.cancel();
}

TEST_CASE (engine_zigzag_overdense_and_parity)
{
    RunEngine e;
    RunParams p = basicParams (4.0, 4); // n = 16, S = 15; D = 7; S-D = 8, b = 4
    p.walk = WalkMode::ZigZag;
    CHECK (e.startRun (trig (60, 72, 0.0), p));
    CHECK_EQ (e.noteCount(), 16); // S-D even: full count
    EvLog log;
    e.pumpUpTo (100.0, log.sink);
    auto pz = onPitches (log.events());
    CHECK_EQ ((int) pz.size(), 16);
    CHECK_EQ (pz.back(), 72); // landing exact (S5.4)
    CHECK_EQ (pz.front(), 60);
    for (size_t i = 1; i < pz.size(); ++i)
        CHECK (pz[i] != pz[i - 1]);
    e.cancel();

    // Parity drop case: S - D odd. n = 15 -> S = 14, S - D = 7 odd -> n = 14.
    RunEngine e2;
    RunParams p2 = basicParams (4.0, 4);
    p2.walk = WalkMode::ZigZag;
    p2.density = 3.5; // round(3.5 * 4) = 14 -> S = 13, S - D = 6 (even, b = 3)
    CHECK (e2.startRun (trig (60, 72, 0.0), p2));
    CHECK_EQ (e2.noteCount(), 14);
    CHECK_EQ (e2.counters().parityDrops, 0);
    e2.cancel();

    // Force an actual parity drop: n = 15 -> S = 14 -> drop to 13.
    RunEngine e3;
    RunParams p3 = basicParams (4.0, 4);
    p3.walk = WalkMode::ZigZag;
    p3.beats = 4;
    p3.density = 3.75; // round(15.0) = 15 -> S = 14, S - D = 7 odd
    CHECK (e3.startRun (trig (60, 72, 0.0), p3));
    CHECK_EQ (e3.noteCount(), 14);
    CHECK_EQ (e3.counters().parityDrops, 1);
    EvLog log3;
    e3.pumpUpTo (100.0, log3.sink);
    auto p3p = onPitches (log3.events());
    CHECK_EQ ((int) p3p.size(), 14);
    CHECK_EQ (p3p.back(), 72);
    for (size_t i = 1; i < p3p.size(); ++i)
        CHECK (p3p[i] != p3p[i - 1]);
    e3.cancel();
}

TEST_CASE (engine_gate_math)
{
    RunEngine e;
    RunParams p = basicParams (2.0, 2); // linear: onsets 0, 2/3, 4/3, 2
    p.gateFraction = 0.6;
    CHECK (e.startRun (trig (60, 67, 0.0), p));
    EvLog log;
    e.pumpUpTo (100.0, log.sink);
    const auto& ev = log.events();
    // off_i = onset_i + 0.6 * (onset_{i+1} - onset_i) for interior notes.
    const double on[4] = { 0.0, 2.0 / 3.0, 4.0 / 3.0, 2.0 };
    int seenOffs = 0;
    for (const auto& e2 : ev)
    {
        if (e2.kind != RunEvent::NoteOff) continue;
        CHECK_EQ (e2.pitch, 60 + 2 * seenOffs); // folded plan: 60,62,64,67
        CHECK_NEAR (e2.beat, on[seenOffs] + 0.6 * (on[seenOffs + 1] - on[seenOffs]), 1e-9);
        ++seenOffs;
    }
    CHECK_EQ (seenOffs, 3); // interior gates only; final held (tail-end steady)
    CHECK_NEAR (e.gateOf (0), 0.6 * (on[1] - on[0]), 1e-12);
    CHECK_NEAR (e.gateOf (1), 0.6 * (on[2] - on[1]), 1e-12);
    CHECK_NEAR (e.gateOf (2), 0.6 * (on[3] - on[2]), 1e-12);
    CHECK_NEAR (e.gateOf (3), 0.0, 1e-12); // final note: no gate
    e.cancel();
}

TEST_CASE (engine_cut_semantics_all_conditions)
{
    // Release cut (S5.6): sounding notes get offs at the cut beat.
    {
        RunEngine e;
        RunParams p = basicParams (2.0, 2);
        CHECK (e.startRun (trig (60, 67, 0.2), p));
        EvLog log;
        e.pumpUpTo (2.5, log.sink); // note 2 emitted and still sounding
        log.clear();
        e.onTriggerNoteRelease (60, 3.0, log.sink); // trigger-note release
        const auto& ev = log.events();
        CHECK (ev.size() >= 1);
        CHECK_EQ (e.isActive(), false);
        CHECK_EQ (e.counters().cuts, 1);
        for (const auto& x : ev)
        {
            CHECK_EQ ((int) x.kind, (int) RunEvent::NoteOff);
            CHECK_NEAR (x.beat, 3.0, 1e-12); // off at the cut offset
        }
        log.clear();
        e.pumpUpTo (100.0, log.sink); // scheduled-not-emitted were discarded
        CHECK_EQ ((int) log.events().size(), 0);
    }
    // Engine-change cut: a trigger note that is not a trigger pitch is a no-op;
    // direct cut() (engine state change / discontinuity / CC123) cuts too.
    {
        RunEngine e;
        RunParams p = basicParams (2.0, 2);
        CHECK (e.startRun (trig (60, 67, 0.2), p));
        EvLog log;
        e.pumpUpTo (2.5, log.sink);
        log.clear();
        e.onTriggerNoteRelease (64, 2.0, log.sink); // not a trigger pitch
        CHECK (e.isActive());
        CHECK_EQ (e.counters().cuts, 0);
        e.cut (4.0, log.sink); // engine change / CC123 / transport seek
        CHECK_EQ (e.isActive(), false);
        CHECK_EQ (e.counters().cuts, 1);
        CHECK ((int) log.events().size() >= 1);
        log.clear();
        e.cut (5.0, log.sink); // no active run: no counter, no events
        CHECK_EQ (e.counters().cuts, 1);
        CHECK_EQ ((int) log.events().size(), 0);
    }
    // One run at a time (S5.6): a trigger while active is ignored.
    {
        RunEngine e;
        RunParams p = basicParams (2.0, 2);
        CHECK (e.startRun (trig (60, 67, 0.0), p));
        CHECK (! e.startRun (trig (48, 72, 1.0), p)); // second engine/channel
        CHECK (e.isActive());
        CHECK_EQ (e.counters().runsStarted, 1);
        e.cancel();
    }
}

TEST_CASE (engine_velocity_stack_integration)
{
    RunEngine e;
    RunParams p = basicParams (2.0, 4); // linear: onsets 0, 4/3, 8/3, 4
    p.beats = 4;
    p.density = 2.0; // n = 8? round(8) = 8... use n = 4 via density 1? keep 4 notes:
    p.density = 1.0; // round(1 * 4) = 4 -> p_i = 0, 1/3, 2/3, 1
    // Absolute beats 0, 4/3, 8/3, 4: d = 0, 1/3, 1/3, 0 -> falloff 1, 1/3, 1/3, 1.
    // barNumerator 4: beats 0 bar (w 1.0), 1 mid (0.75).
    p.accentStrength = 0.25;
    p.arc = 0.0;
    PairTrigger t = trig (60, 72, 0.0, Direction::Up, 100, 100);
    CHECK (t.pitchLo <= t.pitchHi);
    CHECK (e.startRun (t, p));
    EvLog log;
    e.pumpUpTo (100.0, log.sink);
    const auto& ev = onPitches (log.events()); // ons only; offs interleave
    std::vector<int> vs;
    {
        const auto& raw = log.events();
        for (const auto& x : raw)
            if (x.kind == RunEvent::NoteOn) vs.push_back (x.velocity);
    }
    // base = 100 everywhere; accent: beat 0: 1 + .25*1*1 -> 125;
    // beat 4/3: d = 1/3, falloff 1/3, w .75 -> 100 * (1 + .25*.75/3) = 106.25
    //   -> round 106; beat 4: bar -> 125.
    CHECK_EQ ((int) ev.size(), 4);
    if (vs.size() >= 4)
    {
        CHECK_EQ (vs[0], 125);
        const double accentMul1 = 1.0 + 0.25 * 0.75 * (1.0 / 3.0);
        CHECK_EQ (vs[1], (int) (100 * accentMul1 + 0.5));
        CHECK_EQ (vs[2], (int) (100 * accentMul1 + 0.5));
        CHECK_EQ (vs[3], 125);
        CHECK_EQ (e.counters().lastVel, 125);
    }
    e.cancel();
}

TEST_CASE (engine_tail_end_steady)
{
    RunEngine e;
    RunParams p = basicParams (2.0, 2); // n = 4 final note held (S5.6)
    CHECK (e.startRun (trig (60, 67, 0.2), p));
    EvLog log;
    e.pumpUpTo (100.0, log.sink);   // everything due
    auto all = log.events();
    // Final note emitted, no off for the target pitch 67 yet.
    CHECK_EQ ((int) all.size(), 7); // 4 ons + 3 interior offs
    CHECK (lastOnoffPitch (all) != 67);
    log.clear();
    e.pumpUpTo (1000.0, log.sink);
    CHECK_EQ ((int) log.events().size(), 0); // still held, nothing new
    // Trigger release finally cuts the held target note (S5.6 tail).
    e.onTriggerNoteRelease (67, 50.0, log.sink);
    const auto& tail = log.events();
    CHECK_EQ ((int) tail.size(), 1);
    if (tail.size() == 1)
    {
        CHECK_EQ ((int) tail[0].kind, (int) RunEvent::NoteOff);
        CHECK_EQ (tail[0].pitch, 67);
        CHECK_NEAR (tail[0].beat, 50.0, 1e-12);
    }
    CHECK (! e.isActive());
}

// P4 hardening regressions from the live REAPER reports: emission gates at
// high density (S5.5/S5.8: no zero or negative gates at density 16) and the
// accent bar weights (S5.7: host bar origin when available, else the run's
// own start beat is beat 1; custom numerator support).

TEST_CASE (engine_gate_at_high_density)
{
    // Reported with density 16: n = 16 x 16 = 256 notes; the count is full,
    // every interior gate is strictly positive for the practically reachable
    // curve strengths. At k > ~7 the normative power-S map collapses the
    // last couple of adjacent onsets below double precision (and their
    // mirror at the run start), which is a property of S5.5 rather than an
    // emission bug: gaps stay non-negative there.
    for (double curve : { 0.0, 0.25, 0.5, 0.75, 0.9, 1.0 })
    {
        RunEngine e;
        RunParams p = basicParams (16.0, 16);
        p.curveStrength = curve;
        CHECK (e.startRun (trig (60, 72, 0.0), p));
        CHECK_EQ (e.noteCount(), 256);
        for (int i = 0; i + 1 < 256; ++i)
        {
            const double gap = e.onsetOf (i + 1) - e.onsetOf (i);
            CHECK (gap >= 0.0); // monotonic (S5.5)
            CHECK (e.gateOf (i) >= 0.0); // gates never negative (S5.8)
            CHECK (e.gateOf (i) <= 0.6 * gap + 1e-12);
            if (curve <= 0.5)
            {
                CHECK (gap > 0.0);
                CHECK (e.gateOf (i) > 0.0); // no zero gates at density 16
            }
        }
        e.cancel();
    }
}

TEST_CASE (engine_accent_bar_origin_and_weights)
{
    // S5.7: weight = 1.0 on bar lines, 0.75 otherwise, relative to the bar
    // origin (host-provided here); falloff 1 on the line, 0 at half a beat.
    constexpr double wBar = 1.0, wMid = 0.75;
    // Case 1: trigger on beat 8.0, host bar origin at 2.5, numerator 3.
    // startBeat = 8.0 (epsilon 0); n = 8 with linear onsets rel = 8*i/7
    // beats so abs = 8 + 8*i/7. falloff = 1 (all notes on integer-ish...
    // falloff varies): fromBar = abs - 2.5.
    {
        RunEngine e;
        RunParams p = basicParams (1.0, 8);
        p.accentStrength = 0.2;
        p.hasBarOrigin = true;
        p.barOriginBeats = 2.5;
        p.barNumerator = 3;
        CHECK (e.startRun (trig (60, 72, 8.0), p)); // start beat 8.0
        CHECK_EQ (e.noteCount(), 8);
        for (int i = 0; i < 8; ++i)
        {
            const double absBeat = 8.0 + 8.0 * (double) i / 7.0;
            const double fromBar = absBeat - 2.5;
            const bool onBar =
                ((long long) std::floor (fromBar + 1e-9) % 3) == 0;
            const double f = absBeat - std::floor (absBeat);
            const double d = f < 0.5 ? f : 1.0 - f;
            const double falloff = d < 0.5 ? 1.0 - 2.0 * d : 0.0;
            const double w = onBar ? wBar : wMid;
            CHECK_EQ (e.velocityOf (i),
                      (int) std::lround (100.0
                                         * (1.0 + 0.2 * w * falloff)));
        }
        e.cancel();
    }
    // Case 2: no host bar origin -> the run's own start beat is beat 1
    // (S5.7): every numerator-th note from the start is a bar line. Beats 7
    // with density 8/7 -> n = 8 and linear onsets land exactly on integers.
    {
        RunEngine e;
        RunParams p = basicParams (8.0 / 7.0, 7);
        p.accentStrength = 0.2;
        p.hasBarOrigin = false;
        p.barNumerator = 4;
        CHECK (e.startRun (trig (60, 72, 11.3), p)); // start beat 12
        CHECK_EQ (e.noteCount(), 8);
        for (int i = 0; i < 8; ++i)
        {
            const bool onBar = (i % 4) == 0; // rel beat = i
            const double w = onBar ? wBar : wMid;
            CHECK_EQ (e.velocityOf (i),
                      (int) std::lround (100.0 * (1.0 + 0.2 * w)));
        }
        e.cancel();
    }
    // Case 3: falloff: on the line = full weight; half a beat away = zero
    // boost. Beats 4, density 2.25 -> n = 9, linear onsets i/2 beats: odd i
    // sit exactly half a beat off the line. Trigger at 3.75 -> start 4.0.
    {
        RunEngine e;
        RunParams p = basicParams (2.25, 4);
        p.accentStrength = 0.2;
        p.arc = 0.0;
        p.hasBarOrigin = false;
        p.barNumerator = 4;
        CHECK (e.startRun (trig (60, 72, 3.75), p));
        CHECK_EQ (e.noteCount(), 9);
        for (int i = 0; i < 9; ++i)
        {
            if (i % 2)
                CHECK_EQ (e.velocityOf (i), 100); // half-beat: no boost
            else if (i % 8 == 0)
                CHECK_EQ (e.velocityOf (i), 120); // bar line: full weight
            else
                CHECK_EQ (e.velocityOf (i), 115); // mid-bar beat
        }
        e.cancel();
    }
}

