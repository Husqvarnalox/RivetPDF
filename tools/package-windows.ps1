# SPDX-License-Identifier: MPL-2.0
#
# Builds Rivet in Release (MSVC x64, static /MT CRT, static PDFium and static
# vcpkg OpenSSL, mirroring the windows-msvc-pdfium CI job) and produces the
# Windows x64 distribution artifacts in dist\: Rivet-<ver>-windows-x64.zip,
# rivet-cli-<ver>-windows-x64.zip and SHA256SUMS. The binaries are NOT
# Authenticode-signed. See docs/RELEASING.md.
#
# Run from a shell with the MSVC x64 environment (vcvars64 / ilammy/msvc-dev-cmd),
# with cmake and ninja on PATH.
#
# Environment / parameters:
#   RIVET_PDFIUM_ROOT  PDFium install prefix (or -PdfiumRoot)
#   OPENSSL_ROOT_DIR   vcpkg x64-windows-static root (or -OpensslRoot)
#   (this script) network (libcurl) and PKCS#11 are OFF on Windows, as in CI.
[CmdletBinding()]
param(
    [string]$PdfiumRoot = $env:RIVET_PDFIUM_ROOT,
    [string]$OpensslRoot = $env:OPENSSL_ROOT_DIR
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
$BuildDir = Join-Path $Root 'build\release-pkg-win'
$Dist = Join-Path $Root 'dist'
$Arch = 'x64'
$CliTargets = @('rivet-pdf-encrypt', 'rivet-pdf-signature-inspect', 'rivet-signature-inspect')

function Invoke-Native {
    param([string]$Exe, [string[]]$Arguments)
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed with exit code $LASTEXITCODE" }
}

if (-not $PdfiumRoot -or -not (Test-Path (Join-Path $PdfiumRoot 'include\fpdfview.h'))) {
    throw 'Set RIVET_PDFIUM_ROOT (or -PdfiumRoot) to a PDFium install prefix.'
}
if (-not $OpensslRoot -or -not (Test-Path (Join-Path $OpensslRoot 'include\openssl\evp.h'))) {
    throw 'Set OPENSSL_ROOT_DIR (or -OpensslRoot) to the vcpkg x64-windows-static root.'
}
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    throw 'MSVC (cl.exe) not on PATH: run from an x64 Developer shell.'
}
$dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue
if (-not $dumpbin) { throw 'dumpbin.exe not on PATH: run from an x64 Developer shell.' }

$cml = Get-Content -Raw (Join-Path $Root 'CMakeLists.txt')
if ($cml -notmatch '(?s)project\(\s*rivet\b[^)]*?VERSION\s+([0-9][0-9.]*)') { throw 'Cannot read project VERSION from CMakeLists.txt' }
$Version = $Matches[1]
Write-Host "==> Rivet $Version (windows-$Arch)"

Write-Host '==> Configure'
Invoke-Native cmake @(
    '-S', '.', '-B', $BuildDir, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl',
    '-DCMAKE_COMPILE_WARNING_AS_ERROR=ON',
    '-DRIVET_BUILD_TESTS=OFF',
    '-DRIVET_WITH_PDFIUM=ON', "-DRIVET_PDFIUM_ROOT=$PdfiumRoot",
    '-DRIVET_ENABLE_CRYPTO=ON',
    "-DOPENSSL_ROOT_DIR=$OpensslRoot", '-DOPENSSL_USE_STATIC_LIBS=TRUE')

Write-Host '==> Build'
Invoke-Native cmake (@('--build', $BuildDir, '--target', 'rivet') + $CliTargets)

function Find-Exe([string]$Name) {
    $f = Get-ChildItem -Path $BuildDir -Recurse -Filter "$Name.exe" -File |
        Where-Object { $_.FullName -notmatch '[\\/]CMakeFiles[\\/]' } | Select-Object -First 1
    if (-not $f) { throw "$Name.exe not found under $BuildDir" }
    return $f.FullName
}
$AppExe = Find-Exe 'rivet'
$CliExes = @{}
foreach ($t in $CliTargets) { $CliExes[$t] = Find-Exe $t }

# Imported DLLs must be Windows system DLLs: present in System32 (or an API
# set) and not a redistributable C/C++ runtime or third-party library. With
# the static /MT CRT, no vcruntime/ucrt/msvcp import is expected.
$Denied = '^(vcruntime|msvcp|msvcr|ucrtbase|concrt|vcomp|libcrypto|libssl|libcurl|pdfium)'
function Test-SystemOnly([string]$Exe) {
    Write-Host "--- dumpbin /dependents $(Split-Path -Leaf $Exe)"
    $out = & dumpbin /nologo /dependents $Exe
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $Exe" }
    $out | Write-Host
    $dlls = $out | ForEach-Object { if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1].ToLowerInvariant() } }
    $sys32 = Join-Path $env:SystemRoot 'System32'
    $bad = @($dlls | Where-Object {
        $_ -match $Denied -or -not (($_ -like 'api-ms-win-*') -or (Test-Path (Join-Path $sys32 $_)))
    })
    if ($bad.Count -gt 0) { throw "$Exe imports non-system DLLs: $($bad -join ', ')" }
}

Write-Host '==> Verify binaries import only system DLLs'
Test-SystemOnly $AppExe
foreach ($t in $CliTargets) { Test-SystemOnly $CliExes[$t] }

Write-Host '==> Assemble'
if (Test-Path $Dist) { Remove-Item -Recurse -Force $Dist }
New-Item -ItemType Directory -Force -Path $Dist | Out-Null
$Stage = Join-Path ([System.IO.Path]::GetTempPath()) ("rivet-pkg-" + [guid]::NewGuid().ToString('N'))
try {
    $AppName = "Rivet-$Version-windows-$Arch"
    $CliName = "rivet-cli-$Version-windows-$Arch"
    $AppDir = Join-Path $Stage $AppName
    $CliDir = Join-Path $Stage $CliName
    New-Item -ItemType Directory -Force -Path $AppDir, $CliDir | Out-Null

    Copy-Item $AppExe (Join-Path $AppDir 'rivet.exe')
    foreach ($t in $CliTargets) {
        Copy-Item $CliExes[$t] (Join-Path $AppDir "$t.exe")
        Copy-Item $CliExes[$t] (Join-Path $CliDir "$t.exe")
    }
    Copy-Item (Join-Path $Root 'LICENSE') (Join-Path $AppDir 'LICENSE')
    Copy-Item (Join-Path $Root 'LICENSE') (Join-Path $CliDir 'LICENSE')

    @"
Rivet $Version - Windows x64 (alpha)

Run rivet.exe. The command-line tools (rivet-pdf-encrypt.exe,
rivet-pdf-signature-inspect.exe, rivet-signature-inspect.exe) sit next to it.

The executables are statically linked (MSVC /MT CRT, PDFium, OpenSSL) and import
only Windows system DLLs, so no Visual C++ redistributable is needed.
They are NOT Authenticode-signed: Windows SmartScreen will warn on first run
("More info" > "Run anyway"). Verify the download against SHA256SUMS.
This build has no network (libcurl) support and no PKCS#11 provider.
Not tested on a real desktop by the release pipeline; Windows 10 or later, x64.
License: MPL-2.0 (see LICENSE).
"@ | Set-Content -Path (Join-Path $AppDir 'README.txt') -Encoding utf8

    @"
Rivet command-line tools $Version - Windows x64 (alpha)

rivet-pdf-encrypt.exe, rivet-pdf-signature-inspect.exe, rivet-signature-inspect.exe.
Statically linked, system DLL imports only. NOT Authenticode-signed.
Verify against SHA256SUMS. License: MPL-2.0 (see LICENSE).
"@ | Set-Content -Path (Join-Path $CliDir 'README.txt') -Encoding utf8

    $AppZip = Join-Path $Dist "$AppName.zip"
    $CliZip = Join-Path $Dist "$CliName.zip"
    Compress-Archive -Path $AppDir -DestinationPath $AppZip -CompressionLevel Optimal
    Compress-Archive -Path $CliDir -DestinationPath $CliZip -CompressionLevel Optimal

    Write-Host '==> SHA256SUMS'
    $lines = foreach ($z in @($AppZip, $CliZip)) {
        "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $z).Hash.ToLowerInvariant(), (Split-Path -Leaf $z)
    }
    [System.IO.File]::WriteAllText((Join-Path $Dist 'SHA256SUMS'), (($lines -join "`n") + "`n"),
        (New-Object System.Text.UTF8Encoding($false)))
    $lines | Write-Host
}
finally {
    if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
}

Write-Host '==> Done'
Get-ChildItem $Dist | Format-Table Name, Length
