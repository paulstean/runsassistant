#include "Walk.h"

#include <cstdint>

namespace runsp
{

namespace
{
// Fixed planning constants (S5.4). Counts stay small in tests; the walk does
// no allocation: caller provides the output buffer.
inline int mapVirtual (int v, int len, bool dirUp)
{
    return dirUp ? v : (len - 1 - v);
}
} // namespace

WalkResult buildWalk (const int* spanPitches, int spanLength, bool dirUp, int n,
                      WalkMode mode, int* out)
{
    WalkResult result;
    result.count = 0;
    result.parityDrop = false;
    if (spanLength < 1 || n < 1)
        return result;

    const int D = spanLength - 1; // S5.4: scale-tone steps across the span
    int S = n - 1;
    // Virtual axis: start at 0, target at D (mirrored for Down runs, S5.3).
    int seq[4097]; // S+1 <= 4097 since n <= 4096 (S5.3 clamp)

    if (S <= 0)
    {
        // n == 1: single-note run, emitted on the run's start pitch.
        out[0] = spanPitches[mapVirtual (0, spanLength, dirUp)];
        result.count = 1;
        return result;
    }

    if (mode == WalkMode::Fold)
    {
        // S5.4 fold: advance in scale tones, reverse at span boundaries.
        seq[0] = 0;
        int cur = 0, dir = 1;
        for (int k = 1; k < S; ++k)
        {
            int cand = cur + dir;
            if (cand > D || cand < 0)
            {
                dir = -dir;
                cand = cur + dir;
            }
            cur = cand;
            seq[k] = cur;
        }
        // S5.4: final pitch forced to the target; the bounce never doubles a
        // note; the one-off geometric deviation (interior trim) is folded into
        // the final position.
        seq[S] = D;
        // Boundary case: penultimate note already at the target (the natural
        // walk just bounced off it). Trim the interior by pulling the
        // penultimate one tone inside so consecutive pitches never repeat
        // (S5.4). Single-step spans halve the count instead (no interior note
        // exists to trim).
        if (seq[S - 1] == D)
        {
            if (D >= 2)
                seq[S - 1] = D - 2;
            else
            {
                // D == 1: strict alternation cannot end at the target on an
                // even step count; parity-drop one note (counted like the
                // zig-zag drop counter, S5.4/S14 open item 2).
                --S;
                seq[S] = D;
                result.parityDrop = true;
            }
        }
        for (int k = 0; k <= S; ++k)
            out[k] = spanPitches[mapVirtual (seq[k], spanLength, dirUp)];
        result.count = S + 1;
        return result;
    }

    // Zig-zag, S5.4.
    if (S <= D)
    {
        // S5.4: S <= D -> plain scale run (both modes identical).
        for (int k = 0; k < S; ++k)
            seq[k] = k;
        // Endpoint exactness (S5.4 / S5.3 under-filled): force final to target.
        seq[S] = D;
        for (int k = 0; k <= S; ++k)
            out[k] = spanPitches[mapVirtual (seq[k], spanLength, dirUp)];
        result.count = S + 1;
        return result;
    }

    if (((S - D) & 1) != 0)
    {
        // S5.4: S - D odd -> parity drop (n = n - 1) and recompute.
        --S;
        result.parityDrop = true;
    }
    const int b = (S - D) / 2; // backward steps, S5.4
    // S5.4: distribute the b backward steps deterministically, one every
    // floor(S / (b+1)) step positions (t_j = j * period, j = 1..b).
    const long long period = (long long) (S / (b + 1));
    int cur = 0, back = 0;
    long long nextBack = period; // period >= 1: S >= D + 2 >= 2b + 1 > b + 1
    long long never = (long long) S + 2;
    for (int t = 1; t <= S; ++t)
    {
        bool doBack = back < b && (long long) t >= nextBack;
        if (doBack && cur == 0)
        {
            // A backward step cannot leave the span below its start; postpone
            // this scheduled step to the next position (still deterministic).
            nextBack = (long long) t + 1;
            doBack = false;
        }
        if (doBack)
        {
            --cur;
            ++back;
            nextBack = back < b ? (long long) t + period : never;
        }
        else
        {
            ++cur;
        }
        seq[t] = cur;
    }
    seq[0] = 0;
    for (int k = 0; k <= S; ++k)
        out[k] = spanPitches[mapVirtual (seq[k], spanLength, dirUp)];
    result.count = S + 1;
    return result;
}

} // namespace runsp
