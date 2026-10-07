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

    if (S <= D)
    {
        // Under-filled (S < D) and the exact-fit case (S == D): proportional
        // stride map, seq[k] = round(k x D / S) = floor((2kD + S) / 2S). The
        // scale climb is compressed evenly across the available steps (leaps
        // distributed THROUGH the run) instead of advancing step-by-step and
        // leaping to the target at the end. Ends exactly at D; strides are
        // never 0 (D > S makes round(k x D / S) strictly increasing).
        for (int k = 0; k <= S; ++k)
        {
            const long long v =
                ((long long) k * 2LL * (long long) D + (long long) S)
                    / (2LL * (long long) S);
            seq[k] = (int) v;
        }
        for (int k = 0; k <= S; ++k)
            out[k] = spanPitches[mapVirtual (seq[k], spanLength, dirUp)];
        result.count = S + 1;
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
    if (((S - D) & 1) != 0)
    {
        // S5.4: S - D odd -> parity drop (n = n - 1) and recompute.
        --S;
        result.parityDrop = true;
    }
    const int b = (S - D) / 2; // backward steps, S5.4
    // S5.4: distribute the b backward steps deterministically and evenly
    // across the WHOLE run: position j at round(j x S / (b + 1)), j = 1..b
    // (t_b < S always, since b/(b + 1) x S < S). The earlier fixed period
    // floor(S / (b + 1)) clustered all backsteps into the first b x period
    // positions (heavily over-filled runs glued onto a two-note trill and
    // shoved the whole climb into the tail); scheduling by run index keeps
    // the climb spread evenly: net travel t - 2 x back(t) is proportional
    // across the run.
    long long backNext[2048]; // b <= (n - 1 - 1) / 2 <= 2047 (S5.3 clamp)
    {
        const long long twoBp = 2LL * (long long) (b + 1);
        for (int j = 0; j < b; ++j)
        {
            const long long lb = (long long) (j + 1) * (long long) S;
            backNext[j] = (2LL * lb + twoBp / 2) / twoBp; // floor(x + 1/2)
        }
    }
    int cur = 0, back = 0;
    int jp = 0;
    for (int t = 1; t <= S; ++t)
    {
        bool doBack = jp < b && (long long) t >= backNext[jp];
        if (doBack && cur == 0)
        {
            // A backward step cannot leave the span below its start; postpone
            // this scheduled step to the next position (still deterministic;
            // can only happen early in the run, before any backstep fired).
            backNext[jp] = (long long) t + 1;
            doBack = false;
        }
        if (doBack)
        {
            --cur;
            ++back;
            ++jp;
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
