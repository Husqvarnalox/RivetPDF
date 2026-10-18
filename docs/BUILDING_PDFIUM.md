# Building PDFium for Rivet

How to produce the external PDFium build that Rivet links against.

**Rivet never downloads PDFium.** The build system does not fetch, clone, or
vendor PDFium in any configuration. The library must be built or obtained
locally by the developer and provided to CMake via `RIVET_PDFIUM_ROOT`.

Security note: the configuration below keeps PDFium's JavaScript (V8) and XFA
support **disabled**. This is a deliberate Rivet security decision - Rivet
never executes document JavaScript (see `docs/adr/ADR-0007-pdf-javascript-xfa-disabled.md`).

---

## 1. Target configuration

Rivet links a **complete static PDFium library** (`libpdfium.a` on macOS/Linux):
one self-contained archive, no dylib to ship or resolve at runtime. The
verified recipe (all options confirmed against a real checkout, revision
`40ecb4f46dfdcfe6d966bb5e4022e372d22d7595`, 2026-09-24):

| GN arg | Value | Why |
| --- | --- | --- |
| `pdf_enable_v8` | `false` | No JavaScript engine (ADR-0007). |
| `pdf_enable_xfa` | `false` | No XFA form engine (ADR-0007). |
| `pdf_use_skia` | `false` | Rivet does not use Skia; the AGG raster backend is used. |
| `pdf_use_agg` | `true` | AGG-based rasterization, matching `pdf_use_skia = false`. |
| `is_component_build` | `false` | One linkable library instead of component pieces. |
| `pdf_is_complete_lib` | `true` | Bundle PDFium's internal static dependencies (freetype, AGG, ...) into the single archive. |
| `is_debug` | `false` | Release build of PDFium, even when linking into a Debug Rivet build. |
| `use_custom_libcxx` | `false` | **Required for static linking.** With the default `true`, the archive is built against Chromium's libc++ (`std::__Cr` namespace) and cannot link into a binary that uses the platform libc++ (`std::__1`). `false` builds PDFium against the system libc++ headers. |
| `symbol_level` | `1` | Line tables only; keeps the archive at a reasonable size (~50 MB instead of ~280 MB). |

Two consequences of `use_custom_libcxx = false` that you may hit:

- **One upstream source patch** (as of revision `40ecb4f4`):
  `core/fpdfapi/page/cpdf_psengine.cpp` (`PSOP_ROLL`) uses raw pointer
  arithmetic that trips Chromium's `-Wunsafe-buffer-usage` plugin
  (`-Werror`). Apply the minimal local fix - index/`std::next` arithmetic
  instead of `std::begin(...) + n` - after each future `gclient sync`. The
  in-tree copy of the patch used for Rivet's builds is documented in the
  file itself.
- **Consumer framework linking**: a static PDFium does not carry its system
  framework dependencies; `cmake/FindPDFium.cmake` adds them to the imported
  target. On macOS the empirically verified set is `-framework CoreFoundation
  -framework CoreGraphics -framework CoreText -framework Security` (the last
  one via partition_alloc). Linux (`dl`, `m`, pthread) and Windows (`advapi32 bcrypt gdi32 ole32 oleaut32 shell32 user32 winmm`) sets are unverified over-approximations (ADR-0036).

---

## 2. Reproducible build (recommended): `tools/pdfium/`

One recipe builds the same PDFium on macOS, Linux and Windows. Everything that
defines the result is checked in:

| File | Purpose |
| --- | --- |
| `tools/pdfium/PDFIUM_REVISION` | The single pinned PDFium commit (`40ecb4f4...`). |
| `tools/pdfium/args.common.gn` | GN args shared by every OS/CPU (the table in section 1). |
| `tools/pdfium/args.{macos,linux,windows}.gn` | `target_os` and OS-only args (Linux: `use_sysroot=false`). `target_cpu` is generated from `--arch`. |
| `tools/pdfium/patches/*.patch` | Source patches applied on top of the revision (currently the `cpdf_psengine.cpp` fix above), applied idempotently. |
| `tools/pdfium/build-pdfium.sh` | macOS / Linux driver. |
| `tools/pdfium/build-pdfium.ps1` | Windows driver (run from a Visual Studio developer prompt). |

The scripts clone `depot_tools` if needed (`DEPOT_TOOLS_UPDATE=0`; set
`RIVET_DEPOT_TOOLS_REF` to pin a depot_tools ref - PDFium's own `DEPS` pins
the compiler, GN and every other dependency), check out the pinned revision
via a generated `.gclient`, verify `HEAD`, apply the patches, `gn gen`, build
the `pdfium` target and install the layout in section 4. They are the only
place that touches the network; Rivet's CMake never does.

```sh
# macOS (Apple Silicon or Intel host); --arch x64 = x86_64
tools/pdfium/build-pdfium.sh --arch arm64 --install "$HOME/pdfium-install-arm64"
tools/pdfium/build-pdfium.sh --arch x64   --install "$HOME/pdfium-install-x64"

# Linux x64 (needs g++/libstdc++-dev, python3, curl; gclient installs a Chromium clang)
tools/pdfium/build-pdfium.sh --arch x64 --install "$HOME/pdfium-install"
```

```powershell
# Windows x64 (Visual Studio 2022 C++ tools + Windows SDK; developer prompt)
pwsh tools/pdfium/build-pdfium.ps1 -Arch x64 -Install C:\pdfium-install
```

Notes:

- **Universal macOS**: Rivet's universal binary needs a universal PDFium; build
  the arm64 and x64 installs separately and combine the archives with
  `lipo -create -output lib/libpdfium.a <arm64>/lib/libpdfium.a <x64>/lib/libpdfium.a`
  (not automated, not gated in CI).
- **ARM64 variants** (`--arch arm64` on Linux, `-Arch arm64` on Windows) are
  accepted by the scripts and GN, but are neither exercised nor gated in CI.
  Linux arm64 on an x64 host additionally needs a cross sysroot.
- **Windows CRT**: with `is_component_build=false` GN selects the *static* CRT
  (`/MT`). Rivet therefore builds with `/MT` whenever PDFium is ON on MSVC
  (`cmake/RivetMsvcRuntime.cmake`; `-DRIVET_MSVC_STATIC_CRT=OFF` opts out, which
  only works with a PDFium built differently). The release static CRT is used
  for Debug too, because the PDFium archive is always a release build.
- **Linux compiler**: PDFium is compiled by its bundled clang against the
  host libstdc++ (`use_custom_libcxx=false`, `use_sysroot=false`). The archive
  links into a GCC- or Clang-built Rivet.
- Every install gets a `PDFIUM_BUILD_INFO` file (revision, GN args hash, the
  security flags). `FindPDFium.cmake` refuses a root whose info file says
  V8, XFA or Skia was enabled.
- CI (`linux-pdfium`, `windows-msvc-pdfium`) runs these scripts on a cache miss
  and caches the install tree under a key hashed from `tools/pdfium/**`.
- `rivet-smoke-open` (built whenever PDFium is ON, also a ctest) opens a PDF,
  renders page 1, saves a copy through the document assembly and reopens it.

## 3. Manual build (equivalent, for reference)

PDFium is checked out with Chromium's `depot_tools`:

```sh
export PATH="$HOME/depot_tools:$PATH"      # clone depot_tools first
mkdir pdfium && cd pdfium
fetch --no-history pdfium                  # then check out the revision in PDFIUM_REVISION
cd pdfium
# apply tools/pdfium/patches/*.patch, then:
mkdir -p out/release
cat /path/to/rivet/tools/pdfium/args.common.gn /path/to/rivet/tools/pdfium/args.macos.gn > out/release/args.gn
echo 'target_cpu = "arm64"' >> out/release/args.gn
gn gen out/release
autoninja -C out/release pdfium
```

Verify the options with `gn args out/release --list --short`; unknown args make
`gn gen` fail loudly - do not silently drop or rename them.

## 4. Install layout Rivet expects

`cmake/FindPDFium.cmake` expects this layout under `RIVET_PDFIUM_ROOT`
(static-first; a shared `libpdfium.dylib`/`libpdfium.so` build is still
accepted):

```text
<root>/
├── include/
│   ├── fpdfview.h
│   └── ... (the other public fpdf_*.h headers)
├── lib/
│   ├── libpdfium.a        (macOS/Linux static; the preferred layout)
│   └── pdfium.lib         (Windows static, static CRT /MT)
└── PDFIUM_BUILD_INFO      (optional; written by tools/pdfium/*)
```

The public headers and the library must come from the same build. The scripts
produce this layout directly (`out/<dir>/obj/libpdfium.a` / `obj/pdfium.lib`
plus `public/*.h`). Do not define `COMPONENT_BUILD` when compiling against the
static Windows library (that would make the headers declare `dllimport`).

---

## 5. Configure Rivet against the local PDFium

```sh
cmake -S . -B build/debug-pdfium -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DRIVET_WITH_PDFIUM=ON \
      -DRIVET_PDFIUM_ROOT=/absolute/path/to/pdfium-install
cmake --build build/debug-pdfium
ctest --test-dir build/debug-pdfium
build/debug-pdfium/src/platform/macos/rivet /path/to/document.pdf
```

`RIVET_PDFIUM_ROOT` may also be provided through the environment variable
`RIVET_PDFIUM_ROOT`, which the `debug-pdfium` preset consumes:

```sh
export RIVET_PDFIUM_ROOT=/absolute/path/to/pdfium-install
cmake --preset debug-pdfium
cmake --build build/debug-pdfium
```

`FindPDFium.cmake` validates the layout (`fpdfview.h` + the library), creates
the imported target `PDFium::PDFium` (adding the system frameworks required
for static linking on Apple platforms), and configuration fails with a pointer
to this document if the library cannot be found. As a last resort it also
searches standard system paths for an installed PDFium.

---

## 6. Notes and troubleshooting

- **Never committed**: PDFium source trees and binaries are local state.
  Rivet's `.gitignore` excludes `third_party/pdfium/`, `external/pdfium/`,
  `vendor/pdfium/`, and all binary artifacts.
- **Static linking failures with `std::__Cr` symbols**: your PDFium was built
  with the default `use_custom_libcxx = true`. Rebuild with `false` (see the
  table above); mixing libc++ runtimes in one binary is not supported.
- **Version drift**: PDFium tracks Chromium. When you re-sync the checkout,
  re-apply the `cpdf_psengine.cpp` patch if it still applies, rebuild
  `out/release`, and refresh the copies in `RIVET_PDFIUM_ROOT`.
- **Do not enable V8 or XFA** in your `args.gn`. A PDFium built with
  JavaScript support must not be linked into Rivet; see
  `docs/adr/ADR-0007-pdf-javascript-xfa-disabled.md`.
- **Threading**: PDFium's public API is process-globally serialized inside
  Rivet (`PdfiumCallGate` in `src/pdf/pdfium/`); applications embedding the
  adapter do not need extra locking around it.
- Platform status: macOS is verified end to end. The Linux and Windows
  recipes and link sets are implemented (ADR-0036) but were written without
  access to those OSes; the `linux-pdfium` / `windows-msvc-pdfium` CI jobs are
  the verification. Expect to adjust the system-library list in
  `cmake/FindPDFium.cmake` if the first real link reports unresolved symbols.
