# deploy.ps1 - Runs Assistant dev deploy (Windows)
# Copy the fresh VST3/CLAP binaries over the installed copies that REAPER
# actually loads (building alone never deploys).
#
#   pwsh tools/deploy.ps1 [-BuildDir build/win-x64]
#
# Refuses to run while REAPER is open: a loaded plug-in locks its binary and
# the copy either fails or leaves a stale install behind.

param (
    [string]$BuildDir = "build/win-x64"
)

$ErrorActionPreference = "Stop"

$reaper = Get-Process reaper -ErrorAction SilentlyContinue
if ($reaper -ne $null) {
    throw "REAPER is running (pid $($reaper.Id)). Close it first, then re-run."
}

$root = $PSScriptRoot | Split-Path -Parent
$rel = Join-Path $root "$BuildDir/RunsAssistant_artefacts/Release"

$clapSrc = Join-Path $rel "CLAP/Runs Assistant.clap"
$clapDstDir = Join-Path $env:LOCALAPPDATA "Programs/Common/CLAP"
$vst3Src = Join-Path $rel "VST3/Runs Assistant.vst3/Contents"
$vst3Dst = Join-Path $env:COMMONPROGRAMFILES "VST3/Runs Assistant.vst3/Contents"

if (-not (Test-Path -LiteralPath $clapSrc)) {
    throw "Missing $clapSrc (build first: cmake --build --preset win-x64)"
}
if (-not (Test-Path -LiteralPath $vst3Src)) {
    throw "Missing $vst3Src (build first)"
}

New-Item -ItemType Directory -Path $clapDstDir -Force | Out-Null
New-Item -ItemType Directory -Path $vst3Dst -Force | Out-Null

Copy-Item -LiteralPath $clapSrc `
    -Destination (Join-Path $clapDstDir "Runs Assistant.clap") -Force
# Merge the bundle contents: copying the bundle dir itself onto the existing
# dir nests it (Runs Assistant.vst3\Runs Assistant.vst3) instead of updating.
Copy-Item -Path (Join-Path $vst3Src "*") -Destination $vst3Dst `
    -Recurse -Force

Get-Item (Join-Path $clapDstDir "Runs Assistant.clap"),
          (Join-Path $vst3Dst "x86_64-win/Runs Assistant.vst3") |
    Select-Object FullName, LastWriteTime
