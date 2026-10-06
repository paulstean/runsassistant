#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

// Runs Assistant - P0 passthrough processor.
// All S4 parameters are exposed (specification.md section 4/8). The run
// engine itself lands in P1/P2; until then every MIDI event passes through
// with offsets untouched and audio passes through (one silent stereo bus,
// specification.md S2).

namespace runsp
{
// Mode list per specification.md S9 (indices for the Mode parameter).
struct ModeInfo
{
    const char* name; // UI/parameter display name, empty tail-sentinel
    int pitchCount;   // number of pitch classes in the mode
    int offsets[12];  // semitone offsets from tonic, unused tail entries 0
};
}

class RunsProcessor : public juce::AudioProcessor
{
public:
    RunsProcessor();
    ~RunsProcessor() override = default;

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

    juce::AudioProcessorValueTreeState apvts;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Snapshot of the last playhead read (S5.1 spike). Written each block on
    // the audio thread, read by the editor timer.
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

private:
    std::atomic<float>* engineParam = nullptr;
    std::atomic<float>* beatsParam = nullptr;
    std::atomic<float>* densityParam = nullptr;
    std::atomic<float>* curveParam = nullptr;
    std::atomic<float>* accentParam = nullptr;
    std::atomic<float>* arcParam = nullptr;
    std::atomic<float>* tonicParam = nullptr;
    std::atomic<float>* modeParam = nullptr;
    std::atomic<float>* walkParam = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RunsProcessor)
};

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();
