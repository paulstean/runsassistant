#include "PluginProcessor.h"
#include "../editor/PluginEditor.h"

namespace
{
// Mode list per specification.md S9. "Custom" is index kNumModes; a manual
// tick-set in the UI grid switches the Mode dropdown there (P3).
const runsp::ModeInfo kModeList[] =
{
    { "Major (Ionian)",            7, { 0, 2, 4, 5, 7, 9, 11 } },
    { "Dorian",                    7, { 0, 2, 3, 5, 7, 9, 10 } },
    { "Phrygian",                  7, { 0, 1, 3, 5, 7, 8, 10 } },
    { "Lydian",                    7, { 0, 2, 4, 6, 7, 9, 11 } },
    { "Mixolydian",                7, { 0, 2, 4, 5, 7, 9, 10 } },
    { "Aeolian (Natural Minor)",   7, { 0, 2, 3, 5, 7, 8, 10 } },
    { "Locrian",                   7, { 0, 1, 3, 5, 6, 8, 10 } },
    { "Harmonic Minor",            7, { 0, 2, 3, 5, 7, 8, 11 } },
    { "Melodic Minor (ascending)", 7, { 0, 2, 3, 5, 7, 9, 11 } },
    { "Whole Tone",                6, { 0, 2, 4, 6, 8, 10 } },
    { "Octatonic (whole-half)",    8, { 0, 2, 3, 5, 6, 8, 9, 11 } },
    { "Octatonic (half-whole)",    8, { 0, 1, 3, 4, 6, 7, 9, 10 } },
    { "Major Pentatonic",          5, { 0, 2, 4, 7, 9 } },
    { "Minor Pentatonic",          5, { 0, 3, 5, 7, 10 } },
    { "Blues",                     6, { 0, 3, 5, 6, 7, 10 } },
    { "Chromatic",                12, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
};
constexpr int kNumModes = 16; // + index 16 = "Custom"

const char* kTonicNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

juce::String modeName (int index)
{
    if (index < 0 || index > kNumModes)
        return {};
    return index < kNumModes ? juce::String (kModeList[index].name)
                             : juce::String ("Custom");
}
} // namespace

RunsProcessor::RunsProcessor()
    : AudioProcessor (BusesProperties()
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
        .withInput ("Input", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    engineParam  = apvts.getRawParameterValue ("engine");
    beatsParam   = apvts.getRawParameterValue ("beats");
    densityParam = apvts.getRawParameterValue ("density");
    curveParam   = apvts.getRawParameterValue ("curve");
    accentParam  = apvts.getRawParameterValue ("accent");
    arcParam     = apvts.getRawParameterValue ("arc");
    tonicParam   = apvts.getRawParameterValue ("tonic");
    modeParam    = apvts.getRawParameterValue ("mode");
    walkParam    = apvts.getRawParameterValue ("walk");
}

juce::AudioProcessorValueTreeState::ParameterLayout RunsProcessor::createParameterLayout()
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

void RunsProcessor::prepareToPlay (double, int)
{
}

bool RunsProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return (layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo())
           || (layouts.getMainInputChannelSet().isDisabled()
               && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}

void RunsProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ignoreUnused (midi);
    juce::ScopedNoDenormals noDenormals;
    if (buffer.getNumChannels() > 0 && buffer.getNumSamples() > 0)
    {
        // silent audio passthrough bus (specification.md S2)
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            buffer.copyFrom (c, 0, buffer, c, 0, buffer.getNumSamples());
    }
    else
    {
        buffer.clear();
    }

    juce::AudioPlayHead* ph = getPlayHead();
    juce::AudioPlayHead::CurrentPositionInfo info;
    if (ph != nullptr && ph->getCurrentPosition (info))
    {
        playheadSnapshot.valid.store (true);
        playheadSnapshot.ppqPosition.store (info.ppqPosition);
        playheadSnapshot.bpm.store (info.bpm);
        playheadSnapshot.isPlaying.store (info.isPlaying);
        playheadSnapshot.hasTimeSig.store (info.timeSigNumerator != 0
                                           && info.timeSigDenominator != 0);
        playheadSnapshot.timeSigNumerator.store (info.timeSigNumerator);
        playheadSnapshot.timeSigDenominator.store (info.timeSigDenominator);
    }
    else
    {
        playheadSnapshot.valid.store (false);
    }
}

juce::AudioProcessorEditor* RunsProcessor::createEditor()
{
    return new RunsEditor (*this);
}

void RunsProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void RunsProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new RunsProcessor();
}
