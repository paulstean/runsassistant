// P0 headless gate (plan.md P0): parameter defaults/ranges, state
// round-trip, MIDI passthrough invariance with the engine Off.
#include "../Source/processor/PluginProcessor.h"
#include <cstdio>
#include <cstring>

namespace
{
int failures = 0;
#define CHECK(cond) do { if (! (cond)) { ++failures; \
    std::printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

void testDefaults()
{
    RunsProcessor p;
    auto equal = [&] (const char* id, float wantRaw) {
        auto* v = p.apvts.getParameter (id);
        CHECK (v != nullptr);
        if (v == nullptr)
            return;
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (v);
        CHECK (ranged != nullptr);
        if (ranged != nullptr)
            CHECK (juce::approximatelyEqual (
                ranged->convertFrom0to1 (ranged->getDefaultValue()), wantRaw));
    };
    equal ("engine", 0.0f);
    equal ("beats", 4.0f);
    equal ("density", 4.0f);
    equal ("curve", 0.5f);
    equal ("accent", 0.5f);
    equal ("arc", 0.0f);
    equal ("tonic", 0.0f);
    equal ("mode", 0.0f);
    equal ("walk", 0.0f);
}

void testStateRoundTrip()
{
    RunsProcessor a;
    a.apvts.getParameter ("beats")->setValueNotifyingHost (0.25f);
    a.apvts.getParameter ("curve")->setValueNotifyingHost (0.75f);
    juce::MemoryBlock data;
    a.getStateInformation (data);
    RunsProcessor b;
    b.setStateInformation (data.getData(), (int) data.getSize());
    auto* beatsA = a.apvts.getParameter ("beats");
    auto* beatsB = b.apvts.getParameter ("beats");
    CHECK (beatsB != nullptr && beatsA != nullptr
           && beatsA->convertFrom0to1 (beatsA->getValue())
              == beatsB->convertFrom0to1 (beatsB->getValue()));
    CHECK (b.apvts.getParameter ("curve")->getValue() == 0.75f);
}

void testPassthrough()
{
    RunsProcessor p;
    p.prepareToPlay (48000.0, 512);

    // Engine stays Off (CC20 is not the engine CC and not bound), so P2 must
    // still give byte-identical passthrough (S3.3). The old P0 packet used
    // CC87 itself, which is now the engine switch (S3.1) and would change the
    // engine state mid-block.
    juce::MidiBuffer in;
    in.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    in.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90), 17);
    in.addEvent (juce::MidiMessage::controllerEvent (2, 20, 40), 33);
    in.addEvent (juce::MidiMessage::noteOff (1, 60), 100);
    in.addEvent (juce::MidiMessage::pitchWheel (1, 2000), 200);

    juce::AudioBuffer<float> audio (2, 512);
    juce::MidiBuffer out = in;
    p.processBlock (audio, out);

    // P0 invariant: identical event stream, offsets untouched
    CHECK (out.getNumEvents() == in.getNumEvents());
    auto expect = in.begin();
    for (auto e : out)
    {
        CHECK (expect != in.end());
        if (expect == in.end())
            break;
        CHECK (e.samplePosition == (*expect).samplePosition);
        CHECK (e.getMessage().getRawDataSize()
               == (*expect).getMessage().getRawDataSize());
        ++expect;
    }
}

void testSilentAudioPassthrough()
{
    RunsProcessor p;
    p.prepareToPlay (48000.0, 256);
    juce::AudioBuffer<float> audio (2, 256);
    for (int c = 0; c < 2; ++c)
        for (int s = 0; s < 256; ++s)
            audio.setSample (c, s, (float) (c + s) * 0.0001f);
    juce::MidiBuffer midi;
    const float in0 = audio.getSample (0, 100);
    p.processBlock (audio, midi);
    // With an input bus attached: samples unchanged; without: silence
    CHECK (juce::approximatelyEqual (in0, audio.getSample (0, 100)));
}
} // namespace

int main()
{
    testDefaults();
    testStateRoundTrip();
    testPassthrough();
    testSilentAudioPassthrough();
    std::printf ("%s (%d failures)\n",
                 failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
