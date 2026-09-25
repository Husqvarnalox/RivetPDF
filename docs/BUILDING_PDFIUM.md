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
  one via partition_alloc). Linux: verify `-ldl`/pthread against a real build.

---

## 2. Get the PDFium source

PDFium is checked out with Chromium's `depot_tools`.

### Install depot_tools

```sh
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$HOME/depot_tools:$PATH"     # put this in your shell profile
gclient --version                          # one-time depot_tools bootstrap
```

### Fetch the PDFium checkout

```sh
mkdir pdfium && cd pdfium
fetch --no-history pdfium
```

(`fetch pdfium` without `--no-history` also works but downloads far more.)
Keep this tree outside the Rivet repository (or inside a gitignored path such
as `third_party/pdfium/`); Rivet's `.gitignore` excludes common local PDFium
tree locations.

---

## 3. Configure and build the static library

From the `pdfium/` checkout:

```sh
mkdir -p out/release
cat > out/release/args.gn <<'EOF'
pdf_enable_v8 = false
pdf_enable_xfa = false
pdf_use_skia = false
pdf_use_agg = true
is_component_build = false
pdf_is_complete_lib = true
is_debug = false
use_custom_libcxx = false
symbol_level = 1
EOF

gn gen out/release
autoninja -C out/release pdfium
```

The result is `out/release/obj/libpdfium.a` on macOS.

Verify before relying on the options: `gn args out/release --list --short`
prints the args this checkout actually supports. Unknown args make `gn gen`
fail loudly - do not silently drop or rename them.

---

## 4. Assemble the install layout Rivet expects

`cmake/FindPDFium.cmake` expects this layout under `RIVET_PDFIUM_ROOT`
(static-first; a shared `libpdfium.dylib`/`libpdfium.so` build is still
accepted):

```text
<root>/
├── include/
│   ├── fpdfview.h
│   └── ... (the other public fpdf_*.h headers)
└── lib/
    └── libpdfium.a        (macOS/Linux static; the preferred layout)
```

```sh
PDFIUM_ROOT=/absolute/path/to/pdfium-install
mkdir -p "$PDFIUM_ROOT/include" "$PDFIUM_ROOT/lib"
cp /path/to/pdfium/public/*.h "$PDFIUM_ROOT/include/"
cp /path/to/pdfium/out/release/obj/libpdfium.a "$PDFIUM_ROOT/lib/libpdfium.a"
```

The public headers and the library must come from the same build.

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
- Windows uses `pdfium.lib` (static) from an equivalent GN configuration;
  macOS is the first supported platform. Linux static builds have not been
  exercised by Rivet yet.
