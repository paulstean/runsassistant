#include "PluginProcessor.h"
#include "../editor/PluginEditor.h"

#include <cmath>
#include <random>

namespace
{
// Mode list (S9): single source of truth is Source/core/ScaleModel.cpp; the
// "Custom" display name comes from runsp::modeName (16 = Custom).
constexpr int kNumModes = 16; // + index 16 = "Custom"

const char* kTonicNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

juce::String modeName (int index)
{
    if (index < 0 || index > kNumModes)
        return {};
    return index < kNumModes ? juce::String (runsp::modeName (index))
                             : juce::String ("Custom");
}

// Mode CC mapping constant: floor(cc * nModes / 128) with nModes = 17 (16
// named modes plus Custom), D13.
constexpr int kModeCcSteps = 17;

constexpr int kCcNone = 255;          // S4 "none" binding sentinel
constexpr int kCcAllNotesOff = 123;   // S5.6 cut source

// S7 chunk layout: magic "RUN1" + u32 schema_version + raw parameter
// values (9 in v1, 10 from v2: Overlap, 12 from v3: Humanize + Seed) +
// settings (source type + engine numbers + 8 bindings + 4 tuning constants
// + custom mask + 2 humanize tunables from v3) + optional trailing RUNV
// window-size footer. v1 chunks load with Overlap off, v1/v2 chunks load
// with Humanize off and the default tunables (S7 range-clamps +
// backwards-compatible read).
constexpr uint8_t kChunkMagic[4]  = { 'R', 'U', 'N', '1' };
constexpr uint8_t kFooterMagic[4] = { 'R', 'U', 'N', 'V' };
constexpr uint32_t kChunkVersion = 3;
constexpr uint32_t kChunkMinVersion = 1; // v1 chunks load (defaults fill in)
constexpr int kParamsV1 = 9;
constexpr int kParamsV2 = 10;
constexpr int kSettingsBytes = 1                    // source type
    + 1                                             // engine CC number
    + 3 + 3                                         // engine notes / PCs
    + 8                                             // bound CC table
    + 4 * 4                                         // tuning constants
    + 2;                                            // custom tick set
constexpr int kSettingsBytesV3 = kSettingsBytes
    + 4 + 4;                                        // S5.9 humanize tunables
constexpr int kChunkMinSize = 4                     // magic
    + 4                                             // u32 version
    + kParamsV1 * 4                                 // v1 parameter values (min)
    + kSettingsBytes;

// Parameter ids in GUI order (S8): the one list used by the ctor, the
// clipboard JSON and any other id-driven sweep.
const char* const kParamIds[RunsProcessor::kNumParams] = {
    "engine", "beats", "density", "curve", "accent", "arc",
    "tonic", "mode", "walk", "overlap", "humanize", "seed"
};

// Clipboard JSON schema version (S7/S9); bumped when the payload shape
// changes, older versions still load (chunk-style forward/backward rule).
// v2 adds Humanize/Seed params and the two humanize settings keys.
constexpr int kClipboardJsonVersion = 2;

void writeU32 (juce::MemoryOutputStream& mo, uint32_t v) { mo.write (&v, 4); }

uint32_t readU32 (const uint8_t* d)
{
    uint32_t v;
    std::memcpy (&v, d, 4);
    return v;
}

void writeF32 (juce::MemoryOutputStream& mo, float v) { mo.write (&v, 4); }

float readF32 (const uint8_t* d)
{
    uint32_t u;
    std::memcpy (&u, d, 4);
    float v;
    std::memcpy (&v, &u, 4);
    return v;
}

void writeI16 (juce::MemoryOutputStream& mo, int16_t v) { mo.write (&v, 2); }

int16_t readI16 (const uint8_t* d)
{
    int16_t v;
    std::memcpy (&v, d, 2);
    return v;
}

int clampByte (int v) { return juce::jlimit (0, 127, v); }
} // namespace

// Bus layout per personality (specification.md S2: MIDI-only with at most one
// silent passthrough bus where the format demands one). RUNS_NO_AUDIO_INPUT is
// set for the aumi target (see CMakeLists.txt): kAudioUnitType_MIDIProcessor
// reports 0 audio input channels to the host, so a contradicting stereo input
// bus makes auval reject the component and Logic hides it from the MIDI FX
// menu. The MIDI FX slot carries no audio at all.
RunsProcessor::RunsProcessor()
    : AudioProcessor (BusesProperties()
   #if defined(RUNS_NO_AUDIO_INPUT)
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
   #else
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)
   #endif
      ),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    for (int i = 0; i < kNumParams; ++i)
    {
        ranged[i] = dynamic_cast<juce::RangedAudioParameter*> (
            apvts.getParameter (kParamIds[i]));
        rawParam[i] = apvts.getRawParameterValue (kParamIds[i]);
    }
    // live[] starts at the factory defaults (S4); it tracks GUI edits, host
    // automation and bound CCs last-writer-wins (S4).
    for (int i = 0; i < kNumParams; ++i)
        live[i] = ranged[i] != nullptr
                      ? ranged[i]->convertFrom0to1 (ranged[i]->getDefaultValue())
                      : 0.0f;

    // S5.9: session salt for seed 0 (fresh seed each run). Message thread,
    // so std::random_device is fine here; the audio thread only ever does
    // fetch_add + splitmix32 on the result.
    {
        std::random_device rd;
        uint32_t s = (uint32_t) rd() ^ 0xA5A5A5A5u;
        if (s == 0) s = 1;
        humanizeSeedSalt = s;
    }

    runSink.data = runSinkData;
    runSink.cap = kOutCap;
    runSink.count = 0;

    // Belt-and-braces engine switch path (see engineQueue comment): any
    // message-thread change of the "engine" parameter (GUI click, bound-CC
    // mirror, host automation) enqueues the new switch state; processBlock
    // drains the queue before pass 1 (D14 ordering).
    struct EngineParamListener
        : juce::AudioProcessorValueTreeState::Listener
    {
        explicit EngineParamListener (RunsProcessor& p) : owner (p) {}
        void parameterChanged (const juce::String& paramID, float) override
        {
            if (paramID != "engine")
                return;
            owner.diagEngineListenerHits.store (
                owner.diagEngineListenerHits.load (std::memory_order_relaxed)
                    + 1,
                std::memory_order_relaxed);
            // P4 hardening FIX: this listener no longer enqueues into
            // engineQueue. A message-thread listener also fires on host
            // params echoes (REAPER pushes cached discrete-param values back
            // on UI focus events, outside any grace window): the echo
            // re-entered through the queue, reverted the engine state and
            // flipped the GUI highlight (reported: "clicking Up highlights
            // Down", sw:2998). Param-origin switches are now detected by the
            // AUDIO thread only (syncLiveValues raw-atomics change probe),
            // which sits behind the host-echo grace window; genuine host
            // automation still applies once the window passes (S5.6/S11).
        }
        RunsProcessor& owner;
    };
    apvtsListener = std::make_unique<EngineParamListener> (*this);
    apvts.addParameterListener ("engine", apvtsListener.get());
}

RunsProcessor::~RunsProcessor()
{
    apvts.removeParameterListener ("engine", apvtsListener.get());
    apvtsListener.reset();
    stopTimer();
}

juce::AudioProcessorValueTreeState::ParameterLayout
RunsProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "engine", 1 }, "Engine",
        juce::StringArray { "Off", "Up", "Down" }, 1));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "beats", 1 }, "Beats", 1, 16, 4));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "density", 1 }, "Density",
        juce::NormalisableRange<float> (1.0f, 16.0f, 0.0f, 1.0f), 5.0f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "curve", 1 }, "Curve",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.22f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "accent", 1 }, "Accent",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.41f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "arc", 1 }, "Arc",
        juce::NormalisableRange<float> (-1.0f, 1.0f), 0.15f));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "tonic", 1 }, "Tonic", 0, 11, 0,
        juce::AudioParameterIntAttributes()
            .withStringFromValueFunction (
                [] (int v, int) { return juce::String (kTonicNames[v]); })));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "mode", 1 }, "Mode", 0, kNumModes, 0,
        juce::AudioParameterIntAttributes()
            .withStringFromValueFunction (
                [] (int v, int) { return modeName (v); })));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "walk", 1 }, "Walk",
        juce::StringArray { "Fold", "Zig-zag" }, 1));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "overlap", 1 }, "Overlap", true,
        juce::AudioParameterBoolAttributes()
            .withStringFromValueFunction (
                [] (bool v, int) { return juce::String (v ? "On" : "Off"); })));
    // S5.9: humanize toggle + seed. Seed 0 = fresh seed per run (resolved
    // at run start, never stored); any other value reproduces that run.
    // Neither is CC-mappable in v1 (like Overlap - no mirror row, no badge).
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "humanize", 1 }, "Humanize", false,
        juce::AudioParameterBoolAttributes()
            .withStringFromValueFunction (
                [] (bool v, int) { return juce::String (v ? "On" : "Off"); })));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "seed", 1 }, "Seed", 0, 999999, 0));
    return layout;
}

void RunsProcessor::prepareToPlay (double newSampleRate, int newSamplesPerBlock)
{
    sampleRate_ = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    lastBlockSamples_ = newSamplesPerBlock;
    // Preallocated scratch reset; nothing is allocated here (S6.2). All
    // buffers are fixed-capacity members sized at construction.
    pairs.resetAll();
    engine.cancel();
    for (int c = 0; c < 16; ++c) offHandoff[c] = {};
    inCount = 0;
    outCount = 0;
    runSink.count = 0;
    runSink.overflow = false;
    inputDrops_.store (0, std::memory_order_relaxed);
    outDrops_.store (0, std::memory_order_relaxed);
}

bool RunsProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // RUNS_NO_AUDIO_INPUT builds (aumi) declare no input bus at all:
    // getMainInputChannelSet() indexes inputBuses[0], so test emptiness first.
    if (layouts.inputBuses.isEmpty())
        return true;

    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        || layouts.getMainInputChannelSet().isDisabled();
}

// --------------------------------------------------------------------------
// Parameters: ranges (S8), CC mapping (D13), mirrors (S4/S11)

float RunsProcessor::realValueMinOf (int paramIndex)
{
    static constexpr float mins[kNumParams] =
        { 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    return paramIndex >= 0 && paramIndex < kNumParams ? mins[paramIndex] : 0.0f;
}

float RunsProcessor::realValueMaxOf (int paramIndex)
{
    static constexpr float maxs[kNumParams] =
        { 2.0f, 16.0f, 16.0f, 1.0f, 1.0f, 1.0f, 11.0f, (float) kNumModes,
          1.0f, 1.0f, 1.0f, 999999.0f };
    return paramIndex >= 0 && paramIndex < kNumParams ? maxs[paramIndex] : 0.0f;
}

bool RunsProcessor::paramIsDiscrete (int paramIndex)
{
    return paramIndex == kEngineIndex // engine choice
        || paramIndex == 1            // beats (int)
        || paramIndex == 6            // tonic (int)
        || paramIndex == 7            // mode (int indexed)
        || paramIndex == 8            // walk (choice)
        || paramIndex == kOverlapIndex   // overlap (bool)
        || paramIndex == kHumanizeIndex  // humanize (bool)
        || paramIndex == kSeedIndex;     // seed (int)
}

void RunsProcessor::markStateDirty()
{
    // S7: chunk-only edits (settings dialog, custom tick set) mark the host
    // state dirty. The non-parameter-changed path is what reaches the
    // host's dirty flags (VST3 setDirty / CLAP state-mark-dirty). The bare
    // updateHostDisplay() default flags do NOT set nonParameterStateChanged,
    // so plain-chunk edits were never dirtying VST3 projects (REAPER report:
    // "plugin state is not always saved").
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails{}
                          .withNonParameterStateChanged (true));
}

void RunsProcessor::syncLiveValues()
{
    // Last-writer-wins between GUI, host automation and bound CCs (S4):
    // changed raw parameter values refresh live[]; CC edits write live[]
    // directly on the audio thread (audio-synced engine reads) and queue a
    // mirror request for the dirtying path (S11). For the engine parameter a
    // NEW raw value means a GUI / automation edit (S5.6 cut condition); a
    // stale CC mirror must not flip the state.
    engineParamChanged = false;
    for (int i = 0; i < kNumParams; ++i)
    {
        // Audio-thread-safe read: getValue() is not thread-safe; the APVTS
        // raw parameter is an atomic mirror (it stores the PLAIN value, so
        // re-normalize before comparing with the seen[] cache).
        float norm = ranged[i]->getValue();
        if (rawParam[i] != nullptr)
            norm = ranged[i]->convertTo0to1 (
                rawParam[i]->load (std::memory_order_relaxed));
        if (norm != normSeen[i])
        {
            normSeen[i] = norm;
            live[i] = ranged[i]->convertFrom0to1 (norm);
            if (i == kEngineIndex)
            {
                engineParamChanged = true;
                diagEngineParamChanges.store (
                    diagEngineParamChanges.load (std::memory_order_relaxed) + 1,
                    std::memory_order_relaxed);
            }
        }
    }
}

void RunsProcessor::applyBoundCc (int ccNumber, int ccValue, int sampleOffset)
{
    int binding = -1;
    for (int b = 0; b < 8; ++b)
        if (settings.boundCC[b] == ccNumber) { binding = b; break; }
    if (binding < 0)
        return; // not a bound CC; the caller passes it through (S4)

    const int paramIndex = binding + 1; // GUI order: beats..walk (D13)
    const double c = (double) ccValue;
    float real = 0.0f;
    switch (binding)
    {
        case 0:  // Beats: linear CC 0..127 -> 1..16, rounded to the int param
            real = (float) juce::jlimit (1, 16, (int) std::lround (1.0 + c / 127.0 * 15.0));
            break;
        case 1:  // Density: continuous 1..16 notes/beat
            real = (float) (1.0 + c / 127.0 * 15.0); break;
        case 2:  // Curve 0..1
            real = (float) (c / 127.0); break;
        case 3:  // Accent 0..1
            real = (float) (c / 127.0); break;
        case 4:  // Arc -1..+1
            real = (float) (c / 127.0 * 2.0 - 1.0); break;
        case 5:  // Tonic: floor(cc * 12 / 128) (D13)
            real = (float) ((ccValue * 12) / 128); break;
        case 6:  // Mode: floor(cc * 17 / 128), 16 named + Custom (D13)
            real = (float) ((ccValue * kModeCcSteps) / 128); break;
        case 7:  // Walk: 0..63 Fold, 64..127 Zig-zag (D13)
            real = ccValue < 64 ? 0.0f : 1.0f; break;
        default: return; // unreachable: the bindings table has 8 entries
    }
    if (paramIsDiscrete (paramIndex))
        real = (float) juce::jlimit ((int) realValueMinOf (paramIndex),
                                     (int) realValueMaxOf (paramIndex),
                                     (int) std::lround (real));
    else
        real = juce::jlimit (realValueMinOf (paramIndex),
                             realValueMaxOf (paramIndex), real);

    // The engine reads the new value THIS block (last-writer-wins, S4);
    // the parameter itself is only dirtied from the message thread (S11).
    live[paramIndex] = real;
    normSeen[paramIndex] = ranged[paramIndex]->convertTo0to1 (real);
    mirrorFifo.push ({ paramIndex, real, paramIsDiscrete (paramIndex) });
    juce::ignoreUnused (sampleOffset);
}

int RunsProcessor::ccValueForBinding (int binding, float realValue) const
{
    // Inverse of applyBoundCc (the editor's CC badges show this number):
    // returns the CC value in the MIDDLE of the 0..127 bucket that
    // reproduces realValue - the bucket centre for the rounded/floor/threshold
    // maps, the nearest sample for the linear ones. Keep in sync with
    // applyBoundCc; tests/ProcessorTests.cpp testCcValueInverse round-trips
    // the two against each other.
    if (binding < 0 || binding >= 8)
        return -1;
    const int paramIndex = binding + 1; // GUI order: beats..walk (D13)
    const float v = juce::jlimit (realValueMinOf (paramIndex),
                                  realValueMaxOf (paramIndex), realValue);
    int cc = 0;
    switch (binding)
    {
        case 0:  // Beats 1..16 <- jlimit(1,16, round(1 + c*15/127))
        case 1:  // Density 1..16 <- 1 + c*15/127
            cc = juce::roundToInt ((v - 1.0f) * 127.0f / 15.0f);
            break;
        case 2:  // Curve 0..1 <- c/127
        case 3:  // Accent 0..1 <- c/127
            cc = juce::roundToInt (v * 127.0f);
            break;
        case 4:  // Arc -1..+1 <- c/127*2 - 1
            cc = juce::roundToInt ((v + 1.0f) * 127.0f / 2.0f);
            break;
        case 5:  // Tonic t <- floor(c*12/128) (D13): bucket centre
            cc = juce::roundToInt ((v + 0.5f) * 128.0f / 12.0f - 0.5f);
            break;
        case 6:  // Mode m <- floor(c*17/128) (D13): bucket centre
            cc = juce::roundToInt ((v + 0.5f) * 128.0f / (float) kModeCcSteps
                                   - 0.5f);
            break;
        default: // Walk: 0..63 Fold, 64..127 Zig-zag (D13) - bucket centres
            cc = v < 0.5f ? 32 : 96; // round(31.5) / round(95.5)
            break;
    }
    return juce::jlimit (0, 127, cc);
}

int RunsProcessor::ccValueForEngineState (int engineState)
{
    // Inverse of engineStateFromCcValue: the middle of each S3.1 bucket.
    switch (juce::jlimit (0, 2, engineState))
    {
        case 1:  return 60;   // Up   (41..79)
        case 2:  return 104;  // Down (80..127)
        default: return 20;   // Off  (0..40)
    }
}

void RunsProcessor::applyPendingCcMirrors()
{
    // S11 mitigation: CC edits dirty the parameter like GUI edits, but only
    // when the value actually changes (no double-triggered saves).
    runsp::CcMirrorRequest m;
    while (mirrorFifo.pop (m))
    {
        if (m.paramIndex < 0 || m.paramIndex >= kNumParams) continue;
        auto* p = ranged[m.paramIndex];
        if (p == nullptr) continue;
        const float cur = p->convertFrom0to1 (p->getValue());
        const bool changed = m.discrete
            ? ((int) std::lround (cur) != (int) std::lround (m.realValue))
            : (std::fabs (cur - m.realValue) > 1e-4f);
        if (changed)
            p->setValueNotifyingHost (p->convertTo0to1 (m.realValue));
    }
}

void RunsProcessor::startCcMirrorTimer()
{
    // Message-thread Timer draining the mirror FIFO. Started when an editor
    // exists (createEditor runs on the message thread); headless tests call
    // applyPendingCcMirrors() directly instead.
    startTimerHz (30);
}

void RunsProcessor::timerCallback()
{
    applyPendingCcMirrors();
}

void RunsProcessor::recordEditorWindowSize (int w, int h)
{
    // RUNV footer values (S7); the editor (P3) reports resize events here.
    if (w > 0) editorWindowW = juce::jlimit (1, 4096, w);
    if (h > 0) editorWindowH = juce::jlimit (1, 4096, h);
}

void RunsProcessor::getEditorWindowSize (int& w, int& h) const
{
    w = editorWindowW;
    h = editorWindowH;
}

runsp::UiMessage RunsProcessor::latestUiState()
{
    runsp::UiMessage m, last;
    bool any = false;
    while (uiFifo.pop (m)) { last = m; any = true; }
    if (! any)
        return runsp::UiMessage {};
    return last;
}

// --------------------------------------------------------------------------
// Scratch / emission helpers (fixed capacity, no allocation)

void RunsProcessor::collectInput (const juce::MidiBuffer& midi)
{
    // S6.1 step 2: stable stream order; overflow drops with a counter.
    inCount = 0;
    inputEventCount_.store (inputEventCount_.load (std::memory_order_relaxed)
                                + (long long) (int) midi.getNumEvents(),
                            std::memory_order_relaxed);
    for (const auto& e : midi)
    {
        if (inCount >= kInputCap
            || e.numBytes > (int) sizeof (MidiScratchItem::bytes))
        {
            inputDrops_.store (inputDrops_.load (std::memory_order_relaxed) + 1,
                               std::memory_order_relaxed);
            continue;
        }
        auto& it = inScratch[inCount];
        it.sample = e.samplePosition;
        it.size = e.numBytes;
        std::memcpy (it.bytes, e.data, (size_t) e.numBytes);
        inConsumed[inCount] = false;
        ++inCount;
    }
}

double RunsProcessor::samplesPerBeatFor (const PlayheadAdapter::Block& blk) const
{
    const double bpm = blk.bpm > 0.0 ? blk.bpm : 120.0;
    return sampleRate_ > 0.0 ? 60.0 * sampleRate_ / bpm : 22050.0;
}

double RunsProcessor::beatAtSample (const PlayheadAdapter::Block& blk,
                                    int sample) const
{
    return blk.b0 + (double) sample / samplesPerBeatFor (blk); // S5.1
}

int RunsProcessor::sampleAtBeat (const PlayheadAdapter::Block& blk,
                                 double beat) const
{
    // S5.1 exact offset = (onset - B0) * 60 * sampleRate / bpm.
    return (int) std::floor ((beat - blk.b0) * samplesPerBeatFor (blk) + 1e-9);
}

bool RunsProcessor::pushOut (int sample, const uint8_t* data, int size)
{
    if (outCount >= kOutCap || size < 1 || size > (int) sizeof (MidiScratchItem::bytes))
    {
        outDrops_.store (outDrops_.load (std::memory_order_relaxed) + 1,
                         std::memory_order_relaxed);
        return false;
    }
    auto& it = outScratch[outCount++];
    it.sample = sample;
    it.size = size;
    std::memcpy (it.bytes, data, (size_t) size);
    return true;
}

void RunsProcessor::pushNoteMsg (bool on, int channel, int pitch, int velocity,
                                 int sampleOffset)
{
    const int ch = juce::jlimit (1, 16, channel);
    uint8_t bytes[3];
    bytes[0] = (uint8_t) ((on ? 0x90 : 0x80) | (ch - 1));
    bytes[1] = (uint8_t) juce::jlimit (0, 127, pitch);
    bytes[2] = on ? (uint8_t) juce::jlimit (1, 127, velocity) : (uint8_t) 0;
    pushOut (sampleOffset, bytes, 3);
}

void RunsProcessor::pushRunEvent (const runsp::RunEvent& e,
                                  const PlayheadAdapter::Block& blk,
                                  double samplesPerBeat, int numSamples)
{
    // S5.1: sample offset = (onset - B0) * 60 * sampleRate / bpm; events at
    // a block boundary land at the very end of the covering block.
    int sample = (int) std::floor ((e.beat - blk.b0) * samplesPerBeat + 1e-9);
    const int hi = numSamples > 0 ? numSamples - 1 : 0;
    sample = juce::jlimit (0, hi, sample);
    pushNoteMsg (e.kind == runsp::RunEvent::NoteOn, e.channel, e.pitch,
                 e.velocity, sample);
}

void RunsProcessor::emitLatePublish (int channel, int pitch, int velocity,
                                     int sampleOffset)
{
    // S5.2 late publish: note-on followed immediately by note-off, at the
    // note-off's offset (jab) or the flush offset (engine switch). Counted
    // by the tracker and surfaced via the UI FIFO.
    pushNoteMsg (true, channel, pitch, velocity, sampleOffset);
    pushNoteMsg (false, channel, pitch, velocity, sampleOffset);
}

void RunsProcessor::requestEngineState (int index)
{
    // S3.1 GUI three-state switch: queued directly into the switch queue
    // (the audio thread applies it before pass 1, D14; REAPER echoing the
    // host-cached param back must not revert it - see echoGraceBlocks).
    // Issue report: "selecting Up or Down doesn't mark the project as
    // modified". The switch now ALSO mirrors into the engine parameter
    // (same writer as every other GUI edit): the host sees the automation
    // (performance path marks the edit, project becomes modified) and the
    // saved chunk carries the real engine state instead of a stale value.
    // Echo safety: the param now always tracks the real engine state, so a
    // later host echo of the same value dedups in syncLiveValues /
    // doEngineChange; the grace window covers intermediate timing.
    if (index < 0 || index > 2)
        return;
    diagEngineListenerHits.store (
        diagEngineListenerHits.load (std::memory_order_relaxed) + 1,
        std::memory_order_relaxed); // count direct GUI pushes too
    engineQueue.push (index);
    if (auto* p = ranged[kEngineIndex])
    {
        if (p->convertFrom0to1 (p->getValue()) != (float) index)
            p->setValueNotifyingHost (p->convertTo0to1 ((float) index));
    }
    updateHostDisplay (juce::AudioProcessorListener::ChangeDetails{}
                          .withNonParameterStateChanged (true));
}

int RunsProcessor::engineStateFromCcValue (int value)
{
    // S3.1/D13 engine CC ranges: 0-40 Off, 41-79 Up, 80-127 Down. All values
    // select a state now (nothing is "ignored" any more).
    if (value < 41) return 0;
    if (value < 80) return 1;
    return 2;
}

void RunsProcessor::doEngineChange (int newState, int sampleOffset, double beatNow)
{
    // S5.2/S5.6: engine state change flushes pending notes (late publish)
    // and cuts the active run.
    if (newState < 0 || newState > 2 || newState == engineState_)
        return;
    const bool turningOn = engineState_ == 0; // handoff seed below (S3.3)
    engine.cut (beatNow, runSink);
    runsp::PairOutcome outs[16];
    int flushed = 0;
    pairs.flushAll (outs, 16, &flushed);
    for (int k = 0; k < flushed; ++k)
        emitLatePublish (outs[k].channel + 1, outs[k].a.pitch,
                         outs[k].a.velocity, sampleOffset);
    pairs.clearAll(); // state only; latePublishes counter survives (S5.8)
    engineState_ = newState;
    if (turningOn)
    {
        // Engine-off handoff (S3.3): note-ons that passed through while the
        // engine was Off become the channel's pending note, so the next
        // note-on starts the run from it. Passed-through pendings never jab
        // (they already sounded). Stale latches are dropped either way.
        for (int c = 0; c < 16; ++c)
        {
            if (offHandoff[c].held)
                pairs.seedPassedPending (c, offHandoff[c].pitch,
                                         offHandoff[c].vel,
                                         offHandoff[c].beat);
            offHandoff[c] = {};
        }
    }
    diagEngineSwitches.store (diagEngineSwitches.load (std::memory_order_relaxed)
                              + 1, std::memory_order_relaxed);
    // S11 mirror: switch sources that bypass the GUI parameter (bound engine
    // CC, keyswitches, program changes) must still keep the engine parameter
    // in sync so the chunk carries the real state at save time (S7) and the
    // host is notified (project dirty). The message-thread applier marks it
    // dirty ONLY when the value actually changes (S11/S13.9). Host-automation
    // origin: the param already holds this value and the applier skips it.
    // FIFO overflow is counted in mirrorFifo.dropped (S9 overlay); the next
    // switch retries.
    mirrorFifo.push ({ kEngineIndex, (float) newState, true });
    // Host echo guard: param-driven detections in the grace window right
    // after a switch are ignored (see echoGraceBlocks). Scaled to sampleRate.
    echoGraceBlocks.store (juce::jlimit (4, 480,
        (int) (0.5 * sampleRate_ / juce::jmax (1.0, (double) lastBlockSamples_))),
        std::memory_order_relaxed);
    // NOTE: the engine parameter is intentionally NOT written back (no
    // mirror push). REAPER echoes host-cached discrete-param values back on
    // UI focus changes and the write->echo->flip loop made sw run away
    // (sw:2998). The param now follows external writes only; the GUI mirrors
    // the real engine state from the UI FIFO instead (S3.1/S11).
}

void RunsProcessor::mergeEmitAndFlushToHost (juce::MidiBuffer& midi)
{
    // Stable insertion sort by sample offset (stream order preserved within
    // equal offsets; scratch is small; no allocation, S6.2).
    for (int i = 1; i < outCount; ++i)
    {
        const MidiScratchItem key = outScratch[i];
        int j = i - 1;
        while (j >= 0 && outScratch[j].sample > key.sample)
        {
            outScratch[j + 1] = outScratch[j];
            --j;
        }
        outScratch[j + 1] = key;
    }
    midi.clear();
    for (int i = 0; i < outCount; ++i)
        midi.addEvent (outScratch[i].bytes, outScratch[i].size,
                       outScratch[i].sample);
}

// --------------------------------------------------------------------------
// Chunk state per S7: RUN1 magic, version, 9 raw values, settings, optional
// RUNV footer. Runtime state (pending pair / run / PRNG) is never serialized.

void RunsProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // S7: flush pending CC/switch mirrors first so the chunk always carries
    // the LAST held control value (e.g. a bound-CC tweak with the editor
    // closed would otherwise save the stale parameter).
    applyPendingCcMirrors();

    destData.setSize (0, true);
    juce::MemoryOutputStream mo (destData, false);
    mo.write (kChunkMagic, 4);
    writeU32 (mo, kChunkVersion);

    // All parameters as raw (real) values (S7), including Humanize/Seed
    // from schema v3 - the resolved run seed itself is never serialized.
    for (int i = 0; i < kNumParams; ++i)
        writeF32 (mo, ranged[i]->convertFrom0to1 (ranged[i]->getValue()));

    // Settings.
    mo.writeByte ((char) juce::jlimit (0, 2, settings.engineSourceType));
    mo.writeByte ((char) clampByte (settings.engineNumber));
    for (int k = 0; k < 3; ++k)
        mo.writeByte ((char) clampByte (settings.engineNoteNumbers[k]));
    for (int k = 0; k < 3; ++k)
        mo.writeByte ((char) clampByte (settings.enginePcNumbers[k]));
    for (int b = 0; b < 8; ++b)
        mo.writeByte ((char) (settings.boundCC[b] == kCcNone
                                  ? kCcNone : clampByte (settings.boundCC[b])));
    writeF32 (mo, juce::jlimit (0.0f, 100.0f, settings.epsilonMs));
    writeF32 (mo, juce::jlimit (0.0f, 1.0f, settings.gateFraction));
    writeF32 (mo, juce::jlimit (0.0f, 2.0f, settings.downWeight));
    writeF32 (mo, juce::jlimit (0.0f, 2.0f, settings.midBarWeight));
    writeI16 (mo, (int16_t) (settings.customOffsets & 0x0FFF));
    // v3 tail (S5.9): appended after the v2 settings so v2-shaped prefixes
    // still read back; readers stop after customOffsets when version < 3.
    writeF32 (mo, juce::jlimit (0.0f, 50.0f, settings.humanizeVelPercent));
    writeF32 (mo, juce::jlimit (0.0f, 50.0f, settings.humanizeTimingMs));

    // RUNV window-size footer (S7; tolerated-but-optional on read).
    mo.write (kFooterMagic, 4);
    writeI16 (mo, (int16_t) editorWindowW);
    writeI16 (mo, (int16_t) editorWindowH);
}

void RunsProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes < kChunkMinSize)
        return; // truncated: reject, keep current state (S7)
    const auto* d = static_cast<const uint8_t*> (data);
    if (std::memcmp (d, kChunkMagic, 4) != 0)
        return; // wrong magic: reject (S7)
    const uint32_t version = readU32 (d + 4);
    if (version < kChunkMinVersion || version > kChunkVersion)
        return; // unknown schema: reject, keep current state (S7)
    // v1: 9 parameters (Overlap defaults); v2: 10 (Humanize/Seed defaults);
    // v3: all kNumParams. Missing trailing params read as 0 = Overlap off,
    // Humanize off, Seed 0 (S7 backwards-compatible read).
    const int nParams = version >= 3 ? kNumParams
                       : (version >= 2 ? kParamsV2 : kParamsV1);
    size_t p = 8;
    auto need = [&] (size_t n) { return (int) (p + n) <= sizeInBytes; };

    float params[kNumParams] = {};
    for (int i = 0; i < nParams; ++i)
    {
        if (! need (4)) return;
        params[i] = readF32 (d + p);
        p += 4;
    }

    runsp::PluginSettings s;
    // v1/v2 settings end after customOffsets; v3 appends the S5.9 tunables.
    const int settingsBytes = version >= 3 ? kSettingsBytesV3 : kSettingsBytes;
    if (! need (settingsBytes)) return;
    s.engineSourceType = juce::jlimit (0, 2, (int) d[p++]);
    s.engineNumber = clampByte (d[p++]);
    for (int k = 0; k < 3; ++k) s.engineNoteNumbers[k] = clampByte (d[p++]);
    for (int k = 0; k < 3; ++k) s.enginePcNumbers[k] = clampByte (d[p++]);
    for (int b = 0; b < 8; ++b)
    {
        const int v = d[p++];
        s.boundCC[b] = v <= 127 ? v : kCcNone; // range-clamp: 0..127 or none
    }
    s.epsilonMs = juce::jlimit (0.0f, 100.0f, readF32 (d + p)); p += 4;
    s.gateFraction = juce::jlimit (0.0f, 1.0f, readF32 (d + p)); p += 4;
    s.downWeight = juce::jlimit (0.0f, 2.0f, readF32 (d + p)); p += 4;
    s.midBarWeight = juce::jlimit (0.0f, 2.0f, readF32 (d + p)); p += 4;
    s.customOffsets = (uint16_t) (((int16_t) readI16 (d + p)) & 0x0FFF);
    p += 2;
    if (version >= 3)
    {
        // v3 tail (S5.9); the need() above already covers these 8 bytes.
        s.humanizeVelPercent = juce::jlimit (0.0f, 50.0f, readF32 (d + p));
        p += 4;
        s.humanizeTimingMs = juce::jlimit (0.0f, 50.0f, readF32 (d + p));
        p += 4;
    }

    // Optional RUNV footer; any other trailing bytes are ignored (S7). A
    // partial footer tail (1..7 bytes after the settings) is a truncated
    // chunk: reject (S7).
    if ((int) (p + 4) <= sizeInBytes && std::memcmp (d + p, kFooterMagic, 4) == 0)
    {
        if ((int) (p + 8) > sizeInBytes)
            return;
        const int w = (int) readI16 (d + p + 4);
        const int h = (int) readI16 (d + p + 6);
        recordEditorWindowSize (w, h); // footer read path (P3 uses it)
    }
    else if (sizeInBytes - (int) p < 8 && sizeInBytes - (int) p > 0)
        return; // truncated tail

    // Commit: range-clamp every field (S7), parameter edits dirty exactly
    // like GUI edits (S7/S11).
    if (paramIsDiscrete (0))
        params[0] = (float) juce::jlimit (0, 2, (int) std::lround (params[0]));
    else
        params[0] = juce::jlimit (realValueMinOf (0), realValueMaxOf (0), params[0]);
    for (int i = 0; i < kNumParams; ++i)
    {
        if (paramIsDiscrete (i))
            params[i] = (float) juce::jlimit ((int) realValueMinOf (i),
                                              (int) realValueMaxOf (i),
                                              (int) std::lround (params[i]));
        else
            params[i] = juce::jlimit (realValueMinOf (i), realValueMaxOf (i),
                                      params[i]);
        ranged[i]->setValueNotifyingHost (ranged[i]->convertTo0to1 (params[i]));
    }
    settings = s;
}

// --------------------------------------------------------------------------
// Clipboard preset (S7/S9): the whole chunk state as JSON text for the
// editor's Copy to Clipboard / Paste from Clipboard buttons. Same payload as
// the RUN1 chunk (parameters as real values + settings), so a paste behaves
// like a project-state load; validation mirrors the chunk reader (range-clamp
// every field, reject anything that is not ours).

juce::String RunsProcessor::stateToJsonText()
{
    // Flush pending CC mirrors first so the payload carries the LAST held
    // control value, exactly like the chunk write (S7).
    applyPendingCcMirrors();

    auto root = std::make_unique<juce::DynamicObject>();
    root->setProperty ("format", "runs-assistant");
    root->setProperty ("version", kClipboardJsonVersion);

    auto params = std::make_unique<juce::DynamicObject>();
    for (int i = 0; i < kNumParams; ++i)
        params->setProperty (
            kParamIds[i],
            (double) ranged[i]->convertFrom0to1 (ranged[i]->getValue()));
    root->setProperty ("params", juce::var (params.release()));

    auto s = std::make_unique<juce::DynamicObject>();
    s->setProperty ("engineSourceType", settings.engineSourceType);
    s->setProperty ("engineNumber", settings.engineNumber);
    juce::Array<juce::var> notes, pcs, bindings;
    for (int k = 0; k < 3; ++k)
    {
        notes.add (settings.engineNoteNumbers[k]);
        pcs.add (settings.enginePcNumbers[k]);
    }
    for (int b = 0; b < 8; ++b)
        bindings.add (settings.boundCC[b]);
    s->setProperty ("engineNoteNumbers", notes);
    s->setProperty ("enginePcNumbers", pcs);
    s->setProperty ("boundCC", bindings);
    s->setProperty ("epsilonMs", (double) settings.epsilonMs);
    s->setProperty ("gateFraction", (double) settings.gateFraction);
    s->setProperty ("downWeight", (double) settings.downWeight);
    s->setProperty ("midBarWeight", (double) settings.midBarWeight);
    s->setProperty ("customOffsets", settings.customOffsets);
    s->setProperty ("humanizeVelPercent",
                    (double) settings.humanizeVelPercent);
    s->setProperty ("humanizeTimingMs", (double) settings.humanizeTimingMs);
    root->setProperty ("settings", juce::var (s.release()));

    // Indented output: the text is meant to be human-readable when pasted
    // into a plain-text editor. 6 decimals round-trips every float range.
    return juce::JSON::toString (
        juce::var (root.release()),
        juce::JSON::FormatOptions().withMaxDecimalPlaces (6));
}

bool RunsProcessor::applyStateFromJsonText (const juce::String& text,
                                            juce::String& error)
{
    error = {};
    if (text.trim().isEmpty())
    {
        error = "clipboard empty";
        return false;
    }
    const juce::var payload = juce::JSON::parse (text);
    auto* root = payload.getDynamicObject();
    if (root == nullptr)
    {
        error = "not valid JSON";
        return false;
    }
    if (root->hasProperty ("format")
        && root->getProperty ("format").toString() != "runs-assistant")
    {
        error = "not a Runs preset";
        return false;
    }
    const juce::var verVar = root->getProperty ("version");
    const int version = verVar.isVoid() ? 1 : (int) verVar;
    if (version < 1 || version > kClipboardJsonVersion)
    {
        error = "unsupported version";
        return false;
    }
    auto* params = root->getProperty ("params").getDynamicObject();
    if (params == nullptr)
    {
        error = "no params section";
        return false;
    }

    // Numeric field reader: absent keys keep `out`, wrong-typed keys fail.
    auto isNumber = [] (const juce::var& v)
    { return v.isInt() || v.isInt64() || v.isDouble() || v.isBool(); };
    auto number = [&] (juce::DynamicObject* o, const char* key, double& out)
    {
        const juce::var v = o->getProperty (key);
        if (v.isVoid())
            return true;
        if (! isNumber (v))
            return false;
        out = (double) v;
        return true;
    };
    auto intArray = [&] (juce::DynamicObject* o, const char* key, int* dst,
                         int n, bool noneAllowed)
    {
        const juce::var v = o->getProperty (key);
        if (v.isVoid())
            return true;
        const auto* arr = v.getArray();
        if (arr == nullptr || arr->size() < n)
            return false;
        for (int k = 0; k < n; ++k)
        {
            const juce::var e = arr->getUnchecked (k);
            if (! isNumber (e))
                return false;
            const int x = (int) e;
            dst[k] = noneAllowed ? (x >= 0 && x <= 127 ? x : kCcNone)
                                 : juce::jlimit (0, 127, x);
        }
        return true;
    };

    // ---- parameters: start from the current values, override the present
    // ones, then range-clamp exactly like the chunk reader (S7).
    float values[kNumParams];
    for (int i = 0; i < kNumParams; ++i)
        values[i] = ranged[i]->convertFrom0to1 (ranged[i]->getValue());

    int present = 0; // known parameter keys found in the payload
    for (int i = 0; i < kNumParams; ++i)
    {
        const juce::var v = params->getProperty (kParamIds[i]);
        if (v.isVoid())
            continue; // absent: keep the current value
        if (! isNumber (v))
        {
            error = "bad value for " + juce::String (kParamIds[i]);
            return false;
        }
        ++present;
        const double d = (double) v;
        if (paramIsDiscrete (i))
            values[i] = (float) juce::jlimit ((int) realValueMinOf (i),
                                              (int) realValueMaxOf (i),
                                              (int) std::lround (d));
        else
            values[i] = juce::jlimit (realValueMinOf (i), realValueMaxOf (i),
                                      (float) d);
    }
    if (present == 0)
    {
        error = "no params section";
        return false;
    }

    // ---- settings (optional section): same clamps as the chunk reader.
    runsp::PluginSettings s = settings;
    auto* obj = root->getProperty ("settings").getDynamicObject();
    if (obj != nullptr)
    {
        double v;
        if (! number (obj, "engineSourceType", v = s.engineSourceType))
        { error = "bad settings"; return false; }
        s.engineSourceType = juce::jlimit (0, 2, (int) std::lround (v));
        if (! number (obj, "engineNumber", v = s.engineNumber))
        { error = "bad settings"; return false; }
        s.engineNumber = clampByte ((int) std::lround (v));
        if (! intArray (obj, "engineNoteNumbers", s.engineNoteNumbers, 3, false))
        { error = "bad settings"; return false; }
        if (! intArray (obj, "enginePcNumbers", s.enginePcNumbers, 3, false))
        { error = "bad settings"; return false; }
        if (! intArray (obj, "boundCC", s.boundCC, 8, true))
        { error = "bad settings"; return false; }
        if (! number (obj, "epsilonMs", v = s.epsilonMs))
        { error = "bad settings"; return false; }
        s.epsilonMs = (float) juce::jlimit (0.0, 100.0, v);
        if (! number (obj, "gateFraction", v = s.gateFraction))
        { error = "bad settings"; return false; }
        s.gateFraction = (float) juce::jlimit (0.0, 1.0, v);
        if (! number (obj, "downWeight", v = s.downWeight))
        { error = "bad settings"; return false; }
        s.downWeight = (float) juce::jlimit (0.0, 2.0, v);
        if (! number (obj, "midBarWeight", v = s.midBarWeight))
        { error = "bad settings"; return false; }
        s.midBarWeight = (float) juce::jlimit (0.0, 2.0, v);
        if (! number (obj, "customOffsets", v = (double) s.customOffsets))
        { error = "bad settings"; return false; }
        s.customOffsets = (uint16_t) ((int) std::lround (v) & 0x0FFF);
        // v2 payload keys (S5.9): absent in v1 payloads -> keep current.
        if (! number (obj, "humanizeVelPercent", v = s.humanizeVelPercent))
        { error = "bad settings"; return false; }
        s.humanizeVelPercent = (float) juce::jlimit (0.0, 50.0, v);
        if (! number (obj, "humanizeTimingMs", v = s.humanizeTimingMs))
        { error = "bad settings"; return false; }
        s.humanizeTimingMs = (float) juce::jlimit (0.0, 50.0, v);
    }

    // ---- commit: settings first (a Custom-mode tick refresh reads the
    // pasted mask), then every parameter through the normal notify path so
    // attachments and the host see the edits (S7/S11). The engine switch
    // goes through requestEngineState so the switch queue, the parameter and
    // the GUI all follow the same path as a button click (S3.1).
    settings = s;
    for (int i = 0; i < kNumParams; ++i)
        if (i != kEngineIndex)
            ranged[i]->setValueNotifyingHost (
                ranged[i]->convertTo0to1 (values[i]));
    requestEngineState ((int) std::lround (values[kEngineIndex]));
    markStateDirty(); // settings are chunk-only (S7)
    return true;
}

// --------------------------------------------------------------------------
// processBlock, normative S6.1 order

void RunsProcessor::processBlock (juce::AudioBuffer<float>& audio,
                                  juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // One silent audio passthrough bus (specification.md S2).
    if (audio.getNumChannels() > 0 && audio.getNumSamples() > 0)
    {
        for (int c = 0; c < audio.getNumChannels(); ++c)
            audio.copyFrom (c, 0, audio, c, 0, audio.getNumSamples());
    }
    else
    {
        audio.clear();
    }

    const int numSamples = audio.getNumSamples();

    // Reset the block scratch (all preallocated; S6.2).
    inCount = 0;
    outCount = 0;
    runSink.count = 0;
    runSink.overflow = false;

    // S6.1 step 1: playhead read + discontinuity detection + virtual clock
    // when stopped.
    juce::AudioPlayHead* ph = getPlayHead();
    juce::AudioPlayHead::CurrentPositionInfo info;
    const bool haveInfo = ph != nullptr && ph->getCurrentPosition (info);

    syncLiveValues();
    const int beatsParamInt = (int) std::lround (
        juce::jlimit (1.0f, 16.0f, live[1]));
    const double runBeatLen = engine.isActive() ? (double) beatsParamInt : 0.0;
    const auto blk = playhead.update (haveInfo ? &info : nullptr, haveInfo,
                                      numSamples, sampleRate_, runBeatLen);

    if (blk.discont && engine.isActive())        // S5.6: loop wrap / seek
        engine.cut (blk.b0, runSink);
    if (blk.beatSpaceChanged && engine.isActive()) // S5.1 mode switch (see
        engine.cut (blk.b0, runSink);              // deviations list)

    // Engine switch via host automation (S5.6 cut condition): only a NEW raw
    // value triggers it, and only outside the echo grace window; REAPER
    // pushes cached param values back after UI focus changes and would
    // otherwise ping-pong the engine. GUI clicks / requestEngineState bypass
    // the param (queue below) and always win.
    const int guiEngine = (int) std::lround (
        juce::jlimit (0.0f, 2.0f, live[0]));
    int grace = echoGraceBlocks.load (std::memory_order_relaxed);
    echoGraceBlocks.store (grace > 0 ? grace - 1 : 0,
                           std::memory_order_relaxed);
    if (grace == 0 && engineParamChanged && guiEngine != engineState_)
        doEngineChange (guiEngine, 0, blk.b0);

    // Queue path first drain (S6.1 step 3 precedence): the listener enqueued
    // from any message-thread engine edit; doEngineChange dedups equal states.
    {
        int q;
        while (engineQueue.pop (q))
        {
            if (q >= 0 && q <= 2 && q != engineState_)
                doEngineChange (q, 0, blk.b0);
        }
    }
    int engineCurrent = engineState_;

    const double samplesPerBeat = samplesPerBeatFor (blk);

    // P0 spike overlay (editor Until P3; tests read the playheadSnapshot).
    playheadSnapshot.valid.store (haveInfo);
    if (haveInfo)
    {
        playheadSnapshot.ppqPosition.store (blk.ppqPlaying ? blk.b0 : info.ppqPosition);
        playheadSnapshot.bpm.store (blk.bpm);
        playheadSnapshot.hasTimeSig.store (blk.hasTimeSig);
        playheadSnapshot.timeSigNumerator.store (blk.timeSigNumerator);
        playheadSnapshot.timeSigDenominator.store (
            haveInfo ? info.timeSigDenominator : 4);
        playheadSnapshot.isPlaying.store (info.isPlaying);
    }
    else
    {
        playheadSnapshot.ppqPosition.store (playhead.virtualBeat());
        playheadSnapshot.bpm.store (blk.bpm);
        playheadSnapshot.isPlaying.store (false);
    }

    // S6.1 step 2: collect input MIDI (stable stream order).
    collectInput (midi);

    // S6.1 step 3: engine-switch events in stream order (D14: processed
    // before note pairs, even at the same sample offset).
    {
        const int srcType = juce::jlimit (0, 2, settings.engineSourceType);
        for (int i = 0; i < inCount; ++i)
        {
            if (inConsumed[i]) continue;
            const auto& it = inScratch[i];
            const juce::MidiMessage m (it.bytes, it.size);
            bool isEngineEvent = false;
            int newState = -1;
            if (srcType == 0) // Notes (S3.1)
            {
                if (m.isNoteOn())
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        if (m.getNoteNumber() == settings.engineNoteNumbers[k])
                        {
                            // keyswitches absorbed when engine Up or Down,
                            // passed through when engine Off (S3.1)
                            if (engineCurrent != 0)
                            {
                                isEngineEvent = true;
                                newState = k;
                            }
                            break;
                        }
                    }
                }
            }
            else if (srcType == 1) // CC (S3.1): absorbed in every state
            {
                if (m.isController()
                    && m.getControllerNumber() == settings.engineNumber)
                {
                    isEngineEvent = true;
                    const int v = m.getControllerValue();
                    // S3.1/D13 value ranges: 0-40 Off, 41-79 Up, 80-127 Down
                    newState = RunsProcessor::engineStateFromCcValue (v);
                }
            }
            else // PC (S3.1): absorbed in every state
            {
                if (m.isProgramChange())
                {
                    for (int k = 0; k < 3; ++k)
                    {
                        if (m.getProgramChangeNumber()
                            == settings.enginePcNumbers[k])
                        {
                            isEngineEvent = true;
                            newState = k;
                            break;
                        }
                    }
                }
            }
            if (! isEngineEvent) continue;
            inConsumed[i] = true;
            if (newState >= 0 && newState != engineCurrent)
                doEngineChange (newState, it.sample, beatAtSample (blk, it.sample));
            engineCurrent = engineState_;
        }
    }

    const bool engineOn = engineCurrent != 0; // S3.3: Off = passthrough

    // S6.1 step 4: remaining events in input order.
    for (int i = 0; i < inCount; ++i)
    {
        if (inConsumed[i]) continue;
        const auto& it = inScratch[i];
        const juce::MidiMessage m (it.bytes, it.size);
        const int ch = m.getChannel();     // MIDI channel 1..16 (emissions)
        const int chIdx = m.getChannel() - 1; // PairTracker index (0..15)

        if (m.isNoteOn() && ! engineOn)
        {
            // S3.3: passthrough. Engine-off handoff: remember the held
            // note-on so switching the engine on can pair it (S13 handoff
            // rule); a later note-off of the same pitch clears the latch.
            if (chIdx >= 0 && chIdx < 16)
                offHandoff[chIdx] =
                    { m.getNoteNumber(), (int) m.getVelocity(),
                      beatAtSample (blk, it.sample), true };
            pushOut (it.sample, it.bytes, it.size);
            continue;
        }

        if (m.isNoteOff() && ! engineOn)
        {
            if (chIdx >= 0 && chIdx < 16)
            {
                auto& oh = offHandoff[chIdx];
                if (oh.held && oh.pitch == m.getNoteNumber()) oh = {};
            }
            pushOut (it.sample, it.bytes, it.size);
            continue;
        }

        if (engineOn && m.isNoteOn())
        {
            const double beat = beatAtSample (blk, it.sample);
            const runsp::PairOutcome o = pairs.noteOn (
                chIdx, m.getNoteNumber(), (int) m.getVelocity(), beat);
            switch (o.kind)
            {
                case runsp::PairOutcome::One:
                    // single event passes through (S5.2)
                    pushOut (it.sample, it.bytes, it.size);
                    break;
                case runsp::PairOutcome::PublishAndPass:
                {
                    // S5.2 same-pitch: publish the pending note, then pass
                    // the new note-on as an ordinary note.
                    int pendSample = it.sample;
                    if (o.pendingBeat >= blk.b0) // buffered inside this block
                        pendSample = juce::jlimit (0, numSamples - 1,
                                                   sampleAtBeat (blk, o.pendingBeat));
                    // Handoff (S3.3): a passed-through pending already
                    // sounded; emit only the new note.
                    if (o.pendPassedSide == -1)
                        pushNoteMsg (true, ch, o.a.pitch, o.a.velocity, pendSample);
                    pushOut (it.sample, it.bytes, it.size);
                    break;
                }
                case runsp::PairOutcome::LateOnOff:
                    emitLatePublish (ch, o.a.pitch, o.a.velocity, it.sample);
                    break;
                case runsp::PairOutcome::Pair:
                {
                    // S3.2/S5.2: both note-ons consumed; run fires at the
                    // second note-on, aligned forward within epsilon (S5.1).
                    runsp::PairTrigger t;
                    t.pitchLo = o.pitchLo;
                    t.pitchHi = o.pitchHi;
                    t.velLo = o.velLo;
                    t.velHi = o.velHi;
                    t.direction = engineCurrent == 1 ? runsp::Direction::Up
                                                     : runsp::Direction::Down;
                    t.channel = ch; // first note-on of the pair (S5.2 tail)
                    t.triggerBeat = beat;
                    if (! engine.startRun (t, makeRunParams (blk)))
                    {
                        // S3.2 degenerate after endpoint snapping: the two
                        // notes pass through as ordinary notes; late publish
                        // both (consumption is compensated, S5.8 invariant).
                        // Handoff (S3.3): the passed-through pending side
                        // already sounded, so only the new side publishes.
                        if (o.pendPassedSide != 0)
                            emitLatePublish (ch, o.pitchLo, o.velLo, it.sample);
                        if (o.pendPassedSide != 1)
                            emitLatePublish (ch, o.pitchHi, o.velHi, it.sample);
                    }
                    break;
                }
                default: break; // Nothing: consumed silently (S5.2)
            }
            continue;
        }

        if (engineOn && m.isNoteOff())
        {
            // S3.1: the keyswitch note numbers are consumed only in note
            // source mode (P4 hardening: in CC/PC mode these pitches are
            // ordinary melody notes and their offs must pass through).
            bool isKeyswitchOff = false;
            if (settings.engineSourceType == 0)
                for (int k = 0; k < 3; ++k)
                    if (m.getNoteNumber() == settings.engineNoteNumbers[k])
                        isKeyswitchOff = true;
            if (! isKeyswitchOff)
            {
                // S5.6: either trigger note released -> cut at this offset.
                engine.onTriggerNoteRelease (m.getNoteNumber(),
                                             beatAtSample (blk, it.sample),
                                             runSink);
                const runsp::PairOutcome o = pairs.noteOff (
                    chIdx, m.getNoteNumber());
                switch (o.kind)
                {
                    case runsp::PairOutcome::One:
                        pushOut (it.sample, it.bytes, it.size);
                        break;
                    case runsp::PairOutcome::LateOnOff:
                        // jabbed a lone note: publish late (S5.2)
                        emitLatePublish (ch, o.a.pitch, o.a.velocity, it.sample);
                        break;
                    default: break; // Nothing: consumed silently (S5.2)
                }
            }
            continue;
        }

        if (engineOn && m.isController())
        {
            const int cc = m.getControllerNumber();
            if (cc == kCcAllNotesOff)
            {
                engine.cut (beatAtSample (blk, it.sample), runSink); // S5.6
                // CC123 still passes through so passthrough chord notes on
                // the instrument are cleared too (interpretation note).
                pushOut (it.sample, it.bytes, it.size);
                continue;
            }
            bool isBound = false;
            for (int b = 0; b < 8; ++b)
            {
                if (settings.boundCC[b] == cc)
                {
                    applyBoundCc (cc, m.getControllerValue(), it.sample);
                    // inbound bound-CC events are absorbed, never echoed
                    // (S4/S11); inert + passthrough only when engine Off.
                    isBound = true;
                    break;
                }
            }
            if (isBound) continue;
            // any other CC: passthrough with the original offset
            pushOut (it.sample, it.bytes, it.size);
            continue;
        }

        if (engineOn)
        {
            // Any other event (PC, pitch bend, unbound notes...): straight
            // passthrough with the original offset (S5.2/S5.8).
            pushOut (it.sample, it.bytes, it.size);
            continue;
        }

        // S3.3: engine Off -> 100% passthrough, unmodified offsets (notes,
        // keyswitches in note mode, bound CCs: all inert here).
        pushOut (it.sample, it.bytes, it.size);
    }

    // S6.1 step 5: run scheduler - emit due note-ons and gated note-offs at
    // computed offsets, after the block's cut rules were applied inline.
    engine.pumpUpTo (blk.b1, runSink);
    for (int k = 0; k < runSink.count; ++k)
        pushRunEvent (runSink.data[k], blk, samplesPerBeat, numSamples);
    if (runSink.overflow)
        outDrops_.store (outDrops_.load (std::memory_order_relaxed) + 1,
                         std::memory_order_relaxed);

    mergeEmitAndFlushToHost (midi);

    // S6.1 step 6: publish engine-to-UI state through the lock-free FIFO.
    {
        runsp::UiMessage u;
        u.engineState = engineState_;
        int pend = -1;
        if (pairs.pendingOnAny (&pend))
            u.pendingPitch = pend;
        u.runActive = engine.isActive();
        u.notesEmitted = engine.notesEmitted();
        u.noteCount = engine.noteCount();
        const runsp::RunEngine::Counters& ctr = engine.counters();
        u.parityDrops = ctr.parityDrops;
        u.cuts = ctr.cuts;
        u.lastPitch = ctr.lastPitch;
        u.lastVel = ctr.lastVel;
        u.latePublishes = pairs.latePublishes();
        u.inputDrops = inputDrops_.load (std::memory_order_relaxed);
        u.inputEvents = inputEventCount_.load (std::memory_order_relaxed);
        u.mirrorDrops = mirrorFifo.dropped.load (std::memory_order_relaxed);
        u.engineParamX256 = (int) std::lround (
            juce::jlimit (0.0f, 2.0f, live[0]) * 256.0f);
        u.engineParamChanges =
            diagEngineParamChanges.load (std::memory_order_relaxed);
        u.engineSwitches = diagEngineSwitches.load (std::memory_order_relaxed);
        u.engineListenerHits =
            diagEngineListenerHits.load (std::memory_order_relaxed);
        publishedEngineState.store (engineState_, std::memory_order_relaxed);
        u.beat = blk.ppqPlaying ? blk.b0 : playhead.virtualBeat();
        u.bpm = blk.bpm;
        u.runSeed = engine.seedOfLastRun(); // S5.9 overlay seed readout
        uiFifo.push (u); // full FIFO drops (overlay only refreshes later)
    }
}

runsp::RunParams RunsProcessor::makeRunParams (
    const PlayheadAdapter::Block& blk) const
{
    // S4/S5.3/S5.7 inputs for run assembly. clamp everything so the core
    // engine only ever sees legal values.
    runsp::RunParams p;
    p.beats = (int) std::lround (juce::jlimit (1.0f, 16.0f, live[1]));
    p.density = juce::jlimit (1.0, 16.0, (double) live[2]);
    p.curveStrength = juce::jlimit (0.0, 1.0, (double) live[3]);
    p.accentStrength = juce::jlimit (0.0, 1.0, (double) live[4]);
    p.arc = juce::jlimit (-1.0, 1.0, (double) live[5]);
    p.tonic = (int) std::lround (juce::jlimit (0.0f, 11.0f, live[6]));
    p.mode = (int) std::lround (juce::jlimit (0.0f, (float) kNumModes, live[7]));
    p.customOffsets = settings.customOffsets;
    p.walk = live[8] >= 0.5f ? runsp::WalkMode::ZigZag : runsp::WalkMode::Fold;
    p.noteOverlap = live[kOverlapIndex] >= 0.5f; // S5.8 overlap toggle
    p.gateFraction = juce::jlimit (0.0, 1.0, (double) settings.gateFraction);
    // S5.1: epsilon (default 2 ms) converted to beats at the current bpm.
    p.epsilonBeats = juce::jlimit (0.0, 16.0,
        (double) settings.epsilonMs * 0.001 * blk.bpm / 60.0);
    p.barNumerator = blk.hasTimeSig ? blk.timeSigNumerator : 4; // S5.7 4/4 default
    p.hasBarOrigin = blk.hasBarOrigin; // S5.7 host bar origin when provided
    p.barOriginBeats = blk.barOrigin;
    p.alignToGrid = blk.ppqPlaying; // S5.1: virtual clock skips alignment
    // S5.7 accent beat weights: the Settings tunables finally reach the
    // engine (they used to be stored-but-ignored, engine hard-coded them).
    p.downWeight = juce::jlimit (0.0, 2.0, (double) settings.downWeight);
    p.midBarWeight = juce::jlimit (0.0, 2.0, (double) settings.midBarWeight);
    // S5.9: humanize toggle + per-run seed (seed 0 mints a fresh value) and
    // the Settings strengths, with ms -> beats done here at this block's bpm.
    p.humanize = live[kHumanizeIndex] >= 0.5f;
    p.humanizeSeed = p.humanize ? resolveHumanizeSeed (live[kSeedIndex]) : 0;
    p.humanizeVelAmt = juce::jlimit (0.0, 0.5,
        (double) settings.humanizeVelPercent * 0.01);
    p.humanizeTimingBeats = juce::jlimit (0.0, 1.0,
        (double) settings.humanizeTimingMs * 0.001 * blk.bpm / 60.0);
    return p;
}

uint32_t RunsProcessor::resolveHumanizeSeed (float seedParam) const
{
    // S5.9: seed > 0 is explicit and reproducible (no counter use); seed 0
    // mints a fresh one from the session-salted counter. RT-safe by
    // construction: one relaxed fetch_add + pure splitmix32 math, no locks,
    // no allocation - safe from the audio thread (startRun) and from the
    // message thread (offline export) alike; unique slots mean unique seeds.
    const int seedVal = (int) std::lround (
        juce::jlimit (0.0f, 999999.0f, seedParam));
    if (seedVal > 0)
        return (uint32_t) seedVal;
    uint32_t slot = humanizeSeedSalt
        + humanizeSeedCounter.fetch_add (1, std::memory_order_relaxed);
    const uint32_t s = runsp::humanizePrngNext (slot);
    return s != 0 ? s : 1; // 0 is reserved for "humanize off" in the overlay
}

juce::AudioProcessorEditor* RunsProcessor::createEditor()
{
    startCcMirrorTimer(); // message-thread timer for CC mirrors (S11)
    return new RunsEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RunsProcessor();
}
