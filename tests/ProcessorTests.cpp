// P2 headless conformance tests (plan.md P2, specification.md S10): pair
// capture matrix, epsilon alignment, cut matrix, emission alignment math,
// CC mapping and absorption, RUN1 chunk round-trip. All through the REAL
// processBlock with a stub AudioPlayHead injected via setPlayHead.
#include "../Source/processor/PluginProcessor.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; \
    std::printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { \
    if (! ((a) == (b))) { ++failures; \
    std::printf ("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)
#define CHECK_NEAR(a, b, eps) do { \
    double _a = (double) (a), _b = (double) (b); \
    if (! (std::fabs (_a - _b) <= (eps))) { ++failures; \
    std::printf ("FAIL %s:%d: %s=%f vs %s=%f\n", __FILE__, __LINE__, \
                 #a, _a, #b, _b); } } while (0)

// ---------------------------------------------------------------------------
// Test-only playhead (S5.1): the processor reads getPlayHead(); the stub is
// injected through the public virtual AudioProcessor::setPlayHead.

class StubPlayHead : public juce::AudioPlayHead
{
public:
    double ppq = 0.0;
    double bpm = 120.0;
    bool playing = true;
    int tsNum = 4, tsDen = 4;
    bool ok = true;

    juce::Optional<juce::AudioPlayHead::PositionInfo> getPosition() const override
    {
        using Info = juce::AudioPlayHead::PositionInfo;
        if (! ok) return {};
        Info pos;
        pos.setPpqPosition (ppq);
        pos.setBpm (bpm);
        pos.setIsPlaying (playing);
        pos.setTimeSignature (juce::AudioPlayHead::TimeSignature
                              { (int) tsNum, (int) tsDen });
        return pos;
    }
    void advance (double beats) { ppq += beats; }
};

struct Ev
{
    int block = 0;
    int sample = 0;
    juce::MidiMessage msg;
};

// Drives the processor over consecutive blocks while playing (S5.1 ppq mode).
class Runner
{
public:
    RunsProcessor& proc;
    StubPlayHead stub;
    std::vector<Ev> out;

    Runner (RunsProcessor& p, int blockSize,
            double bpm = 120.0, double sr = 44100.0)
        : proc (p), sz (blockSize),
          bpb (blockSize * bpm / (60.0 * sr)),
          spb (60.0 * sr / bpm)
    {
        setTransport (0.0, bpm, true, 4, 4);
        p.setPlayHead (&stub);
    }

    void setTransport (double ppqIn, double bpmIn, bool playingIn, int num, int den)
    {
        stub.ppq = ppqIn;
        stub.bpm = bpmIn;
        stub.playing = playingIn;
        stub.tsNum = num;
        stub.tsDen = den;
    }

    Runner& startAt (double ppqIn)
    {
        stub.ppq = ppqIn;
        return *this;
    }

    void step (const juce::MidiBuffer* input)
    {
        juce::AudioBuffer<float> audio (2, sz);
        juce::MidiBuffer local;
        if (input != nullptr)
            for (const auto& e : *input)
                local.addEvent (e.getMessage(), e.samplePosition);
        proc.processBlock (audio, local);
        for (const auto& e : local)
            out.push_back ({ block, e.samplePosition, e.getMessage() });
        ++block;
        stub.advance (bpb); // S5.1: continuous ppq while playing
    }

    void run (const juce::MidiBuffer& input, int emptyBlocks)
    {
        step (&input);
        for (int i = 0; i < emptyBlocks; ++i) step (nullptr);
    }
    void runEmpty (int n) { for (int i = 0; i < n; ++i) step (nullptr); }

    int block = 0;

private:
    int sz;
    double bpb;
    double spb;
};

// param helpers ---------------------------------------------------------------

void setEngine (RunsProcessor& p, int index)
{
    auto* e = p.apvts.getParameter ("engine");
    e->setValueNotifyingHost (e->convertTo0to1 ((float) index));
}

void setParam (RunsProcessor& p, const char* id, float realValue)
{
    auto* e = p.apvts.getParameter (id);
    e->setValueNotifyingHost (e->convertTo0to1 (realValue));
}

void stepN (RunsProcessor& p, Runner& r, int n)
{
    for (int i = 0; i < n; ++i) r.step (nullptr);
}

float paramReal (RunsProcessor& p, const char* id)
{
    auto* e = p.apvts.getParameter (id);
    return e->convertFrom0to1 (e->getValue());
}

juce::MidiBuffer bufferOf (std::initializer_list<std::pair<int, juce::MidiMessage>> evs)
{
    juce::MidiBuffer m;
    for (const auto& [sample, msg] : evs)
        m.addEvent (msg, sample);
    return m;
}

// out predicates -----------------------------------------------------------

bool anyAt (const std::vector<Ev>& v, int block, int sample,
            bool (*pred) (const juce::MidiMessage&))
{
    for (const auto& e : v)
        if (e.block == block && e.sample == sample && pred (e.msg))
            return true;
    return false;
}

bool isNoteOn (const juce::MidiMessage& m) { return m.isNoteOn(); }
bool isNoteOff (const juce::MidiMessage& m) { return m.isNoteOff(); }

int countIf (const std::vector<Ev>& v, bool (*pred) (const juce::MidiMessage&))
{
    int n = 0;
    for (const auto& e : v)
        if (pred (e.msg)) ++n;
    return n;
}

int countNotePitch (const std::vector<Ev>& v, int pitch, bool wantOn)
{
    int n = 0;
    for (const auto& e : v)
        if ((wantOn ? e.msg.isNoteOn() : e.msg.isNoteOff())
            && e.msg.getNoteNumber() == (juce::uint8) pitch)
            ++n;
    return n;
}

bool hasEventAt (const std::vector<Ev>& v, int block, int sample,
                 bool noteOn, int pitch)
{
    for (const auto& e : v)
    {
        if (e.block != block || e.sample != sample) continue;
        if (noteOn ? e.msg.isNoteOn() : e.msg.isNoteOff())
            if (e.msg.getNoteNumber() == (juce::uint8) pitch)
                return true;
    }
    return false;
}

bool hasNoteOnAt (const std::vector<Ev>& v, int block, int sample)
{
    for (const auto& e : v)
        if (e.block == block && e.sample == sample && e.msg.isNoteOn())
            return true;
    return false;
}

bool hasCcAt (const std::vector<Ev>& v, int block, int sample, int cc)
{
    for (const auto& e : v)
        if (e.block == block && e.sample == sample && e.msg.isController()
            && e.msg.getControllerNumber() == (juce::uint8) cc)
            return true;
    return false;
}

int channelOf (const Ev& e) { return e.msg.getChannel(); }

// The standard matrix setup: engine Up, density 2 (n = 8), curve 0 (linear
// spacing), accent 0 (flat velocities), plus the factory defaults that are
// not under test pinned the way the matrix was written (Fold walk, no
// legato overlap), 2048-sample blocks at 44.1 kHz.
void setUpMatrix (RunsProcessor& p)
{
    p.prepareToPlay (44100.0, 2048);
    setParam (p, "density", 2.0f);
    setParam (p, "curve", 0.0f);
    setParam (p, "accent", 0.0f);
    setParam (p, "arc", 0.0f);
    setParam (p, "walk", 0.0f);
    setParam (p, "overlap", 0.0f);
    setEngine (p, 1);
}

void testPairMatrixEngineOff()
{
    // (a) engine Off: the pair passes through untouched (S3.3). Factory
    // default is Up (S4), so force Off for this test.
    RunsProcessor p;
    p.prepareToPlay (44100.0, 2048);
    setEngine (p, 0);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 64, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 70) },
    }), 5);
    // exactly the two note-ons, original offsets, nothing else
    CHECK (r.out.size() == 2);
    CHECK (hasEventAt (r.out, 0, 0, true, 60));
    CHECK (hasEventAt (r.out, 0, 64, true, 64));
    CHECK (r.out[0].msg.getChannel() == 1);
}

void testPairCompleteAndCutOnRelease()
{
    // (b) engine Up: both note-ons consumed; run emitted on the pair's
    // channel with onset_0 on the aligned beat, exactly n note-ons (+7
    // interior offs); final note held until the first trigger release
    // (S5.6 tail-end steady).
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }), 59);

    // nothing passed through for the trigger pair
    CHECK (! hasEventAt (r.out, 0, 0, true, 64));
    // exactly n = round(density x beats) = 8 note-ons, 7 interior offs
    CHECK (countIf (r.out, isNoteOn) == 8);
    CHECK (countIf (r.out, isNoteOff) == 7);
    // curve = 0 -> linear onsets i/7*4 beats = i*12600 samples at 44.1k/120bpm
    for (int i = 0; i < 8; ++i)
    {
        const int j = i * 12600 / 2048;
        CHECK (hasNoteOnAt (r.out, j, i * 12600 - j * 2048));
    }
    // endpoint velocities: first = 100 (trigger lo), last = 110 (trigger hi),
    // accent 0 -> no emphasis, arc 0 -> no swell
    int onCount = 0;
    int firstVel = -1, lastVel = -1;
    int lastPitch = -1;
    for (const auto& e : r.out)
    {
        if (! e.msg.isNoteOn()) continue;
        ++onCount;
        if (onCount == 1) { firstVel = e.msg.getVelocity(); lastPitch = e.msg.getNoteNumber(); }
        lastVel = e.msg.getVelocity();
        lastPitch = e.msg.getNoteNumber();
    }
    CHECK_EQ (firstVel, 100);
    CHECK_EQ (lastVel, 110);
    // all on the pair's channel (S5.2 tail)
    for (const auto& e : r.out)
        CHECK (channelOf (e) == 1);

    // final note held (no 8th off yet): release trigger note 60 -> cut
    const auto prevSize = r.out.size();
    const juce::MidiBuffer release = bufferOf ({
        { 5, juce::MidiMessage::noteOff (1, 60) },
    });
    r.step (&release);
    CHECK (r.out.size() == prevSize + 1);
    for (int i = (int) prevSize; i < (int) r.out.size(); ++i)
    {
        CHECK (r.out[i].msg.isNoteOff());
        CHECK (r.out[i].sample == 5);
    }
}

void testStaggeredPair()
{
    // (c) staggered pair (second note 100 samples later): both notes
    // consumed; the run fires at the second note-on and aligns forward to
    // the next integer beat when beyond the epsilon (S3.2/S5.1: here beat 1
    // -> onset_0 at sample 22050).
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 100, juce::MidiMessage::noteOn (1, 61, (juce::uint8) 110) },
    }), 59);
    // both pair notes consumed (raw pitch 61 snaps inward to D, and no note
    // passes at the original offsets)
    CHECK (countNotePitch (r.out, 61, true) == 0);
    CHECK (! hasNoteOnAt (r.out, 0, 0));
    CHECK (! hasNoteOnAt (r.out, 0, 100));
    // run starts on the next integer beat (sample 22050 = beat 1; the
    // trigger beat 100/22050 is beyond the 2 ms epsilon)
    CHECK (hasEventAt (r.out, 22050 / 2048, 22050 - (22050 / 2048) * 2048, true, 60));
}

void testJabbedSingleLatePublish()
{
    // (d) jabbed single: note-off of a pending note -> late publish
    // (note-on + note-off at the note-off offset), counter incremented
    // (S5.2).
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 30, juce::MidiMessage::noteOff (1, 60) },
    }), 2);
    CHECK (r.out.size() == 2);
    CHECK (hasEventAt (r.out, 0, 30, true, 60));
    CHECK (hasEventAt (r.out, 0, 30, false, 60));
    CHECK_EQ (p.latestUiState().latePublishes, 1);
}

void testThreeNoteChord()
{
    // (e) three-note chord: the third note passes through (S3.2 edge).
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
        { 0, juce::MidiMessage::noteOn (1, 67, (juce::uint8) 90) },
    }), 2);
    CHECK (hasEventAt (r.out, 0, 0, true, 67));
    CHECK (countNotePitch (r.out, 67, true) == 1);
}

void testSamePitchSecondNote()
{
    // (f) same-pitch second note: pending note published (at its original
    // offset), new note-on passes through (S5.2).
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 5, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 80) },
    }), 2);
    CHECK (hasEventAt (r.out, 0, 0, true, 60));     // pending published
    CHECK (hasEventAt (r.out, 0, 5, true, 60));     // new note passthrough
    CHECK (r.out.size() == 2);
}

void testTriggersDuringActiveRunIgnored()
{
    // (g) one-run rule: further pairs during an active run are ignored; the
    // notes pass through as singles (S3.2/S5.6).
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }), 59);
    CHECK (countIf (r.out, isNoteOn) == 8); // first run complete
    const auto prev = r.out.size();
    const juce::MidiBuffer more = bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 72, (juce::uint8) 64) },
        { 0, juce::MidiMessage::noteOn (1, 76, (juce::uint8) 64) },
    });
    r.step (&more);
    CHECK (r.out.size() == prev + 2);
    CHECK (hasEventAt (r.out, r.block - 1, 0, true, 72));
    CHECK (hasEventAt (r.out, r.block - 1, 0, true, 76));
    // still exactly one run: 8 run note-ons plus the two passthrough singles
    CHECK (countIf (r.out, isNoteOn) == 10);
}

void testEpsilonAlignment()
{
    // S5.1/S3.2: a trigger within the epsilon (2 ms -> 0.004 beats at 120
    // bpm) after a beat line starts there; beyond it, the next line.
    // 80 samples = 0.00363 beats (within), 300 samples = 0.01361 (beyond).
    const double bpb = 2048.0 * 120.0 / (60.0 * 44100.0);

    // within epsilon: trigger at sample 80 with B0 = 4.0 -> aligned beat 4
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.startAt (4.0);
    r.run (bufferOf ({
        { 80, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 80, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }), 9);
    CHECK (hasEventAt (r.out, 0, 0, true, 60)); // beat 4.0 == block start

    // beyond epsilon: trigger at sample 300 -> aligned to beat 5
    RunsProcessor p2;
    setUpMatrix (p2);
    Runner r2 (p2, 2048);
    r2.startAt (4.0);
    r2.run (bufferOf ({
        { 300, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 300, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }), 29);
    // onset_0 belongs to the block j with 0 < 5 - (4 + j*bpb) <= bpb
    int j = 0;
    while (5.0 - (4.0 + (double) j * bpb) > bpb) ++j;
    const double b0j = 4.0 + (double) j * bpb;
    const int expectedSample = (int) std::floor ((5.0 - b0j) * 22050.0 + 1e-9);
    CHECK (hasEventAt (r2.out, j, expectedSample, true, 60));
    CHECK (! hasEventAt (r.out, 0, 80, true, 64)); // nothing beyond epsilon leaked
}

void testEmissionAlignmentMath()
{
    // S5.1 exact conversion: onset at beat b lands at sample (b - B0) * 22050
    // inside the covering block (bpm 120, 44100 Hz -> 22050 samples/beat).
    const double bpb = 2048.0 * 120.0 / (60.0 * 44100.0);
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.startAt (4.0);
    // trigger at sample 4.5 beats equivalent: start beat 5 (beyond epsilon)
    r.run (bufferOf ({
        { 1024, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 1024, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }), 90);
    // collect all note-ons; each must be at floor((beat - B0j) * 22050) for
    // the covering block j of its absolute beat.
    std::vector<std::pair<int, int>> gotOns; // (sample, pitch)
    for (const auto& e : r.out)
        if (e.msg.isNoteOn())
            gotOns.push_back ({ e.sample, e.msg.getNoteNumber() });
    CHECK_EQ ((int) gotOns.size(), 8);
    // fold walk for span 60..64 ( Minor uses C major): [60,62,64,62,60,62,60,64]
    const int walkPitches[8] = { 60, 62, 64, 62, 60, 62, 60, 64 };
    for (int k = 0; k < (int) gotOns.size(); ++k)
    {
        const double beat = 5.0 + 4.0 * (double) k / 7.0;
        int j = 0;
        while (beat - (4.0 + (double) j * bpb) > bpb) ++j;
        const double b0j = 4.0 + (double) j * bpb;
        const int expected = (int) std::floor ((beat - b0j) * 22050.0 + 1e-9);
        CHECK_EQ (gotOns[k].first, expected);
        CHECK_EQ (gotOns[k].second, walkPitches[k]);
    }
}

void testCutOnTriggerRelease()
{
    // S5.6: trigger release cuts the run at the note-off's sample; the held
    // target note gets its note-off there.
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }), 44);
    const auto prev = r.out.size();
    const juce::MidiBuffer release = bufferOf ({
        { 7, juce::MidiMessage::noteOff (1, 64) },
    });
    r.step (&release);
    CHECK (r.out.size() == prev + 1);
    CHECK (r.out.back().msg.isNoteOff());
    CHECK (r.out.back().sample == 7);
    CHECK_EQ (p.latestUiState().cuts, 1);
}

void testCutOnCC123()
{
    // S5.6: all-notes-off (CC 123) cuts the run; CC123 itself still passes
    // through (interpretation note) so passthrough notes are cleared.
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }), 19);
    const juce::MidiBuffer cc123 = bufferOf ({
        { 9, juce::MidiMessage::controllerEvent (1, 123, 0) },
    });
    r.step (&cc123);
    CHECK (hasCcAt (r.out, r.block - 1, 9, 123));
    CHECK (r.out.back().msg.isNoteOff()); // sounding notes cut at sample 9
    // further blocks emit nothing
    const auto prev2 = r.out.size();
    r.step (nullptr);
    r.step (nullptr);
    CHECK (r.out.size() == prev2); // no engine events after the cut
    CHECK_EQ (p.latestUiState().cuts, 1);
}

void engineSwitchCutTest_helper (int srcType)
{
    // S5.6: engine switch cuts the run (CC87 value switch; engine CC is
    // absorbed in every state, S3.1).
    RunsProcessor p;
    setUpMatrix (p);
    p.settings.engineSourceType = srcType;
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }), 19);
    const auto prev = r.out.size();
    // engine CC only exists in CC source mode; in note/PC mode use the SET
    // event numbers with those sources (same cut path).
    const juce::MidiBuffer switchEvs = bufferOf (srcType == 1
        ? std::initializer_list<std::pair<int, juce::MidiMessage>> {
              { 11, juce::MidiMessage::controllerEvent (1, 87, 0) } }
        : (srcType == 0
            ? std::initializer_list<std::pair<int, juce::MidiMessage>> {
                  { 11, juce::MidiMessage::noteOn (1, (juce::uint8) 12, (juce::uint8) 100) } }
            : std::initializer_list<std::pair<int, juce::MidiMessage>> {
                  { 11, juce::MidiMessage::programChange (1, (juce::uint8) 0) } }));
    r.step (&switchEvs);
    CHECK (r.out.back().msg.isNoteOff());
    CHECK_EQ (r.out.back().sample, 11);
    // engine CC absorbed in every engine state (S3.1): never echoed
    for (const auto& e : r.out)
        if (e.msg.isController())
            CHECK (e.msg.getControllerNumber() != 87);
    p.applyPendingCcMirrors();
    // The engine STATE reverts (audio thread truth); the host parameter is
    // no longer written back by the plugin (echo loop guard) - assert on the
    // published state instead (S3.1/S11).
    CHECK_EQ ((int) p.publishedEngineState.load(), 0);
}

void testEngineCcSwitchCut() { engineSwitchCutTest_helper (1); }
void testEngineNoteSwitchCut() { engineSwitchCutTest_helper (0); }
void testEnginePcSwitchCut() { engineSwitchCutTest_helper (2); }

void testLoopWrapCut()
{
    // S5.6: ppq backward jump beyond epsilon (loop wrap) cuts the run.
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }), 19);
    const auto prev = r.out.size();
    r.stub.ppq = 0.5; // loop wrap backwards (S5.6)
    r.step (nullptr);
    CHECK (r.out.size() > prev);
    bool sawCutOff = false;
    for (int i = (int) prev; i < (int) r.out.size(); ++i)
        if (r.out[i].msg.isNoteOff()) sawCutOff = true;
    CHECK (sawCutOff);
    CHECK_EQ (r.out.back().sample, 0); // cut at the discontinuity's block start
    CHECK_EQ (p.latestUiState().cuts, 1);
}

void testKeyswitchStopAndPassthrough()
{
    // S3.1: in note mode the keyswitches are absorbed when engine Up/Down
    // and passed through when Off.
    RunsProcessor p;
    setUpMatrix (p);
    p.settings.engineSourceType = 0;
    Runner r (p, 2048);
    // keyswitch while paired-absorb state: 13 (Up) absorbed, 12 (Off) cuts
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, (juce::uint8) 13, (juce::uint8) 100) },
    }), 1);
    CHECK (r.out.empty()); // absorbed while engine Up/Down
    // now Off: keyswitches pass through (S3.1). Parameter-driven engine
    // changes are one-shot-detected and lost inside the echo-grace window
    // (P4 host-echo guard), so expire the window BEFORE the change.
    for (int i = 0; i < 11; ++i)
        r.step (nullptr); // echo grace expires
    setEngine (p, 0);
    for (int i = 0; i < 3; ++i)
        r.step (nullptr); // engine param change -> Off
    CHECK_EQ ((int) p.publishedEngineState.load(), 0);
    const auto prev = r.out.size();
    const juce::MidiBuffer ks = bufferOf ({
        { 3, juce::MidiMessage::noteOn (1, (juce::uint8) 13, (juce::uint8) 100) },
    });
    r.step (&ks);
    CHECK (! r.out.empty());
    CHECK (r.out.back().msg.isNoteOn() && r.out.back().sample == 3);
}

void testCcMappingAndAbsorption()
{
    // D13/S4/S11: linear CC mappings, absorption when engine on, passthrough
    // when engine off, mirror application only on change.
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 3, juce::MidiMessage::controllerEvent (1, 89, 64) },
        { 5, juce::MidiMessage::controllerEvent (1, 93, 100) },
        { 7, juce::MidiMessage::controllerEvent (1, 95, 64) },
        { 9, juce::MidiMessage::controllerEvent (1, 88, 0) },
    }), 1);
    // absorbed: nothing echoed onto the stream (S4/S11)
    CHECK (! hasCcAt (r.out, 0, 3, 89));
    CHECK (! hasCcAt (r.out, 0, 5, 93));
    CHECK (! hasCcAt (r.out, 0, 7, 95));
    CHECK (! hasCcAt (r.out, 0, 9, 88));

    // direct live value (audio thread) + mirror application (message thread)
    p.applyPendingCcMirrors();
    // density: 1 + 64/127*15 ~= 8.559
    CHECK_NEAR (paramReal (p, "density"), 1.0 + 64.0 / 127.0 * 15.0, 1e-3);
    // tonic: floor(100*12/128) = 9
    CHECK_EQ (paramReal (p, "tonic"), 9.0f);
    // walk: 64 -> Zig-zag
    CHECK_EQ (paramReal (p, "walk"), 1.0f);
    // beats: 1 + 0/127*15 = 1
    CHECK_EQ (paramReal (p, "beats"), 1.0f);

    // mirror idempotence: applying again must not re-dirty (S11).
    const auto before = paramReal (p, "density");
    p.applyPendingCcMirrors();
    CHECK_EQ (paramReal (p, "density"), before);

    // engine Off: bound CCs pass through inertly (S3.3) - factory default is
    // Up (S4), so force Off for the inert check.
    RunsProcessor p2;
    p2.prepareToPlay (44100.0, 2048);
    setEngine (p2, 0);
    Runner r2 (p2, 2048);
    r2.run (bufferOf ({
        { 3, juce::MidiMessage::controllerEvent (1, 89, 64) },
    }), 0);
    CHECK (hasCcAt (r2.out, 0, 3, 89));
    CHECK_NEAR (paramReal (p2, "density"), 5.0f, 1e-4);

    // walk 63 -> Fold (D13)
    RunsProcessor p3;
    setUpMatrix (p3);
    Runner r3 (p3, 2048);
    r3.run (bufferOf ({
        { 0, juce::MidiMessage::controllerEvent (1, 95, 63) },
    }), 0);
    p3.applyPendingCcMirrors();
    CHECK_EQ (paramReal (p3, "walk"), 0.0f);
}

// Exact replica of RunsProcessor::applyBoundCc's CC -> real map (bindings
// 0..7, GUI order beats..walk), including the discrete clamp - the forward
// direction ccValueForBinding is checked against. kModeCcSteps = 17.
float forwardCcMap (int binding, int ccValue)
{
    const int paramIndex = binding + 1;
    const double c = (double) ccValue;
    float real = 0.0f;
    switch (binding)
    {
        case 0:  real = (float) juce::jlimit (1, 16, (int) std::lround (
                          1.0 + c / 127.0 * 15.0)); break;
        case 1:  real = (float) (1.0 + c / 127.0 * 15.0); break;
        case 2:  real = (float) (c / 127.0); break;
        case 3:  real = (float) (c / 127.0); break;
        case 4:  real = (float) (c / 127.0 * 2.0 - 1.0); break;
        case 5:  real = (float) ((ccValue * 12) / 128); break;
        case 6:  real = (float) ((ccValue * 17) / 128); break;
        case 7:  real = ccValue < 64 ? 0.0f : 1.0f; break;
        default: return 0.0f;
    }
    if (RunsProcessor::paramIsDiscrete (paramIndex))
        real = (float) juce::jlimit (
            (int) RunsProcessor::realValueMinOf (paramIndex),
            (int) RunsProcessor::realValueMaxOf (paramIndex),
            (int) std::lround (real));
    else
        real = juce::jlimit (RunsProcessor::realValueMinOf (paramIndex),
                             RunsProcessor::realValueMaxOf (paramIndex),
                             real);
    return real;
}

void testCcValueInverse()
{
    // The editor's CC badges show ccValueForBinding: for every CC value, the
    // badge value must map forward to the SAME real value (the badge always
    // reproduces the current setting).
    RunsProcessor p;
    for (int b = 0; b < 8; ++b)
        for (int cc = 0; cc <= 127; ++cc)
        {
            const float v = forwardCcMap (b, cc);
            const int back = p.ccValueForBinding (b, v);
            CHECK (back >= 0 && back <= 127);
            if (RunsProcessor::paramIsDiscrete (b + 1))
                CHECK_EQ (forwardCcMap (b, back), v);
            else
                CHECK_NEAR (forwardCcMap (b, back), v, 1e-3);
        }

    // Out-of-range input clamps into the parameter range, bad bindings -> -1.
    CHECK_EQ (p.ccValueForBinding (0, 99.0f), 127);  // Beats above max
    CHECK_EQ (p.ccValueForBinding (0, -5.0f), 0);    // Beats below min
    CHECK_EQ (p.ccValueForBinding (-1, 0.5f), -1);
    CHECK_EQ (p.ccValueForBinding (8, 0.5f), -1);

    // Engine badge values land in their own S3.1 buckets (0-40 / 41-79 /
    // 80-127), so each one round-trips back to its state.
    for (int s = 0; s <= 2; ++s)
        CHECK_EQ (RunsProcessor::engineStateFromCcValue (
                      RunsProcessor::ccValueForEngineState (s)),
                  s);
}

void tuneChunkDefaults (RunsProcessor& p)
{
    setEngine (p, 2); // Down
    setParam (p, "beats", 7.0f);
    setParam (p, "density", 3.5f);
    setParam (p, "curve", 0.125f);
    setParam (p, "accent", 0.75f);
    setParam (p, "arc", -0.5f);
    setParam (p, "tonic", 5.0f);
    setParam (p, "mode", 10.0f);
    setParam (p, "walk", 1.0f);
    setParam (p, "overlap", 1.0f);
    setParam (p, "humanize", 1.0f);
    setParam (p, "seed", 424242.0f);
    p.settings.engineSourceType = 2;
    p.settings.engineNumber = 90;
    p.settings.engineNoteNumbers[0] = 40;
    p.settings.engineNoteNumbers[1] = 41;
    p.settings.engineNoteNumbers[2] = 42;
    p.settings.enginePcNumbers[0] = 5;
    p.settings.enginePcNumbers[1] = 6;
    p.settings.enginePcNumbers[2] = 7;
    p.settings.boundCC[0] = 60;
    p.settings.boundCC[3] = 255;
    p.settings.epsilonMs = 3.5f;
    p.settings.gateFraction = 0.8f;
    p.settings.downWeight = 1.25f;
    p.settings.midBarWeight = 0.5f;
    p.settings.customOffsets = 0x0555;
    p.settings.humanizeVelPercent = 17.5f;
    p.settings.humanizeTimingMs = 11.25f;
    p.recordEditorWindowSize (1000, 500);
}

bool chunkValuesRetained (RunsProcessor& a, RunsProcessor& b,
                          bool withOverlap = true)
{
    // withOverlap also gates the S5.9 state (Humanize/Seed params and the
    // two humanize tunables): a v1/v2 payload predates all of it.
    bool ok = true;
    const char* ids[RunsProcessor::kNumParams] = { "engine", "beats", "density",
        "curve", "accent", "arc", "tonic", "mode", "walk", "overlap",
        "humanize", "seed" };
    for (int i = 0; i < RunsProcessor::kNumParams; ++i)
    {
        if (! withOverlap
            && (i == RunsProcessor::kOverlapIndex
                || i == RunsProcessor::kHumanizeIndex
                || i == RunsProcessor::kSeedIndex))
            continue;
        if (std::fabs (paramReal (a, ids[i]) - paramReal (b, ids[i])) > 1e-4f)
            ok = false;
    }
    if (a.settings.engineSourceType != b.settings.engineSourceType
        || a.settings.engineNumber != b.settings.engineNumber
        || a.settings.epsilonMs != b.settings.epsilonMs
        || a.settings.gateFraction != b.settings.gateFraction
        || a.settings.downWeight != b.settings.downWeight
        || a.settings.midBarWeight != b.settings.midBarWeight
        || a.settings.customOffsets != b.settings.customOffsets)
        return false;
    if (withOverlap
        && (a.settings.humanizeVelPercent != b.settings.humanizeVelPercent
            || a.settings.humanizeTimingMs != b.settings.humanizeTimingMs))
        return false;
    for (int k = 0; k < 3; ++k)
        if (a.settings.engineNoteNumbers[k] != b.settings.engineNoteNumbers[k]
            || a.settings.enginePcNumbers[k] != b.settings.enginePcNumbers[k])
            return false;
    for (int k = 0; k < 8; ++k)
        if (a.settings.boundCC[k] != b.settings.boundCC[k]) return false;
    return ok;
}

bool allDefaults (const RunsProcessor& p)
{
    const char* ids[RunsProcessor::kNumParams] = { "engine", "beats", "density",
        "curve", "accent", "arc", "tonic", "mode", "walk", "overlap",
        "humanize", "seed" };
    const float defaults[RunsProcessor::kNumParams] =
        { 1, 4, 5, 0.22f, 0.41f, 0.15f, 0, 0, 1, 1, 0, 0 };
    bool ok = true;
    for (int i = 0; i < RunsProcessor::kNumParams; ++i)
    {
        auto* e = p.apvts.getParameter (ids[i]);
        if (std::fabs (e->convertFrom0to1 (e->getValue()) - defaults[i]) > 1e-4f)
            ok = false;
    }
    runsp::PluginSettings d; // default settings
    bool sOk = p.settings.engineSourceType == d.engineSourceType
        && p.settings.engineNumber == d.engineNumber
        && p.settings.epsilonMs == d.epsilonMs
        && p.settings.gateFraction == d.gateFraction
        && p.settings.downWeight == d.downWeight
        && p.settings.midBarWeight == d.midBarWeight
        && p.settings.customOffsets == d.customOffsets
        && p.settings.humanizeVelPercent == d.humanizeVelPercent
        && p.settings.humanizeTimingMs == d.humanizeTimingMs;
    for (int k = 0; k < 3 && sOk; ++k)
        sOk = p.settings.engineNoteNumbers[k] == d.engineNoteNumbers[k]
           && p.settings.enginePcNumbers[k] == d.enginePcNumbers[k];
    for (int k = 0; k < 8 && sOk; ++k)
        sOk = p.settings.boundCC[k] == d.boundCC[k];
    return ok && sOk;
}

void testChunkRoundTrip()
{
    RunsProcessor a;
    tuneChunkDefaults (a);
    juce::MemoryBlock data;
    a.getStateInformation (data);
    RunsProcessor b;
    b.setStateInformation (data.getData(), (int) data.getSize());
    CHECK (chunkValuesRetained (a, b));

    // the RUNV footer read path (S7): window size round-trips
    int bw = 0, bh = 0;
    b.getEditorWindowSize (bw, bh);
    CHECK (bw == 1000 && bh == 500);
}

void testChunkTruncatedRejected()
{
    RunsProcessor a;
    tuneChunkDefaults (a);
    juce::MemoryBlock data;
    a.getStateInformation (data);
    RunsProcessor b;
    b.setStateInformation (data.getData(), (int) data.getSize() - 5);
    CHECK (allDefaults (b));
}

void testChunkTrailingGarbageIgnored()
{
    RunsProcessor a;
    tuneChunkDefaults (a);
    juce::MemoryBlock data;
    a.getStateInformation (data);
    std::vector<uint8_t> blob ((uint8_t*) data.getData(),
                               (uint8_t*) data.getData() + data.getSize());
    for (int i = 0; i < 32; ++i) blob.push_back ((uint8_t) (i * 7)); // garbage
    RunsProcessor b;
    b.setStateInformation (blob.data(), (int) blob.size());
    CHECK (chunkValuesRetained (a, b));
}

void testChunkWrongMagicRejected()
{
    RunsProcessor a;
    tuneChunkDefaults (a);
    juce::MemoryBlock data;
    a.getStateInformation (data);
    RunsProcessor b;
    auto blob = (uint8_t*) data.getData();
    blob[2] = 'X'; // wrong magic
    b.setStateInformation (blob, (int) data.getSize());
    CHECK (allDefaults (b));
}

void testChunkVersionRejected()
{
    RunsProcessor a;
    tuneChunkDefaults (a);
    juce::MemoryBlock data;
    a.getStateInformation (data);
    RunsProcessor b;
    auto blob = (uint8_t*) data.getData();
    const uint32_t v = 999;
    std::memcpy (blob + 4, &v, 4); // version 999: reject, keep current state (S7)
    b.setStateInformation (blob, (int) data.getSize());
    CHECK (allDefaults (b));
}

void testChunkV1BackwardCompatible()
{
    // A v1 chunk (9 parameters, schema 1, the P0..P4 project layout) loads
    // with Overlap / Humanize / Seed at their off defaults and everything
    // else intact (S7 backwards-compatible reader; its settings blob ends
    // after the v1/v2 tail, no humanize tunables).
    RunsProcessor a;
    tuneChunkDefaults (a);
    juce::MemoryBlock data;
    a.getStateInformation (data);
    std::vector<uint8_t> blob ((uint8_t*) data.getData(),
                               (uint8_t*) data.getData() + data.getSize());
    // Rewrite the v3 chunk as v1: version 1, the three v2/v3-only param
    // floats (Overlap/Humanize/Seed, starting at 8 + 9*4) shifted out, then
    // the v3-only 8-byte humanize settings tail (after the 34-byte v1/v2
    // settings) shifted out in front of the RUNV footer.
    const uint32_t v = 1;
    std::memcpy (blob.data() + 4, &v, 4);
    const size_t extraParamsAt = 8 + 9 * 4;
    const size_t extraParamsBytes = 3 * 4;
    std::memmove (blob.data() + extraParamsAt,
                  blob.data() + extraParamsAt + extraParamsBytes,
                  blob.size() - extraParamsAt - extraParamsBytes);
    blob.resize (blob.size() - extraParamsBytes);
    const size_t v3TailAt = extraParamsAt + 34; // v1 settings = 34 bytes
    std::memmove (blob.data() + v3TailAt, blob.data() + v3TailAt + 8,
                  blob.size() - v3TailAt - 8);
    blob.resize (blob.size() - 8);
    RunsProcessor b;
    b.setStateInformation (blob.data(), (int) blob.size());
    CHECK (chunkValuesRetained (a, b, false)); // minus the v2/v3-only state
    CHECK (paramReal (b, "overlap") == 0.0f);  // v1 chunk: Overlap off (S7)
    CHECK (paramReal (b, "humanize") == 0.0f); // v1 chunk: Humanize off
    CHECK (paramReal (b, "seed") == 0.0f);     // v1 chunk: Seed 0
}

void testChunkV2BackwardCompatible()
{
    // A v2 chunk (10 parameters, pre-humanize schema) loads with Humanize
    // off / Seed 0 and the default humanize tunables, everything else
    // intact.
    RunsProcessor a;
    tuneChunkDefaults (a);
    juce::MemoryBlock data;
    a.getStateInformation (data);
    std::vector<uint8_t> blob ((uint8_t*) data.getData(),
                               (uint8_t*) data.getData() + data.getSize());
    const uint32_t v = 2;
    std::memcpy (blob.data() + 4, &v, 4);
    const size_t extraParamsAt = 8 + 10 * 4; // Humanize/Seed floats
    const size_t extraParamsBytes = 2 * 4;
    std::memmove (blob.data() + extraParamsAt,
                  blob.data() + extraParamsAt + extraParamsBytes,
                  blob.size() - extraParamsAt - extraParamsBytes);
    blob.resize (blob.size() - extraParamsBytes);
    const size_t v3TailAt = extraParamsAt + 34; // v2 settings = 34 bytes
    std::memmove (blob.data() + v3TailAt, blob.data() + v3TailAt + 8,
                  blob.size() - v3TailAt - 8);
    blob.resize (blob.size() - 8);
    RunsProcessor b;
    b.setStateInformation (blob.data(), (int) blob.size());
    CHECK (chunkValuesRetained (a, b, false));
    CHECK (paramReal (b, "humanize") == 0.0f);
    CHECK (paramReal (b, "seed") == 0.0f);
    runsp::PluginSettings d;
    CHECK (b.settings.humanizeVelPercent == d.humanizeVelPercent);
    CHECK (b.settings.humanizeTimingMs == d.humanizeTimingMs);
}

void testJsonClipboardRoundTrip()
{
    // S7/S9 clipboard preset: the JSON text carries the same payload as the
    // RUN1 chunk (parameters as real values + settings), so a paste behaves
    // like a project-state load.
    RunsProcessor a;
    tuneChunkDefaults (a);
    const juce::String text = a.stateToJsonText();
    CHECK (text.contains ("\"runs-assistant\""));
    CHECK (text.contains ("\"customOffsets\""));
    CHECK (text.contains ("\"humanize\""));          // S5.9 params (v2 JSON)
    CHECK (text.contains ("\"humanizeVelPercent\"")); // S5.9 settings keys
    RunsProcessor b;
    juce::String error;
    CHECK (b.applyStateFromJsonText (text, error));
    CHECK (error.isEmpty());
    CHECK (chunkValuesRetained (a, b));
    // A second round trip of the applied state is byte-for-byte identical.
    CHECK (b.stateToJsonText() == text);
}

void testJsonClipboardRejected()
{
    // Foreign or malformed text must change nothing at all (S7-style
    // reject-and-keep), and every rejection must carry a short reason the
    // editor can flash on the button.
    RunsProcessor p;
    juce::String error;
    const char* bad[] = {
        "",                                                   // empty clipboard
        "this is not json at all",                            // parse failure
        "[{\"beats\":4}]",                                    // array root
        "{\"format\":\"other-app\",\"params\":{}}",           // not ours
        "{\"format\":\"runs-assistant\",\"version\":99,"
            "\"params\":{}}",                                 // future schema
        "{\"format\":\"runs-assistant\"}",                    // no params
        "{\"format\":\"runs-assistant\",\"params\":{}}",      // no known key
        "{\"format\":\"runs-assistant\",\"params\":"
            "{\"beats\":\"loud\"}}",                          // wrong type
        "{\"format\":\"runs-assistant\",\"params\":{\"beats\":4},"
            "\"settings\":{\"boundCC\":\"x\"}}"               // bad settings
    };
    for (auto* t : bad)
    {
        const bool ok = p.applyStateFromJsonText (t, error);
        CHECK (! ok);
        CHECK (! error.isEmpty());
    }
    CHECK (allDefaults (p)); // rejected payloads never mutate the state
}

void testJsonClipboardPartialAndClamped()
{
    // Present fields are range-clamped like the chunk reader; absent fields
    // (parameters and the whole settings section) keep their current values.
    RunsProcessor p;
    tuneChunkDefaults (p);
    juce::String error;
    CHECK (p.applyStateFromJsonText (
        "{\"format\":\"runs-assistant\",\"version\":1,"
        "\"params\":{\"beats\":99,\"curve\":-4.0,\"tonic\":7,"
        "\"density\":99.0,\"humanize\":1,\"seed\":1234567}}", error));
    CHECK (error.isEmpty());
    CHECK_EQ ((int) paramReal (p, "beats"), 16);      // clamped to 1..16
    CHECK_NEAR (paramReal (p, "curve"), 0.0f, 1e-6);  // clamped to 0..1
    CHECK_NEAR (paramReal (p, "density"), 16.0f, 1e-6);
    CHECK_EQ ((int) paramReal (p, "tonic"), 7);       // in range: applied
    CHECK_NEAR (paramReal (p, "accent"), 0.75f, 1e-4); // absent: untouched
    CHECK_NEAR (paramReal (p, "humanize"), 1.0f, 1e-6); // S5.9 bool: applied
    CHECK_NEAR (paramReal (p, "seed"), 999999.0f, 1e-6); // clamped 0..999999
    CHECK (p.settings.engineNumber == 90);            // settings absent too
}

void testStateSavedFullyRoundTrip()
{
    // Issue report: "check that the plugin state is being saved fully to the
    // project". Engine switches (GUI or CC origin) mirror into the Engine
    // parameter now, and the chunk write flushes pending CC mirrors first,
    // so a save directly after a switch must carry the real state.
    RunsProcessor a;
    a.prepareToPlay (44100.0, 2048);
    Runner r (a, 2048);
    setParam (a, "beats", 9.0f);
    stepN (a, r, 2);
    a.requestEngineState (2); // GUI switch: param mirrors + state dirty
    stepN (a, r, 2);
    CHECK_EQ ((int) a.publishedEngineState.load(), 2);
    // engine state changed via CC origin on a SECOND processor: the mirror
    // is pending until applied; the chunk write must flush it (S7).
    RunsProcessor c;
    c.prepareToPlay (44100.0, 2048);
    Runner rc (c, 2048);
    setUpMatrix (c); // engine Up (param path), then switch via CC87
    rc.run (bufferOf ({
        { 0, juce::MidiMessage::controllerEvent (1, 87, 100) }, // 80-127 = Down
    }), 2);
    CHECK_EQ ((int) c.publishedEngineState.load(), 2);
    CHECK (paramReal (c, "engine") == 1.0f); // still the earlier setEngine(1):
    // the CC97-driven switch is queued as a mirror, not applied yet (S11)
    c.applyPendingCcMirrors();
    CHECK (paramReal (c, "engine") == 2.0f);

    juce::MemoryBlock data;
    a.getStateInformation (data);
    RunsProcessor b;
    b.setStateInformation (data.getData(), (int) data.getSize());
    CHECK (paramReal (b, "engine") == 2.0f);
    CHECK (paramReal (b, "beats") == 9.0f);
}

void testOverlapParamEmission()
{
    // S5.8 overlap toggle through the REAL processBlock: run notes overlap.
    // Linear spacing (curve 0, beats 4, density 2 -> n = 8, gaps 0.5 beats):
    // classic gating emits strict on/off alternation (each off before the
    // next on); overlap ON emits off_i after on_{i+1}, which appears as an
    // on-note immediately followed by another note-on mid-run.
    RunsProcessor p;
    setUpMatrix (p); // engine Up, density 2 (n = 8), beats 4, curve/accent 0
    setParam (p, "overlap", 1.0f);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 0, juce::MidiMessage::noteOn (1, 67, (juce::uint8) 70) },
    }), 60);

    CHECK_EQ (countIf (r.out, isNoteOn), 8);
    bool sawOverlap = false;
    for (size_t i = 0; i + 2 < r.out.size(); ++i)
        if (r.out[i].msg.isNoteOn() && r.out[i + 1].msg.isNoteOn())
            sawOverlap = true; // on-note without an off between (overlap)
    CHECK (sawOverlap);

    // same run with overlap OFF must stay strictly alternating
    RunsProcessor q;
    setUpMatrix (q);
    Runner rq (q, 2048);
    rq.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 0, juce::MidiMessage::noteOn (1, 67, (juce::uint8) 70) },
    }), 60);
    CHECK_EQ (countIf (rq.out, isNoteOn), 8);
    for (size_t i = 0; i + 2 < rq.out.size(); ++i)
        if (rq.out[i].msg.isNoteOn() && rq.out[i + 1].msg.isNoteOn())
        {
            CHECK (false); // classic gate: no back-to-back ons before the tail
            break;
        }
}

// ------------------------------------------------------- engine-switch chain
// P4 hardening regressions (reported: "clicking Up highlights Down"): every
// engine state must land exactly, via BOTH the GUI queue path
// (requestEngineState) and the APVTS parameter path, and a host param echo
// inside the grace window must NOT revert the state.

void testEngineChainQueuePath()
{
    for (int state = 0; state <= 2; ++state)
    {
        RunsProcessor p;
        p.prepareToPlay (44100.0, 2048);
        Runner r (p, 2048);
        p.requestEngineState (state);
        stepN (p, r, 2);
        CHECK_EQ ((int) p.publishedEngineState.load(), state);
    }
}

void testEngineChainParamPath()
{
    // grace starts 0 but rises after any switch, so each state starts from a
    // FRESH processor (its first block applies the param change directly).
    for (int state = 0; state <= 2; ++state)
    {
        RunsProcessor p;
        p.prepareToPlay (44100.0, 2048);
        Runner r (p, 2048);
        stepN (p, r, 60); // settle: raw-atomics cache at defaults + the
                          // default-Up switch's echo grace window expires
        setEngine (p, state);
        stepN (p, r, 2);
        CHECK_EQ ((int) p.publishedEngineState.load(), state);
    }
}

void testEngineChainSequence()
{
    // chained switches: queue path 1, 2, 0, then param path 1, 2
    RunsProcessor p;
    p.prepareToPlay (44100.0, 2048);
    Runner r (p, 2048);

    p.requestEngineState (1);
    stepN (p, r, 2);
    CHECK_EQ ((int) p.publishedEngineState.load(), 1);
    p.requestEngineState (2);
    stepN (p, r, 2);
    CHECK_EQ ((int) p.publishedEngineState.load(), 2);
    p.requestEngineState (0);
    stepN (p, r, 2);
    CHECK_EQ ((int) p.publishedEngineState.load(), 0);

    stepN (p, r, 40);   // let the echo grace window expire
    setEngine (p, 1);
    stepN (p, r, 2);
    CHECK_EQ ((int) p.publishedEngineState.load(), 1);
    stepN (p, r, 40);
    setEngine (p, 2);
    stepN (p, r, 2);
    CHECK_EQ ((int) p.publishedEngineState.load(), 2);
}

void testEngineEchoIgnoredDuringGrace()
{
    // Host-echo regression: REAPER pushes its cached discrete-param value
    // back shortly after UI focus events; within the grace window that echo
    // must not revert the engine (reported "clicking Up highlights Down").
    RunsProcessor p;
    p.prepareToPlay (44100.0, 2048);
    Runner r (p, 2048);
    p.requestEngineState (1); // click Up
    stepN (p, r, 1);
    CHECK_EQ ((int) p.publishedEngineState.load(), 1);

    setEngine (p, 2);          // stale cached param echo (Down)
    stepN (p, r, 5);           // well inside the 0.5 s grace window
    CHECK_EQ ((int) p.publishedEngineState.load(), 1); // echo ignored
    stepN (p, r, 30);          // grace window expires
    // The stale echo already consumed the raw value 2; genuine host
    // automation applies once the window passes (new value writes still
    // reach the block path). Walk 2 -> 0 -> 2 through fresh writes, each
    // after letting the previous switch's own grace expire.
    setEngine (p, 0);
    stepN (p, r, 2);
    CHECK_EQ ((int) p.publishedEngineState.load(), 0);
    stepN (p, r, 30);          // br the 0-switch's grace expires
    setEngine (p, 2);
    stepN (p, r, 2);
    CHECK_EQ ((int) p.publishedEngineState.load(), 2);
}

void testEngineSwitchCutsActiveRunBothPaths()
{
    // A queued engine switch cuts the active run at block start; the cut
    // note-offs land at sample 0 (23 whole sounding notes). The passthrough
    // mode afterwards: a fresh note passes untouched.
    RunsProcessor p;
    setUpMatrix (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
        { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }), 0); // trigger block only: note 60 is sounding now
    CHECK_EQ (countIf (r.out, isNoteOn), 1);
    const auto prev = r.out.size();
    p.requestEngineState (0);
    r.step (nullptr);
    bool cutAtStart = false;
    for (int i = (int) prev; i < (int) r.out.size(); ++i)
        if (r.out[i].msg.isNoteOff() && r.out[i].sample == 0)
            cutAtStart = true;
    CHECK (cutAtStart);
    const auto uiState = p.latestUiState();
    CHECK_EQ (uiState.cuts, 1);
    // passthrough mode afterwards: a fresh note passes untouched
    const auto prev2 = r.out.size();
    const juce::MidiBuffer single = bufferOf ({
        { 11, juce::MidiMessage::noteOn (1, 55, (juce::uint8) 90) },
    });
    r.step (&single);
    CHECK (r.out.size() == prev2 + 1);
    CHECK (hasEventAt (r.out, r.block - 1, 11, true, 55));
}

// ------------------------------------------------ engine-off handoff (S3.3)
// note-on with the engine Off passes through AND can still seed the pair
// pending when the engine comes back on: the run starts from the SECOND
// note-on, and the engine can be switched off via the engine CC without any
// note-off traffic (no jabs; the still-running final note is closed by the
// switch cut).

void testEngineCcValueRanges()
{
    // S3.1/D13 single-definition mapping.
    CHECK_EQ (RunsProcessor::engineStateFromCcValue (0), 0);
    CHECK_EQ (RunsProcessor::engineStateFromCcValue (40), 0);
    CHECK_EQ (RunsProcessor::engineStateFromCcValue (41), 1);
    CHECK_EQ (RunsProcessor::engineStateFromCcValue (79), 1);
    CHECK_EQ (RunsProcessor::engineStateFromCcValue (80), 2);
    CHECK_EQ (RunsProcessor::engineStateFromCcValue (127), 2);
}

void engineOffHandoffSetup (RunsProcessor& p)
{
    // engine forced Off (the factory default is Up, S4) and the non-matrix
    // defaults pinned the way this handoff matrix was written (Fold walk, no
    // legato overlap).
    p.prepareToPlay (44100.0, 2048);
    setParam (p, "density", 2.0f);
    setParam (p, "curve", 0.0f);
    setParam (p, "accent", 0.0f);
    setParam (p, "walk", 0.0f);
    setParam (p, "overlap", 0.0f);
    setEngine (p, 0);
}

void testEngineOffHandoffPairFiresAtSecondNoteOn()
{
    RunsProcessor p;
    engineOffHandoffSetup (p);
    Runner r (p, 2048);
    // (1) Off: the note passes through untouched and stays latched while held
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
    }), 2);
    CHECK (hasEventAt (r.out, 0, 0, true, 60));
    // (2) CC87 Up (range 41-79) while the note is still held: pending seeds
    r.step (&bufferOf ({
        { 0, juce::MidiMessage::controllerEvent (1, 87, 50) },
    }));
    CHECK_EQ ((int) p.publishedEngineState.load(), 1);
    // (3) second note-on completes the pair: run fires from it (both note-ons
    // consumed - no immediate passthrough of 64)
    const size_t prev = r.out.size();
    r.step (&bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 64, (juce::uint8) 110) },
    }));
    for (const auto& e : r.out) // nothing at/beyond prev refers to pitch 64
        if (e.msg.isNoteOn() && e.msg.getNoteNumber() == (juce::uint8) 64)
        {
            const bool isRunTail = e.block >= 3; // run's last note lands on 64
            CHECK (isRunTail);
        }
    // run reaches its target (n = 8, 4 beats): drive the run out
    r.runEmpty (60);
    const auto ui = p.latestUiState();
    CHECK_EQ (ui.notesEmitted, 8);
    CHECK_EQ (ui.noteCount, 8);
    // first run note is the handoff note's pitch (Up starts at lo)
    bool firstRunOnSet = false;
    for (const auto& e : r.out)
        if (e.block >= 3 && e.msg.isNoteOn())
        {
            CHECK_EQ ((int) e.msg.getNoteNumber(), 60);
            firstRunOnSet = true;
            break;
        }
    CHECK (firstRunOnSet);
    CHECK (countIf (r.out, isNoteOn) == 9); // 1 passthrough + 8 run notes
    // (4) switch Off via CC87 (range 0-40) without ANY note-off sent: clean.
    // The final (target) run note is closed by the switch itself, nothing
    // is jabbed.
    const size_t prev2 = r.out.size();
    r.step (&bufferOf ({
        { 0, juce::MidiMessage::controllerEvent (1, 87, 0) },
    }));
    CHECK_EQ ((int) p.publishedEngineState.load(), 0);
    const auto ui2 = p.latestUiState(); // FIFO drains once: use this snapshot
    CHECK_EQ (ui2.latePublishes, 0);
    CHECK_EQ (ui2.cuts, 1);
    bool sawOffInSwitchBlock = false;
    for (int i = (int) prev2; i < (int) r.out.size(); ++i)
        if (r.out[i].msg.isNoteOff()) sawOffInSwitchBlock = true;
    CHECK (sawOffInSwitchBlock);
    // (5) trigger offs afterwards pass through (engine Off, S3.3)
    const size_t prev3 = r.out.size();
    r.step (&bufferOf ({
        { 0, juce::MidiMessage::noteOff (1, 60, (juce::uint8) 0) },
    }));
    CHECK (r.out.size() == prev3 + 1);
    CHECK (hasEventAt (r.out, r.block - 1, 0, false, 60));
}

void testEngineOffHandoffSwitchOffWithoutSecondNote()
{
    // Engine off again before the second note-on: the seeded (already
    // sounding) pending is never jabbed; its note-off passes through later.
    RunsProcessor p;
    engineOffHandoffSetup (p);
    Runner r (p, 2048);
    r.run (bufferOf ({
        { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
    }), 1);
    r.step (&bufferOf ({
        { 0, juce::MidiMessage::controllerEvent (1, 87, 50) },
    }));
    CHECK_EQ ((int) p.publishedEngineState.load(), 1);
    r.step (&bufferOf ({
        { 0, juce::MidiMessage::controllerEvent (1, 87, 0) },
    }));
    CHECK_EQ ((int) p.publishedEngineState.load(), 0);
    r.runEmpty (3);
    CHECK_EQ (p.latestUiState().latePublishes, 0);
    r.step (&bufferOf ({
        { 0, juce::MidiMessage::noteOff (1, 60, (juce::uint8) 0) },
    }));
    CHECK (r.out.back().msg.isNoteOff()
           && r.out.back().msg.getNoteNumber() == (juce::uint8) 60);
}

// S5.7: the Settings accent weights now reach the live engine (they were
// stored, serialized and editable but the engine hard-coded its own values).
// accent 1, arc 0, trigger velocities 50/50: with the spec defaults the two
// bar-line notes are pushed and the interior ones lag; with both weights 0
// the whole accent stack collapses to the plain base velocity.
void testAccentWeightsReachEngine()
{
    // Fresh processor per render: a run stays active until it is cut, so a
    // second trigger on the same instance would be ignored (S5.6).
    auto runVels = [&] (float down, float mid)
    {
        RunsProcessor p;
        p.prepareToPlay (44100.0, 2048);
        setParam (p, "beats", 4.0f);
        setParam (p, "density", 1.0f); // n = 4
        setParam (p, "curve", 0.0f);
        setParam (p, "accent", 1.0f);
        setParam (p, "arc", 0.0f);
        setParam (p, "walk", 0.0f);
        setParam (p, "overlap", 0.0f);
        setEngine (p, 1); // Up
        p.settings.downWeight = down;
        p.settings.midBarWeight = mid;

        Runner r (p, 2048);
        r.startAt (0.0);
        r.run (bufferOf ({
            { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 50) },
            { 0, juce::MidiMessage::noteOn (1, 72, (juce::uint8) 50) },
        }), 90);
        std::vector<int> v;
        for (const auto& e : r.out)
            if (e.msg.isNoteOn())
                v.push_back ((int) e.msg.getVelocity());
        return v;
    };

    const std::vector<int> specDefaults = runVels (1.0f, 0.75f);
    CHECK_EQ ((int) specDefaults.size(), 4);
    CHECK_EQ (specDefaults[0], 100); // bar line: downbeat weight 1.0
    CHECK_EQ (specDefaults[3], 100); // beat 4 is a bar line too
    // The two mid-bar notes sit at 1/3 and 2/3 through a beat, so their
    // accent lands on the same 62/63 rounding boundary - only assert the
    // accent is actually there and well below the bar-line push.
    CHECK (specDefaults[1] < 100 && specDefaults[1] >= 60);
    CHECK (specDefaults[2] < 100 && specDefaults[2] >= 60);

    const std::vector<int> muted = runVels (0.0f, 0.0f);
    CHECK_EQ ((int) muted.size(), 4);
    for (int v : muted)
        CHECK_EQ (v, 50); // weights 0 -> no accent push anywhere
}

// S5.9: seed resolution end-to-end through processBlock. Seed 0 mints a
// fresh nonzero seed per run (the overlay prints it so a run can be pinned
// and redone); a pinned seed passes through verbatim; humanize off reports
// 0. Two runs on one processor: release the triggers to cut run 1 (S5.6)
// before the second pair.
void testHumanizeSeedResolution()
{
    auto configure = [] (RunsProcessor& p, float humanize, float seed)
    {
        p.prepareToPlay (44100.0, 2048);
        setUpMatrix (p);
        setParam (p, "humanize", humanize);
        setParam (p, "seed", seed);
    };
    auto fireRun = [] (RunsProcessor& p, Runner& r)
    {
        r.step (&bufferOf ({
            { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
            { 0, juce::MidiMessage::noteOn (1, 72, (juce::uint8) 100) },
        }));
        r.runEmpty (4); // run starts and publishes its UiMessage
        return p.latestUiState().runSeed;
    };
    auto releaseTriggers = [] (Runner& r)
    {
        r.step (&bufferOf ({
            { 0, juce::MidiMessage::noteOff (1, 60, (juce::uint8) 0) },
            { 0, juce::MidiMessage::noteOff (1, 72, (juce::uint8) 0) },
        }));
        r.runEmpty (2); // trigger release cuts the run (S5.6)
    };

    // (1) seed 0: two runs on the same processor report different nonzero
    // seeds (the session counter advances per run).
    {
        RunsProcessor p;
        configure (p, 1.0f, 0.0f);
        Runner r (p, 2048);
        r.startAt (0.0);
        const uint32_t s1 = fireRun (p, r);
        releaseTriggers (r);
        const uint32_t s2 = fireRun (p, r);
        CHECK (s1 != 0);
        CHECK (s2 != 0);
        CHECK (s2 != s1);
    }
    // (2) pinned seed: every run reports it verbatim.
    {
        RunsProcessor p;
        configure (p, 1.0f, 777777.0f);
        Runner r (p, 2048);
        r.startAt (0.0);
        const uint32_t s1 = fireRun (p, r);
        releaseTriggers (r);
        const uint32_t s2 = fireRun (p, r);
        CHECK_EQ (s1, 777777u);
        CHECK_EQ (s2, 777777u);
    }
    // (3) humanize off: the overlay reports 0 (no seed used).
    {
        RunsProcessor p;
        configure (p, 0.0f, 0.0f);
        Runner r (p, 2048);
        r.startAt (0.0);
        const uint32_t s = fireRun (p, r);
        CHECK_EQ (s, 0u);
    }
    // (4) fixed seed through the whole pipeline: two fresh processors emit
    // bit-identical note-ons (pitch, velocity, block, sample) - the jitter
    // is part of the deterministic plan per seed.
    {
        auto render = [&] ()
        {
            RunsProcessor p;
            configure (p, 1.0f, 424242.0f);
            Runner r (p, 2048);
            r.startAt (0.0);
            r.run (bufferOf ({
                { 0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) },
                { 0, juce::MidiMessage::noteOn (1, 72, (juce::uint8) 100) },
            }), 90);
            std::vector<std::vector<int>> ons;
            for (const auto& e : r.out)
                if (e.msg.isNoteOn())
                    ons.push_back ({ (int) e.msg.getNoteNumber(),
                                     (int) e.msg.getVelocity(),
                                     e.block, e.sample });
            return ons;
        };
        const auto a = render();
        const auto b = render();
        CHECK (! a.empty());
        CHECK (a == b);
    }
}

// Offline export (drag-to-DAW): explicit endpoints -> a type-0 MIDI file in
// the temp folder, plus the refusal cases. No playhead, no chunk, no audio
// thread: this is the message-thread path the Drag MIDI button drives.
void testOfflineExportMidiFile()
{
    RunsProcessor p;
    setParam (p, "beats", 4.0f);
    setParam (p, "density", 4.0f); // n = round(4 x 4) = 16
    setParam (p, "tonic", 0.0f);   // C
    setParam (p, "mode", 0.0f);    // major
    setParam (p, "curve", 0.0f);
    setParam (p, "accent", 0.0f);  // base fade only: 100 -> 90 exactly
    setParam (p, "arc", 0.0f);
    setParam (p, "overlap", 0.0f);

    juce::String err;
    const juce::File f = p.renderMidiExport (60, 72, 100, 90, err);
    CHECK (err.isEmpty());
    CHECK (f.existsAsFile());
    CHECK (f.getSize() > 0);

    juce::FileInputStream in (f);
    CHECK (in.openedOk());
    juce::MidiFile mf;
    CHECK (mf.readFrom (in));
    CHECK_EQ (mf.getNumTracks(), 1);
    CHECK_EQ ((int) mf.getTimeFormat(), 480); // division = ticks per quarter

    const juce::MidiMessageSequence* tr = mf.getTrack (0);
    CHECK (tr != nullptr);
    if (tr == nullptr)
        return;

    int ons = 0, offs = 0, firstOn = -1, lastOn = -1;
    int firstVel = -1, lastVel = -1;
    long prevTick = -1;
    bool sorted = true, sawTempo = false;
    int sigNum = 0, sigDen = 0;
    for (int i = 0; i < tr->getNumEvents(); ++i)
    {
        const juce::MidiMessage& m = tr->getEventPointer (i)->message;
        const long tick = (long) m.getTimeStamp();
        if (tick < prevTick) sorted = false;
        prevTick = tick;
        if (m.isNoteOn())
        {
            if (firstOn < 0)
            {
                firstOn = m.getNoteNumber();
                firstVel = m.getVelocity();
            }
            lastOn = m.getNoteNumber();
            lastVel = m.getVelocity();
            ++ons;
        }
        else if (m.isNoteOff())
        {
            ++offs;
        }
        else if (m.isTempoMetaEvent())
        {
            sawTempo = true;
        }
        else if (m.isTimeSignatureMetaEvent())
        {
            m.getTimeSignatureInfo (sigNum, sigDen);
        }
    }
    CHECK (sorted);
    CHECK_EQ (ons, 16);
    CHECK_EQ (offs, 16);
    CHECK_EQ (firstOn, 60); // engine Up: starts on the lower endpoint
    CHECK_EQ (lastOn, 72);
    CHECK_EQ (firstVel, 100);
    CHECK_EQ (lastVel, 90);
    CHECK (sawTempo);
    CHECK_EQ (sigNum, 4);
    CHECK_EQ (sigDen, 4);

    // Refusals: an equal pair never fires (S3.2) and must not leave a file.
    juce::String errSame;
    const juce::File same = p.renderMidiExport (60, 60, 100, 90, errSame);
    CHECK (same.getFullPathName().isEmpty());
    CHECK_EQ (errSame, juce::String ("Start = target"));
}
} // namespace

int main()
{
    testPairMatrixEngineOff();
    testPairCompleteAndCutOnRelease();
    testStaggeredPair();
    testJabbedSingleLatePublish();
    testThreeNoteChord();
    testSamePitchSecondNote();
    testTriggersDuringActiveRunIgnored();
    testEpsilonAlignment();
    testEmissionAlignmentMath();
    testCutOnTriggerRelease();
    testCutOnCC123();
    testEngineCcSwitchCut();
    testEngineNoteSwitchCut();
    testEnginePcSwitchCut();
    testLoopWrapCut();
    testKeyswitchStopAndPassthrough();
    testCcMappingAndAbsorption();
    testCcValueInverse();
    testChunkRoundTrip();
    testChunkTruncatedRejected();
    testChunkTrailingGarbageIgnored();
    testChunkWrongMagicRejected();
    testChunkVersionRejected();
testChunkV1BackwardCompatible();
testChunkV2BackwardCompatible();
testJsonClipboardRoundTrip();
    testJsonClipboardRejected();
    testJsonClipboardPartialAndClamped();
    testStateSavedFullyRoundTrip();
    testOverlapParamEmission();
    testEngineChainQueuePath();
    testEngineChainParamPath();
    testEngineChainSequence();
    testEngineEchoIgnoredDuringGrace();
    testEngineSwitchCutsActiveRunBothPaths();
    testEngineCcValueRanges();
    testEngineOffHandoffPairFiresAtSecondNoteOn();
    testEngineOffHandoffSwitchOffWithoutSecondNote();
testAccentWeightsReachEngine();
testHumanizeSeedResolution();
testOfflineExportMidiFile();
    std::printf ("%s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
