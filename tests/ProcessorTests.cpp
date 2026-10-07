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
// spacing), accent 0 (flat velocities), 2048-sample blocks at 44.1 kHz.
void setUpMatrix (RunsProcessor& p)
{
    p.prepareToPlay (44100.0, 2048);
    setParam (p, "density", 2.0f);
    setParam (p, "curve", 0.0f);
    setParam (p, "accent", 0.0f);
    setEngine (p, 1);
}

void testPairMatrixEngineOff()
{
    // (a) engine Off: the pair passes through untouched (S3.3).
    RunsProcessor p;
    p.prepareToPlay (44100.0, 2048);
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
    // now Off: keyswitches pass through (S3.1)
    setEngine (p, 0);
    r.step (nullptr); // engine param change -> Off
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

    // engine Off: bound CCs pass through inertly (S3.3)
    RunsProcessor p2;
    p2.prepareToPlay (44100.0, 2048);
    Runner r2 (p2, 2048);
    r2.run (bufferOf ({
        { 3, juce::MidiMessage::controllerEvent (1, 89, 64) },
    }), 0);
    CHECK (hasCcAt (r2.out, 0, 3, 89));
    CHECK_NEAR (paramReal (p2, "density"), 4.0f, 1e-4);

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
    p.recordEditorWindowSize (1000, 500);
}

bool chunkValuesRetained (RunsProcessor& a, RunsProcessor& b)
{
    bool ok = true;
    const char* ids[RunsProcessor::kNumParams] = { "engine", "beats", "density",
        "curve", "accent", "arc", "tonic", "mode", "walk" };
    for (int i = 0; i < RunsProcessor::kNumParams; ++i)
        if (std::fabs (paramReal (a, ids[i]) - paramReal (b, ids[i])) > 1e-4f)
            ok = false;
    if (a.settings.engineSourceType != b.settings.engineSourceType
        || a.settings.engineNumber != b.settings.engineNumber
        || a.settings.epsilonMs != b.settings.epsilonMs
        || a.settings.gateFraction != b.settings.gateFraction
        || a.settings.downWeight != b.settings.downWeight
        || a.settings.midBarWeight != b.settings.midBarWeight
        || a.settings.customOffsets != b.settings.customOffsets)
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
        "curve", "accent", "arc", "tonic", "mode", "walk" };
    const float defaults[RunsProcessor::kNumParams] = { 0, 4, 4, 0.5f, 0.5f, 0, 0, 0, 0 };
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
        && p.settings.customOffsets == d.customOffsets;
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
    std::memcpy (blob + 4, &v, 4); // version 999: reject, keep state
    b.setStateInformation (blob, (int) data.getSize());
    CHECK (allDefaults (b));
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
    testChunkRoundTrip();
    testChunkTruncatedRejected();
    testChunkTrailingGarbageIgnored();
    testChunkWrongMagicRejected();
    testChunkVersionRejected();
    std::printf ("%s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
