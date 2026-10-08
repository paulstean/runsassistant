# Runs Assistant P4 release packer (Windows).
# Assembles a versioned zip from a Release build into out/ (gitignored):
#   pwsh tools/make-release.ps1 [-Version 0.1.0]
# Version defaults to PROJECT_VERSION in CMakeLists.txt (single source of
# truth); -Version overrides it for one-off packs.
# Contains VST3 + CLAP + docs (plan.md P4: no installer, no REAPER binaries,
# AU pack happens on a Mac in the D3 post-v1 phase):
#   Runs Assistant.vst3 / Runs Assistant.clap
#   UserManual.html (docs/)   RELEASE_NOTES.md   README.md   LICENSE (if any)
param (
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent  # repo root
if ($Version -eq "") {
    $m = Select-String -LiteralPath (Join-Path $root "CMakeLists.txt") `
        -Pattern 'project\s*\(\s*RunsAssistant\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)'
    if ($m -ne $null -and $m.Matches.Count -gt 0) {
        $Version = $m.Matches[0].Groups[1].Value
    }
    else {
        throw "Could not parse PROJECT_VERSION from CMakeLists.txt"
    }
}
$rel = Join-Path $root "build/win-x64/RunsAssistant_artefacts/Release"
$out = Join-Path $root "out"
$stage = Join-Path $out "RunsAssistant-$Version-win-x64"

if (-not (Test-Path -LiteralPath $rel)) {
    throw "Release artefacts not found: $rel (build first: cmake --preset win-x64 && cmake --build --preset win-x64)"
}
# Fresh stage every run: scanning a bundle dir onto an existing dir nests it
# (Runs Assistant.vst3\Runs Assistant.vst3), so remove first.
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

$vst3 = Join-Path $rel "VST3/Runs Assistant.vst3"
if (Test-Path -LiteralPath $vst3) {
    Copy-Item -LiteralPath $vst3 -Destination (Join-Path $stage "Runs Assistant.vst3") -Recurse -Force
}
$clap = Join-Path $rel "CLAP/Runs Assistant.clap"
if (Test-Path -LiteralPath $clap) {
    Copy-Item -LiteralPath $clap -Destination (Join-Path $stage "Runs Assistant.clap") -Force
}
Copy-Item -LiteralPath (Join-Path $root "README.md") -Destination (Join-Path $stage "README.md") -Force
Copy-Item -LiteralPath (Join-Path $root "RELEASE_NOTES.md") -Destination (Join-Path $stage "RELEASE_NOTES.md") -Force
Copy-Item -LiteralPath (Join-Path $root "docs/UserManual.html") -Destination (Join-Path $stage "UserManual.html") -Force
if (Test-Path -LiteralPath (Join-Path $root "LICENSE")) {
    Copy-Item -LiteralPath (Join-Path $root "LICENSE") -Destination (Join-Path $stage "LICENSE") -Force
}

$zip = Join-Path $out "RunsAssistant-$Version-win-x64.zip"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -Force

$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
"$hash  RunsAssistant-$Version-win-x64.zip" |
    Set-Content -LiteralPath (Join-Path $out "RunsAssistant-$Version-win-x64.sha256") -NoNewline

Get-Item $zip | Select-Object Name, Length
Get-Content (Join-Path $out "RunsAssistant-$Version-win-x64.sha256")
