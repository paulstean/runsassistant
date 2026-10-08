# Runs Assistant - VST3 / CLAP / AU Specification

**Status:** Draft v1 (2026-10) | **Target:** JUCE 8 + CMake, C++17 | **License:** AGPLv3
(JUCE 8 open-source tier). No reference implementation; this spec is the behavioral
master. Sections below are definitive; open questions are listed in section 14 and
must be resolved by experiment or decision, not silently.

Runs Assistant is a MIDI "middleware" plug-in inserted before a virtual instrument.
The user plays **two notes together** (an unordered pitch range) while the runs engine
is armed in a direction (up or down); the plug-in consumes the pair and emits a
realistic **run** of scale notes from the start pitch to the target pitch over a
chosen number of host beats. Realism has two pillars:

* **Shape in time** - note onsets follow an S (bezier-like) curve along the run:
  slow, quick, then slowing again, instead of a metronomic sweep. One slider sets
  the strength of the curve; 0 percent is a linear (uniform) sweep.
* **Metric velocity emphasis** - on-the-beat notes play louder, scaled by warp
  proximity to the nearest beat (downbeats louder than mid-bar beats). One slider
  sets the strength; 0 percent is flat.

Velocity is additionally a fade from the first trigger note's velocity to the
target note's velocity, times an optional swell/taper arc, so a run can start fff
and end ppp (or vice versa) by how the pair is played. A toggle chooses whether
produced notes overlap slightly (for instruments whose legato mode listens for
overlapping notes) or stop before the next note starts.

---

## 1. Agreed decisions

| # | Decision | Choice |
|---|---|---|
| D1 | Framework / language / license | JUCE 8, C++17, CMake, AGPLv3, free community project. Conventions and house rules mirror the Eloquent project (`H:\Projects\Eloquent`) |
| D2 | Formats | VST3 + CLAP + AUv2 (both AU personalities) + Standalone (dev only), mirroring Eloquent (see section 2) |
| D3 | Trigger | Two notes played together (unordered range). Run fires on the note-on of the second note of the pair. Immediate start, aligned forward to the next beat |
| D4 | Direction | GUI three-state switch Off / Up / Down, also settable via keyswitch source. Up/Down is the emission direction; the pair itself is an unordered range |
| D5 | Pass-through | Engine Off: all MIDI passes through unmodified. Engine On: trigger pair and keyswitch traffic are absorbed; all other MIDI passes through |
| D6 | Run cut | A run is cut immediately when either trigger note is released, or when the engine state changes, or on host transport discontinuity (loop wrap / seek). One run at a time; triggers during an active run are ignored |
| D7 | Timing | Grid-locked to the host playhead (ppq). While stopped, a virtual grid at last-known bpm (default 120) from the trigger moment |
| D8 | Density | Continuous slider, notes per beat, 1..16. Total run notes = round(density x beats). The timing curve warps onset spacing; note duration is not user-visible |
| D9 | Shape | S-curve strength slider 0..100 percent. 0 = uniform spacing; positive = symmetric slow-fast-slow. Implementation: normalized power-S map (section 5.6); no negative direction in v1 |
| D10 | Pitch walk | When the requested note count exceeds the scale degrees in the span: user chooses Fold (bounce back at the boundaries) or Zig-zag (periodic single backward steps distributed through the run) |
| D11 | Velocity stack | base fade (trigger-note velocities) x accent emphasis (beat proximity) x arc (swell/taper), clamped 1..127 |
| D12 | Scale | Tonic + mode dropdowns plus an always-visible 12-pitch-class tick grid. Mode selection ticks the grid; manual ticks switch the dropdown to Custom |
| D13 | Remote control | Engine switch source choosable: MIDI notes, CC values, or program changes. Default CC87 (0-40=off, 41-79=up, 80-127=down); note mode default C0 / C#0 / D0; PC mode default 0 / 1 / 2. Every other parameter maps to a user-chosen CC, defaults CC88 onward in GUI order; the GUI mirrors incoming CC values |
| D14 | Ordering | Within a block, engine-switch events (CC / PC / keyswitch notes) are processed before note (pair) events, even at the same sample offset |
| D15 | State | Opaque versioned project chunk for all parameters and CC mappings. Runtime state (pending pair notes, active run, PRNG) is never persisted. No text preset files in v1 |
| D16 | UI | No graphical display of the run. Panel of controls only; dark theme matching Eloquent house style. All state edits immediate (no undo manager in v1) |

---

## 2. Formats and hosts

Same personalities and rationale as Eloquent (`H:\Projects\Eloquent\native\specification.md` S2);
this plug-in is MIDI-only with at most one silent audio passthrough bus where the
format demands one.

| Format | Personality | Notes |
|---|---|---|
| CLAP | `note-effect` | Reference target for engine proving |
| VST3 | `Instrument|Synth` category, one silent stereo bus | Hosts require a category with audio buses to expose MIDI routing; audio passes through untouched |
| AUv2 | `aumf` (MIDI-controlled effect) and `aumi` (MIDI FX slot) | Both personalities from one codebase |
| Standalone | JUCE wrapper | Dev/test only |

Primary validation host v1: REAPER (Windows, VST3 + CLAP). Other hosts best-effort;
no per-host routing table needed because the plug-in only emits notes on its own
output, which hosts deliver inline in the chain (the VST3 legacy-CC-host caveats of
Eloquent do not apply to notes).

---

## 3. Engine state and keyswitching

### 3.1 Engine states

Three states: **Off** (pass-through), **Up**, **Down**. Boots Off; the state persists
in the project chunk.

Switch sources (settings):

| Source | Off | Up | Down | Default |
|---|---|---|---|---|
| CC | CC87 value 0-40 | CC87 value 41-79 | CC87 value 80-127 | yes |
| Notes | C0 (12) | C#0 (13) | D0 (14) | - |
| PC | 0 | 1 | 2 | - |

* The CC number / note numbers / PC numbers are user-editable in settings.
* CC mode: the engine CC value selects a state by range (0-40, 41-79, 80-127);
  every value selects a state. The engine CC event itself is absorbed in every
  engine state.
* Note mode: the keyswitch notes are absorbed when the engine is Up or Down and
  passed through when the engine is Off.
* The GUI three-state switch mirrors and drives the same state; changing it from
  the GUI while a run is active cuts the run (section 5.6).

Engine-off handoff: a note-on that passed through while Off and is still **held**
seeds as that channel's pending note when the engine switches to Up/Down, so the
next note-on completes the pair right there and the run starts from that second
note-on (section 5.2 with a passed-through pending). Such handoff pendings never
jab and survive engine switches: they already sounded live, so switching the
engine off again via the engine CC requires no note-off traffic - a still-running
run is closed by the switch cut (section 5.6) and the normally sounding notes
keep passing through.

### 3.2 Trigger pair detection

An unordered pair is formed when a note-on arrives while exactly one other note is
currently **held and unpublished** on the same MIDI channel (section 5.2 defines
unpublished). On the second note-on of the pair:

* The run fires immediately at that sample offset; run start aligns forward to the
  next integer beat (section 5.3). If the trigger already sits on a beat line
  (within the alignment epsilon), the run starts on that beat.
* "Start pitch" is the lower of the two pitches, "target pitch" the higher; the
  engine direction selects which end the run starts from. Pair velocities pair with
  their pitches for the velocity ramp (section 5.4).
* Both trigger note-ons are absorbed and never emitted.

Degenerate pairs (equal pitches, or pitches that are not available in the current
scale after endpoint snapping, section 5.4) do not fire; the two notes pass through
as ordinary notes.

While a run is active, further note-ons are routed by the singles rule below and
never start a second run.

Edge: exactly two notes may also mean the user is playing a two-note chord as melody
while engine Up/Down. Accepted design: while the engine is on, any two-note chord
fires a run (that is the plug-in's purpose).

### 3.3 Engine off

Engine Off: MIDI passes through unmodified, including the trigger pair, keyswitch
notes (in note mode), and the engine CC does not exist as a consumer (engine CC
events pass through too; the engine is simply silent).

---

## 4. Parameters

All parameters are exposed as host-automatable parameters and carry a CC binding
(D13). Defaults in GUI order; "bound CC" is the MIDI CC number that drives the
parameter (0..127 or none). Incoming bound-CC events are absorbed when the engine is
on and mirrored to the GUI; when the engine is Off, CCs pass through and the
bindings are inert.

| # | Parameter | Range | Default | Default CC |
|---|---|---|---|---|
| 1 | Engine switch | Off / Up / Down | Up | 87 (source-defined, see 3.1) |
| 2 | Beats (run length) | 1..16, integer | 4 | 88 |
| 3 | Density | 1..16 notes/beat, continuous | 5 | 89 |
| 4 | Curve strength | 0..100 percent | 22 | 90 |
| 5 | Accent strength | 0..100 percent | 41 | 91 |
| 6 | Arc | -100..+100 percent | +15 | 92 |
| 7 | Tonic | C..B (12 steps) | C | 93 |
| 8 | Mode | index into mode list | Major | 94 |
| 9 | Walk mode | Fold / Zig-zag | Zig-zag | 95 |
| 10 | Note overlap | Off / On | On | none (not CC-mappable in v1) |

CC value mapping for continuous parameters: linear scale CC 0..127 to the parameter
range. Tonic: floor(CC x 12 / 128). Mode: floor(CC x nmodes / 128). Walk: 0..63 =
Fold, 64..127 = Zig-zag. Engine switch keeps its dedicated mapping (source table in
3.1).

The panel shows a `ccNN:VV` badge beside every CC-mapped control (and beside the
engine buttons when the engine source is CC): NN is the bound CC number and VV the
CC value in the middle of the bucket of values that reproduce the control's current
value; a control with no binding, or the engine in Notes/PC source mode, shows
`cc:--`.

Discrete parameters (tonic, mode, walk) also update from their GUI controls at any
time; direction of mirroring is always last-writer-wins between GUI, host
automation, and bound CCs, with no echo back onto the MIDI stream.

Settings (chunk state, not host parameters in v1):

* Engine switch source type: Notes / CC / PC.
* Engine switch event numbers (CC number, or the three keyswitch notes, or three
  PC numbers).
* Per-parameter bound CC numbers (table above), each may be "none".
* Alignment epsilon (section 5.1), accent falloff constants (section 5.7),
  note gate fraction (section 5.8) - visible in Settings as raw numeric fields with
  defaults; tuning knobs, not performance controls.

Duplicate CC assignments in the bindings table are refused in the UI with a
warning badge (including the engine CC).

---

## 5. Run engine

### 5.1 Grid lock and transport

The engine consumes the host playhead per block: ppqPosition, bpm, time signature
(numerator for bar lines; defaults 4/4 when absent), isPlaying. Beats are the unit
of scheduling; all run onsets are computed in beat space and converted to sample
offsets during processBlock.

* **Playing with ppq:** onsets are absolute ppq positions; a note is emitted on the
  first block whose ppq span (block start to block end, linearly interpolated) covers
  the onset, with its offset computed inside that block. Tempo ramps are honored for
  free because positions are beat-space, not seconds.
* **Stopped or no ppq:** virtual clock. At run start (trigger moment), anchor =
  current virtual beat; the virtual clock advances `blockSamples x bpm / (60 x
  sampleRate)` beats per block at last-known bpm (default 120 until anything is
  known). Run onsets are beats past the anchor; accents fall on integer offsets of
  the anchor, so the first beat of the run is accented.
* **Alignment:** run start beat = the next integer beat at or after the trigger
  position. Epsilon (default 2 ms converted to beats at the current bpm): a trigger
  landing within epsilon after a beat line counts as on that line (human anticipation
  tolerance). Virtual-clock mode skips alignment (trigger defines beat 0 as above).

### 5.2 Pair pending state (absorption mechanics)

The trigger notes must be absorbed, but a single note passing through must still
reach the instrument. Engine Up/Down only:

* Note-on with no other published-or-pending note on that channel: buffered as
  **pending**, nothing emitted yet.
* Note-on while exactly one pending note exists on that channel (any note count in
  the melody above one pending note is passed through immediately): **pair forms**;
  both notes consumed; also latches the pending note's sample-time offset for
  epsilon alignment; run starts.
* Note-off of a pending note before pairing: publish it late as note-on
  (at the note-off's offset) followed immediately by note-off (phrase becomes a
  rest-length note). Counted and surfaced in the debug overlay; musically this is
  the "jabbed a lone note" case.
* Note-on while a pending note exists but the second note equals the first pitch:
  no pair; publish the pending note and treat the new note-on as a normal
  passthrough note-on.
* Note-on while one note is already sounding on the instrument and a *second*
  melodic note arrives (pending note already published): straightforward
  passthrough (no pending note exists to pair with). False triggers from accidental
  near-simultaneous melody notes are a documented risk (section 12); the pair
  window is exactly "both notes still unpublished", which requires the second
  note-on to land before the first note-off.
* Engine switch to Off: flush all pending notes (publish late as above), cancel
  the active run.
* Note-offs of consumed pair notes are consumed silently when engine Up/Down.

Pending state is per MIDI channel. Channels are otherwise not filtered; events are
passed through on their original channel. Emitted run notes use the channel of the
first note-on of the pair.

### 5.3 Run assembly

On trigger (second note-on, sample offset o):

```
span_lo, span_hi = pair pitches sorted ascending
span_lo, span_hi = snap each to the nearest scale pitch class,
                   snapped inward (span never grows; see 5.4)
v_start = trigger note velocity at span_lo's pitch-owner
v_end   = trigger note velocity at span_hi's pitch-owner
run_direction = engine state (Up: span_lo -> span_hi; Down: span_hi -> span_lo)
n = clamp(round(density x beats), 1, 4096)
start_beat = aligned beat (5.1)
onset_i (i = 0..n-1), in beats from start_beat:
    p_i = i / (n - 1)                      [endpoints inclusive]
    onset_i = beats x curve_map(p_i)       [section 5.5]
pitch_i = walk sequence (section 5.4) in run_direction
vel_i   = clamp(round(base_i x arc_i x accent_i), 1, 127)
    base_i = v_start + (v_end - v_start) x p_i        [linear by note index]
    arc_i  = 1 + arcSlider x p_i                      [arc -1..+1 -> 0..2 multiplier]
    accent_i = 1 + accentStrength x weight x falloff_i
        falloff_i = max(0, 1 - 2 x d_i), d_i = beats to nearest integer beat
        weight = 1.0 on a bar line, 0.75 otherwise (time signature numerator)
gate_i  = 0.6 x (onset_{i+1} - onset_i)   interior notes
final note: no fixed gate - held (tail-end steady, section 5.6)
```

(curve_map is defined in section 5.5.)

If `n - 1 < scale degree count in span` the run is under-filled: only `n` notes
are emitted, spread as a proportional stride map across the whole span (the
climb is compressed across the run; 5.4) and the pitch walk still ends exactly
on the target.

### 5.4 Pitch walk (Fold and Zig-zag)

Let D = number of scale-tone steps from span_lo to span_hi inclusive count of
distinct scale pitches along the walk, and S = n - 1 steps required.

* **Fold:** the walk advances in scale tones; at a span boundary it reverses
  direction. Endpoint exactness: with fold, both ends are visited when the walk
  bounces, so the final pitch is forced to the target regardless (folded walks
  that would overshoot the target note count are trimmed from the interior, never
  from the endpoints: the walk starts at the start pitch and the last pitch is the
  target pitch; intermediate notes alternate direction at the boundaries).
* **Zig-zag:** the walk is monotonic-forward with periodic single backward steps.
  Number of backward steps b = (S - D) / 2 (this is integral when S - D is even).
  The b backward steps are distributed as evenly as possible across the run (backstep
  j at round(j x S / (b+1)); deterministic, no randomness in v1; net travel is
  proportional to position across the run). If
  S - D is odd, drop a note (n = n - 1, one fewer note than asked) and recompute;
  count the dropped note in the debug overlay. Under-fill (S < D) and the exact
  fit (S == D) use the proportional stride map instead - both modes produce the
  same compressed climb then.
* Consecutive pitches never repeat (a backward step is exactly one tone back; fold
  turns at the boundary without doubling a note).
* Endpoints: pitch_0 = run start, pitch_{n-1} = run target, always exact.

The user chooses the mode per parameter D10; the choice is live at trigger time.

### 5.5 Shape (S-curve map)

```
curve_map(p) = 1 / (1 + ((1 - p) / p) ^ e),  e = 1 - 0.9 x curveStrength
```

* curveStrength 0 -> e = 1 -> y = p exactly (linear).
* Larger strength -> smaller e -> the onset spacing widens at both rims
  (deliberate start, settling target note) and the middle of the run is
  traversed quickly: slow, quick, slow (section 1).
* Symmetric: y(1 - p) = 1 - y(p); mid anchor y(0.5) = 0.5.
* Monotonic; endpoints exact. Cheap evaluation; no inversion needed.

The v1 draft inverted this family (exponent k = 1 + 9 x curveStrength); a
k > 1 exponent crowds the onsets AT the rims, which reads as fast, slow, fast
and collapses beyond ~5 percent strength (REAPER pass). See the deviations
list (13.11).

Discretionary alternative (cubic bezier control points) documented for comparison;
v1 ships the power map because it is one formula and exactly linear at 0 percent.

### 5.6 Cut conditions

The active run ends immediately (no fade) when:

* either trigger note is released (absorbed note-off seen),
* the engine state changes (GUI / bound engine CC / host automation),
* a host transport discontinuity is detected while playing: ppq backward jump
  beyond epsilon (loop wrap, click-back) or a forward jump larger than one run beat
  without an intervening trigger (seek),
* all-notes-off (CC 123) is received.

On cut: any already-emitted run notes still sounding get immediate note-offs at the
cut's sample offset; scheduled-but-not-yet-emitted notes are discarded. Cut
execution is always inline in processBlock; nothing waits.

A run that has produced all n note-ons (and all interior note-offs) is **tail-end
steady**: the final target note remains held and is NOT gated by section 5.8; its
note-off waits for the first trigger-note release, which arrives at some later
sample offset. (Example: trigger notes held for 8 beats with Beats = 4 - the run
spawns over beats 1-4, then the target note rings on beats 4-8 until note-off.)
While tail-end steady, the cut rules of this section still apply (each cut source
ends the held target note the same way); after the target note's note-off is sent
the run is finished and no UI or host notification is required.

### 5.7 Velocity accent model

Accent proximity falloff and beat weights use the same d_i beat-distance measure as
onsets. Bar-line detection needs the time signature numerator; hosts that do not
provide one default to 4/4 (every 4th beat of the virtual clock is a bar line;
bar origin = host bar start when available, else the run's own start beat counts
as beat 1).

Constants (settings-exposed raw values with defaults above): downbeat weight 1.0,
mid-bar beat weight 0.75, falloff reaching zero at half a beat from the grid line.
These are documented as v1 tunables, reviewed by ear in the REAPER test pass. The
engine applies these Settings values (they travel with the run's parameter block);
they are not free-standing constants.

### 5.8 Emission details

* Emitted notes: note-on/note-off pairs on the pair's channel; velocity per 5.3.
* Gate: 60 percent of the gap to the next onset. The final target note is the
  exception (tail-end steady, section 5.6): it holds until the first trigger-note
  release or another cut condition, so Beats controls the run's sweep span while
  the held pair controls when the target note actually ends.
  No user-facing note-length control in v1 (the gate fraction is a Settings value).
* **Overlap toggle** (host parameter, section 4 row 10, default off): with
  overlap ON each interior note is held slightly PAST the next note-on - its
  note-off lands one overhang later, where the overhang is the clamped gate
  fraction (0.05 minimum so the overlap never vanishes) of the gap FOLLOWING the
  next onset; for the last interior note (no following gap) the overhang uses its
  own gap, extending past the target onset into its held tail. The note-off must
  stay strictly before the note-on after that, because Fold walks can revisit a
  pitch two steps apart (a late note-off would kill the retriggered pitch).
  Overlap only shifts interior note-offs; cut semantics (5.6) and the tail-end
  hold are unchanged. Off: the classic 5.5-percentage gate (note-off before the
  next note-on). The overhang share reuses the gate-fraction tuning value.
* All emitted events carry exact sample offsets (block-relative). No input MIDI
  event is dropped silently: pass-through events pass; absorbed events are
  consumed by documented behavior (pair keyswitch / keyswitch notes / bound CCs).
* MIDI invariant: every input event is either re-sent unmodified or consumed by a
  documented rule. The debug overlay counts late-published singles, zig-zag parity
  drops, and cuts.

---

## 6. Processor specification

### 6.1 processBlock order (normative)

1. Read host playhead (ppq, bpm, timeSig, isPlaying, timeInSamples); update
   transport-discontinuity detection and the virtual clock when stopped.
2. Collect input MIDI into a preallocated scratch buffer ordered by sample offset
   (stable within offset stream order). No allocation; overflow drops with a
   debug counter (same house rule as Eloquent).
3. Pass 1: engine-switch events (CC / PC / keyswitch note mode) in stream order;
   update engine state; flush pending and cut any active run on state change.
4. Pass 2: remaining events in input order - pair mechanics (5.2), bound-CC
   mirrors for other parameters, pass-through of everything else (unpublished
   singles are held per 5.2).
5. Run scheduler: emit due run note-ons and note-offs at computed offsets; apply
   cut rules (5.6) for releases / discontinuities pending from this block.
6. Publish engine-to-UI state (engine state, active run pitch walk index, last
   emitted pitch/velocity, counters) through the lock-free FIFO.

### 6.2 Real-time rules

No allocation, no locks, no file I/O, no UI calls on the audio thread. All scratch
preallocated in prepareToPlay. Denormal-safe; no float comparisons without epsilon.

---

## 7. State, persistence

Chunk: magic `RUN1` + u32 schema_version + all parameters (section 4) + settings
(engine source type + numbers + bindings table + tuning constants + GUI window
size footer `RUNV`, same pattern as Eloquent). Validation: magic + version check,
range-clamp every field, ignore trailing bytes, reject truncated chunks (keep
current state). Schema v2 adds the Overlap parameter; v1 chunks load with
Overlap off. Runtime state (pending pair notes, active run, PRNG) is never
persisted. No text preset files in v1. Every parameter edit marks the
host state dirty (VST3 dirty flag / CLAP `stateMarkDirty`) except pure GUI
geometry. The engine switch (GUI, CC, keyswitch, PC) mirrors into the Engine
parameter for the host dirty/notification path, and pending CC mirrors are
flushed into parameters before the chunk is written so a save always carries
the last held control values.

No text preset files v1 (D15). Undo: none (D16). Reset to defaults action in
Settings restores factory parameter values and CC bindings (not undoable in v1).

The same payload can also be copied to and pasted from the system clipboard as
JSON from the editor (section 9, Copy to Clipboard / Paste from Clipboard): it
carries all parameters as real values plus the settings, and it only ever
travels through the clipboard, never the file system (D15 still holds). Paste
validates like the chunk reader (reject anything that is not ours, range-clamp
every present field, keep absent fields), so a bad clipboard changes nothing.

---

## 8. Host parameters

Section 4's table is exposed verbatim: Engine (discrete 3), Beats (int 1..16),
Density (float 1..16), Curve (float 0..1), Accent (float 0..1), Arc (float -1..+1),
Tonic (int 0..11 with value-to-string), Mode (int indexed), Walk (bool/discrete 2),
Overlap (bool). Settings and CC bindings are chunk-only, never parameters.

---

## 9. UI specification

Default window 960 x 480, resizable within 720 x 480 .. 1400 x 600, DPI-aware,
dark theme (Eloquent palette). One graph: the curve shape preview (below).
No list, no playhead display.

```
+----------------------------------------------------------------------------+
| RUNS ASSISTANT      [Settings]   Engine: [Off] [Up] [Down]                  |
+----------------------------------------------------------------------------+
| Tonic [C v]   Mode [Major v]                                               |
| [x]C [x]C# [x]D [x]D# [x]E [x]F [x]F# [x]G [x]G# [x]A [x]A# [x]B           |
+----------------------------------------------------------------------------+
| Beats  [ 4 ]      Density [====o----] 5.0 n/beat                           |
| Curve   [==o-------] 22%   Accent [===o-----] 41%    Arc [==o-----] +15%    |
| Walk: ( ) Fold  (x) Zig-zag   Overlap: [x] On                              |
| [ curve preview: smooth curve bottom-left to top-right (Up), mirrored for Down ] |
+----------------------------------------------------------------------------+
| Midi Export                                                                |
| Start [C4 v] v[======] 100   Target [C5 v] v[======] 90      [Drag MIDI]   |
| Start C4 and Target C5 are not in the scale  (red; shown only when out)    |
+----------------------------------------------------------------------------+
| Copy & Paste Settings Between Runs Assistant Instances                     |
| [Copy to Clipboard] [Paste from Clipboard]               [Debug overlay]   |
+----------------------------------------------------------------------------+
```

* **Engine switch:** three toggle buttons; exactly one active. Mirror of the same
  state that CC / PC / keyswitch drives.
* **Scale rows:** Tonic and Mode dropdowns plus the twelve pitch-class checkboxes,
  always visible. Choosing a tonic + mode re-ticks the boxes; manually un-ticking
  or ticking any box switches the Mode dropdown to `Custom` and keeps the current
  tick set as an explicit pitch-class set stored relative to the shown tonic label.
  Chromatic is a preset (all twelve ticked).
* **Mode list v1:** Major (Ionian), Dorian, Phrygian, Lydian, Mixolydian, Aeolian
  (Natural Minor), Locrian, Harmonic Minor, Melodic Minor (ascending), Whole Tone,
  Octatonic (whole-half), Octatonic (half-whole), Major Pentatonic, Minor
  Pentatonic, Blues, Chromatic, Custom.
* **Sliders:** Beats (stepped 1..16, drag or click +/-), Density (continuous,
  readout n/beat one decimal), Curve and Accent (percent), Arc (bipolar percent).
  Hover tooltips state the effect in plain language. All sliders mirror bound CCs
  (subsection 4) in real time without dirtying the chunk from MIDI input.
* **Walk radios:** Fold / Zig-zag, one selection.
* **Curve shape preview:** a square static graph under the Walk/Overlap row plots
  the run itself as one smooth curve: horizontal = exact onset time
  (`beats x curveMap(i/(n-1))`, the engine's own maths sampled across the run;
  n = round(density x beats) live from the sliders, clamped for display),
  vertical = pitch progress through the run. With Engine up the curve runs from
  bottom-left (first note, start of time) to top-right (last note, end of time);
  with Engine down it is mirrored, from top-left to bottom-right (Engine off
  shows the up shape). One dot per run note sits on the curve at its exact
  onset. A faint diagonal ghost marks the even (0 percent) sweep: at 0 percent
  the curve is that straight line, and higher curve values bend it away from it
  (slow start, quick middle, slow finish). Curve/Beats/Density changes and the
  Engine switch re-render it live. Illustrative only: not interactive, no
  playhead cursor.
* **Overlap toggle:** Off/On checkbox next to the walk radios (host parameter,
  saved in the chunk); see 5.8 for the emission rule.
* **Export row (offline render, drag to the arrange window):** `Start` and
  `Target` note dropdowns (C1..C8, defaults C4 / C5), one velocity slider each
  (1..127, defaults 100 / 90) and a **Drag MIDI** button. All four inputs are
  session-only: not host parameters, not CC-mapped, not written to the chunk or
  the clipboard payload. The row renders ONE complete run from the current
  parameter and settings state plus those inputs and hands a type-0 MIDI file
  to the OS as a native file drag the user can drop into the arrange window:
  * direction follows the endpoints (lower-to-higher = Up, higher-to-lower =
    Down); the pair is ordered by pitch as in 3.2, and Start carries the run's
    first velocity while Target carries its last;
  * the file starts at beat 0, uses 480 ticks per quarter, carries tempo and
    time-signature meta events from the current transport (120 bpm / 4-4 when
    the host has reported neither) and a track name; the run anchors its own
    bar lines at beat 0 (5.7 first-anchor rule) and skips grid alignment (5.1
    virtual-clock rule), so a drop at a bar line puts the accents on bar lines;
  * the held final note (5.6 tail-end steady) is closed by an explicit
    note-off one beat after the run's last onset;
  * events are written in non-decreasing beat order with the note-off leading
    any same-beat note-on (5.8 / 6.1 ordering), including overlap runs;
  * an equal pair, or any input the engine refuses, reports a short reason on
    the button instead of rendering (no dialogs, no file).
  * the row sits under a full-width rule and the heading `Midi Export`. While
    Start and/or Target lies outside the selected scale (tonic, mode or manual
    ticks) a red line under the row names the offending note(s), that dropdown
    turns red and its tooltip spells out the snap. Advisory only: the render
    still snaps both endpoints inward (5.3) and only refuses when the snapped
    span collapses (3.2); the warning line keeps its row so the layout never
    jumps.
  Rendering is message-thread only; file I/O here is allowed (6.2 governs the
  audio thread). Press-and-drag starts the native drag; a plain click only
  flashes a hint.
* **Settings dialog:** engine source type (Notes / CC / PC) + its event numbers;
  per-parameter CC bindings table with conflict prevention; tuning constants
  (alignment epsilon, gate fraction, downbeat weight, mid-bar beat weight);
  Reset to defaults.
* **Copy to Clipboard / Paste from Clipboard:** bottom-row buttons that move the
  whole state through the system clipboard as indented JSON text. The payload is
  the chunk's data, keyed by parameter id: a `params` object with every
  parameter as a real value (engine, beats, density, curve, accent, arc, tonic,
  mode, walk, overlap) plus a `settings` object (engine source type and numbers,
  the eight CC bindings, the four tuning constants, the custom tick set), wrapped
  with `format: "runs-assistant"` and a schema `version`. Copy flushes pending
  CC mirrors first, so the text carries the last held control values. Paste
  accepts only its own format and schema version, applies through the normal
  parameter-notify path (engine through the regular switch path), range-clamps
  every present field like the chunk reader and keeps absent fields; anything
  else is rejected without changing anything. Both actions give short
  label feedback on the button (Copied / Pasted / reason), no dialogs. The row
  sits under a full-width rule and the heading `Copy & Paste Settings Between
  Runs Assistant Instances`.
* **Debug overlay:** engine state, pending note (if any), active run progress
  (last emitted pitch/velocity, notes emitted / n), counters (parity drops,
  late-published singles, cuts), current playhead beat and bpm.
* Keyboard: nothing special required; Esc closes Settings. Screen-reader labels on
  all controls.

---

## 10. Validation and acceptance

* Headless conformance vectors (CTest): pair detection matrix (staggered / jabs /
  three-note chords / same-pitch), absorption + flush on engine-off, epsilon
  alignment, endpoint snapping, fold and zig-zag walks (parity drop case
  included), curve map monotonicity + linearity at 0 percent + symmetric anchors,
  velocity stack math + clamps, accent falloff at exact/half-beat distances,
   gate math, tail-end hold of the final note until trigger release,
   cut semantics (each condition of 5.6), zig-zag parity drop counter,
  chunk round-trip + truncated rejection, clipboard JSON round-trip +
  foreign/incompatible text rejection + clamp-on-paste.
* Real-time gates: `pluginval` strict all formats; no-allocation instrumented
  processBlock run.
* REAPER manual checklist (per build): engine on, pair triggers on the beat ->
  run emits aligned; engine off -> full passthrough; absorb/refund of jabs
  correct in the REAPER MIDI item view (altered notes land downstream);
  curve slider sweep audible (spacing sane); accent audible against a click;
  fff->ppp across the pair velocities; arc sweep; fold vs zig-zag audible on an
  over-dense run; cut on early release; loop wrap cuts; save/load round trip;
  CC87 + CC88..95 remote control with GUI mirroring; Copy to Clipboard /
  Paste from Clipboard restores the whole state (and clamps a hand-edited
  value).
* Acceptance for v1 is the REAPER pass plus CI green; other hosts compile-load
  tested only (best-effort).

---

## 11. Risks and mitigations

* **False pair triggers from melodic playing** (two fast legato notes before the
  first note-off) -> pair window is deliberately strict (needs both still
  unpublished); documented behavior, tuning via the user's melodic style;
  potential future setting (pair window length in ms, disabled-timeout variant).
* **Late-published jab notes** sound compressed; counted + visible in overlay so
  users understand their melody jabs during engine-on.
* **Zig-zag parity drop** changes the audible note count by one (n = n - 1);
  documented; alternative (one 2-step backward) listed as an experiment flag.
* **Hosts without ppq / timeSig** -> virtual clock defaults; documented.
* **Bound-CC trap:** engine CC87 arriving from a "pass through" DAW loop could
  double-trigger state saves; mitigation: bound-CC input is consumed, never echoed,
  and parameter edits from CC go through the same dirtying path as GUI edits only
  when the value actually changes.

---

## 12. Deferred v2

Pair window length + grace-note timeout; velocity humanization (per-note random
deflection); negative
curve direction (fast start); chord/trill ornaments in runs; scale inference from
the trigger pair; text preset files; undo manager; light theme + full theme
editor; AAX; Linux release binaries; per-channel run state (several pairs at
once).

---

## 13. Intentional deviations / simplifications vs Eloquent conventions

1. No JSON preset files v1 (chunk only); no bank library, no undo manager.
2. UI is a control panel only (no graph, no inspector, no list).
3. Late publishing of unpaired single notes (section 5.2) is a new mechanism with
   no Eloquent analog; it exists so melody survives while the engine is on.
4. No per-host routing documentation: note output (unlike CC legacy output)
   delivers inline in mainstream hosts; residual host issues documented when
   discovered, per host.
5. Switching between real ppq beat space and the stopped virtual clock
   (playing/stop boundary) cuts an active run: scheduled onsets in one beat
   space are meaningless in the other (interpretation of 5.6 "transport
   discontinuity").
6. All-notes-off (CC 123) both passes through AND cuts the active run:
   downstream instruments still need their own all-notes-off for passthrough
   chord notes.
7. Chunk: the optional RUNV window-size footer is always written; a short
   tail (1..7 bytes) after the settings payload is treated as a truncated
   chunk (rejected) rather than a footer (both rules from section 7).
8. Degenerate snapped pairs (S5.4 snap removes the span after the pair was
   consumed) are compensated by late-publishing both trigger notes at the
   trigger offset, keeping the S5.8 "no silent drops" invariant.
9. "Playing" is read as ppq known: hosts that report playing without ppq
   fall back to the virtual clock rather than the real grid (host limitation,
   found in the JUCE wrapper).
10. Bar-line origin (S5.7): the host bar start is used only in ppq-playing
    mode; JUCE's `PositionInfo -> CurrentPositionInfo` conversion leaves
    `ppqPositionOfLastBarStart = 0` when the host never provides it, so
    "available" is undetectable. In ppq mode the origin 0 IS the ppq
    downbeat (planned behavior: host bar start when available; shipped:
    host value in ppq mode, run start otherwise); in virtual-clock mode bar
    lines are anchored on the run's own start beat exactly as S5.7's
    fallback specifies. Reason: the ppq grid origin is a downbeat in every
    mainstream host, and "unset" cannot be distinguished from "bar 0".

Any further deviation found during implementation must be appended here with the
planned behavior, the shipped behavior, and the reason.

11. **Curve map direction (S5.5).** Planned: power-S with exponent
    k = 1 + 9 x curveStrength. Shipped: the same family with the INVERTED
    exponent e = 1 - 0.9 x curveStrength. Reason (REAPER pass): with k > 1
    the onsets crowded AT the rims - an instant burst at the start, a
    metronome-style hole in the middle, an instant burst at the end; only
    strengths up to ~5 percent were usable. The inherited introduction
    (S1) reads slow, quick, then slowing again, and the lowest exponent of
    the same family (e <= 1) produces exactly that: onset spacing widens at
    both rims and rushes through the middle, linear at 0 percent, all
    anchor/monotonic/symmetry properties kept. Same reason documented in
    CurveMap.h.

12. **Note overlap toggle (S5.8).** Not in the v1 table; added as host
    parameter row 10 (default off, chunk covered by schema v2) after the
    owner report "add a toggle to choose if produced notes overlap" - many
    virtual instruments engage their legato mode only when the notes
    overlap. It reuses the gate-fraction tuning value as the overhang share
    and is not CC-mappable (bindings table unchanged at 8 rows; every
    parameter list enumerates 9 GUI rows for the engine + 8 bindings).

13. **Engine-switch mirroring (S3.1/S7).** Planned: GUI engine switches
    bypass the host parameter entirely. Shipped: the switch mirrors into
    the Engine parameter (and non-parameter chunk edits set the host dirty
    flag through the nonParameterStateChanged change detail). Reason:
    bypassing left the host unaware (project not marked modified; saved
    chunk carried a stale engine state). Echo safety is preserved because
    the parameter now always tracks the real engine state (same-value
    echoes dedup; the grace window covers stale host echoes of older
    cached values).

15. **Beat-count scaling of the walk (S5.4, REAPER pass).** Planned: the
    under-filled run advances one scale tone per step and the target is
    forced onto the final note (advance-then-leap); over-filled zig-zag
    schedules the b backward steps at one every floor(S / (b+1)) positions
    from wherever the walk happened to be. Shipped: under-filled runs use a
    proportional stride map (seq[k] = round(k x D / S)) so the whole climb
    is compressed across the run (leaps spread through the walk, no
    terminal leap); over-filled zig-zag schedules backstep j at
    round(j x S / (b + 1)), spreading backsteps across the entire run so
    net travel scales with position. Reason (REAPER pass): advance-then-leap
    landed the target note mid-run for short runs against wide spans (read
    as a truncated run plus a jump), and the fixed-period distribution
    clustered every backstep into the first b x period positions - a 2-note
    trill for most of the run with the climb shoved into the last few
    steps. Both endpoints and the parity contract are unchanged.

14. **Mirror flush at save (S7).** Planned: parameter mirrors applied only
    while an editor Timer runs. Shipped: getStateInformation drains the
    pending mirror FIFO first, and the mirror Timer is started from the
    editor; a bound-CC tweak with the editor closed still lands in the
    chunk at the next save because the flush runs inside the state write.

---

## 14. Open items (to settle by experiment or decision)

1. Endpoint-inclusive timing (first note exactly at run start, last exactly at
   run end) vs last onset slightly before the end. Default: endpoints inclusive.
2. Zig-zag parity handling: drop-note vs double-back-step. v1: drop + counter.
3. Accent falloff shape (linear vs exponential half-beat reach) - by ear in the
   REAPER pass.
4. Early-release cut feel (section 5.6) - the user explicitly wants to try it;
   if jarring, a "release lets the run finish" mode is the fallback (v2 flag).
5. Whether singles emitted during a pending-pair window should carry reduced
   velocity; v1: untouched pass-through velocities.
