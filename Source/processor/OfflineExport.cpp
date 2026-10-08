#include "PluginProcessor.h"
#include "../core/OfflineRun.h"

#include <cmath>

// Offline export: one complete run for explicit endpoints, written as a
// type-0 MIDI file for drag-and-drop into the DAW arrange window (session-
// only feature: no host parameters, no chunk schema change, no audio-thread
// involvement - everything here runs on the message thread).

namespace
{
constexpr int kExportChannel = 0;      // MIDI channel 1 (0-based in RunEvent)
constexpr double kExportTailBeats = 1.0; // final note rings one beat past the end
constexpr int kExportTicksPerQuarter = 480;
constexpr double kFallbackBpm = 120.0;

// The export has no playhead, so the run anchors its own bar lines at beat 0
// (S5.7 first-anchor rule) and skips grid alignment (S5.1 virtual-clock rule
// - the trigger already sits on a beat line).
int sanitiseDenominator (int d)
{
    int best = 4;
    for (int v : { 1, 2, 4, 8, 16, 32 })
        if (std::abs (v - d) < std::abs (best - d)) best = v;
    return best;
}
} // namespace

runsp::RunParams RunsProcessor::makeExportRunParams (double bpm,
                                                     int barNumerator) const
{
    // Same mapping as makeRunParams(), but the values come from the atomic
    // raw parameters (this runs on the message thread; live[] is the audio
    // thread's mirror) and there is no playhead block to derive from.
    auto real = [this] (int i, float lo, float hi)
    {
        const float v = rawParam[i] != nullptr
                            ? rawParam[i]->load (std::memory_order_relaxed)
                            : lo;
        return (double) juce::jlimit (lo, hi, v);
    };

    runsp::RunParams p;
    p.beats = (int) std::lround (real (1, 1.0f, 16.0f));
    p.density = real (2, 1.0f, 16.0f);
    p.curveStrength = real (3, 0.0f, 1.0f);
    p.accentStrength = real (4, 0.0f, 1.0f);
    p.arc = real (5, -1.0f, 1.0f);
    p.tonic = (int) std::lround (real (6, 0.0f, 11.0f));
    p.mode = (int) std::lround (real (7, 0.0f, (float) runsp::kCustomMode));
    p.customOffsets = settings.customOffsets;
    p.walk = real (8, 0.0f, 1.0f) >= 0.5 ? runsp::WalkMode::ZigZag
                                         : runsp::WalkMode::Fold;
    p.noteOverlap = real (9, 0.0f, 1.0f) >= 0.5; // S5.8 overlap toggle
    p.gateFraction = juce::jlimit (0.0, 1.0, (double) settings.gateFraction);
    p.epsilonBeats = 0.0; // unused while alignToGrid is false
    p.barNumerator = juce::jlimit (0, 64, barNumerator);
    p.hasBarOrigin = false;
    p.barOriginBeats = 0.0;
    p.alignToGrid = false;
    p.downWeight = juce::jlimit (0.0, 2.0, (double) settings.downWeight);
    p.midBarWeight = juce::jlimit (0.0, 2.0, (double) settings.midBarWeight);
    // S5.9: humanize rides along the export so the dragged MIDI matches the
    // live run; seed 0 mints a fresh value, a fixed seed re-renders exactly.
    p.humanize = real (10, 0.0f, 1.0f) >= 0.5;
    p.humanizeSeed = p.humanize ? resolveHumanizeSeed (real (11, 0.0f, 999999.0f))
                                : 0;
    p.humanizeVelAmt = juce::jlimit (0.0, 0.5,
        (double) settings.humanizeVelPercent * 0.01);
    p.humanizeTimingBeats = juce::jlimit (0.0, 1.0,
        (double) settings.humanizeTimingMs * 0.001 * bpm / 60.0);
    return p;
}

juce::File RunsProcessor::renderMidiExport (int startPitch, int targetPitch,
                                            int velStart, int velTarget,
                                            juce::String& error)
{
    error.clear();
    startPitch = juce::jlimit (0, 127, startPitch);
    targetPitch = juce::jlimit (0, 127, targetPitch);
    velStart = juce::jlimit (1, 127, velStart);
    velTarget = juce::jlimit (1, 127, velTarget);

    if (startPitch == targetPitch)
    {
        error = "Start = target";
        return {};
    }

    const double bpmRaw = playheadSnapshot.bpm.load (std::memory_order_relaxed);
    const double bpm = bpmRaw > 0.0 ? bpmRaw : kFallbackBpm;
    const bool hasTimeSig =
        playheadSnapshot.hasTimeSig.load (std::memory_order_relaxed);
    const int num = hasTimeSig
                        ? juce::jlimit (1, 32, playheadSnapshot.timeSigNumerator
                                                    .load (std::memory_order_relaxed))
                        : 4;
    const int den = hasTimeSig
                        ? sanitiseDenominator (
                              playheadSnapshot.timeSigDenominator
                                  .load (std::memory_order_relaxed))
                        : 4;

    // S3.2: the unordered pair is ordered by pitch, and the run direction
    // decides which end is the START - so the start velocity rides the run's
    // first note (Up: pitchLo, Down: pitchHi) and the target velocity the
    // other end.
    const bool up = startPitch < targetPitch;
    runsp::PairTrigger t;
    t.pitchLo = up ? startPitch : targetPitch;
    t.pitchHi = up ? targetPitch : startPitch;
    t.velLo = up ? velStart : velTarget;
    t.velHi = up ? velTarget : velStart;
    t.direction = up ? runsp::Direction::Up : runsp::Direction::Down;
    t.channel = kExportChannel;
    t.triggerBeat = 0.0;

    const runsp::RunParams p = makeExportRunParams (bpm, num);

    // Exact capacity: n note-ons + n note-offs (a zig-zag parity drop only
    // shrinks n), plus a little slack so the core never has to report one.
    const long long nL = std::lround (p.density * (double) p.beats);
    const int n = (int) juce::jlimit (1LL, 4096LL, nL);
    exportEvents.resize ((size_t) (2 * n + 8));

    const runsp::OfflineRunResult r = runsp::renderRunOffline (
        t, p, kExportTailBeats, exportEvents.data (),
        (int) exportEvents.size ());
    if (! r.ok ())
    {
        switch (r.error)
        {
            case runsp::OfflineRunResult::Degenerate:
                error = "Same scale degree"; // snapped span collapsed (S3.2)
                break;
            case runsp::OfflineRunResult::Overflow:
            case runsp::OfflineRunResult::BadBuffer:
                error = "Run too long";
                break;
            default:
                error = "Export failed";
                break;
        }
        return {};
    }

    juce::MidiMessageSequence seq;
    {
        juce::MidiMessage name =
            juce::MidiMessage::textMetaEvent (3, "Runs Assistant"); // 3 = track name
        name.setTimeStamp (0);
        seq.addEvent (name);
        juce::MidiMessage sig =
            juce::MidiMessage::timeSignatureMetaEvent (num, den);
        sig.setTimeStamp (0);
        seq.addEvent (sig);
        juce::MidiMessage tempo = juce::MidiMessage::tempoMetaEvent (
            (int) std::lround (60000000.0 / juce::jmax (1.0, bpm)));
        tempo.setTimeStamp (0);
        seq.addEvent (tempo);
    }

    for (int i = 0; i < r.eventCount; ++i)
    {
        const runsp::RunEvent& e = exportEvents[(size_t) i];
        juce::MidiMessage m =
            e.kind == runsp::RunEvent::NoteOn
                ? juce::MidiMessage::noteOn (e.channel + 1, e.pitch,
                                             (juce::uint8) e.velocity)
                : juce::MidiMessage::noteOff (e.channel + 1, e.pitch);
        m.setTimeStamp (e.beat * kExportTicksPerQuarter);
        seq.addEvent (m);
    }

    juce::MidiFile file;
    file.setTicksPerQuarterNote (kExportTicksPerQuarter);
    file.addTrack (seq);

    juce::MemoryOutputStream bytes;
    if (! file.writeTo (bytes, 0)) // 0 = format 0 (single track)
    {
        error = "Write failed";
        return {};
    }

    const juce::File dir = juce::File::getSpecialLocation (
                               juce::File::tempDirectory)
                               .getChildFile ("RunsAssistant");
    dir.createDirectory (); // failure surfaces on the write below
    const juce::File out = dir.getChildFile ("runs-assistant-export.mid");
    if (! out.replaceWithData (bytes.getData (), bytes.getDataSize ()))
    {
        error = "Write failed";
        return {};
    }
    return out;
}
