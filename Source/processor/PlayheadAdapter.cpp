#include "PlayheadAdapter.h"

PlayheadAdapter::Block PlayheadAdapter::update (
    const juce::AudioPlayHead::CurrentPositionInfo* info, bool haveInfo,
    int blockSamples, double sampleRate, double runBeatLen)
{
    Block blk;

    // S5.1: bpm (default 120 until anything is known) and time signature
    // (numerator for bar lines; default 4/4 when absent).
    blk.bpm = (haveInfo && info->bpm > 0.0) ? info->bpm : 120.0;
    blk.hasTimeSig = haveInfo && info->timeSigNumerator > 0
                     && info->timeSigDenominator > 0;
    blk.timeSigNumerator = blk.hasTimeSig ? info->timeSigNumerator : 4;

    blk.beatsPerBlock = (blockSamples > 0 && sampleRate > 0.0)
                            ? (double) blockSamples * blk.bpm / (60.0 * sampleRate)
                            : 0.0;

    // Playing with ppq (S5.1 first bullet). CurrentPositionInfo cannot express
    // a missing ppq, so "playing" is treated as "ppq present" (REAPER, the
    // primary validation host, provides ppq while playing; see the P0 spike).
    // P4 hardening FIX (S13.9): hosts that report playing WITHOUT a ppq
    // position (ppqPosition < 0 in CurrentPositionInfo) must fall back to the
    // virtual clock, not run on a garbage beat base (b0 = -1 shifted every
    // onset). hosts  that report playing without ppq fall back to the
    // virtual clock rather than the real grid (S13 deviation 9).
    const bool nowPpq = haveInfo && info->isPlaying && info->ppqPosition >= 0.0;
    if (nowPpq)
    {
        if (hadPpq_)
        {
            blk.b0 = info->ppqPosition;
            blk.b1 = blk.b0 + blk.beatsPerBlock;

            // S5.6 discontinuity detection while playing: backward jump beyond
            // epsilon (loop wrap / click-back) or forward jump larger than one
            // run beat of the active run without an intervening trigger (seek).
            if (blk.b0 < lastB0_ - kBackwardTol)
            {
                blk.discont = true;
            }
            else
            {
                const double jump = blk.b0 - lastB1_;
                if (jump > (runBeatLen > 0.0 ? runBeatLen : 0.0) + kBackwardTol)
                    blk.discont = true;
            }
        }
        else
        {
            // Stopped (virtual beat space) -> playing (ppq beat space): onsets
            // of a virtual-space run have no meaning in ppq space.
            blk.b0 = info->ppqPosition;
            blk.b1 = blk.b0 + blk.beatsPerBlock;
            blk.beatSpaceChanged = true;
        }
        hadPpq_ = true;
        lastB0_ = blk.b0;
        lastB1_ = blk.b1;
    }
    else
    {
        // Stopped or no ppq (S5.1 second bullet): virtual clock anchored at
        // the run start; alignment is skipped (trigger defines beat 0). If we
        // were playing on ppq, an active run is in ppq space and cannot be
        // continued.
        blk.ppqPlaying = false;
        if (hadPpq_)
            blk.beatSpaceChanged = true;
        blk.b0 = virtualBeat_;
        blk.b1 = blk.b0 + blk.beatsPerBlock;
        virtualBeat_ = blk.b1;
        hadPpq_ = false;
        lastB0_ = blk.b0;
        lastB1_ = blk.b1;
        return blk;
    }

        blk.ppqPlaying = true;
        // P4 hardening FIX (S5.7): host bar-line origin, ppq mode only.
        // JUCE's PositionInfo->CurrentPositionInfo conversion leaves
        // ppqPositionOfLastBarStart = 0 when the host never provides it, so
        // "available" cannot be detected: the ppq grid origin 0 IS a valid
        // downbeat for ppq timing. In virtual-clock mode the engine anchors
        // bar lines on the run's own start beat instead (S5.7 fallback).
        blk.hasBarOrigin = blk.hasTimeSig && info->ppqPositionOfLastBarStart >= 0.0;
        blk.barOrigin = blk.hasBarOrigin ? info->ppqPositionOfLastBarStart : 0.0;
        if (blk.discont)
    {
        // Re-anchor so the next block compares against the new position.
        lastB0_ = blk.b0;
        lastB1_ = blk.b1;
    }
    return blk;
}
