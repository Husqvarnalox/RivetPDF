# SPDX-License-Identifier: MPL-2.0
#
# Windows counterpart of build-pdfium.sh: builds the static pdfium.lib Rivet
# links against and assembles the layout cmake/FindPDFium.cmake expects:
#
#   <Install>\include\*.h
#   <Install>\lib\pdfium.lib
#   <Install>\PDFIUM_BUILD_INFO
#
# Usage (from a Visual Studio developer prompt / after ilammy/msvc-dev-cmd):
#   pwsh tools/pdfium/build-pdfium.ps1 -Install C:\pdfium-install [-Arch x64|arm64]
#                                      [-Work .pdfium-work] [-DepotTools DIR]
#
# Requires: Visual Studio 2022 C++ build tools + Windows SDK (local toolchain,
# DEPOT_TOOLS_WIN_TOOLCHAIN=0), git, Python is supplied by depot_tools.
# arm64 is supported by GN but not gated in CI.
param(
    [Parameter(Mandatory = $true)][string]$Install,
    [string]$Arch = "x64",
    [string]$Work = (Join-Path (Get-Location) ".pdfium-work"),
    [string]$DepotTools = ""
)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

function Invoke-Native([string]$What, [scriptblock]$Block) {
    & $Block
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

switch ($Arch.ToLower()) {
    { $_ -in "x64", "x86_64", "amd64" } { $cpu = "x64" }
    { $_ -in "arm64", "aarch64" } { $cpu = "arm64" }
    default { throw "unsupported -Arch: $Arch" }
}

$revision = (Get-Content (Join-Path $here "PDFIUM_REVISION") -Raw).Trim()
New-Item -ItemType Directory -Force -Path $Work, $Install | Out-Null
$Work = (Resolve-Path $Work).Path
$Install = (Resolve-Path $Install).Path
if (-not $DepotTools) { $DepotTools = Join-Path $Work "depot_tools" }

# --- depot_tools -------------------------------------------------------------
if (-not (Test-Path (Join-Path $DepotTools "gclient.bat"))) {
    $refArgs = @()
    if ($env:RIVET_DEPOT_TOOLS_REF) { $refArgs = @("--branch", $env:RIVET_DEPOT_TOOLS_REF) }
    Invoke-Native "git clone depot_tools" {
        git clone --depth 1 @refArgs https://chromium.googlesource.com/chromium/tools/depot_tools.git $DepotTools
    }
}
$env:PATH = "$DepotTools;$env:PATH"
# Bootstrap depot_tools' bundled python/git/cipd tools once (needed by gn and gclient hooks).
if (-not (Test-Path (Join-Path $DepotTools "python3_bin_reldir.txt"))) {
    Invoke-Native "bootstrap depot_tools" { cmd /c "`"$DepotTools\update_depot_tools.bat`"" }
}
$env:DEPOT_TOOLS_UPDATE = "0"          # reproducible: no self-update mid-run
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = "0"   # use the installed Visual Studio

# --- source at the pinned revision -------------------------------------------
@"
solutions = [
  {
    "name": "pdfium",
    "url": "https://pdfium.googlesource.com/pdfium.git@$revision",
    "deps_file": "DEPS",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
"@ | Set-Content -Encoding ascii (Join-Path $Work ".gclient")

Push-Location $Work
try {
    & gclient sync --no-history --revision "pdfium@$revision"
    if ($LASTEXITCODE -ne 0) {
        Invoke-Native "gclient sync" { gclient sync --revision "pdfium@$revision" }
    }
} finally { Pop-Location }

$src = Join-Path $Work "pdfium"
$actual = (git -C $src rev-parse HEAD).Trim()
if ($actual -ne $revision) { throw "checkout is $actual, expected $revision" }

# --- patches (idempotent) ------------------------------------------------------
foreach ($patch in Get-ChildItem (Join-Path $here "patches") -Filter *.patch | Sort-Object Name) {
    git -C $src apply --ignore-whitespace --reverse --check $patch.FullName 2>$null
    if ($LASTEXITCODE -eq 0) { Write-Host "patch already applied: $($patch.Name)"; continue }
    Invoke-Native "git apply --check" { git -C $src apply --ignore-whitespace --check $patch.FullName }
    Invoke-Native "git apply" { git -C $src apply --ignore-whitespace $patch.FullName }
    Write-Host "applied patch: $($patch.Name)"
}

# --- gn args + build -----------------------------------------------------------
$out = Join-Path $src "out\rivet-windows-$cpu"
New-Item -ItemType Directory -Force -Path $out | Out-Null
$gnArgs = (Get-Content (Join-Path $here "args.common.gn") -Raw) + "`n" +
        (Get-Content (Join-Path $here "args.windows.gn") -Raw) + "`n" +
        "target_cpu = `"$cpu`"`n"
# LF line endings and no BOM, as gn expects.
[System.IO.File]::WriteAllText((Join-Path $out "args.gn"), ($gnArgs -replace "`r`n", "`n"),
    (New-Object System.Text.UTF8Encoding($false)))

Push-Location $src
try {
    Invoke-Native "gn gen" { gn gen $out }
    Invoke-Native "autoninja" { autoninja -C $out pdfium }
} finally { Pop-Location }

$lib = Join-Path $out "obj\pdfium.lib"
if (-not (Test-Path $lib)) { throw "expected library not produced: $lib" }

# --- install layout ------------------------------------------------------------
Remove-Item -Recurse -Force (Join-Path $Install "include"), (Join-Path $Install "lib") -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path $Install "include"), (Join-Path $Install "lib") | Out-Null
Copy-Item (Join-Path $src "public\*.h") (Join-Path $Install "include")
Copy-Item $lib (Join-Path $Install "lib\pdfium.lib")

$argsHash = (Get-FileHash -Algorithm SHA256 (Join-Path $out "args.gn")).Hash.ToLower()
function Get-Flag([string]$Name) {
    $line = Select-String -Path (Join-Path $out "args.gn") -Pattern "^\s*$Name\s*=\s*(.+)$" | Select-Object -Last 1
    if ($line) { $line.Matches[0].Groups[1].Value.Trim() } else { "" }
}
@(
    "revision=$revision",
    "target_os=windows",
    "target_cpu=$cpu",
    "args_sha256=$argsHash",
    "pdf_enable_v8=$(Get-Flag 'pdf_enable_v8')",
    "pdf_enable_xfa=$(Get-Flag 'pdf_enable_xfa')",
    "pdf_use_skia=$(Get-Flag 'pdf_use_skia')",
    "use_custom_libcxx=$(Get-Flag 'use_custom_libcxx')",
    "crt=static"
) | Set-Content -Encoding ascii (Join-Path $Install "PDFIUM_BUILD_INFO")

Write-Host "PDFium $revision (windows/$cpu) installed to $Install"
Write-Host "Configure Rivet with: -DRIVET_WITH_PDFIUM=ON -DRIVET_PDFIUM_ROOT=$Install"
