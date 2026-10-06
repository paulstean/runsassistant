#include "PluginEditor.h"

namespace
{
// Eloquent dark-theme palette (house style, specification.md D16).
constexpr uint32_t kBgColour = 0xff1e1f22;
constexpr uint32_t kPanelColour = 0xff26282b;
constexpr uint32_t kTextColour = 0xffc8ccd0;
constexpr uint32_t kAccentColour = 0xff8ab4f8;

juce::String onOff (bool v) { return v ? "yes" : "no"; }
} // namespace

SpikeOverlay::SpikeOverlay (RunsProcessor& p)
    : processor (p)
{
    spikeLogPath = juce::SystemStats::getEnvironmentVariable (
        "RUNSASSISTANT_SPIKE_LOG", "");
    for (auto& l : statusLabels)
    {
        l.setColour (juce::Label::textColourId, juce::Colour (kTextColour));
        l.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (l);
    }
    startTimerHz (10);
}

void SpikeOverlay::timerCallback()
{
    auto& s = processor.playheadSnapshot;
    statusLabels[0].setText ("ppq: " + juce::String (s.ppqPosition.load(), 4),
                             juce::dontSendNotification);
    statusLabels[1].setText ("bpm: " + juce::String (s.bpm.load(), 2),
                             juce::dontSendNotification);
    statusLabels[2].setText ("timeSig: " + juce::String (
                                 s.timeSigNumerator.load()) + "/" +
                             juce::String (s.timeSigDenominator.load()) +
                             (s.hasTimeSig.load() ? "" : " (absent)"),
                             juce::dontSendNotification);
    statusLabels[3].setText ("playing: " + onOff (s.isPlaying.load()),
                             juce::dontSendNotification);
    statusLabels[4].setText ("valid: " + onOff (s.valid.load()),
                             juce::dontSendNotification);
    if (spikeLogPath.isNotEmpty() && s.valid.load())
        appendSpikeLog();
}

void SpikeOverlay::appendSpikeLog()
{
    auto& s = processor.playheadSnapshot;
    juce::File f (spikeLogPath);
    f.appendText (juce::String (juce::Time::getMillisecondCounter()) + ","
                  + juce::String (s.ppqPosition.load(), 6) + ","
                  + juce::String (s.bpm.load(), 4) + ","
                  + juce::String (s.timeSigNumerator.load()) + ","
                  + juce::String (s.timeSigDenominator.load()) + ","
                  + (s.isPlaying.load() ? "1" : "0") + "\n");
}

void SpikeOverlay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (kPanelColour));
}

void SpikeOverlay::resized()
{
    auto area = getLocalBounds().reduced (8);
    const int h = 20;
    for (auto& l : statusLabels)
        l.setBounds (area.removeFromTop (h));
    area.removeFromTop (4);
}

RunsEditor::RunsEditor (RunsProcessor& p)
    : AudioProcessorEditor (p), processor (p), overlay (p)
{
    title.setText ("RUNS ASSISTANT (P0 passthrough + playhead spike)",
                   juce::dontSendNotification);
    title.setColour (juce::Label::textColourId, juce::Colour (kAccentColour));
    title.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (title);
    addAndMakeVisible (overlay);
    setResizeLimits (720, 320, 1400, 600);
    setSize (960, 420);
}

void RunsEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (kBgColour));
}

void RunsEditor::resized()
{
    auto area = getLocalBounds().reduced (12);
    title.setBounds (area.removeFromTop (28));
    overlay.setBounds (area.removeFromTop (220).reduced (0, 8));
}
