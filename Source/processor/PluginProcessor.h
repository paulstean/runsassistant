#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../core/PairTracker.h"
#include "../core/RunEngine.h"
#include "PlayheadAdapter.h"

// Runs Assistant - the real processor (plan.md P2, specification.md S4-S8).
//
// processBlock follows the normative S6.1 order exactly:
//   1. playhead read + discontinuity detection + virtual clock when stopped
//   2. collect input MIDI into preallocated scratch (stable stream order)
//   3. pass 1: engine-switch events (D14: before notes at the same offset)
//   4. pass 2: pair mechanics (S5.2) + bound-CC mirrors + passthrough
//   5. run scheduler: due note-ons/offs at computed sample offsets + cuts
//   6. publish engine-to-UI state through the lock-free FIFO
//
// House rules: no allocation/locks/file-IO/message calls on the audio thread;
// all scratch preallocated in prepareToPlay; overflow drops with a counter;
// every input event is passed through or consumed by a documented rule (S5.8).

namespace runsp
{
// Mode tables live in Source/core/ScaleModel.h (single definition; the P0
// duplicate was retired).

// S4/D13 settings: chunk state only, never host parameters (S7).
struct PluginSettings
{
    int engineSourceType = 1;                 // 0 Notes, 1 CC, 2 PC (S3.1 table)
    int engineNumber = 87;                    // CC number when CC mode (S3.1)
    int engineNoteNumbers[3] = { 12, 13, 14 }; // keyswitch Off/Up/Down (S3.1)
    int enginePcNumbers[3] = { 0, 1, 2 };      // PC Off/Up/Down (S3.1)
    int boundCC[8] = { 88, 89, 90, 91, 92, 93, 94, 95 }; // S4 defaults; 255 = none
    float epsilonMs = 2.0f;                   // S5.1 alignment epsilon
    float gateFraction = 0.6f;                // S5.8 gate (interior notes)
    float downWeight = 1.0f;                  // S5.7 v1 tunables
    float midBarWeight = 0.75f;
    uint16_t customOffsets = 0x0AB5;          // S9 custom tick set (relative tonic)
};

// S6.1 step 6: engine-to-UI state message (small, trivially copyable).
struct UiMessage
{
    int engineState = 0;        // 0 Off / 1 Up / 2 Down
    int pendingPitch = -1;      // pending note on any channel, -1 none
    bool runActive = false;
    int notesEmitted = 0;       // active run: notes emitted / n (S9 overlay)
    int noteCount = 0;
    int lastPitch = -1, lastVel = -1;
    int parityDrops = 0;        // zig-zag parity drops (S5.4/S5.8)
    int latePublishes = 0;      // jabbed lone notes (S5.2)
    int cuts = 0;               // runs cut (S5.6)
    int inputDrops = 0;         // input scratch overflows (S6.1 step 2)
    long long inputEvents = 0;  // cumulative input events seen (diagnostics)
    int mirrorDrops = 0;        // CC-mirror FIFO overflows (S11)
    int engineParamX256 = 0;    // diagnostics: engine param seen by audio
    int engineParamChanges = 0; // diagnostics: param-change detections
    int engineSwitches = 0;     // diagnostics: engine state transitions
    int engineListenerHits = 0; // diagnostics: APVTS listener hits
    double beat = 0.0;          // current playhead beat / virtual beat (S9)
    double bpm = 120.0;
};

// S11 mitigation: audio thread must not notify the host; it pushes mirror
// requests and a message-thread Timer applies them (only when the value
// actually changes, so CC edits dirty exactly like GUI edits).
struct CcMirrorRequest
{
    int paramIndex = 0;  // 0..8, GUI order (S8)
    float realValue = 0.0f;
    bool discrete = false; // compare with integer rounding when applying
};

// Fixed-capacity SPSC ring. Producer = audio thread, consumer = message
// thread. No allocation; the producer counts drops instead of blocking.
template <typename T, int Cap>
struct SpscRing
{
    SpscRing() { for (int i = 0; i < Cap + 1; ++i) slots[i] = T(); }
    bool push (const T& v)
    {
        const int h = head.load (std::memory_order_relaxed);
        const int next = h == Cap ? 0 : h + 1;
        const int t = tail.load (std::memory_order_acquire);
        if (next == t)
        {
            dropped.store (dropped.load (std::memory_order_relaxed) + 1,
                           std::memory_order_relaxed);
            return false;
        }
        slots[h] = v;
        head.store (next, std::memory_order_release);
        return true;
    }
    bool pop (T& out)
    {
        const int t = tail.load (std::memory_order_relaxed);
        const int h = head.load (std::memory_order_acquire);
        if (t == h) return false;
        out = slots[t];
        tail.store (t == Cap ? 0 : t + 1, std::memory_order_release);
        return true;
    }
    std::atomic<int> dropped { 0 };
private:
    T slots[Cap + 1]; // one slot wasted to distinguish full from empty
    std::atomic<int> head { 0 };
    std::atomic<int> tail { 0 };
};

} // namespace runsp

class RunsProcessor : public juce::AudioProcessor, private juce::Timer
{
public:
    RunsProcessor();
    ~RunsProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Runs Assistant"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // Clipboard preset (S7/S9): the whole state - every parameter as a real
    // value plus the settings - as JSON text, for the editor's Copy to
    // Clipboard / Paste from Clipboard buttons. Same payload as the RUN1
    // chunk, but the text only travels through the system clipboard: this is
    // not a preset file (D15).
    juce::String stateToJsonText();
    // Applies text produced by stateToJsonText(). Returns false and fills
    // `error` with a short reason when the payload is not ours; nothing is
    // applied in that case. Present fields are range-clamped like the chunk
    // reader, absent fields keep their current values.
    bool applyStateFromJsonText (const juce::String& text, juce::String& error);

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;
    // APVTS listener object (defined in the .cpp) that enqueues engine
    // switches from any message-thread param change into engineQueue.
    std::unique_ptr<juce::AudioProcessorValueTreeState::Listener> apvtsListener;

    // S7 settings: editor/settings dialog (P3) and chunk I/O own this; the
    // audio thread only reads it.
    runsp::PluginSettings settings;

    // P3 hook: editor window size kept for the RUNV footer (S7).
    void recordEditorWindowSize (int w, int h);
    void getEditorWindowSize (int& w, int& h) const;

    // S11: applies queued CC mirror requests (message thread / tests).
    void applyPendingCcMirrors();

    // S6.1 step 6 FIFO readback: drains everything and returns the newest
    // snapshot. The editor timer will use the same FIFO (P3).
    runsp::UiMessage latestUiState();

    // GUI / programmatic engine switch (S3.1): message thread writes the
    // requested state straight into the switch queue; the engine param is
    // NOT involved (hosts may echo param values back asynchronously -
    // REAPER does for discrete VST3 params - and must not revert the GUI).
    void requestEngineState (int index);

    // Snapshot of the last playhead read (P0 spike; editor uses it until P3).
    struct PlayheadSnapshot
    {
        std::atomic<bool> valid { false };
        std::atomic<double> ppqPosition { -1.0 };
        std::atomic<double> bpm { 0.0 };
        std::atomic<int> timeSigNumerator { 0 };
        std::atomic<int> timeSigDenominator { 0 };
        std::atomic<bool> isPlaying { false };
        std::atomic<bool> hasTimeSig { false };
    };
    PlayheadSnapshot playheadSnapshot;

    static constexpr int kNumParams = 10;
    static constexpr int kEngineIndex = 0;
    static constexpr int kOverlapIndex = 9; // S5.8 overlap toggle

    // S4 parameter real-value ranges shared by the CC mapping (D13) and the
    // chunk clamps (S7).
    static float realValueMinOf (int paramIndex);
    static float realValueMaxOf (int paramIndex);
    static bool  paramIsDiscrete  (int paramIndex);

    // S3.1/D13 engine CC value ranges (single definition; the tests mirror
    // it): 0-40 = Off, 41-79 = Up, 80-127 = Down.
    static int engineStateFromCcValue (int value);

    // S7: mark the host state dirty (VST3 dirty flag / CLAP state-mark-dirty
    // through the non-parameter-changed path) for chunk-only edits.
    void markStateDirty();

private:
    // ---- preallocated scratch (prepareToPlay; fixed capacity, S6.2) -------
    struct MidiScratchItem
    {
        int sample = 0;
        int size = 0;
        uint8_t bytes[16];
    };
    static constexpr int kInputCap = 512;   // S6.1 step 2 scratch
    static constexpr int kOutCap = 2048;    // passthrough + run emission

    // ---- processBlock helpers (all allocation-free) ----------------------
    void collectInput (const juce::MidiBuffer& midi);
    void syncLiveValues();
    runsp::RunParams makeRunParams (const PlayheadAdapter::Block& blk) const;
    double samplesPerBeatFor (const PlayheadAdapter::Block& blk) const;
    double beatAtSample (const PlayheadAdapter::Block& blk, int sample) const;
    int sampleAtBeat (const PlayheadAdapter::Block& blk, double beat) const;
    void applyBoundCc (int ccNumber, int ccValue, int sampleOffset);
    void emitLatePublish (int channel, int pitch, int velocity, int sampleOffset);
    void pushNoteMsg (bool on, int channel, int pitch, int velocity,
                      int sampleOffset);
    bool pushOut (int sample, const uint8_t* data, int size);
    void pushRunEvent (const runsp::RunEvent& e, const PlayheadAdapter::Block& blk,
                       double samplesPerBeat, int numSamples);
    void doEngineChange (int newState, int sampleOffset, double beatNow);
    void mergeEmitAndFlushToHost (juce::MidiBuffer& midi);

    // Message-thread timer for CC mirrors (S11); started on editor open.
    void startCcMirrorTimer();
    void timerCallback() override;

    juce::RangedAudioParameter* ranged[kNumParams] = {};         // GUI order
    std::atomic<float>* rawParam[kNumParams] = {};
    // live[] starts at the raw defaults until the ctor copies them from the
    // APVTS (S4). normSeen must never coincide with a legal first value, so
    // ALL slots start at -1 (a zero-init here masked the overlap bool's
    // first change in the live[] sync and pinned runs to the old default).
    float live[kNumParams]       = { 0, 4, 4, 0.5f, 0.5f, 0, 0, 0, 0, 0 };
    float normSeen[kNumParams]   = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f,
                                     -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };

    double sampleRate_ = 48000.0;
    int lastBlockSamples_ = 512;

    PlayheadAdapter playhead;

    // Engine state
    int engineState_ = 0;                 // 0 Off / 1 Up / 2 Down (S3.1)
    bool engineParamChanged = false;      // raw engine param changed this block

    // Belt-and-braces engine switch path: APVTS listener enqueue (message
    // thread) -> drain at block start (audio thread, before pass 1, D14).
    // Cuts/flushes use the same doEngineChange path; doEngineChange dedups.
    runsp::SpscRing<int, 16> engineQueue;

    // Diagnostics (S9 overlay): last engine param value the audio thread
    // saw, the number of param-change detections and queue events.
    std::atomic<int> diagEngineParamX256 { 0 };
    std::atomic<int> diagEngineParamChanges { 0 };
    std::atomic<int> diagEngineSwitches { 0 };
    std::atomic<int> diagEngineListenerHits { 0 }; // APVTS listener fires

    // Host-echo guard (S11 mitigation analog): after any engine switch the
    // host may push its own cached param value back (REAPER does this for
    // discrete VST3 params on UI focus changes). Param-driven switches are
    // ignored for a short grace window after every switch; genuine host
    // automation still applies once the window passes. GUI switches bypass
    // the param entirely (engineQueue), so they always win.
    std::atomic<int> echoGraceBlocks { 0 };

    // Engine-off handoff (S3.3): per channel, the last note-on that passed
    // through while the engine was Off and is still held. Turning the engine
    // to Up/Down seeds it as the channel's pending note, so a second note-on
    // starts the run (S5.2 pair mechanics with a handoff-latched pending).
    struct OffHandoffNote
    {
        int pitch = -1, vel = 0;
        double beat = 0.0;
        bool held = false;
    };
    OffHandoffNote offHandoff[16];

    runsp::PairTracker pairs;
    runsp::RunEngine engine;

    // Scratch (fixed capacity, preallocated at construction; prepareToPlay
    // only resets them).
    MidiScratchItem inScratch[kInputCap];
    bool inConsumed[kInputCap] = {};
    int inCount = 0;
    std::atomic<int> inputDrops_ { 0 };
    std::atomic<long long> inputEventCount_ { 0 }; // diagnostics (overlay)

public:
    // Current engine state mirrored for UI sync (message thread reads it).
    std::atomic<int> publishedEngineState { 0 };

private:

    MidiScratchItem outScratch[kOutCap];
    int outCount = 0;
    std::atomic<int> outDrops_ { 0 };

    runsp::RunEvent runSinkData[kOutCap];
    runsp::RunEventSink runSink;

    // Lock-free channels (S6.1 step 6 + S11)
    runsp::SpscRing<runsp::UiMessage, 64> uiFifo;
    runsp::SpscRing<runsp::CcMirrorRequest, 256> mirrorFifo;

    int editorWindowW = 960; // RUNV footer values (S7; editor updates in P3)
    int editorWindowH = 420;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RunsProcessor)
};

    juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();