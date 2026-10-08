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

// ------------------------------------------------ engine-off handoff (S3.3)

TEST_CASE (pair_seed_passed_pending_and_handoff)
{
    PairTracker t;
    CHECK (t.seedPassedPending (0, 60, 100, 0.2)); // note sounded live while Off
    // Its off passes through normally: never jabbed, not counted.
    CHECK_EQ ((int) t.noteOff (0, 60).kind, (int) PairOutcome::One);
    CHECK_EQ (t.latePublishes(), 0);
    // ... and the channel is free afterwards.
    CHECK (t.seedPassedPending (0, 62, 90, 0.3));
    auto b = t.noteOn (0, 67, 40, 0.4); // second note-on completes the pair
    CHECK_EQ ((int) b.kind, (int) PairOutcome::Pair);
    CHECK_EQ (b.pendPassedSide, 0);     // the pending (lo) side had passed
    // The passed side's off passes through; the new side's off consumed.
    CHECK_EQ ((int) t.noteOff (0, 62).kind, (int) PairOutcome::One);
    CHECK_EQ ((int) t.noteOff (0, 67).kind, (int) PairOutcome::Nothing);
    CHECK_EQ (t.latePublishes(), 0);
}

TEST_CASE (pair_handoff_pending_survives_flush_and_never_jabs)
{
    PairTracker t;
    CHECK (t.seedPassedPending (0, 60, 100, 0.2));
    // Engine switch (any state): passed-through pendings survive, no jab.
    PairOutcome outs[16];
    int published = 99;
    t.flushAll (outs, 16, &published);
    CHECK_EQ (published, 0);
    auto on = t.noteOn (1, 64, 90, 0.5); // other channel unaffected
    CHECK_EQ ((int) on.kind, (int) PairOutcome::Nothing); // buffers normally
    t.clearAll(); // hub: passed pendings survive clearAll too
    CHECK (t.pendingOnAny (nullptr));
    // A normal engine pending still flushes with a jab as before.
    PairTracker u;
    CHECK_EQ ((int) u.noteOn (0, 62, 80, 0.1).kind, (int) PairOutcome::Nothing);
    PairOutcome outs2[16];
    int published2 = 0;
    u.flushAll (outs2, 16, &published2);
    CHECK_EQ (published2, 1);
    CHECK_EQ ((int) outs2[0].kind, (int) PairOutcome::LateOnOff);
}

TEST_CASE (pair_seed_fails_when_channel_busy)
{
    PairTracker t;
    CHECK_EQ ((int) t.noteOn (0, 60, 100, 0.0).kind, (int) PairOutcome::Nothing);
    CHECK (! t.seedPassedPending (0, 64, 90, 0.1)); // pending occupies
    auto b = t.noteOn (0, 64, 90, 0.2);
    CHECK (b.hasPair);
    CHECK_EQ (b.pendPassedSide, -1); // ordinary pair (S5.2/S3.2)
    // pair-held notes busy the channel too (S5.2 first bullet)
    PairTracker u;
    CHECK_EQ ((int) u.noteOn (0, 60, 100, 0.0).kind, (int) PairOutcome::Nothing);
    CHECK (u.noteOn (0, 64, 90, 0.1).hasPair);
    CHECK (! u.seedPassedPending (0, 67, 100, 0.2));
    // out-of-range channel never seeds
    PairTracker w;
    CHECK (! w.seedPassedPending (16, 60, 100, 0.0));
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
    // S=3 <= D=4: under-filled -> proportional stride map (S5.4): the climb
    // is compressed across the steps (leaps spread through the run), both
    // walk modes identical, ending exactly on the target.
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
    // Under-filled plan (S=3, D=4), stride map: 60,62,65,67.
    const int offPitch[3] = { 60, 62, 65 };
    int seenOffs = 0;
    for (const auto& e2 : ev)
    {
        if (e2.kind != RunEvent::NoteOff) continue;
        CHECK_EQ (e2.pitch, offPitch[seenOffs]);
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
    // every interior gate is strictly positive for every curve strength.
    // (The pre-revision 5.5 map collapsed adjacent onsets below double
    // precision at k > ~7; the revised inverted map has bounded slopes in
    // the interior, S13.11.)
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
            CHECK (gap > 0.0);
            CHECK (e.gateOf (i) >= 0.0); // gates never negative (S5.8)
            CHECK (e.gateOf (i) <= 0.6 * gap + 1e-12);
        }
        e.cancel();
    }
}

TEST_CASE (engine_legato_overlap_gates)
{
    // S5.8 overlap toggle: each interior note-off lands strictly AFTER the
    // next note-on and strictly BEFORE the note-on after that; the last
    // interior note's off lands past the target onset (own-gap overhang).
    // Linear onsets (curve 0, n = 4, 2 beats): rel onsets 0, 2/3, 4/3, 2.
    RunEngine e;
    RunParams p = basicParams (2.0, 2);
    p.noteOverlap = true;
    CHECK (e.startRun (trig (60, 67, 0.0), p));
    EvLog log;
    e.pumpUpTo (100.0, log.sink);
    auto all = log.events();

    // onsets: 0, 2/3, 4/3, 2. Offs: 0.6 of the following gap past the next
    // onset for j <= n-3, own-gap overhang for j = n-2:
    //   off0 = 2/3 + 0.6*(4/3-2/3) = 2/3 + 0.4
    //   off1 = 4/3 + 0.6*(2-4/3)   = 4/3 + 0.4
    //   off2 = 2   + 0.6*(2-4/3)   = 2   + 0.4 (past the target onset)
    CHECK_EQ ((int) all.size(), 7); // 4 ons + 3 interior offs
    std::vector<RunEvent> ons, offs;
    for (const auto& ev : all)
        (ev.kind == RunEvent::NoteOn ? ons : offs).push_back (ev);
    CHECK_EQ ((int) ons.size(), 4);
    CHECK_EQ ((int) offs.size(), 3);
    if (ons.size() == 4 && offs.size() == 3)
    {
        CHECK_NEAR (offs[0].beat, 2.0 / 3.0 + 0.6 * 2.0 / 3.0, 1e-9);
        CHECK_NEAR (offs[1].beat, 4.0 / 3.0 + 0.6 * 2.0 / 3.0, 1e-9);
        CHECK_NEAR (offs[2].beat, 2.0 + 0.6 * 2.0 / 3.0, 1e-9);
        // strict overlap: off_i after on_{i+1}, before on_{i+2}
        CHECK (offs[0].beat > ons[1].beat && offs[0].beat < ons[2].beat);
        CHECK (offs[1].beat > ons[2].beat && offs[1].beat < ons[3].beat);
        CHECK (offs[2].beat > ons[3].beat);
    }
    // gateOf reports the actual (overlapping) gate
    CHECK (e.gateOf (0) > e.onsetOf (1) - e.onsetOf (0));
    e.cancel();
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


// S5.9 humanization: seeded velocity deflection + onset jitter. The engine
// must be exactly reproducible per seed, bounded, order-preserving, and
// inert (byte-for-byte the deterministic plan) when humanize is off.
namespace
{
struct Plan
{
    std::vector<int> vels;
    std::vector<double> onsets;
};

Plan capturePlan (const RunEngine& e)
{
    Plan plan;
    for (int i = 0; i < e.noteCount(); ++i)
    {
        plan.vels.push_back (e.velocityOf (i));
        plan.onsets.push_back (e.onsetOf (i));
    }
    return plan;
}

RunParams humanizedParams (double amt, double timing, uint32_t seed)
{
    RunParams p = basicParams (8.0, 4); // n = 32 notes
    p.humanize = true;
    p.humanizeSeed = seed;
    p.humanizeVelAmt = amt;
    p.humanizeTimingBeats = timing;
    return p;
}
} // namespace

TEST_CASE (engine_humanize_off_is_inert)
{
    // Humanize off: the extra RunParams fields must be ignored entirely,
    // even when they carry wild strengths (the processor passes 0 amounts
    // only for tidiness - off means off regardless).
    RunEngine base, off;
    RunParams p0 = basicParams (8.0, 4);
    RunParams p1 = basicParams (8.0, 4);
    p1.humanize = false;
    p1.humanizeSeed = 123456u;
    p1.humanizeVelAmt = 0.5;
    p1.humanizeTimingBeats = 0.2;
    CHECK (base.startRun (trig (60, 72, 2.0), p0));
    CHECK (off.startRun (trig (60, 72, 2.0), p1));
    CHECK_EQ (base.noteCount(), off.noteCount());
    const Plan a = capturePlan (base), b = capturePlan (off);
    CHECK (a.vels == b.vels);
    CHECK (a.onsets == b.onsets);
    CHECK_EQ (off.seedOfLastRun(), 0u); // seed reported only when humanize ran
    base.cancel();
    off.cancel();
}

TEST_CASE (engine_humanize_same_seed_reproduces)
{
    const RunParams p = humanizedParams (0.3, 0.1, 424242u);
    RunEngine e1, e2;
    CHECK (e1.startRun (trig (60, 72, 2.0), p));
    CHECK (e2.startRun (trig (60, 72, 2.0), p));
    CHECK_EQ (e1.noteCount(), e2.noteCount());
    const Plan a = capturePlan (e1), b = capturePlan (e2);
    CHECK (a.vels == b.vels);
    CHECK (a.onsets == b.onsets);
    CHECK_EQ (e1.seedOfLastRun(), 424242u);
    // The humanized plan must actually differ from the deterministic one,
    // otherwise the test proves nothing.
    RunEngine d;
    RunParams p0 = basicParams (8.0, 4);
    CHECK (d.startRun (trig (60, 72, 2.0), p0));
    const Plan plain = capturePlan (d);
    CHECK (a.vels != plain.vels);
    bool moved = false;
    for (int i = 0; i < (int) a.onsets.size(); ++i)
        if (std::fabs (a.onsets[(size_t) i] - plain.onsets[(size_t) i]) > 1e-9)
            moved = true;
    CHECK (moved);
    e1.cancel(); e2.cancel(); d.cancel();
}

TEST_CASE (engine_humanize_diff_seed_varies)
{
    // Two seeds over the same run must not produce the same plan (the
    // draws feed both the velocity and the timing stream).
    const Plan a = [] {
        RunEngine e;
        CHECK (e.startRun (trig (60, 72, 2.0), humanizedParams (0.4, 0.1, 1u)));
        const Plan p = capturePlan (e);
        e.cancel();
        return p;
    }();
    const Plan b = [] {
        RunEngine e;
        CHECK (e.startRun (trig (60, 72, 2.0),
                           humanizedParams (0.4, 0.1, 999983u)));
        const Plan p = capturePlan (e);
        e.cancel();
        return p;
    }();
    CHECK (a.vels != b.vels);
    CHECK (a.onsets != b.onsets);
}

TEST_CASE (engine_humanize_bounds_and_order)
{
    // Velocity: deflected values stay inside 1..127 even at extreme base
    // velocities and the maximum 50 percent deflection. Timing: every
    // onset stays inside [0, beats] and the plan stays non-decreasing, for
    // both curve ends (the order fix-up is what protects the S5.8
    // off-before-on contract).
    for (double curve : { 0.0, 1.0 })
    {
        for (uint32_t seed : { 7u, 20260101u, 900001u })
        {
            RunEngine e;
            RunParams p = humanizedParams (0.5, 0.25, seed);
            p.curveStrength = curve;
            CHECK (e.startRun (trig (60, 72, 1.0), p));
            double prev = -1.0;
            for (int i = 0; i < e.noteCount(); ++i)
            {
                const int v = e.velocityOf (i);
                CHECK (v >= 1 && v <= 127);
                const double o = e.onsetOf (i);
                CHECK (o >= 0.0 && o <= (double) p.beats);
                CHECK (o >= prev); // note order never inverts
                prev = o;
            }
            e.cancel();
        }
    }
    // Boundary bases: velocity 1 (deflection floor) and 127 (ceiling).
    for (int edge : { 1, 127 })
    {
        RunEngine e;
        RunParams p = humanizedParams (0.5, 0.0, 777u);
        CHECK (e.startRun (trig (60, 72, 0.0, Direction::Up, edge, edge), p));
        for (int i = 0; i < e.noteCount(); ++i)
        {
            const int v = e.velocityOf (i);
            CHECK (v >= 1 && v <= 127);
        }
        e.cancel();
    }
}
