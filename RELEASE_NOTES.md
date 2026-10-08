# Runs Assistant 0.1.3 - Release Notes

Date: 2026-10-09 | Platforms: Windows x64 + macOS universal (arm64 + x86_64)
| Formats: VST3 + CLAP (+ AU on macOS) | License: AGPLv3 (JUCE 8 open-source tier)

Runs Assistant is a MIDI "middleware" plug-in: play two notes together to fire a
realistic scale run between them - shaped in time (S-curve) and accented to the
beat, over a chosen number of host beats. Insert it before your virtual
instrument; it makes no sound of its own and passes everything it does not
consume straight through.

## Contents of the release zips

| File | Purpose |
|---|---|
| `Runs Assistant.vst3` | VST3 plug-in bundle (both platforms) |
| `Runs Assistant.clap` | CLAP plug-in (both platforms) |
| `Runs Assistant.component` | Audio Unit, `aumf` MIDI-controlled effect (macOS only) |
| `RunsAssistantMFX.component` | Audio Unit, `aumi` for Logic's MIDI FX slot (macOS only) |
| `UserManual.html` | Full user manual (open in any browser) |
| `RELEASE_NOTES.md` | This file |
| `README.md` | Project overview, build-from-source instructions |
| `LICENSE` | GNU Affero General Public License v3 |

Zip names: `RunsAssistant-0.1.3-win-x64.zip`,
`RunsAssistant-0.1.3-mac-universal.zip`. A SHA-256 file accompanies the
Windows zip.

## Install

**Windows**

1. Close your DAW.
2. Copy `Runs Assistant.vst3` into `C:\Program Files\Common Files\VST3\`
   and/or `Runs Assistant.clap` into
   `%LOCALAPPDATA%\Programs\Common\CLAP\` (or your CLAP folder).
3. Rescan plugins and insert Runs Assistant on a MIDI track **before** the
   instrument (Runs Assistant -> instrument -> mixer).

**macOS**

1. Close your DAW.
2. Copy `Runs Assistant.vst3` into `/Library/Audio/Plug-Ins/VST3/`,
   `Runs Assistant.clap` into `~/Library/Audio/Plug-Ins/CLAP/`, and the Audio
   Units into `/Library/Audio/Plug-Ins/Components/`:
   * `Runs Assistant.component` - insert as a MIDI-controlled effect on an
     instrument track (any AU host),
   * `RunsAssistantMFX.component` - the `aumi` personality for Logic's MIDI
     FX slot.
3. **Unsigned build:** the CI zips carry an ad-hoc signature only (no
   Developer-ID / notarisation). Clear the quarantine attribute on everything
   you copied, e.g.
   `sudo xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/`
   (plus any user-level plug-in folder such as
   `~/Library/Audio/Plug-Ins/`).
4. **Force an AU rescan.** macOS caches AU scan results, and a failure
   recorded *before* step 3 stays cached - clearing quarantine alone does not
   re-run the check. Quit Logic first, then either use Logic's
   Plug-in Manager -> Reset & Rescan All, or flush the system cache:
   `killall -9 AudioComponentRegistrar` and delete
   `~/Library/Caches/AudioUnitCache/com.apple.audiounits.cache` (plus
   `com.apple.audiounits.sandboxed.cache` if it exists), then start Logic.
5. Optional but decisive - both commands must print
   `AU VALIDATION PASSED`, otherwise Logic will not list the plug-in:
   `auval -v aumf Run1 Runs` and `auval -v aumi Run2 Runs`.
   (`auval -a` lists every registered AU with its type/subtype/manufacturer.)
6. Insert Runs Assistant **before** the instrument: the `aumf` personality in
   an Audio FX slot, the MFX personality in Logic's MIDI FX slot (the slot
   above the instrument).

Both builds are universal (arm64 + x86_64).

## What's new in 0.1.3

* **Logic sees the MFX personality again** - `RunsAssistantMFX.component`
  (`aumi`, Logic's MIDI FX slot) was compiled with the shared stereo audio
  input bus, while the AU wrapper reports 0 input channels for
  `kAudioUnitType_MIDIProcessor`. `auval` rejected the component on that
  mismatch, and Logic leaves anything that fails validation out of its menus
  (and caches the failure). The aumi build now declares no input bus
  (specification S2: a passthrough bus only "where the format demands one").
* **AU validation gates the mac build** - `mac.yml` installs both components
  and runs `auval -v aumf Run1 Runs` + `auval -v aumi Run2 Runs` before it
  packages the zip, so a component Logic would hide can no longer ship.
* **macOS install steps** - quarantine removal is no longer presented as the
  whole fix: the docs now cover flushing the AU registration cache (a failed
  scan stays cached), the two `auval` checks, and where each personality is
  inserted. Version bumped to 0.1.3 so Logic re-evaluates the changed MFX
  channel layout instead of trusting its 0.1.2 cache.

## What's new in 0.1.2

* **Humanize (S5.9)** - seeded velocity deflection and timing jitter with
  main-panel `H.vel` / `H.time` strength sliders; `Seed 0` draws a fresh seed
  every run, a pinned seed replays the identical pattern (also for offline
  renders). Chunk schema v3; old projects load with Humanize off, Seed 0.
* **Bound-CC badges** - every CC-mapped control shows `ccNN:VV` (bound CC
  number and the value that reproduces its position); `cc:--` when unbound.
* **MIDI export** - the `Midi Export` row renders one complete run from the
  current settings and drags a type-0 `.mid` file straight into your arrange
  window (Start/Target notes + velocities, tempo and time signature from the
  transport). Red advisory line when an endpoint is outside the scale.
* **Copy / Paste between instances** - whole state (engine, scale, all
  parameters, settings) moves through the system clipboard as JSON text.
* **Curve shape preview** - live graph of the run's timing curve with one dot
  per note, Up/Down aware, against a straight 0% reference ghost.
* **Engine CC87 is now ranged** - 0-40 = Off, 41-79 = Up, 80-127 = Down, so a
  single fader can select all three states. Engine-off handoff: a note still
  held when you arm the engine seeds the pending pair, so the next note
  triggers right there.
* **UI polish** - section headings (`Midi Export`, `Copy & Paste Settings...`),
  factory defaults matching the manual's example set, 960x480 default window.
* **Validation** - randomized-session invariant harness (`InvariantsTests`)
  asserting byte-level MIDI stream invariants; manual CI workflows for Windows
  and macOS; pluginval gate; release packer.

## Earlier versions

* **0.1.1** - fix for a REAPER engine-parameter echo loop; engine clicks route
  around the host parameter with an echo grace window; version in the title.
* **0.1.0** - first packaged Windows build (P4): VST3 + CLAP, full run engine,
  S9 editor panel, CC mapping, settings dialog, debug overlay.

## Requirements and notes

* Primary validation host: REAPER 7.x (Windows, VST3 + CLAP). macOS builds are
  CI-produced and load-tested only; other hosts are best-effort.
* The VST3 appears as an instrument with one silent stereo bus - that is a host
  category requirement; only MIDI matters and audio passes through untouched.
* One run at a time. Two notes played together are the trigger; while the
  engine is on, very fast legato playing can occasionally form an accidental
  pair - switch the engine to Off for straight melodic playing.
* No undo manager and no preset files in v1: use Copy/Paste for settings
  before experimenting.
* Known-unfinished: the manual REAPER verification checklist (plan P4) is
  pending owner pass. AU validation (`auval`, both personalities) now gates
  the macOS build in `mac.yml`.

## Getting involved

Source, issue tracker and the normative `specification.md` live in the
repository. Contributions welcome - see `AGENTS.md` for the dev loop
(build -> ctest -> deploy -> REAPER pass).
