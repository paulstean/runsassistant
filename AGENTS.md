# AGENTS.md - Runs Assistant development guide

Runs Assistant is a JUCE MIDI middleware plug-in (VST3/CLAP/AU): play two
notes together to fire a scale run between them (S-curve shape in time +
metric velocity emphasis). See **specification.md** (normative) +
**plan.md** (build order). This file defines how we build, verify, and ship.

## Repository layout

```
specification.md    normative behavior master (S1..S14)
plan.md             phased build order (P0..P5)
Source/core/        plain C++17 engine, ZERO JUCE includes (include-check in CMake)
Source/processor/   JUCE AudioProcessor + playhead adapter + chunk I/O + CC mapping
Source/editor/      JUCE UI (S9 panel, Theme.h = house look)
tests/              headless conformance tools (coretest framework, CTest)
libs/               pinned submodules: JUCE 8.0.15, clap-juce-extensions
tools/deploy.ps1    dev deploy into the REAPER scan folders
tools/make-release.ps1  versioned zip packer (out/, gitignored)
```

## Environment

- Windows, PowerShell 7 (`pwsh`). REAPER 7.x is the primary validation host.
- CMake 3.27+ with the `win-x64` preset (VS 2026). Submodules required.
- Keep all source files ASCII-only.

## Dev loop

Building does NOT deploy: REAPER loads its installed copies, never the
files under `build/`. ALWAYS deploy after building (standing instruction):
after `ctest` is green, run

`pwsh tools/deploy.ps1`

which copies the fresh CLAP into `%LOCALAPPDATA%\Programs\Common\CLAP\`
and merges the VST3 bundle into `%COMMONPROGRAMFILES%\VST3\` (merge, do
not nest). Reopen REAPER and re-add the plug-in. If REAPER is running, the
deploy aborts (a loaded plug-in locks its binary). Never kill REAPER
without the user's permission; ask, then stop `reaper.exe` and re-run
deploy.

## Conventions

- `Source/core` never includes JUCE (enforced by a configure-time include
  check in CMakeLists.txt).
- No allocation / locks / file I/O / UI calls on the audio thread; all
  scratch preallocated in prepareToPlay; overflow drops with a counter.
- Every consumed MIDI event is consumed by a documented spec rule (S3.2,
  S5.8); pass-through events pass with original offsets.
- Tests: `ctest --test-dir build/win-x64 -C Release --output-on-failure`
  must stay green after every change (P0/P1/P2/P3 tools + P4
  InvariantsTests randomized-session harness).

## Git conventions

- Branch: `main`. Commit style: `milestone(pN): short summary` or
  `fix(scope): ...`. Push after each phase (each phase in a runnable state).
- Never commit build outputs, out/, or deployed binaries.

## Phase status

| Phase | Status |
|---|---|
| P0 scaffold + passthrough spike | built + ctest; REAPER load pass pending |
| P1 core engine + headless tests | done |
| P2 processor + chunk + CC mapping | done |
| P3 editor | done |
| P4 CI/pluginval/release packaging | in place; REAPER checklist pending |
| P5 v2 backlog | not scheduled |

Manual REAPER verification (owner) after each build, per plan P4:
engine on + pair on beat -> run aligned/audible; engine off passthrough;
jab late-publish; curve/accent/arc sweeps; fold vs zig-zag; cuts on early
release + loop wrap; save/load round trip; CC87 + CC88..95 with mirroring;
humanize on/off + pinned seed replay (overlay seed readout).
