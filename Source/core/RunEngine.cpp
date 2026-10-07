#include "RunEngine.h"

#include <cmath>
#include <cstring>

namespace runsp
{

namespace
{
constexpr double kBeatEps = 1e-9; // float-comparison guard (S6.2 house rule)

inline double weightForBeat (double fromBarOrigin, int barNumerator)
{
    // S5.7: weight = 1.0 on a bar line, 0.75 otherwise. Bar lines are
    // fromBarOrigin %% timeSigNumerator == 0; the caller supplies the beat
    // distance from the bar origin and the time-signature numerator; 0 or
    // absent numerator disables bar detection.
    if (barNumerator <= 0) return runsp::kMidBarWeight;
    const long long b = (long long) std::floor (fromBarOrigin + kBeatEps);
    return (((b % (long long) barNumerator) + (long long) barNumerator)
                % (long long) barNumerator) == 0
               ? runsp::kDownbeatWeight
               : runsp::kMidBarWeight;
}
} // namespace

double RunEngine::gateOf (int i) const
{
    // S5.8: gate = fraction x gap to the next onset, interior notes only;
    // the final note has no gate (tail-end steady, S5.6).
    if (! active_ || i < 0 || i >= count_ - 1) return 0.0;
    return gate_* (onsets_[i + 1] - onsets_[i]);
}

bool RunEngine::startRun (const PairTrigger& t, const RunParams& p)
{
    if (active_) return false; // S5.6: triggers during an active run are ignored

    ScaleModel scale (p.tonic, p.mode, p.customOffsets);

    // S5.3: snap each span endpoint inward; S3.2: degenerate pair (equal after
    // snap) does not fire.
    int spanLo = 0, spanHi = 0;
    if (scale.snapSpan (t.pitchLo, t.pitchHi, &spanLo, &spanHi))
        return false;

    // S5.3: n = clamp(round(density x beats), 1, 4096).
    long long nL = (long long) std::lround (p.density * (double) p.beats);
    if (nL < 1) nL = 1;
    if (nL > kMaxNotes) nL = kMaxNotes;
    int n = (int) nL;

    int spanPitches[130];
    const int L = scale.buildSpan (spanLo, spanHi, spanPitches, 130);
    if (L < 1) return false; // cannot walk an empty scale

    const bool dirUp = t.direction == Direction::Up;
    int walkOut[kMaxNotes];
    // S5.4: walk in run direction; zig-zag may parity-drop n (counted).
    WalkResult wr = buildWalk (spanPitches, L, dirUp, n, p.walk, walkOut);
    count_ = wr.count;
    if (wr.parityDrop) ++counters_.parityDrops;
    if (count_ < 1) return false;

    // S5.1: aligned start beat = next integer beat at or after the trigger; a
    // trigger landing within epsilon after a beat line counts on that line.
    // ADDITIVE (P2): virtual-clock mode skips alignment (trigger is beat 0).
    // S5.1: aligned start beat = next integer beat at or after the trigger; a
    // trigger landing within epsilon after a beat line counts on that line.
    // ADDITIVE (P2): virtual-clock mode skips alignment (trigger is beat 0).
    if (p.alignToGrid)
    {
        const long long base = (long long) std::floor (t.triggerBeat);
        const double frac = t.triggerBeat - (double) base;
        startBeat_ = (frac <= p.epsilonBeats + kBeatEps) ? (double) base
                                                         : (double) (base + 1);
    }
    else
    {
        startBeat_ = t.triggerBeat;
    }
    beats_ = (double) p.beats;

    // S5.3: onsets in beat space, endpoints inclusive.
    const double dn = (double) count_;
    for (int i = 0; i < count_; ++i)
    {
        const double pIdx = count_ > 1 ? (double) i / (dn - 1.0) : 0.0;
        onsets_[i] = beats_ * curveMap (pIdx, p.curveStrength);
    }

    // S5.3: velocities (linear by note index; accent bar weights per S5.7,
    // absolute beat distance computed here from the aligned start beat).
    // S5.7: downbeat weight at bar lines. Bar origin: the host bar start
    // when available (passed via RunParams.hasBarOrigin), else the run's own
    // start beat counts as beat 1 (S5.7).
    const double barOrigin = p.hasBarOrigin ? p.barOriginBeats : startBeat_;
    const double vStart = dirUp ? (double) t.velLo : (double) t.velHi;
    const double vEnd   = dirUp ? (double) t.velHi : (double) t.velLo;
    gate_ = p.gateFraction < 0.0 ? 0.0
            : (p.gateFraction > 1.0 ? 1.0 : p.gateFraction);
    for (int i = 0; i < count_; ++i)
    {
        const double absBeat = startBeat_ + onsets_[i];
        const double f = absBeat - std::floor (absBeat);
        const double d = f < 0.5 ? f : 1.0 - f;
        const double falloff = accentFalloff (d);
        const double w = weightForBeat (absBeat - barOrigin, p.barNumerator);
        pitches_[i] = (uint8_t) walkOut[i];
        vels_[i] = (uint8_t) computeVelocity (vStart, vEnd,
            count_ > 1 ? (double) i / (dn - 1.0) : 0.0,
            p.arc, p.accentStrength, w, falloff);
    }

    active_ = true;
    channel_ = t.channel;
    trigRawLo_ = t.pitchLo;
    trigRawHi_ = t.pitchHi;
    nextIdx_ = 0;
    ++counters_.runsStarted;
    return true;
}

void RunEngine::pumpUpTo (double beatNow, RunEventSink& out)
{
    // Caller-driven emission (S5.1/S6.1 step 5): interleaved in time order,
    // note-off before note-on when both land on the same beat (gate <= gap).
    // All comparisons in absolute beat space (start beat + relative onset).
    while (active_)
    {
        bool progressed = false;

        // Due interior note-off (earliest first).
        for (int j = 0; j < nextIdx_ && ! progressed; ++j)
        {
            if (offSent_[j]) continue;
            if (j == count_ - 1) continue; // tail-end steady: no gate (S5.6)
            const double tOff = startBeat_ + onsets_[j] + gate_ * (onsets_[j + 1] - onsets_[j]);
            if (tOff <= beatNow + kBeatEps)
            {
                RunEvent e;
                e.kind = RunEvent::NoteOff;
                e.channel = channel_;
                e.pitch = pitches_[j];
                e.beat = tOff;
                offSent_[j] = true;
                out.push (e);
                progressed = true;
            }
        }
        if (progressed) continue;

        // Due note-on.
        if (nextIdx_ < count_ && startBeat_ + onsets_[nextIdx_] <= beatNow + kBeatEps)
        {
            RunEvent e;
            e.kind = RunEvent::NoteOn;
            e.channel = channel_;
            e.pitch = pitches_[nextIdx_];
            e.velocity = vels_[nextIdx_];
            e.beat = startBeat_ + onsets_[nextIdx_];
            offSent_[nextIdx_] = false;
            ++nextIdx_;
            counters_.lastPitch = e.pitch;
            counters_.lastVel = e.velocity;
            out.push (e);
            progressed = true;
            continue;
        }
        if (! progressed) break;
    }
}

void RunEngine::cut (double beatNow, RunEventSink& out)
{
    if (! active_) return;
    // S5.6: immediate note-offs for still-sounding emitted notes at the cut
    // offset; scheduled-but-unemitted notes are discarded.
    for (int j = 0; j < nextIdx_; ++j)
    {
        if (offSent_[j]) continue;
        RunEvent e;
        e.kind = RunEvent::NoteOff;
        e.channel = channel_;
        e.pitch = pitches_[j];
        e.beat = beatNow;
        out.push (e);
    }
    active_ = false;
    nextIdx_ = 0;
    ++counters_.cuts;
}

void RunEngine::onTriggerNoteRelease (int rawPitch, double beatNow, RunEventSink& out)
{
    // S5.6: either trigger note release cuts the run.
    if (! active_) return;
    if (rawPitch != trigRawLo_ && rawPitch != trigRawHi_) return;
    cut (beatNow, out);
}

} // namespace runsp
