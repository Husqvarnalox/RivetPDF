# Building PDFium for Rivet

How to produce the external PDFium build that Rivet links against.

**Rivet never downloads PDFium.** The build system does not fetch, clone, or
vendor PDFium in any configuration. The library must be built or obtained
locally by the developer and provided to CMake via `RIVET_PDFIUM_ROOT`.

Security note: the configuration below keeps PDFium's JavaScript (V8) and XFA
support **disabled**. This is a deliberate Rivet security decision - Rivet
never executes document JavaScript (see `docs/adr/ADR-0007-pdf-javascript-xfa-disabled.md`).

---

## 1. Get the PDFium source

PDFium is checked out with Chromium's `depot_tools`.

### Install depot_tools

```sh
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
export PATH="$HOME/depot_tools:$PATH"     # put this in your shell profile
```

### Fetch the PDFium checkout

Option A - `fetch`:

```sh
mkdir pdfium && cd pdfium
fetch pdfium
cd ..
```

Option B - equivalent `gclient` steps:

```sh
mkdir pdfium && cd pdfium
gclient config https://pdfium.googlesource.com/pdfium.git
gclient sync
```

Both produce a `pdfium/` checkout. Keep this tree outside the Rivet
repository (or inside a gitignored path such as `third_party/pdfium/`);
Rivet's `.gitignore` excludes common local PDFium tree locations.

---

## 2. Configure the PDFium build

From the `pdfium/` checkout, create the output directory and an `args.gn`
with exactly these contents:

```sh
mkdir -p out/release
cat > out/release/args.gn <<'EOF'
pdf_enable_v8 = false
pdf_enable_xfa = false
pdf_use_skia = false
pdf_use_agg = true
is_component_build = false
is_debug = false
EOF
```

Rationale:

| Setting | Why |
| --- | --- |
| `pdf_enable_v8 = false` | No JavaScript engine. Security requirement; see ADR-0007. |
| `pdf_enable_xfa = false` | No XFA form engine. Security requirement; see ADR-0007. |
| `pdf_use_skia = false` | Rivet does not use Skia; the default AGG raster backend is used. |
| `pdf_use_agg = true` | AGG-based rasterization, matching `pdf_use_skia = false`. |
| `is_component_build = false` | Produces a single linkable `pdfium` library instead of component DLLs. |
| `is_debug = false` | Release build of PDFium, even when linking into a Debug Rivet build. |

Then generate and build:

```sh
gn gen out/release
autoninja -C out/release pdfium
```

The result is the static/dynamic PDFium library under `out/release/` (on
macOS: `out/release/pdfium.dylib`).

---

## 3. Assemble the install layout Rivet expects

`cmake/FindPDFium.cmake` expects this layout under `RIVET_PDFIUM_ROOT`:

```text
<root>/
├── include/
│   ├── fpdfview.h
│   ├── fpdf_doc.h
│   └── ... (the other fpdf_*.h public headers)
└── lib/
    └── libpdfium.dylib     (macOS; libpdfium.so on Linux, pdfium.lib on Windows)
```

On macOS:

```sh
PDFIUM_ROOT=/absolute/path/to/pdfium-install
mkdir -p "$PDFIUM_ROOT/include" "$PDFIUM_ROOT/lib"

# Public headers (pdfium/public/*.h)
cp /path/to/pdfium/public/*.h "$PDFIUM_ROOT/include/"

# The built library
cp /path/to/pdfium/out/release/pdfium.dylib "$PDFIUM_ROOT/lib/libpdfium.dylib"
```

(`FindPDFium.cmake` searches for names `pdfium` / `libpdfium`, so either name
works inside `lib/`; the documented layout uses `libpdfium.dylib`.)

---

## 4. Configure Rivet against the local PDFium

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
the imported target `PDFium::PDFium`, and configuration fails with a pointer
to this document if the library cannot be found. As a last resort it also
searches standard system paths for an installed PDFium.

---

## 5. Notes and troubleshooting

- **Never committed**: PDFium source trees and binaries are local state.
  Rivet's `.gitignore` excludes `third_party/pdfium/`, `external/pdfium/`,
  `vendor/pdfium/`, and all binary artifacts.
- **Runtime library lookup (macOS)**: if the `rivet` executable cannot locate
  `libpdfium.dylib` at launch, adjust the dylib's install name with
  `install_name_tool -change ...` or place a copy of the dylib next to the
  executable.
- **Version drift**: PDFium tracks Chromium. When you re-sync the PDFium
  checkout, rebuild `out/release` and refresh the copies in `RIVET_PDFIUM_ROOT`;
  the public headers and the library must come from the same build.
- **Do not enable V8 or XFA** in your `args.gn`. A PDFium built with
  JavaScript support must not be linked into Rivet; see
  `docs/adr/ADR-0007-pdf-javascript-xfa-disabled.md`.
- Linux uses the same steps with `libpdfium.so`; Windows uses `pdfium.lib`
  (import library for `pdfium.dll`). macOS is the first supported platform.
