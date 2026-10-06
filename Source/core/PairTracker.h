#pragma once

// PairTracker: per-MIDI-channel pending/publish/consume state machine,
// specification.md S5.2 exactly. Plain C++17, no JUCE, no allocation.
//
// Per channel (no cross-channel interaction):
//  - note-on with no pending note: buffered as pending (latches beat position
//    for epsilon alignment).
//  - note-on while exactly one pending note exists: pair forms; both notes
//    consumed; returns lower/upper pitch + matched velocities + the pending
//    note's latched offset.
//  - note-off of a pending note before pairing: late publish (note-on at the
//    note-off's position followed immediately by note-off); counted.
//  - note-on while pending exists with the same pitch: publish the pending
//    note normally and pass the new note-on through (no pair).
//  - note-offs of already-paired notes: consumed silently.
//  - flush(): engine-switch to Off: publish pending notes late (S5.2).

#include <cstdint>

namespace runsp
{

struct PairNoteEvent
{
    int pitch = 0;
    int velocity = 0;
    bool isOn = true; // false: note-off passthrough
};

struct PairOutcome
{
    enum Kind
    {
        Nothing,        // consumed silently (paired-note-off, cancels)
        One,            // single event passes through (a)
        LateOnOff,      // late publish: a = note-on, b = note-off at same offset
        PublishAndPass, // same-pitch second note: a = pending publish, b = new note
        Pair            // pair formed: pitchLo/Hi + velocities + pendingBeat
    };
    Kind kind = Nothing;
    PairNoteEvent a;
    PairNoteEvent b;
    bool hasPair = false;
    int pitchLo = 0, pitchHi = 0;
    int velLo = 0, velHi = 0;
    double pendingBeat = 0.0; // pending note's latched beat position (S5.2)
};

class PairTracker
{
public:
    // True when a pending note had to be published late (S5.2 jab case).
    int latePublishes() const { return latePublishes_; }

    // Reset everything (engine state composition; no events produced).
    void resetAll();

    // S5.2 flush: publish all pending notes late (engine switch to Off).
    void flushAll (PairOutcome* outs, int cap, int* publishedCount);

    PairOutcome noteOn (int channel, int pitch, int velocity, double beatPosition);
    PairOutcome noteOff (int channel, int pitch);

private:
    struct ChannelState
    {
        bool hasPending = false;
        int pendPitch = 0, pendVel = 0;
        double pendBeat = 0.0;
        // Consumed pair latches: their note-offs are consumed silently (S5.2),
        // and while still held they occupy the channel (third chord note
        // passes through, S3.2 edge).
        bool pairHeld0 = false, pairHeld1 = false;
        int pairPitch0 = -1, pairPitch1 = -1;
        // Held published notes (emitted passthrough) block further buffering
        // while they sound (S5.2 first bullet: no published-or-pending note).
        int publishedHeld[8];
        int publishedCount = 0;

        void clear()
        {
            hasPending = false;
            pairHeld0 = pairHeld1 = false;
            pairPitch0 = pairPitch1 = -1;
            publishedCount = 0;
        }
        bool hasHeldAnything() const
        {
            return pairHeld0 || pairHeld1 || publishedCount > 0;
        }
        void addPublished (int pitch)
        {
            if (publishedCount < 8)
                publishedHeld[publishedCount++] = pitch;
        }
        bool removePublished (int pitch)
        {
            for (int i = 0; i < publishedCount; ++i)
                if (publishedHeld[i] == pitch)
                {
                    publishedHeld[i] = publishedHeld[--publishedCount];
                    return true;
                }
            return false;
        }
    };

    static constexpr int kNumChannels = 16;
    ChannelState channels_[kNumChannels];
    int latePublishes_ = 0;
};

} // namespace runsp
