// Runs Assistant - live-session invariant harness (plan.md P4 hardening,
// specification.md S10). Drives the REAL RunsProcessor::processBlock with a
// stub playhead across long, deterministic, randomized sessions and asserts
// global invariants on the emitted MIDI stream:
//
//   (a) engine Off: output byte-identical to input (offsets + bytes)
//   (b) runs: no consecutive identical pitches; first pitch in-scale; last
//       emitted pitch == pair target (completed runs); interior onset
//       spacing matches beats x curve_map(i/(n-1)) relative to the run's
//       start beat; onsets monotonic in time; emitted note count per run ==
//       n or n-1 with the delta == parityDrops counter delta
//   (c) velocities: integers 1..127; curve=accent=arc=0 -> pure base fade
//       values within +/-1 rounding
//   (d) note integrity: interior note-offs at onset + gate_fraction x gap;
//       final target closes only via a cut; cuts close all sounding notes;
//       counter consistency (cuts == mirror cuts, latePublishes == jabs,
//       inputEventCount == pushed count, drops == 0)
//   (e) publishedEngineState in 0..2; mid-session engine switches cut the
//       active run (or none was active)
//
// The harness keeps an independent mirror of the normative S6.1 pipeline
// (its own transport/grid rules per S5.1 and the S5.3-S5.8 curve/velocity/
// gate formulas; pitch walk + scale snapping reuse the JUCE-free core
// classes with their own vector tests). The REAL emitted stream is compared
// per block against the mirror as per-sample BYTE multisets. Failure output
// names the seed, the invariant and a compact event dump. Deterministic.

#include "../Source/processor/PluginProcessor.h"
#include "../Source/core/ScaleModel.h"
#include "../Source/core/Walk.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <tuple>
#include <vector>

// ============================================================================
// splitmix64 PRNG (deterministic; no deps)
// ============================================================================
namespace rix {

struct Prng
{
    uint64_t state;
    explicit Prng (uint64_t seed = 0x9E3779B97F4A7C15ull) : state (seed) {}

    static inline uint64_t advance (uint64_t& s)
    {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    uint64_t next() { return advance (state); }

    double unit() { return (double) (next() >> 11) / 9007199254740992.0; }
    double range (double lo, double hi) { return lo + unit() * (hi - lo); }
    int irange (int lo, int hi)
    {
        return lo + (int) (next() % (uint64_t) (hi - lo + 1));
    }
    bool chance (double p) { return unit() < p; }
};

} // namespace rix

// ============================================================================
// failure helpers
// ============================================================================
namespace {

using namespace runsp; // core engine types (ScaleModel, Walk, Direction, ...)

int failures = 0;
unsigned long long gSeed = 0;
std::string gInv = "session";

void fail (const char* detail)
{
    ++failures;
    std::printf ("FAIL seed=%llu invariant=%s: %s\n",
                 (unsigned long long) gSeed, gInv.c_str(), detail);
}

void failLL (const char* what, long long got, long long want)
{
    ++failures;
    std::printf ("FAIL seed=%llu invariant=%s: %s got=%lld want=%lld\n",
                 (unsigned long long) gSeed, gInv.c_str(), what, got, want);
}

void failNear (const char* what, double got, double want, double eps)
{
    ++failures;
    std::printf ("FAIL seed=%llu invariant=%s: %s got=%f want=%f (eps %g)\n",
                 (unsigned long long) gSeed, gInv.c_str(), what, got, want,
                 eps);
}

#define CHECK(cond) do { if (! (cond)) fail (#cond); } while (0)
#define CHECK_EQ(a, b) do { \
    if (! ((a) == (b))) failLL (#a " == " #b, (long long) (a), (long long) (b)); \
} while (0)
#define CHECK_NEQ(a, b) do { \
    if ((a) == (b)) failLL (#a " != " #b, (long long) (a), (long long) (b)); \
} while (0)
#define CHECK_NEAR(a, b, eps) do { \
    double _a = (double) (a), _b = (double) (b); \
    if (! (std::fabs (_a - _b) <= (eps))) \
        failNear (#a " ~= " #b, _a, _b, (double) (eps)); \
} while (0)

// ============================================================================
// Test playhead (same pattern as ProcessorTests StubPlayHead)
// ============================================================================

class StubPlayHead : public juce::AudioPlayHead
{
public:
    double ppq = 0.0;
    double bpm = 120.0;
    bool playing = true;
    int tsNum = 4, tsDen = 4;

    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
    {
        juce::AudioPlayHead::PositionInfo pos;
        pos.setPpqPosition (ppq);
        pos.setBpm (bpm);
        pos.setIsPlaying (playing);
        pos.setTimeSignature (juce::AudioPlayHead::TimeSignature
                              { (int) tsNum, (int) tsDen });
        return pos;
    }
};

// ============================================================================
// Byte-level per-sample event identity
// ============================================================================

using EvKey = std::tuple<int, int, int, int>; // sample, b0, b1, b2
using EvMultiset = std::map<EvKey, int>;

struct MirrorEv
{
    int sample = 0;
    int b0 = 0, b1 = -1, b2 = -1;
};

void addBytes (EvMultiset& m, int sample, const unsigned char* b, int n)
{
    ++m[std::make_tuple (sample, (int) b[0], n > 1 ? (int) b[1] : -1,
                         n > 2 ? (int) b[2] : -1)];
}

void addMirror (EvMultiset& m, const MirrorEv& e)
{
    ++m[std::make_tuple (e.sample, e.b0, e.b1, e.b2)];
}

void dumpDiff (const char* what, const EvMultiset& a, const EvMultiset& b)
{
    int printed = 0;
    for (const auto& kv : a)
    {
        auto it = b.find (kv.first);
        const int other = it != b.end() ? it->second : 0;
        if (other == kv.second) continue;
        char buf[160];
        std::snprintf (buf, sizeof (buf),
                       "  %s s%d b[%02X %02X %02X] x%d (other %d)\n", what,
                       std::get<0> (kv.first), std::get<1> (kv.first),
                       std::get<2> (kv.first), std::get<3> (kv.first),
                       kv.second, other);
        std::printf ("%s", buf);
        if (++printed >= 8) { std::printf ("  ...\n"); break; }
    }
}

void multisetsMatch (const EvMultiset& mirror, const EvMultiset& actual)
{
    EvMultiset onlyM, onlyA;
    for (const auto& kv : mirror)
    {
        auto it = actual.find (kv.first);
        const int other = it != actual.end() ? it->second : 0;
        if (other < kv.second) onlyM[kv.first] = kv.second - other;
    }
    for (const auto& kv : actual)
    {
        auto it = mirror.find (kv.first);
        const int other = it != mirror.end() ? it->second : 0;
        if (other < kv.second) onlyA[kv.first] = kv.second - other;
    }
    if (onlyM.empty() && onlyA.empty()) return;
    ++failures;
    std::printf ("FAIL seed=%llu invariant=%s: emitted-stream mismatch\n",
                 (unsigned long long) gSeed, gInv.c_str());
    dumpDiff ("actual-only", onlyA, onlyM);
    dumpDiff ("mirror-only", onlyM, onlyA);
}

// ============================================================================
// Mirror clock: rules of PlayheadAdapter (S5.1/S5.6/S13.9) re-implemented
// ============================================================================

struct MirrorClock
{
    bool hadPpq = false;
    double lastB0 = 0.0, lastB1 = 0.0;
    double virtualBeat = 0.0;

    bool ppqPlaying = false;
    bool discont = false;
    bool beatSpaceChanged = false;
    double curB0 = 0.0, curB1 = 0.0;
    double bpb = 0.0;
    double bpm = 120.0;
    double sampleRate = 48000.0;
    int blockSamples = 0;

    void update (double ppq, double bpmIn, bool playing, bool haveInfo,
                 int blockSamplesIn, double runBeatLen)
    {
        blockSamples = blockSamplesIn;
        bpm = (haveInfo && bpmIn > 0.0) ? bpmIn : 120.0;
        bpb = (blockSamples > 0 && sampleRate > 0.0)
                  ? (double) blockSamples * bpm / (60.0 * sampleRate)
                  : 0.0;

        const bool nowPpq = haveInfo && playing && ppq >= 0.0;
        discont = false;
        beatSpaceChanged = false;

        if (nowPpq)
        {
            if (hadPpq)
            {
                curB0 = ppq;
                curB1 = curB0 + bpb;
                if (curB0 < lastB0 - 1e-3)
                    discont = true;
                else if (curB0 - lastB1
                             > (runBeatLen > 0.0 ? runBeatLen : 0.0) + 1e-3)
                    discont = true;
            }
            else
            {
                curB0 = ppq;
                curB1 = curB0 + bpb;
                beatSpaceChanged = true;
            }
            hadPpq = true;
            lastB0 = curB0;
            lastB1 = curB1;
            ppqPlaying = true;
        }
        else
        {
            ppqPlaying = false;
            beatSpaceChanged = hadPpq;
            curB0 = virtualBeat;
            curB1 = curB0 + bpb;
            virtualBeat = curB1;
            hadPpq = false;
            lastB0 = curB0;
            lastB1 = curB1;
        }
    }
};

// ============================================================================
// Independent spec formulas (S5.5 curve, S5.3/S5.7 velocity stack)
// ============================================================================

inline double curveFormula (double p, double curveStrength)
{
    if (p <= 0.0) return 0.0;
    if (p >= 1.0) return 1.0;
    if (curveStrength <= 0.0) return p;
    const double k = 1.0 + 9.0 * curveStrength;
    const double r = (1.0 - p) / p;
    return 1.0 / (1.0 + std::pow (r, k));
}

inline int velocityFormula (double vStart, double vEnd, double pIdx,
                            double arc, double accent, double weight,
                            double falloff)
{
    const double base = vStart + (vEnd - vStart) * pIdx;
    const double arcMul = 1.0 + arc * pIdx;
    const double accentMul = 1.0 + accent * weight * falloff;
    long v = std::lround (base * arcMul * accentMul);
    if (v < 1) v = 1;
    if (v > 127) v = 127;
    return (int) v;
}

// ============================================================================
// Mirror parameter snapshot (replica of live[] + the D13 CC mapping)
// ============================================================================

struct MirrorParams
{
    int beats = 4;
    double density = 4.0;
    double curve = 0.5;
    double accent = 0.5;
    double arc = 0.0;
    int tonic = 0;
    int mode = 0;
    uint16_t customOffsets = 0x0AB5;
    bool zigzag = false;
    double gateFrac = 0.6;
    double epsilonBeats = 0.0;
    int barNumerator = 4;
    bool hasBarOrigin = false;
    double barOrigin = 0.0;
    bool alignToGrid = true;
};

// Injection: raw MIDI bytes at a block-relative sample.
struct Inj
{
    int sample = 0;
    int n = 3;
    unsigned char b[3] = { 0x90, 60, 100 };
};

Inj mkNoteOn (int sample, int ch, int pitch, int vel)
{
    Inj e;
    e.sample = sample;
    e.b[0] = (unsigned char) (0x90 | (ch - 1));
    e.b[1] = (unsigned char) juce::jlimit (0, 127, pitch);
    e.b[2] = (unsigned char) juce::jlimit (1, 127, vel);
    return e;
}

Inj mkNoteOff (int sample, int ch, int pitch)
{
    Inj e;
    e.sample = sample;
    e.b[0] = (unsigned char) (0x80 | (ch - 1));
    e.b[1] = (unsigned char) juce::jlimit (0, 127, pitch);
    e.b[2] = 0;
    return e;
}

Inj mkCC (int sample, int ch, int cc, int val)
{
    Inj e;
    e.sample = sample;
    e.b[0] = (unsigned char) (0xB0 | (ch - 1));
    e.b[1] = (unsigned char) juce::jlimit (0, 127, cc);
    e.b[2] = (unsigned char) juce::jlimit (0, 127, val);
    return e;
}

Inj mkPC (int sample, int ch, int program)
{
    Inj e;
    e.sample = sample;
    e.n = 2;
    e.b[0] = (unsigned char) (0xC0 | (ch - 1));
    e.b[1] = (unsigned char) juce::jlimit (0, 127, program);
    e.b[2] = 0;
    return e;
}

// ============================================================================
// Mirror session: per-session state + the normative S6.1 pipeline
// ============================================================================

struct RunRecord
{
    bool dirUp = true;
    int channel = 0;
    int trigLo = -1, trigHi = -1;
    int plannedN = 0, count = 0;
    bool parityDrop = false;
    bool completed = false;
    double startBeat = 0.0, beats = 4.0, gateFrac = 0.6;
    double curve = 0.0, accent = 0.0, arc = 0.0;
    int barNumerator = 4;
    double barOrigin = 0.0;
    int tonic = 0, mode = 0;
    uint16_t customOffsets = 0x0AB5;
    int vStart = 100, vEnd = 100;
    int snappedLo = -1, snappedHi = -1;
    std::vector<double> relOnsets;
    std::vector<int> pitches, vels;
};

class MirrorSession
{
public:
    runsp::PairTracker pairs;
    MirrorClock clock;
    std::vector<RunRecord> runs;

    int engineState = 0;
    int grace = 0;
    int pendingParamEngine = -1;
    std::vector<int> engineQueue;
    long long cuts = 0;
    long long parityDrops = 0;

    float liveBeats = 4.0f, liveDensity = 4.0f, liveCurve = 0.5f;
    float liveAccent = 0.5f, liveArc = 0.0f;
    float liveTonic = 0.0f, liveMode = 0.0f, liveWalk = 0.0f;
    uint16_t customOffsets = 0x0AB5;

    int prepareBlockSamples = 512;
    double sampleRate = 48000.0;
    int engineSourceType = 1;
    int engineNumber = 87;
    int engineNoteNumbers[3] = { 12, 13, 14 };
    int enginePcNumbers[3] = { 0, 1, 2 };
    int boundCC[8] = { 88, 89, 90, 91, 92, 93, 94, 95 };

    double stubPpq = 0.0, stubBpm = 120.0;
    bool stubPlaying = true;

    double curB0 = 0.0, curB1 = 0.0, spb = 0.0;
    int blockSamples = 0;
    unsigned long long blockIndex = 0;
    int tsNum = 4;

    std::vector<MirrorEv> expect;
    std::vector<std::pair<int, float>> pendingSets; // (paramIndex, realValue)

    int runNext = -1;
    int liveNextOn = 0;
    std::vector<char> liveOffSent;
    long long jabLates = 0;

    double beatOfSample (int sample) const
    {
        return curB0 + (double) sample / spb;
    }

    int conv (double beat) const
    {
        if (blockSamples <= 0 || spb <= 0.0) return 0;
        int s = (int) std::floor ((beat - curB0) * spb + 1e-9);
        return std::min (std::max (s, 0), blockSamples - 1);
    }

    bool engineOn() const { return engineState != 0; }
    bool runActive() const { return runNext >= 0; }

    int beatsNow() const
    {
        return (int) std::lround (juce::jlimit (1.0f, 16.0f, liveBeats));
    }

    MirrorParams runParams() const
    {
        MirrorParams p;
        p.beats = beatsNow();
        p.density = juce::jlimit (1.0, 16.0, (double) liveDensity);
        p.curve = juce::jlimit (0.0, 1.0, (double) liveCurve);
        p.accent = juce::jlimit (0.0, 1.0, (double) liveAccent);
        p.arc = juce::jlimit (-1.0, 1.0, (double) liveArc);
        p.tonic = (int) std::lround (juce::jlimit (0.0f, 11.0f, liveTonic));
        p.mode = (int) std::lround (juce::jlimit (0.0f, 16.0f, liveMode));
        p.customOffsets = customOffsets;
        p.zigzag = liveWalk >= 0.5f;
        p.gateFrac = 0.6;                       // settings untouched
        p.epsilonBeats = 2.0 * 0.001 * clock.bpm / 60.0; // 2 ms in beats
        p.barNumerator = tsNum;                 // stub time signature (S5.7)
        // S5.7 + JUCE conversion caveat (identical to PlayheadAdapter):
        // the bar origin applies in ppq mode only (bar start 0); the
        // virtual clock anchors bars on the run's own start beat.
        p.hasBarOrigin = clock.ppqPlaying;
        p.barOrigin = 0.0;
        p.alignToGrid = clock.ppqPlaying;
        return p;
    }

    void cutRunAt (int sample)
    {
        if (runNext < 0) return;
        const RunRecord& r = runs[(size_t) runNext];
        for (int j = 0; j < liveNextOn; ++j)
            if (! liveOffSent[(size_t) j])
            {
                MirrorEv e;
                e.sample = sample;
                e.b0 = 0x80 | (r.channel - 1);
                e.b1 = r.pitches[(size_t) j];
                e.b2 = 0;
                expect.push_back (e);
            }
        runNext = -1;
        ++cuts;
    }

    void doChange (int newState, int sampleOffset, int cutSample)
    {
        if (newState < 0 || newState > 2 || newState == engineState) return;
        cutRunAt (cutSample);
        runsp::PairOutcome outs[16];
        int flushed = 0;
        pairs.flushAll (outs, 16, &flushed);
        for (int k = 0; k < flushed; ++k)
        {
            const int ch = outs[k].channel + 1;
            MirrorEv e;
            e.sample = sampleOffset;
            e.b0 = 0x90 | (ch - 1);
            e.b1 = outs[k].a.pitch;
            e.b2 = outs[k].a.velocity;
            expect.push_back (e);
            e.b0 = 0x80 | (ch - 1);
            e.b2 = 0;
            expect.push_back (e);
        }
        pairs.clearAll();
        engineState = newState;
        grace = juce::jlimit (4, 480, (int) (0.5 * sampleRate
                                            / (double) std::max (
                                                1, prepareBlockSamples)));
    }

    void pumpTo (double deadline)
    {
        if (runNext < 0) return;
        RunRecord& r = runs[(size_t) runNext];
        while (true)
        {
            bool progressed = false;
            for (int j = 0; j < liveNextOn && ! progressed; ++j)
            {
                if (liveOffSent[(size_t) j]) continue;
                if (j == r.count - 1) continue; // tail-end steady (S5.6)
                const double tOff = r.startBeat + r.relOnsets[(size_t) j]
                                    + r.gateFrac
                                          * (r.relOnsets[(size_t) j + 1]
                                             - r.relOnsets[(size_t) j]);
                if (tOff <= deadline + 1e-9)
                {
                    MirrorEv e;
                    e.sample = conv (tOff);
                    e.b0 = 0x80 | (r.channel - 1);
                    e.b1 = r.pitches[(size_t) j];
                    e.b2 = 0;
                    expect.push_back (e);
                    liveOffSent[(size_t) j] = 1;
                    progressed = true;
                }
            }
            if (progressed) continue;
            if (liveNextOn < r.count
                && r.startBeat + r.relOnsets[(size_t) liveNextOn]
                       <= deadline + 1e-9)
            {
                MirrorEv e;
                e.sample = conv (r.startBeat
                                 + r.relOnsets[(size_t) liveNextOn]);
                e.b0 = 0x90 | (r.channel - 1);
                e.b1 = r.pitches[(size_t) liveNextOn];
                e.b2 = r.vels[(size_t) liveNextOn];
                expect.push_back (e);
                liveOffSent[(size_t) liveNextOn] = 0;
                ++liveNextOn;
                progressed = true;
                continue;
            }
            break;
        }
        if (liveNextOn >= r.count)
            runs[(size_t) runNext].completed = true;
    }

    bool startRun (const runsp::PairOutcome& o, double triggerBeat,
                   Direction dir)
    {
        const MirrorParams p = runParams();
        RunRecord rec;
        ScaleModel scale (p.tonic, p.mode, p.customOffsets);
        int spanLo = 0, spanHi = 0;
        if (scale.snapSpan (o.pitchLo, o.pitchHi, &spanLo, &spanHi))
            return false; // degenerate pair (S3.2)

        long long nL = (long long) std::lround (p.density * (double) p.beats);
        if (nL < 1) nL = 1;
        if (nL > 4096) nL = 4096;
        rec.plannedN = (int) nL;

        int spanPitches[130];
        const int L = scale.buildSpan (spanLo, spanHi, spanPitches, 130);
        if (L < 1) return false;

        const bool dirUp = dir == Direction::Up;
        int walkOut[4096];
        WalkResult wr = buildWalk (spanPitches, L, dirUp, rec.plannedN,
                                   p.zigzag ? WalkMode::ZigZag
                                            : WalkMode::Fold,
                                   walkOut);
        if (wr.count < 1) return false;
        rec.count = wr.count;
        rec.parityDrop = wr.parityDrop;
        if (wr.parityDrop) ++parityDrops;

        if (p.alignToGrid)
        {
            const long long base = (long long) std::floor (triggerBeat);
            const double frac = triggerBeat - (double) base;
            rec.startBeat = (frac <= p.epsilonBeats + 1e-9)
                                ? (double) base
                                : (double) (base + 1);
        }
        else
        {
            rec.startBeat = triggerBeat; // virtual clock skips alignment
        }
        rec.beats = (double) p.beats;
        rec.gateFrac = 0.6;
        rec.barNumerator = p.barNumerator;
        rec.barOrigin = p.hasBarOrigin ? p.barOrigin : rec.startBeat;
        rec.dirUp = dirUp;
        rec.channel = 1; // pair channel (harness pairs live on channel 1)
        rec.trigLo = o.pitchLo;
        rec.trigHi = o.pitchHi;
        rec.snappedLo = spanLo;
        rec.snappedHi = spanHi;
        rec.curve = p.curve;
        rec.accent = p.accent;
        rec.arc = p.arc;
        rec.tonic = p.tonic;
        rec.mode = p.mode;
        rec.customOffsets = p.customOffsets;
        rec.vStart = dirUp ? o.velLo : o.velHi;
        rec.vEnd = dirUp ? o.velHi : o.velLo;

        for (int i = 0; i < rec.count; ++i)
        {
            const double pIdx = rec.count > 1
                                    ? (double) i / (rec.count - 1.0)
                                    : 0.0;
            rec.relOnsets.push_back (rec.beats
                                     * curveFormula (pIdx, p.curve));
            rec.pitches.push_back (walkOut[i]);
        }
        for (int i = 0; i < rec.count; ++i)
        {
            const double pIdx = rec.count > 1
                                    ? (double) i / (rec.count - 1.0)
                                    : 0.0;
            const double absBeat = rec.startBeat + rec.relOnsets[(size_t) i];
            const double f = absBeat - std::floor (absBeat);
            const double d = f < 0.5 ? f : 1.0 - f;
            const double falloff = d < 0.5 ? 1.0 - 2.0 * d : 0.0;
            double w = 0.75;
            if (rec.barNumerator > 0)
            {
                const double fromBar = absBeat - rec.barOrigin;
                const long long bb = (long long) std::floor (fromBar + 1e-9);
                w = (((bb % rec.barNumerator) + rec.barNumerator)
                         % rec.barNumerator) == 0
                        ? 1.0
                        : 0.75;
            }
            rec.vels.push_back (velocityFormula (rec.vStart, rec.vEnd, pIdx,
                                                 rec.arc, rec.accent, w,
                                                 falloff));
        }

        runs.push_back (rec);
        runNext = (int) runs.size() - 1;
        liveNextOn = 0;
        liveOffSent.assign ((size_t) rec.count, 0);
        return true;
    }

    // Exact replica of applyBoundCc (D13) for the mirror's parameters.
    void applyBoundCc (int binding, int ccValue)
    {
        const int paramIndex = binding + 1;
        const double c = (double) ccValue;
        float real = 0.0f;
        switch (binding)
        {
            case 0:
                real = (float) juce::jlimit (1, 16,
                                             (int) std::lround (1.0 + c / 127.0 * 15.0));
                break;
            case 1: real = (float) (1.0 + c / 127.0 * 15.0); break;
            case 2: real = (float) (c / 127.0); break;
            case 3: real = (float) (c / 127.0); break;
            case 4: real = (float) (c / 127.0 * 2.0 - 1.0); break;
            case 5: real = (float) ((ccValue * 12) / 128); break;
            case 6: real = (float) ((ccValue * 17) / 128); break;
            case 7: real = ccValue < 64 ? 0.0f : 1.0f; break;
            default: return;
        }
        static const bool discrete[8] =
            { true, false, false, false, false, true, true, true };
        if (discrete[binding])
            real = (float) juce::jlimit (
                (int) realMinOf (paramIndex), (int) realMaxOf (paramIndex),
                (int) std::lround (real));
        else
            real = juce::jlimit (realMinOf (paramIndex),
                                 realMaxOf (paramIndex), real);
        switch (paramIndex)
        {
            case 1: liveBeats = real; break;
            case 2: liveDensity = real; break;
            case 3: liveCurve = real; break;
            case 4: liveAccent = real; break;
            case 5: liveArc = real; break;
            case 6: liveTonic = real; break;
            case 7: liveMode = real; break;
            case 8: liveWalk = real; break;
            default: break;
        }
    }

    static float realMinOf (int i)
    {
        static const float m[9] = { 0, 1, 1, 0, 0, -1, 0, 0, 0 };
        return m[(size_t) i];
    }
    static float realMaxOf (int i)
    {
        static const float m[9] = { 2, 16, 16, 1, 1, 1, 11, 16, 1 };
        return m[(size_t) i];
    }

    // The normative S6.1 pipeline over this block's injections.
    void processBlock (const std::vector<Inj>& inj)
    {
        expect.clear();

        // pending parameter edits apply at block start (syncLiveValues)
        for (const auto& ps : pendingSets)
        {
            if (ps.first == 0)
            {
                if (pendingParamEngine < 0)
                    pendingParamEngine = (int) std::lround (ps.second);
            }
            else
            {
                switch (ps.first)
                {
                    case 1: liveBeats = ps.second; break;
                    case 2: liveDensity = ps.second; break;
                    case 3: liveCurve = ps.second; break;
                    case 4: liveAccent = ps.second; break;
                    case 5: liveArc = ps.second; break;
                    case 6: liveTonic = ps.second; break;
                    case 7: liveMode = ps.second; break;
                    case 8: liveWalk = ps.second; break;
                    default: break;
                }
            }
        }
        pendingSets.clear();

        const double runBeatLen = runActive() ? (double) beatsNow() : 0.0;
        clock.update (stubPpq, stubBpm, stubPlaying, true, blockSamples,
                      runBeatLen);
        curB0 = clock.curB0;
        curB1 = clock.curB1;
        spb = (clock.bpm > 0.0) ? (60.0 * sampleRate / clock.bpm) : 0.0;
        blockSamples = clock.blockSamples;

        if (clock.discont && runActive())
            cutRunAt (0);
        if (clock.beatSpaceChanged && runActive())
            cutRunAt (0);

        // grace decay + param-origin engine change (one-shot detection
        // behind the echo grace window - mirrors syncLiveValues exactly)
        const int graceBefore = grace;
        if (graceBefore > 0)
            grace = graceBefore - 1;
        if (pendingParamEngine >= 0)
        {
            if (graceBefore == 0 && pendingParamEngine != engineState)
                doChange (pendingParamEngine, 0, 0);
            pendingParamEngine = -1; // detection consumed (normSeen cache)
        }
        for (size_t qi = 0; qi < engineQueue.size(); ++qi)
        {
            const int q = engineQueue[qi];
            if (q >= 0 && q <= 2 && q != engineState)
                doChange (q, 0, 0);
        }
        engineQueue.clear();

        const int srcType = juce::jlimit (0, 2, engineSourceType);
        int engineCurrent = engineState;
        std::vector<char> consumed (inj.size(), 0);

        // pass 1: engine-switch events in stream order (D14)
        for (size_t i = 0; i < inj.size(); ++i)
        {
            const juce::MidiMessage m (inj[i].b, inj[i].n);
            bool isEngineEvent = false;
            int newState = -1;
            if (srcType == 0)
            {
                if (m.isNoteOn())
                    for (int k = 0; k < 3; ++k)
                        if (m.getNoteNumber()
                            == (juce::uint8) engineNoteNumbers[k])
                        {
                            if (engineCurrent != 0)
                            {
                                isEngineEvent = true;
                                newState = k;
                            }
                            break;
                        }
            }
            else if (srcType == 1)
            {
                if (m.isController()
                    && m.getControllerNumber() == (juce::uint8) engineNumber)
                {
                    isEngineEvent = true;
                    const int v = m.getControllerValue();
                    if (v <= 2) newState = v;
                }
            }
            else if (m.isProgramChange())
            {
                for (int k = 0; k < 3; ++k)
                    if (m.getProgramChangeNumber()
                        == (juce::uint8) enginePcNumbers[k])
                    {
                        isEngineEvent = true;
                        newState = k;
                        break;
                    }
            }
            if (! isEngineEvent) continue;
            consumed[i] = 1;
            if (newState >= 0 && newState != engineCurrent)
                doChange (newState, inj[i].sample, inj[i].sample);
            engineCurrent = engineState;
        }

        const bool engineOn = engineCurrent != 0;

        // pass 2: pair mechanics, bound CCs, passthrough (S5.2)
        for (size_t i = 0; i < inj.size(); ++i)
        {
            if (consumed[i]) continue;
            const juce::MidiMessage m (inj[i].b, inj[i].n);
            const int ch = m.getChannel();
            const int chIdx = ch - 1;
            const int sample = inj[i].sample;

            auto pb = [&] (bool on, int pitch, int vel, int offs)
            {
                MirrorEv e;
                e.sample = offs;
                e.b0 = (on ? 0x90 : 0x80) | (ch - 1);
                e.b1 = juce::jlimit (0, 127, pitch);
                e.b2 = on ? juce::jlimit (1, 127, vel) : 0;
                expect.push_back (e);
            };
            auto pushRaw = [&]
            {
                MirrorEv e;
                e.sample = sample;
                e.b0 = (int) inj[i].b[0];
                e.b1 = inj[i].n > 1 ? (int) inj[i].b[1] : -1;
                e.b2 = inj[i].n > 2 ? (int) inj[i].b[2] : -1;
                expect.push_back (e);
            };

            if (engineOn && m.isNoteOn())
            {
                const double beat = beatOfSample (sample);
                const auto o =
                    pairs.noteOn (chIdx, m.getNoteNumber(), m.getVelocity(),
                                  beat);
                switch (o.kind)
                {
                    case runsp::PairOutcome::One:
                        pb (true, m.getNoteNumber(), m.getVelocity(), sample);
                        break;
                    case runsp::PairOutcome::PublishAndPass:
                    {
                        // S5.2: pending published; new note passes through.
                        int pendSample = sample;
                        if (o.pendingBeat >= curB0)
                            pendSample = conv (o.pendingBeat);
                        pb (true, o.a.pitch, o.a.velocity, pendSample);
                        pb (true, m.getNoteNumber(), m.getVelocity(), sample);
                        break;
                    }
                    case runsp::PairOutcome::LateOnOff:
                        pb (true, o.a.pitch, o.a.velocity, sample);
                        pb (false, o.a.pitch, o.a.velocity, sample);
                        break;
                    case runsp::PairOutcome::Pair:
                    {
                        const Direction dir = engineCurrent == 1
                                                  ? Direction::Up
                                                  : Direction::Down;
                        if (runActive())
                        {
                            // one-run rule: consumption compensated by late
                            // publishes of both notes (S5.6/S13.8)
                            pb (true, o.pitchLo, o.velLo, sample);
                            pb (false, o.pitchLo, o.velLo, sample);
                            pb (true, o.pitchHi, o.velHi, sample);
                            pb (false, o.pitchHi, o.velHi, sample);
                        }
                        else if (startRun (o, beat, dir))
                        {
                            // plan built; pump emits
                        }
                        else
                        {
                            // degenerate after snapping (S13.8)
                            pb (true, o.pitchLo, o.velLo, sample);
                            pb (false, o.pitchLo, o.velLo, sample);
                            pb (true, o.pitchHi, o.velHi, sample);
                            pb (false, o.pitchHi, o.velHi, sample);
                        }
                        break;
                    }
                    default: break; // Nothing: consumed silently (S5.2)
                }
                continue;
            }

            if (engineOn && m.isNoteOff())
            {
                bool isKeyswitchOff = false;
                if (srcType == 0)
                    for (int k = 0; k < 3; ++k)
                        if (m.getNoteNumber()
                            == (juce::uint8) engineNoteNumbers[k])
                            isKeyswitchOff = true;
                if (! isKeyswitchOff)
                {
                    if (runActive())
                    {
                        const RunRecord& r = runs[(size_t) runNext];
                        if (m.getNoteNumber() == (juce::uint8) r.trigLo
                            || m.getNoteNumber()
                                   == (juce::uint8) r.trigHi)
                            cutRunAt (sample); // S5.6 release cut
                    }
                    const auto o =
                        pairs.noteOff (chIdx, m.getNoteNumber());
                    switch (o.kind)
                    {
                        case runsp::PairOutcome::One:
                            pb (false, m.getNoteNumber(), m.getVelocity(),
                                sample);
                            break;
                        case runsp::PairOutcome::LateOnOff:
                            pb (true, o.a.pitch, o.a.velocity, sample);
                            pb (false, o.a.pitch, o.a.velocity, sample);
                            ++jabLates;
                            break;
                        default: break; // consumed silently (S5.2)
                    }
                }
                continue;
            }

            if (engineOn && m.isController())
            {
                const int cc = m.getControllerNumber();
                if (cc == 123)
                {
                    cutRunAt (sample); // S5.6: all-notes-off cuts
                    pushRaw();         // and passes through (S13.6)
                    continue;
                }
                bool isBound = false;
                for (int b = 0; b < 8; ++b)
                    if (boundCC[b] == cc)
                    {
                        applyBoundCc (b, m.getControllerValue());
                        isBound = true; // absorbed (S4/S11)
                        break;
                    }
                if (isBound) continue;
                pushRaw();
                continue;
            }

            pushRaw(); // PC / bend / engine-off passthrough (S3.3/S5.2)
        }

        pumpTo (curB1);
    }
};

// ============================================================================
// Structural invariants over emitted runs
// ============================================================================

void checkRun (const RunRecord& r)
{
    gInv = "run-invariants";
    CHECK (r.count >= 1);
    CHECK_EQ (r.count, r.parityDrop ? r.plannedN - 1 : r.plannedN);

    for (int i = 1; i < r.count; ++i)
        CHECK_NEQ (r.pitches[(size_t) i], r.pitches[(size_t) i - 1]);

    ScaleModel sc (r.tonic, r.mode, r.customOffsets);
    CHECK (sc.inScale (r.pitches[0] % 12));
    for (int i = 0; i < r.count; ++i)
        CHECK (sc.inScale (r.pitches[(size_t) i] % 12));

    if (r.completed && r.count >= 1)
        CHECK_EQ (r.pitches.back(), r.dirUp ? r.snappedHi : r.snappedLo);

    for (int i = 1; i < r.count; ++i)
    {
        // S5.5: the curve map is monotonic; at extreme curve x density
        // (k = 10, n > 100) adjacent onsets collapse below double
        // precision, so the assembled spacing is non-decreasing by
        // construction (the per-block comparison verifies each emitted
        // sample offset exactly). Gates stay non-negative per note.
        CHECK (r.relOnsets[(size_t) i] >= r.relOnsets[(size_t) i - 1]);
        CHECK_NEAR (r.relOnsets[(size_t) i],
                    r.beats
                        * curveFormula (((double) i / (double) (r.count - 1)),
                                        r.curve),
                    1e-6);
    }

    for (int i = 0; i < r.count; ++i)
    {
        const int v = r.vels[(size_t) i];
        CHECK (v >= 1 && v <= 127);
        if (r.accent == 0.0 && r.arc == 0.0)
        {
            const double pIdx = r.count > 1 ? (double) i / (r.count - 1.0)
                                            : 0.0;
            double fade = r.vStart + (r.vEnd - r.vStart) * pIdx;
            long lv = std::lround (fade);
            if (lv < 1) lv = 1;
            if (lv > 127) lv = 127;
            CHECK (std::abs (v - (int) lv) <= 1);
        }
    }

    for (int i = 0; i + 1 < r.count; ++i)
    {
        const double gap = r.relOnsets[(size_t) i + 1]
                           - r.relOnsets[(size_t) i];
        CHECK (gap >= 0.0);              // monotonic (S5.5)
        CHECK (r.gateFrac * gap >= 0.0); // gates never negative (S5.8)
    }
}

// ============================================================================
// Session driver
// ============================================================================

struct HeldNote
{
    int midiCh = 1;
    int pitch = 0;
    int dueBlock = 0;
};

long long totalRuns = 0, totalCuts = 0, totalJobs = 0;

void runSession (unsigned long long seed, bool engineOnScenario)
{
    gSeed = seed;
    rix::Prng rng (seed * 6364136223846793005ull + 7777);
    gInv = "setup";

    const double sr = rng.chance (0.5) ? 44100.0 : 48000.0;
    const int bss[5] = { 256, 480, 512, 1024, 2048 };
    int blockSize = bss[rng.irange (0, 4)];
    double bpm = rng.range (60.0, 200.0);
    int tsNum = rng.irange (1, 7);
    bool playing = true;
    double ppq = rng.range (0.0, 8.0);

    RunsProcessor proc;
    proc.settings.engineSourceType =
        engineOnScenario ? (rng.chance (0.35) ? rng.irange (0, 2) : 1) : 1;
    if (engineOnScenario)
    {
        if (rng.chance (0.25))
            proc.settings.engineNumber = rng.irange (0, 127);
        if (proc.settings.engineSourceType == 0)
            for (int k = 0; k < 3; ++k)
                proc.settings.engineNoteNumbers[k] = 24 + rng.irange (0, 11);
        if (proc.settings.engineSourceType == 2)
            for (int k = 0; k < 3; ++k)
                proc.settings.enginePcNumbers[k] = rng.irange (0, 9);
    }
    proc.prepareToPlay (sr, blockSize);

    MirrorSession M;
    M.sampleRate = sr;
    M.clock.sampleRate = sr;
    M.prepareBlockSamples = blockSize;
    M.engineSourceType = proc.settings.engineSourceType;
    M.engineNumber = proc.settings.engineNumber;
    for (int k = 0; k < 3; ++k)
    {
        M.engineNoteNumbers[k] = proc.settings.engineNoteNumbers[k];
        M.enginePcNumbers[k] = proc.settings.enginePcNumbers[k];
    }

    StubPlayHead stub;
    stub.ppq = ppq;
    stub.bpm = bpm;
    stub.playing = playing;
    stub.tsNum = tsNum;
    stub.tsDen = 4;
    proc.setPlayHead (&stub);

    std::vector<HeldNote> held;
    long long injectedEvents = 0;
    bool jabChannelUsed[8] = {};
    int jabActiveUntil = -1;
    bool jabWaitingOff = false;
    int jabMidiCh = -1, jabPitch = -1, jabOffDue = -1;

    const int blocks = rng.irange (150, 400);
    int stopFor = 0;
    runsp::UiMessage lastU {};

    for (int blkIdx = 0; blkIdx < blocks; ++blkIdx)
    {
        // ---- transport rolls (tempo / blocksize / wrap / seek / stop) -----
        if (blkIdx > 0)
        {
            if (stopFor > 0)
            {
                --stopFor;
                playing = false;
            }
            else if (! playing)
            {
                playing = true; // resume
                ppq = rng.range (0.0, 8.0);
            }
            else
            {
                if (rng.chance (0.06))
                {
                    playing = false;
                    stopFor = rng.irange (1, 5);
                }
                else if (playing && rng.chance (0.08))
                    ppq = std::max (0.0, ppq - rng.range (1.0, 8.0)); // wrap
                else if (playing && rng.chance (0.08))
                    ppq += rng.range (0.5, 6.0); // seek forward
                if (rng.chance (0.18)) bpm = rng.range (60.0, 200.0);
                if (rng.chance (0.10)) tsNum = rng.irange (1, 7);
                if (rng.chance (0.15)) blockSize = bss[rng.irange (0, 4)];
            }
        }
        stub.ppq = ppq;
        stub.bpm = bpm;
        stub.playing = playing;
        stub.tsNum = tsNum;
        M.stubPpq = ppq;
        M.stubBpm = bpm;
        M.stubPlaying = playing;
        M.tsNum = tsNum;
        M.blockSamples = blockSize;
        M.blockIndex = blkIdx;

        // ---- traffic -------------------------------------------------------
        std::vector<Inj> inj;
        auto add = [&] (const Inj& e)
        {
            Inj x = e;
            x.sample = juce::jlimit (0, std::max (1, blockSize) - 1,
                                     e.sample);
            inj.push_back (x);
        };

        // held-note releases (pairs, chords, same-pitch, keyswitch-ons)
        for (size_t h = held.size(); h-- > 0;)
        {
            if (held[h].dueBlock <= blkIdx)
            {
                add (mkNoteOff (rng.irange (0, std::max (0, blockSize - 1)),
                                held[h].midiCh, (int) held[h].pitch));
                held.erase (held.begin() + (long) h);
            }
        }

        // jab note-off landing (guaranteed LateOnOff: no switches in flight)
        if (jabWaitingOff && blkIdx >= jabOffDue)
        {
            CHECK (M.engineState != 0); // no switch traffic during a jab
            add (mkNoteOff (rng.irange (0, std::max (0, blockSize - 1)),
                            jabMidiCh, jabPitch));
            jabWaitingOff = false;
            jabChannelUsed[jabMidiCh - 2] = false;
            jabPitch = -1;
            jabMidiCh = -1;
            jabOffDue = -1;
        }

        int nEv = rng.irange (0, 3)
                  + (rng.chance (0.15) ? rng.irange (1, 3) : 0);
        bool switchInjectedThisBlock = false;
        for (int t = 0; t < nEv; ++t)
        {
            const int r = rng.irange (0, 99);
            const int sample = rng.irange (0, std::max (0, blockSize - 1));

            if (r < 30) // trigger pair (rapid re-pairs emerge naturally)
            {
                int p1 = rng.irange (48, 83), p2 = p1;
                while (p2 == p1) p2 = rng.irange (48, 83);
                const int gap = rng.chance (0.5)
                                    ? 0
                                    : std::min (std::max (0, blockSize - 1
                                                                   - sample),
                                                rng.irange (1, 64));
                add (mkNoteOn (sample, 1, p1, rng.irange (40, 120)));
                add (mkNoteOn (sample + gap, 1, p2, rng.irange (40, 120)));
                held.push_back ({ 1, p1, blkIdx + rng.irange (0, 20) });
                held.push_back ({ 1, p2, blkIdx + rng.irange (0, 20) });
            }
            else if (r < 38 && engineOnScenario && M.engineState != 0
                     && ! switchInjectedThisBlock
                     && blkIdx > jabActiveUntil) // jab on a fresh channel
            {
                int jc = -1;
                for (int cand = 0; cand < 8; ++cand)
                    if (! jabChannelUsed[cand])
                    {
                        jc = cand;
                        break;
                    }
                if (jc >= 0)
                {
                    jabChannelUsed[jc] = true;
                    jabMidiCh = 2 + jc;
                    jabPitch = rng.irange (48, 83);
                    jabOffDue = blkIdx + rng.irange (1, 3);
                    jabActiveUntil = jabOffDue + 2;
                    jabWaitingOff = true; // no switches while jab in flight
                    add (mkNoteOn (sample, jabMidiCh, jabPitch,
                                   rng.irange (40, 110)));
                }
            }
            else if (r < 46) // three-note chord (third note passes)
            {
                int p1 = rng.irange (48, 83), p2 = p1, p3 = p1;
                while (p2 == p1) p2 = rng.irange (48, 83);
                while (p3 == p1 || p3 == p2) p3 = rng.irange (48, 83);
                add (mkNoteOn (sample, 1, p1, 90));
                add (mkNoteOn (sample, 1, p2, 90));
                add (mkNoteOn (sample, 1, p3, 90));
                held.push_back ({ 1, p1, blkIdx + rng.irange (0, 20) });
                held.push_back ({ 1, p2, blkIdx + rng.irange (0, 20) });
                held.push_back ({ 1, p3, blkIdx + rng.irange (0, 20) });
            }
            else if (r < 52) // same-pitch double note-on
            {
                const int p = rng.irange (48, 83);
                add (mkNoteOn (sample, 1, p, 80));
                add (mkNoteOn (sample + 1, 1, p, 70));
                held.push_back ({ 1, p, blkIdx + rng.irange (0, 20) });
                held.push_back ({ 1, p, blkIdx + rng.irange (0, 20) });
            }
            else if (r < 68 && engineOnScenario
                     && M.engineSourceType == 1
                     && blkIdx > jabActiveUntil
                     && ! switchInjectedThisBlock) // engine CC traffic
            {
                const int v = rng.chance (0.8) ? rng.irange (0, 2)
                                               : rng.irange (3, 127);
                if (v <= 2) switchInjectedThisBlock = true;
                add (mkCC (sample, 1, M.engineNumber, v));
            }
            else if (r < 74 && engineOnScenario
                     && M.engineSourceType == 0
                     && blkIdx > jabActiveUntil
                     && ! switchInjectedThisBlock) // keyswitch traffic
            {
                const int k = rng.irange (0, 2);
                switchInjectedThisBlock = true;
                add (mkNoteOn (sample, 1, M.engineNoteNumbers[k], 100));
            }
            else if (r < 80 && engineOnScenario
                     && M.engineSourceType == 2
                     && blkIdx > jabActiveUntil
                     && ! switchInjectedThisBlock) // PC engine traffic
            {
                const int k = rng.chance (0.8) ? rng.irange (0, 2)
                                               : rng.irange (3, 9);
                if (k <= 2) switchInjectedThisBlock = true;
                add (mkPC (sample, 1, k <= 2 ? M.enginePcNumbers[k] : k));
            }
            else if (r < 90) // bound CC (absorbed engine-on, inert passthrough)
            {
                const int ccNum = 88 + rng.irange (0, 7);
                if (ccNum != M.engineNumber) // engine CC has its own branch
                    add (mkCC (sample, 1, ccNum, rng.irange (0, 127)));
            }
            else if (rng.chance (0.5)) // noise CC (never an engine number)
            {
                int cn = rng.irange (100, 119);
                if (cn == M.engineNumber) cn = cn < 119 ? cn + 1 : cn - 1;
                add (mkCC (sample, 8 + rng.irange (0, 8), cn,
                           rng.irange (0, 127)));
            }
            else // noise PC (never a keyswitch PC number)
            {
                add (mkPC (sample, 8 + rng.irange (0, 8),
                           10 + rng.irange (0, 109)));
            }
        }

        // engine switches: queue path (GUI). The host-parameter path is
        // covered deterministically by ProcessorTests (testEngineChainParamPath
        // + testEngineEchoIgnoredDuringGrace); in randomized sessions its
        // message-thread detection timing is not reproducible.
        if (engineOnScenario && blkIdx > jabActiveUntil
            && ! switchInjectedThisBlock)
        {
            if (rng.chance (0.04))
            {
                const int s = rng.irange (0, 2);
                proc.requestEngineState (s);
                M.engineQueue.push_back (s);
            }
        }
        // parameter edits (never the engine param here)
        if (rng.chance (0.08))
        {
            static const char* ids[8] = { "beats", "density", "curve",
                "accent", "arc", "tonic", "mode", "walk" };
            const int which = rng.irange (0, 7);
            float v = 0.0f;
            switch (which)
            {
                case 0: v = (float) rng.irange (1, 16); break;
                case 1: v = (float) rng.range (1.0, 16.0); break;
                case 2: v = (float) rng.range (0.0, 1.0); break;
                case 3: v = (float) rng.range (0.0, 1.0); break;
                case 4: v = (float) rng.range (-1.0, 1.0); break;
                case 5: v = (float) rng.irange (0, 11); break;
                case 6: v = (float) rng.irange (0, 16); break;
                case 7: v = (float) rng.irange (0, 1); break;
            }
            auto* e = proc.apvts.getParameter (ids[which]);
            e->setValueNotifyingHost (e->convertTo0to1 (v));
            M.pendingSets.push_back ({ which + 1, v });
        }
        // engine source type changes
        if (engineOnScenario && blkIdx > jabActiveUntil && rng.chance (0.02))
        {
            const int t = rng.irange (0, 2);
            if (t != M.engineSourceType)
            {
                proc.settings.engineSourceType = t;
                M.engineSourceType = t;
            }
        }

        std::stable_sort (inj.begin(), inj.end(),
                          [] (const Inj& a, const Inj& b)
                          { return a.sample < b.sample; });
        injectedEvents += (long long) inj.size();

        // ---- mirror + real processBlock -------------------------------------
        M.processBlock (inj);

        juce::MidiBuffer midi;
        for (const auto& e : inj)
            midi.addEvent (e.b, e.n, e.sample);
        juce::AudioBuffer<float> audio (2, blockSize);
        proc.processBlock (audio, midi);

        // ---- per-block comparisons ------------------------------------------
        gInv = "stream-match";
        if (engineOnScenario)
        {
            EvMultiset ms, as;
            for (const auto& e : M.expect)
                addMirror (ms, e);
            for (const auto& e : midi)
                addBytes (as, e.samplePosition, e.data, e.numBytes);
            multisetsMatch (ms, as);
        }
        else
        {
            // (a) engine Off: byte-identical passthrough
            EvMultiset is, as;
            for (const auto& e : inj)
                addBytes (is, e.sample, e.b, e.n);
            for (const auto& e : midi)
                addBytes (as, e.samplePosition, e.data, e.numBytes);
            multisetsMatch (is, as);
        }

        gInv = "engine-state-range";
        const int pub = proc.publishedEngineState.load();
        CHECK (pub >= 0 && pub <= 2);
        CHECK_EQ (pub, M.engineState);

        proc.applyPendingCcMirrors();
        lastU = proc.latestUiState(); // editor-Timer-style FIFO drain

        // advance the host position while playing (S5.1)
        if (playing)
            ppq += (double) blockSize * bpm / (60.0 * sr);
    }

    // ---- session-end counters + invariants ---------------------------------
    gInv = "counters";
    const runsp::UiMessage u = lastU;
    CHECK_EQ (u.parityDrops, (long long) M.parityDrops);
    CHECK_EQ (u.cuts, M.cuts);
    CHECK_EQ (u.latePublishes, M.jabLates);
    CHECK_EQ ((long long) u.inputEvents, injectedEvents);
    CHECK_EQ (u.inputDrops, 0);
    CHECK_EQ (u.mirrorDrops, 0);

    totalRuns += (long long) M.runs.size();
    totalCuts += M.cuts;
    totalJobs += M.jabLates;

    for (const auto& r : M.runs)
        checkRun (r);

    gInv = "session";
}

} // namespace

int main()
{
    for (int i = 0; i < 12; ++i)
        runSession (1000 + (unsigned long long) i, false); // engine Off
    for (int i = 0; i < 40; ++i)
        runSession (2000 + (unsigned long long) i, true);  // full mix

    std::printf ("summary: runs=%lld cuts=%lld jab-lates=%lld\n", totalRuns,
                 totalCuts, totalJobs);
    std::printf ("%s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
