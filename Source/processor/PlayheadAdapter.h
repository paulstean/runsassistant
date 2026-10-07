#pragma once

// PlayheadAdapter: specification.md S5.1 grid lock / transport handling
// (processor layer; P2, plan.md). Per block it turns the raw host position
// info into the beat-space block span the engine schedules against:
//
//  * Playing with ppq (S5.1 first bullet): block start beat B0 =
//    info.ppqPosition, block end beat B1 = B0 + blockSamples * bpm / (60 x
//    sampleRate). Onsets are absolute beats; the caller converts them to
//    sample offsets inside the covering block.
//  * Stopped or missing ppq (S5.1 second bullet): virtual clock. It advances
//    beatsPerBlock per block at the last-known bpm (default 120). A trigger
//    defines its own beat 0, so alignment is skipped in this mode (see
//    RunParams.alignToGrid).
//  * Discontinuity detection (S5.6): while playing, a ppq backward jump
//    beyond a small tolerance (loop wrap / click-back) or a forward jump
//    larger than one run beat of the active run (seek) asks for a cut.
#include <juce_audio_basics/juce_audio_basics.h>

class PlayheadAdapter
{
public:
    // What one processBlock needs for scheduling (S5.1, S6.1 step 1).
    struct Block
    {
        bool ppqPlaying = false;      // host ppq timing active this block
        bool discont = false;         // S5.6 loop wrap / seek -> cut request
        bool beatSpaceChanged = false;// engine beat space <-> new mode switch
        double bpm = 120.0;           // default 120 until anything is known
        bool hasTimeSig = false;      // absent -> 4/4 defaults (S5.1/S5.7)
        int timeSigNumerator = 4;
        bool hasBarOrigin = false;    // host bar-start ppq available (S5.7)
        double barOrigin = 0.0;       // host bar start beat (S5.7)
        double b0 = 0.0;              // block START beat (B0)
        double b1 = 0.0;              // block END beat (B1)
        double beatsPerBlock = 0.0;
    };

    // runBeatLen: length in beats of the active run (0 when no run active);
    // only used as the forward-seek jump tolerance per S5.6.
    Block update (const juce::AudioPlayHead::CurrentPositionInfo* info,
                  bool haveInfo, int blockSamples, double sampleRate,
                  double runBeatLen);

    // Virtual clock position (valid in non-ppq mode; S5.1 virtual bullet).
    double virtualBeat() const { return virtualBeat_; }
    bool lastWasPpq() const { return hadPpq_; }

private:
    // Backward-jump tolerance: anything smaller than 1e-9 drift over blocks
    // would be float noise; 1e-3 beats is well below any real trigger
    // activity and far below a loop wrap.
    static constexpr double kBackwardTol = 1e-3;

    bool hadPpq_ = false;
    double lastB0_ = 0.0;
    double lastB1_ = 0.0;
    double virtualBeat_ = 0.0;
};
