# Building Rivet

How to configure, build, test, and run Rivet from source.

Rivet is built with CMake (>= 3.28) and the Ninja generator. macOS is the
first supported development platform; Windows and Linux support is planned.

---

## 1. Prerequisites (macOS)

| Tool | Requirement | Install |
| --- | --- | --- |
| Xcode / AppleClang | Xcode toolchain with AppleClang 21 or newer (C++23 support) | `xcode-select --install` plus Xcode from the App Store; `xcode-select -p` must point at Xcode (not only the CLT) for Objective-C++ |
| CMake | 3.28 or newer | `brew install cmake` |
| Ninja | any recent version | `brew install ninja` |

Verify:

```sh
clang++ --version   # AppleClang 21+
cmake --version     # >= 3.28
ninja --version
```

PDFium is optional (see `docs/BUILDING_PDFIUM.md`). By default Rivet builds
**without** PDFium: the application shell, widgets, and all non-PDF subsystems
build and run, and PDF features are disabled at runtime.

---

## 2. Configure, build, test, run

All commands use the presets from `CMakePresets.json`. Build trees live under
`build/<preset>/`.

```sh
# Configure
cmake --preset debug

# Build
cmake --build build/debug

# Run the tests (CTest)
ctest --test-dir build/debug
# or, via the test preset:
ctest --preset debug

# Launch the application
build/debug/src/platform/macos/rivet
```

The `rivet` executable lands under `build/debug/src/platform/macos/` because
the macOS platform layer hosts the application and defines the executable
entry point.

### Presets

| Preset | Build type | Notes |
| --- | --- | --- |
| `debug` | Debug | default development configuration |
| `release` | Release | optimized |
| `relwithdebinfo` | RelWithDebInfo | optimized with debug info |
| `sanitizers` | Debug | adds `RIVET_ENABLE_SANITIZERS=ON` (ASan + UBSan) |
| `debug-pdfium` | Debug | adds `RIVET_WITH_PDFIUM=ON` (requires a local PDFium, see `docs/BUILDING_PDFIUM.md`) |

Each preset has matching `build` and `test` presets:

```sh
cmake --build build/sanitizers
ctest --preset sanitizers
```

A `debug-pdfium` build requires PDFium to be available first:

```sh
export RIVET_PDFIUM_ROOT=/absolute/path/to/pdfium-install   # see docs/BUILDING_PDFIUM.md
cmake --preset debug-pdfium
cmake --build build/debug-pdfium
ctest --preset debug-pdfium
build/debug-pdfium/src/platform/macos/rivet /path/to/document.pdf
```

### Manual configure (without presets)

```sh
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
```

---

## 3. CMake options

| Option | Default | Effect |
| --- | --- | --- |
| `RIVET_WITH_PDFIUM` | `OFF` | Build the PDFium-backed engine (`rivet_pdfium`) and enable PDF features. Requires a locally provided PDFium via `RIVET_PDFIUM_ROOT`. Rivet never downloads PDFium. |
| `RIVET_BUILD_TESTS` | `ON` | Build the per-module test executables and register them with CTest. |
| `RIVET_ENABLE_SANITIZERS` | `OFF` | Build with AddressSanitizer and UndefinedBehaviorSanitizer. The `sanitizers` preset turns this on. |

| Variable | Effect |
| --- | --- |
| `RIVET_PDFIUM_ROOT` | Absolute path to a local PDFium install (`include/` + `lib/`). May also be supplied as the `RIVET_PDFIUM_ROOT` environment variable. Only used when `RIVET_WITH_PDFIUM=ON`. |

If `RIVET_WITH_PDFIUM=ON` but PDFium cannot be found, configuration fails with
an error pointing at `docs/BUILDING_PDFIUM.md`.

---

## 4. Building without PDFium vs with PDFium

- **Without** (`RIVET_WITH_PDFIUM=OFF`, the default): `rivet_pdf` is compiled
  with a null engine. `createEngine()` returns a backend whose
  `isAvailable()` is `false`; opening a document reports `NotAvailable`. The
  shell still launches. Useful for working on core, render, UI, and editor
  code without a PDFium checkout.
- **With** (`RIVET_WITH_PDFIUM=ON`): `rivet_pdfium` is compiled and linked
  against the imported target `PDFium::PDFium` created by
  `cmake/FindPDFium.cmake` from the local install at `RIVET_PDFIUM_ROOT`.
  Building PDFium itself is a separate, manual step:
  see `docs/BUILDING_PDFIUM.md`.

---

## 5. Project layout

```text
rivet/
├── CMakeLists.txt        # top level: options, PDFium wiring, subsystem registration
├── CMakePresets.json     # configure/build/test presets
├── cmake/                # FindPDFium.cmake, warning and sanitizer helpers
├── src/
│   ├── core/             # rivet_core: geometry, StrongId, Error/Result, Log, Time, Bitmap
│   │   ├── geometry/
│   │   └── async/        # TaskScheduler, SerialExecutor, IMainThreadDispatcher
│   ├── render/           # rivet_render: transforms, layout, zoom, tiles, cache
│   ├── pdf/              # rivet_pdf: engine interfaces + null engine
│   │   └── pdfium/       # rivet_pdfium: PDFium adapter (RIVET_WITH_PDFIUM=ON only)
│   ├── editor/           # rivet_editor: Command/CommandStack, DocumentSession, DocumentRenderer
│   ├── ui/               # rivet_ui: retained-mode widgets, PaintContext
│   ├── platform/         # rivet_platform: abstraction headers (file dialog, main-thread dispatch)
│   │   └── macos/        # rivet_platform_macos: AppKit host, CoreGraphics paint, rivet executable
│   └── app/              # rivet_app: shell wiring, DocumentSession management
├── tests/                # per-module test executables + tests/harness (RivetTest.h)
├── docs/                 # this documentation and docs/adr/
└── build/                # preset build trees (gitignored)
```

Notes:

- Subsystems register their own `CMakeLists.txt`; the top level skips a
  subdirectory until its `CMakeLists.txt` exists, so the tree can grow
  incrementally during development.
- Target naming follows the layering in `docs/ARCHITECTURE.md`; the dependency
  direction is enforced by what each target links against.
- `compile_commands.json` is exported (`CMAKE_EXPORT_COMPILE_COMMANDS=ON`) for
  clangd/editor tooling.
