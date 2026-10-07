# Runs Assistant

A MIDI "middleware" plug-in (VST3 / CLAP / AU): play **two notes together** to
fire a realistic scale **run** between them - shape-in-time (S-curve) and
metric velocity emphasis, over a host-beat span. Community / free / open
source (AGPLv3, per JUCE 8 licensing).

* [specification.md](specification.md) - normative behavioral specification.
* [plan.md](plan.md) - phased build order.

| Phase | Status |
|---|---|
| P0 scaffold + passthrough spike | done (REAPER load pass pending user) |
| P1 core engine (JUCE-free) | done |
| P2 processor + state + CC mapping | done (REAPER manual pass pending user) |
| P3 editor | done (REAPER manual pass pending user) |
| P4 validation + release | CI + pluginval + release packer in place; REAPER checklist pending user |

## Stack

JUCE 8.0.15, C++17, CMake 3.27+. Dependency pins are git submodules
(`libs/JUCE` tag 8.0.15, `libs/clap-juce-extensions`).

## Toolchain

Windows: Visual Studio 2022/2026 with C++ workload + Windows SDK.
CMake 3.27+. First configure needs network (JUCE pulls the VST3 SDK).

## Build (Windows)

```pwsh
git submodule update --init --recursive --depth 1
cmake --preset win-x64
cmake --build --preset win-x64
ctest --test-dir build/win-x64 --output-on-failure
```

Binaries land under `build/win-x64/RunsAssistant_artefacts/Release/`
(`VST3/`, `CLAP/`, `Standalone/` subfolders). `RUNS_ENABLE_CLAP=OFF`
skips the CLAP wrapper.

## Deploy to REAPER (dev loop)

Building does NOT deploy: close REAPER first (a loaded plug-in locks its
binary), then:

```pwsh
pwsh tools/deploy.ps1
```

which copies the fresh CLAP into `%LOCALAPPDATA%\Programs\Common\CLAP\`
and merges the VST3 bundle into `%COMMONPROGRAMFILES%\VST3\`.

## Playhead spike (P0)

The debug overlay (bottom-right toggle) shows the current playhead read
(beat / bpm) and engine state. To capture a per-block CSV log in REAPER,
set an environment variable before starting REAPER:

```pwsh
$env:RUNSASSISTANT_SPIKE_LOG = "H:\tmp\runs-spike.csv"
```

## Release (P4)

```pwsh
pwsh tools/make-release.ps1
```

packs `out\RunsAssistant-<version>-win-x64.zip` (VST3 + CLAP + README
+ SHA256). CI (`.github/workflows/ci.yml`) builds all formats, runs CTest,
and gates the VST3/CLAP through pluginval strictness 7.
