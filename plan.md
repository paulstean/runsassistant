# Runs Assistant - Build Plan (VST3 / CLAP / AU)

Companion to `native/specification.md` (read the spec first; section numbers below
refer to it as S1..S14). Stack: JUCE 8 + CMake + C++17, AGPLv3, community/free.
Conventions and house rules mirror the Eloquent project (`H:\Projects\Eloquent`);
each phase ends in a testable state with explicit exit criteria. This plug-in is
smaller than Eloquent: the whole plan is roughly **3-5 weeks part-time**.

Target folder map (scaffolded in P0, filled in per phase):

```
RunsAssistant/
  specification.md   behavioral master (normative)
  plan.md            this file (build order)
  README.md          quickstart (toolchain, configure, build, test)  [P0]
  CMakeLists.txt     root build (JUCE fetch-pin, targets per format, tests)
  tools/
    deploy.ps1       copy VST3/CLAP into the host scan folders (REAPER dev loop)
  Source/
    core/            plain C++17 engine: RunEngine, scales, walk, curve map,
                     velocity stack, pair state machine (no JUCE includes)
    processor/       JUCE AudioProcessor + format shims + playhead adapter +
                     chunk I/O + CC mapping
    editor/          JUCE UI: panel per S9
  tests/             headless conformance vectors + instrumented realtime tests
```

Conventions (carried over from Eloquent): ASCII-only sources; no
allocation/locks/file-IO on the audio thread; `Source/core` never includes JUCE
headers (enforced by a CI include-check); every consumed MIDI event is either
re-sent or consumed by a documented rule (S3.2, S5.8).

---

## P0 - Scaffold + passthrough spike (1-2 days)

Goal: all formats build and pass MIDI through untouched before any engine work.

Tasks:
1. Pin JUCE 8 (same fetch-pin strategy as Eloquent) + CMake presets (win-x64).
2. Scaffold one `AudioProcessor` that passes MIDI through (offsets untouched) and
   exposes the S4 parameters (incl. engine switch as a discrete parameter).
3. Build VST3 + CLAP + both AU personalities + Standalone (AU built, deferred to
   D3's post-v1 validation).
4. Port `tools/deploy.ps1` from Eloquent (close REAPER first, CLAP to
   `%LOCALAPPDATA%\Programs\Common\CLAP\`, merge VST3 bundle into
   `%COMMONPROGRAMFILES%\VST3\`).
5. Spike: log ppq/bpm/timeSig/isPlaying per block in REAPER playing and stopped,
   confirming the S5.1 assumptions (does REAPER provide ppq while stopped; does
   it provide time signature?).

Exit criteria:
* All binaries build on Windows; VST3 + CLAP load in REAPER and pass MIDI
  unchanged (recorded MIDI comparison with/without the plug-in).
* Playhead assumptions documented in the spec's open-items log or confirmed.

## P1 - Core engine, JUCE-free (1 week)

Goal: `Source/core` implements the whole engine with headless tests; no JUCE.

Tasks:
1. `ScaleModel`: tonic + mode tables (S9 mode list) + custom 12-bit map; endpoint
   snap with tie-toward-the-other-end; degree distance computation.
2. `PairTracker` per channel: pending/publish/consume state machine per S5.2
   (explicit transitions; channel-scoped).
3. `Walk`: monotonic, zig-zag (Bresenham distribution + parity drop + counter),
   fold (leg sums per S5.4; exact target landing), all behind one interface.
4. `CurveMap`: power-S formula (S5.5); property tests (monotonic, y(0)=0, y(1)=1,
   linear at k=1, symmetric).
5. `VelocityModel`: base ramp, accent proximity with bar/mid weights + clamps,
   arc multiplier; exact integer output 1..127.
6. `RunEngine`: assembly per S5.3, scheduled emission list in beat space, gate
   math, cut conditions (S5.6), one-run-at-a-time rule, engine-switch side
   effects.
7. Tests (run with CTest): the S10 vector list as test cases, parameterized
   sweeps over beats x density x spans x both directions x both walk modes.

Exit criteria:
* All vectors green; fold landing exactness proven across the sweep; parity
  drop counting correct; curve map linearity at 0 percent bit-exact.

## P2 - Processor + state + CC mapping (3-4 days)

Goal: a real plug-in that drives with the host transport and round-trips state.

Tasks:
1. Playhead adapter per S5.1 (ppq interpolation, stopped virtual clock,
   discontinuity detection for cuts).
2. `RunsProcessor`: `prepareToPlay` preallocs scratch; `processBlock` follows
   S6.1 order (engine-switch pass first, then notes - S/D14 ordering); run
   emission at computed sample offsets.
3. Host parameters (S8) + mirror logic (GUI, host automation, bound CCs
   last-writer-wins); bound-CC absorption rules.
4. Chunk I/O per S7 (`RUN1` header, clamps, trailing-byte tolerance,
   truncated-chunk rejection, `RUNV` window-size footer).

Exit criteria:
* Headless tests of the full processor: pair capture matrix, epsilon alignment,
   cut matrix, chunk round-trip (retains all parameters + CC map + settings).
* In REAPER: pair on beat -> run aligned and audible; release cuts; save/reload
   keeps state.

## P3 - Editor (1-1.5 weeks)

Goal: the S9 panel, matching Eloquent look-and-feel (dark theme, same widget
style), no graph.

Tasks:
1. Layout shell + dark theme.
2. Engine buttons; Tonic/Mode combos; 12 pitch-class tick row with
   auto-switch to Custom; sliders with readouts (Beats stepped, Density
   continuous, Curve, Accent percent, Arc bipolar); Walk radios.
3. Settings dialog: source type + event numbers, CC bindings table with
   conflict prevention, tuning constants, Reset to defaults.
4. Debug overlay (S9): engine state, pending note, run progress, counters,
   playhead beat.
5. Parameter mirroring from host automation and bound CCs into the controls.

Exit criteria (manual, REAPER):
* Panel matches S9; every control does what its section says.
* CC87 and the CC88..95 bindings remote-control parameters with GUI mirroring.
* Settings dialog round-trips; reset works.
*"No stuck notes or hangs after 100 rapid pairs; window resize keeps layout
 intact at min/max sizes."

## P4 - Validation, release (2-3 days)

Goal: shippable community zip.

Tasks:
1. CTest suite green in CI (Windows runner; GitHub Actions workflow like the
   Eloquent one, minus macOS until D3's post-v1 AU phase).
2. `pluginval` strict on VST3 + CLAP.
3. REAPER manual checklist from S10 fully signed; note deviations in the spec's
   section 13 list.
4. Release: versioned zip (VST3 + CLAP) without REAPER binaries; no installer.

Exit criteria:
* CI green; pluginval logs archived; spec S13 deviations recorded.

## P5 - v2 backlog (not scheduled)

Per S12: pair-window tuning + grace-note timeout,
fast-start curve direction, ornaments (chords/trills),
scale inference, presets, undo, AU port, light theme, AAX, Linux, multi-run
polyphony.

---

## Effort rollup

P0 1-2 d + P1 1 wk + P2 3-4 d + P3 1-1.5 wk + P4 2-3 d = roughly **3-5 weeks
part-time**. P0+P1 yields a proven headless engine in ~1 week; P0 alone already
gives a usable MIDI passthrough plug-in in the DAW.
