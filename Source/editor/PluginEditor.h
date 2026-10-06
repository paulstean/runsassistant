#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../processor/PluginProcessor.h"

// P0 spike overlay (plan.md P0 item 5): live playhead readout, replacing the
// layout with the real S9 panel in P3.
class SpikeOverlay : public juce::Component, private juce::Timer
{
public:
    explicit SpikeOverlay (RunsProcessor& p);
    ~SpikeOverlay() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void appendSpikeLog();

    RunsProcessor& processor;
    juce::Label statusLabels[6];
    juce::String spikeLogPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpikeOverlay)
};

class RunsEditor : public juce::AudioProcessorEditor
{
public:
    explicit RunsEditor (RunsProcessor& p);
    ~RunsEditor() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    RunsProcessor& processor;
    juce::Label title;
    SpikeOverlay overlay;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RunsEditor)
};
