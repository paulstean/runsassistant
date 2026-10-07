#include "PluginProcessor.h"
#include "../editor/PluginEditor.h"

#include <cmath>

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

// S7 chunk layout: magic "RUN1" + u32 schema_version + 9 raw parameter
// values + settings (source type + engine numbers + 8 bindings + 4 tuning
// constants + custom mask) + optional trailing RUNV window-size footer.
constexpr uint8_t kChunkMagic[4]  = { 'R', 'U', 'N', '1' };
constexpr uint8_t kFooterMagic[4] = { 'R', 'U', 'N', 'V' };
constexpr uint32_t kChunkVersion = 1;
constexpr int kSettingsBytes = 1                    // source type
    + 1                                             // engine CC number
    + 3 + 3                                         // engine notes / PCs
    + 8                                             // bound CC table
    + 4 * 4                                         // tuning constants
    + 2;                                            // custom tick set
constexpr int kChunkMinSize = 4                     // magic
    + 4                                             // u32 version
    + RunsProcessor::kNumParams * 4                 // raw parameter values
    + kSettingsBytes;

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

RunsProcessor::RunsProcessor()
    : AudioProcessor (BusesProperties()
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    const char* ids[kNumParams] = { "engine", "beats", "density", "curve",
                                    "accent", "arc", "tonic", "mode", "walk" };
    for (int i = 0; i < kNumParams; ++i)
    {
        ranged[i] = dynamic_cast<juce::RangedAudioParameter*> (
            apvts.getParameter (ids[i]));
        rawParam[i] = apvts.getRawParameterValue (ids[i]);
    }
    // live[] starts at the factory defaults (S4); it tracks GUI edits, host
    // automation and bound CCs last-writer-wins (S4).
    for (int i = 0; i < kNumParams; ++i)
        live[i] = ranged[i] != nullptr
                      ? ranged[i]->convertFrom0to1 (ranged[i]->getDefaultValue())
                      : 0.0f;

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
        juce::StringArray { "Off", "Up", "Down" }, 0));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "beats", 1 }, "Beats", 1, 16, 4));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "density", 1 }, "Density",
        juce::NormalisableRange<float> (1.0f, 16.0f, 0.0f, 1.0f), 4.0f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "curve", 1 }, "Curve",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "accent", 1 }, "Accent",
        juce::NormalisableRange<float> (0.0f, 1.0f), 0.5f));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "arc", 1 }, "Arc",
        juce::NormalisableRange<float> (-1.0f, 1.0f), 0.0f));
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
        juce::StringArray { "Fold", "Zig-zag" }, 0));
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
    inCount = 0;
    outCount = 0;
    runSink.count = 0;
    runSink.overflow = false;
    inputDrops_.store (0, std::memory_order_relaxed);
    outDrops_.store (0, std::memory_order_relaxed);
}

bool RunsProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return (layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo())
           || (layouts.getMainInputChannelSet().isDisabled()
               && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}

// --------------------------------------------------------------------------
// Parameters: ranges (S8), CC mapping (D13), mirrors (S4/S11)

float RunsProcessor::realValueMinOf (int paramIndex)
{
    static constexpr float mins[kNumParams] =
        { 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f };
    return paramIndex >= 0 && paramIndex < kNumParams ? mins[paramIndex] : 0.0f;
}

float RunsProcessor::realValueMaxOf (int paramIndex)
{
    static constexpr float maxs[kNumParams] =
        { 2.0f, 16.0f, 16.0f, 1.0f, 1.0f, 1.0f, 11.0f, (float) kNumModes, 1.0f };
    return paramIndex >= 0 && paramIndex < kNumParams ? maxs[paramIndex] : 0.0f;
}

bool RunsProcessor::paramIsDiscrete (int paramIndex)
{
    return paramIndex == kEngineIndex // engine choice
        || paramIndex == 1            // beats (int)
        || paramIndex == 6            // tonic (int)
        || paramIndex == 7            // mode (int indexed)
        || paramIndex == 8;           // walk (choice)
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
    // S3.1 GUI three-state switch: queued directly (bypasses the host
    // parameter; the GUI mirrors the real state via the UI FIFO). The audio
    // thread dedups identical states.
    if (index < 0 || index > 2)
        return;
    diagEngineListenerHits.store (
        diagEngineListenerHits.load (std::memory_order_relaxed) + 1,
        std::memory_order_relaxed); // count direct GUI pushes too
    engineQueue.push (index);
}

void RunsProcessor::doEngineChange (int newState, int sampleOffset, double beatNow)
{
    // S5.2/S5.6: engine state change flushes pending notes (late publish)
    // and cuts the active run.
    if (newState < 0 || newState > 2 || newState == engineState_)
        return;
    engine.cut (beatNow, runSink);
    runsp::PairOutcome outs[16];
    int flushed = 0;
    pairs.flushAll (outs, 16, &flushed);
    for (int k = 0; k < flushed; ++k)
        emitLatePublish (outs[k].channel + 1, outs[k].a.pitch,
                         outs[k].a.velocity, sampleOffset);
    pairs.clearAll(); // state only; latePublishes counter survives (S5.8)
    engineState_ = newState;
    diagEngineSwitches.store (diagEngineSwitches.load (std::memory_order_relaxed)
                              + 1, std::memory_order_relaxed);
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
    destData.setSize (0, true);
    juce::MemoryOutputStream mo (destData, false);
    mo.write (kChunkMagic, 4);
    writeU32 (mo, kChunkVersion);

    // All 9 parameters as raw (real) values (S7).
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
    if (readU32 (d + 4) != kChunkVersion)
        return; // unknown schema: reject, keep current state (S7)
    size_t p = 8;
    auto need = [&] (size_t n) { return (int) (p + n) <= sizeInBytes; };

    float params[kNumParams];
    for (int i = 0; i < kNumParams; ++i)
    {
        if (! need (4)) return;
        params[i] = readF32 (d + p);
        p += 4;
    }

    runsp::PluginSettings s;
    if (! need (kSettingsBytes)) return;
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
                    if (v <= 2) newState = v; // 0 Off / 1 Up / 2 Down; else ignored
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
                        emitLatePublish (ch, o.pitchLo, o.velLo, it.sample);
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
    p.gateFraction = juce::jlimit (0.0, 1.0, (double) settings.gateFraction);
    // S5.1: epsilon (default 2 ms) converted to beats at the current bpm.
    p.epsilonBeats = juce::jlimit (0.0, 16.0,
        (double) settings.epsilonMs * 0.001 * blk.bpm / 60.0);
    p.barNumerator = blk.hasTimeSig ? blk.timeSigNumerator : 4; // S5.7 4/4 default
    p.hasBarOrigin = blk.hasBarOrigin; // S5.7 host bar origin when provided
    p.barOriginBeats = blk.barOrigin;
    p.alignToGrid = blk.ppqPlaying; // S5.1: virtual clock skips alignment
    return p;
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
