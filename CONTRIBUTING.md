# Contributing

Rivet is an early-stage project. Please open an issue before starting a large change.

## Prerequisites

C++23 compiler (AppleClang 21+ on macOS; GCC on Linux for the portable layers), CMake ≥ 3.28, Ninja. PDFium is optional and must be built locally: see [docs/BUILDING_PDFIUM.md](docs/BUILDING_PDFIUM.md).

## Build and test

```sh
cmake --preset debug
cmake --build build/debug
ctest --test-dir build/debug
```

For changes touching PDF code, also build with `debug-pdfium` (with `RIVET_PDFIUM_ROOT` set). For threading changes, run the `sanitizers` preset and a `-fsanitize=thread` build. Further options: [docs/BUILDING.md](docs/BUILDING.md).

## Rules

- Warnings are errors (`-Werror`); new code must build warning-free on AppleClang and GCC.
- `FPDF_*` is used only inside `src/pdf/pdfium/`; AppKit/CoreGraphics/CoreText only in `src/platform/macos/`.
- Follow the threading contract in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#5-threading-model).

## Pull requests

Keep each PR to one coherent change, include tests, and describe what you verified. CI (macOS and Ubuntu) must pass.
