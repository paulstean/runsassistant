#pragma once

// RunEngine: run assembly per specification.md S5.3, cut semantics S5.6,
// emission details S5.8. Plain C++17, NO JUCE, no allocation/locks on call
// paths (caller-driven: the engine knows nothing about MIDI messages or
// processBlock; the processor converts beats to samples and orders events
// per S6.1 in P2). Single namespace runsp. No randomness anywhere.

#include <cstdint>

#include "ScaleModel.h"
#include "Walk.h"
#include "CurveMap.h"
#include "VelocityModel.h"

namespace runsp
{

enum class Direction { Up, Down };

// Plain event struct; caller converts beat positions to sample offsets (S5.1).
struct RunEvent
{
    enum Kind { NoteOn, NoteOff };
    Kind kind = NoteOn;
    int channel = 0;
    int pitch = 0;
    int velocity = 0; // 0 for NoteOff
    double beat = 0.0; // absolute beat position (onset or note-off time)
};

// Fixed-capacity sink owned by the caller. No allocation; overflow is flagged.
struct RunEventSink
{
    RunEvent* data = nullptr;
    int cap = 0;
    int count = 0;
    bool overflow = false;
    void push (const RunEvent& e)
    {
        if (count < cap) data[count++] = e;
        else overflow = true;
    }
};

struct RunParams
{
    int beats = 4;             // 1..16 integer (S4)
    double density = 4.0;      // 1..16 notes/beat (S4/D8)
    double curveStrength = 0.5;// 0..1 (S4)
    double accentStrength = 0.5; // 0..1 (S4)
    double arc = 0.0;          // -1..+1 (S4)
    int tonic = 0;             // 0..11 (S4)
    int mode = 0;              // 0..16, 16 = Custom (S9)
    uint16_t customOffsets = 0;// Custom tick-set relative to tonic (S9)
    WalkMode walk = WalkMode::Fold; // S4/D10
    double gateFraction = 0.6; // S5.8 settings value (clamped 0..1)
    double epsilonBeats = 0.0; // S5.1 alignment epsilon (beats)
    int barNumerator = 4;      // timeSig numerator for bar lines, 0 = none (S5.7)
    // ADDITIVE (P2): S5.1 virtual-clock mode skips alignment (the trigger
    // defines beat 0); ppq mode aligns forward to the next integer beat.
    bool alignToGrid = true;
};

struct PairTrigger
{
    // Unordered pair, already sorted by the caller or snapped inside (S5.3).
    int pitchLo = 0, pitchHi = 0;
    int velLo = 0, velHi = 0;  // velocities paired with their pitches (S3.2)
    Direction direction = Direction::Up; // engine state (D4)
    int channel = 0;
    double triggerBeat = 0.0;  // beat position of the second note-on (S3.2)
};

class RunEngine
{
public:
    struct Counters
    {
        int parityDrops = 0; // zig-zag parity drops (S5.4/S5.8)
        int cuts = 0;        // cuts (S5.6)
        int runsStarted = 0;
        int lastPitch = -1, lastVel = -1;
    };

    // S5.3: start a run. False when a run is already active (S5.6: one run at
    // a time; triggers during an active run are ignored) or the snapped span
    // is degenerate (S3.2: consumed, does not fire).
    bool startRun (const PairTrigger& t, const RunParams& p);

    bool isActive() const { return active_; }
    int channelOfActiveRun() const { return channel_; }

    // Caller-driven scheduler: emit all due run note-ons and gated note-offs
    // whose beat is <= beatNow, in time order (offs before ons on a tie).
    void pumpUpTo (double beatNow, RunEventSink& out);

    // S5.6: cut the active run now: note-offs for still-sounding emitted notes
    // at the cut beat; scheduled-but-unemitted notes are discarded.
    void cut (double beatNow, RunEventSink& out);

    // S5.6: either trigger note released -> cut. Only acts while active and
    // only for the two trigger pitches (raw pair MIDI pitches).
    void onTriggerNoteRelease (int rawPitch, double beatNow, RunEventSink& out);

    // Engine switch to Off: cut any active run (S5.2/S5.6).
    void onEngineOff (double beatNow, RunEventSink& out) { cut (beatNow, out); }

    // Drop the active run silently (test/setup convenience).
    void cancel() { active_ = false; count_ = 0; nextIdx_ = 0; }

    const Counters& counters() const { return counters_; }

    // Debug overlay data (S6.1 step 6 / S9).
    int notesEmitted() const { return nextIdx_; }
    int noteCount() const { return count_; }
    double gateOf (int i) const; // gate in beats; defined for interior notes
    double onsetOf (int i) const { return i >= 0 && i < count_ ? onsets_[i] : 0.0; }
    int pitchOf (int i) const { return i >= 0 && i < count_ ? pitches_[i] : -1; }
    int velocityOf (int i) const { return i >= 0 && i < count_ ? vels_[i] : -1; }
    double startBeat() const { return startBeat_; }

private:
    static constexpr int kMaxNotes = 4096; // S5.3 n clamp
    static constexpr int kMaxSounding = kMaxNotes;

    bool active_ = false;
    int channel_ = 0;
    int trigRawLo_ = -1, trigRawHi_ = -1; // raw trigger pitches for release cuts
    int count_ = 0;      // emitted plan length n (after any parity drop)
    int nextIdx_ = 0;    // next note to emit
    double startBeat_ = 0.0;
    double beats_ = 0.0;
    double gate_ = 0.6;
    double onsets_[kMaxNotes];
    uint8_t pitches_[kMaxNotes];
    uint8_t vels_[kMaxNotes];
    bool offSent_[kMaxNotes];

    Counters counters_;};

} // namespace runsp
