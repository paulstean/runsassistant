#include "PairTracker.h"

#include <cstring>

namespace runsp
{

void PairTracker::resetAll()
{
    for (int c = 0; c < kNumChannels; ++c)
        channels_[c].clear();
    latePublishes_ = 0;
}

void PairTracker::clearAll()
{
    for (int c = 0; c < kNumChannels; ++c)
        channels_[c].clear();
    // latePublishes_ intentionally kept (session diagnostics, S5.8).
}

void PairTracker::flushAll (PairOutcome* outs, int cap, int* publishedCount)
{
    // S5.2: engine switch to Off: flush all pending notes (publish late).
    if (publishedCount != nullptr) *publishedCount = 0;
    for (int c = 0; c < kNumChannels; ++c)
    {
        ChannelState& ch = channels_[c];
        if (! ch.hasPending) continue;
        ch.hasPending = false;
        if (outs != nullptr && cap > 0 && *publishedCount < cap)
        {
            PairOutcome o;
            o.kind = PairOutcome::LateOnOff;
            o.channel = c; // tracker channel index (P2 routing)
            o.a = { ch.pendPitch, ch.pendVel, true };
            o.b = { ch.pendPitch, ch.pendVel, false };
            outs[(*publishedCount)++] = o;
        }
    }
}

PairOutcome PairTracker::noteOn (int channel, int pitch, int velocity, double beatPosition)
{
    PairOutcome o;
    if (channel < 0 || channel >= kNumChannels)
    {
        o.kind = PairOutcome::One;
        o.a = { pitch, velocity, true };
        return o;
    }
    ChannelState& ch = channels_[channel];

    if (ch.hasPending)
    {
        if (pitch == ch.pendPitch)
        {
            // S5.2: same pitch: publish pending normally, pass new note through.
            o.kind = PairOutcome::PublishAndPass;
            o.a = { ch.pendPitch, ch.pendVel, true };
            o.b = { pitch, velocity, true };
            o.pendingBeat = ch.pendBeat; // latch for late offset restore (P2)
            ch.addPublished (ch.pendPitch);
            ch.addPublished (pitch);
            ch.hasPending = false;
        }
        else
        {
            // S5.2/S3.2: pair forms; both consumed. Velocities pair with their
            // pitches (S3.2); pending latched offset returned for alignment.
            o.kind = PairOutcome::Pair;
            o.hasPair = true;
            o.pendingBeat = ch.pendBeat;
            if (pitch < ch.pendPitch)
            {
                o.pitchLo = pitch;          o.velLo = velocity;
                o.pitchHi = ch.pendPitch;   o.velHi = ch.pendVel;
            }
            else
            {
                o.pitchLo = ch.pendPitch;   o.velLo = ch.pendVel;
                o.pitchHi = pitch;          o.velHi = velocity;
            }
            ch.pairHeld0 = ch.pairHeld1 = true;
            ch.pairPitch0 = ch.pendPitch;
            ch.pairPitch1 = pitch;
            ch.hasPending = false;
        }
        return o;
    }

    // No pending note: buffer as pending only when the channel is completely
    // free (S5.2 first bullet: no published-or-pending note). Held consumed
    // pair notes (three-note chord edge, S3.2) or published notes (melodic
    // second note, S5.2 fifth bullet) pass through instead.
    if (ch.hasHeldAnything())
    {
        o.kind = PairOutcome::One;
        o.a = { pitch, velocity, true };
        ch.addPublished (pitch);
        return o;
    }

    ch.hasPending = true;
    ch.pendPitch = pitch;
    ch.pendVel = velocity;
    ch.pendBeat = beatPosition; // latch for epsilon alignment (S5.2)
    return o;
}

PairOutcome PairTracker::noteOff (int channel, int pitch)
{
    PairOutcome o;
    if (channel < 0 || channel >= kNumChannels)
    {
        o.kind = PairOutcome::One;
        o.a = { pitch, 0, false };
        return o;
    }
    ChannelState& ch = channels_[channel];

    if (ch.hasPending && pitch == ch.pendPitch)
    {
        // S5.2: jabbed a lone note: late publish at the note-off's position,
        // counted and surfaced in the debug overlay.
        ++latePublishes_;
        o.kind = PairOutcome::LateOnOff;
        o.a = { ch.pendPitch, ch.pendVel, true };
        o.b = { ch.pendPitch, ch.pendVel, false };
        ch.hasPending = false;
        return o;
    }

    if (ch.pairHeld0 && pitch == ch.pairPitch0)
    {
        // S5.2: note-offs of consumed pair notes are consumed silently.
        ch.pairHeld0 = false;
        return o;
    }
    if (ch.pairHeld1 && pitch == ch.pairPitch1)
    {
        ch.pairHeld1 = false;
        return o;
    }

    if (ch.removePublished (pitch))
    {
        // Emitted passthrough note ends: pass its note-off through normally.
        o.kind = PairOutcome::One;
        o.a = { pitch, 0, false };
        return o;
    }

    // Default: pass through (S5.8 invariant, no event dropped silently).
    o.kind = PairOutcome::One;
    o.a = { pitch, 0, false };
    return o;
}

} // namespace runsp
