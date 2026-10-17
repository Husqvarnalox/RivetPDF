# Building Rivet

How to configure, build, test, and run Rivet from source.

Rivet is built with CMake (>= 3.28) and the Ninja generator. macOS is the
first supported development platform; Linux has an X11 desktop shell (see
section 6) and Windows a Win32/Direct2D shell (section 7).

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
│   │   ├── macos/        # rivet_platform_macos: AppKit host, CoreGraphics paint, rivet executable
│   │   └── linux/        # rivet_platform_linux: X11 host, cairo paint, rivet executable
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

---

## 6. Linux (X11 desktop shell)

`src/platform/linux` implements every platform abstraction on X11 (Xlib) +
cairo, with fontconfig + HarfBuzz + cairo-ft for text, libpng/libjpeg for
Replace Image and optional CUPS printing. It runs natively on X11 and on
Wayland desktops through XWayland. Only this directory uses these libraries.

Packages (Debian/Ubuntu):

```sh
sudo apt-get install cmake ninja-build g++ pkg-config \
    libx11-dev libcairo2-dev libfreetype-dev libharfbuzz-dev \
    libfontconfig-dev libpng-dev libjpeg-dev libcups2-dev
# runtime helpers (optional): zenity or kdialog (file dialogs), xdg-utils (links)
```

Build and run exactly as on macOS (`cmake -S . -B build/debug -G Ninja`,
`cmake --build build/debug`, `build/debug/src/platform/linux/rivet file.md`).
GCC 13+ or Clang 17+ is required for C++23.

When the X11/cairo/font/image development packages are missing, CMake prints
a STATUS message and skips only the `rivet` executable; the portable
libraries and tests still build. Without libcups the build has no printing
(`printService` is null).

Behaviour and current limits of the Linux shell:

- **Window**: one top-level window (min 940x600), UTF-8 title
  (`_NET_WM_NAME`), `*` title prefix for unsaved changes, close button and
  Ctrl+Q go through the same unsaved-changes prompts as macOS (an accepted
  window close ends the app: Rivet has one window). SIGINT/SIGTERM request a
  quit the same way.
- **Menus**: X11 has no global menu, so `app::buildMenuBar()` is drawn as an
  in-window menu strip (mouse, F10 + arrow keys, Ctrl shortcuts).
- **Keys**: Ctrl is the primary shortcut modifier (macOS Cmd), Alt = Option,
  Super = the macOS Control key; Ctrl+Tab cycles tabs. Text input goes
  through the X input method (dead keys/compose); on-the-spot IME
  pre-editing is not supported.
- **HiDPI**: scale from `RIVET_SCALE`, else `GDK_SCALE`, else `Xft.dpi`/96.
  Changing it requires a restart.
- **File dialogs**: `zenity`, else `kdialog`, run as a subprocess (no shell).
  Without either tool, Open reports "install zenity or kdialog" and Save As
  is unavailable. xdg-desktop-portal is not used yet.
- **Alerts**: drawn with Rivet's own widgets in a modal transient window;
  like `NSAlert`, main-thread tasks wait until the alert is answered.
- **Clipboard**: CLIPBOARD selection, UTF-8 text; no INCR transfers (very
  large texts) and no clipboard-manager hand-off on exit.
- **Printing** (CUPS): no print panel; the default CUPS destination and all
  pages after a Print/Cancel confirmation. No default printer = an error.
- **Scrolling**: wheel notches (core X11 buttons 4-7); no XInput2 smooth
  scrolling yet.
- **Links** open through `xdg-open`.

## 7. Windows (Win32 + Direct2D desktop shell)

`src/platform/windows` implements every platform abstraction with Win32
windows and menus, Direct2D drawing, DirectWrite text, WIC image decoding,
the common item dialogs, TaskDialog alerts, the Win32 clipboard,
`ShellExecuteW` for links and GDI printing. Windows 10 (1903+) and 11, x64
and ARM64. Only this directory (plus the Win32 file APIs of
`core/io/AtomicFileWriter`) uses Windows APIs.

Build from a "Developer PowerShell for VS 2022" (MSVC 19.4x, C++23) with
CMake and Ninja:

```powershell
cmake -S . -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_COMPILE_WARNING_AS_ERROR=ON
cmake --build build/debug
ctest --test-dir build/debug --output-on-failure
build/debug/src/platform/windows/rivet.exe file.pdf
```

clang/LLVM-MinGW also builds it (CI-independent cross check, e.g. from the
`mstorsjo/llvm-mingw` image with `-DCMAKE_SYSTEM_NAME=Windows` and
`x86_64-w64-mingw32-clang++` / `aarch64-w64-mingw32-clang++`).

PDFium on Windows: the code side is in place (the `_WIN32` branch of
`src/pdf/pdfium/PdfiumFileSource.cpp` streams the file with `CreateFileW` and
positioned `ReadFile` calls, share mode read|write|delete, so documents open
without a full read and can be replaced by the atomic writer), but no PDFium
binary is wired into the Windows build or CI yet. It was only syntax-checked
against the PDFium headers with the llvm-mingw cross compiler and has never
run on Windows, so the supported Windows configuration stays
`RIVET_WITH_PDFIUM=OFF` for now. The shell supplies a WIC JPEG encoder
(`WindowsImageEncoder`), so Save Reduced Copy has its image encoder there once
PDFium is available. There is no Windows OCR engine yet (Recognize Text is
disabled; see ADR-0027).

`rivet.exe` embeds a manifest: per-monitor-v2 DPI awareness, common controls
v6 and UTF-8 as the process code page (so `std::filesystem::path::string()`
is UTF-8).

Behaviour and current limits of the Windows shell:

- **Window**: one top-level window (min 940x600), `*` title prefix for
  unsaved changes, dark title bar when the system app theme is dark. The
  close button, Alt+F4 and File > Exit (Ctrl+Q) go through the same
  unsaved-changes prompts as macOS; an accepted close ends the app. Logoff
  and shutdown (`WM_QUERYENDSESSION`) are not intercepted yet.
- **Menus**: a native menu bar built from `app::buildMenuBar()`; enable and
  check states refresh when a menu opens; Alt/F10 keyboard access.
- **Keys**: Ctrl is the primary shortcut modifier (macOS Cmd), Alt = Option,
  the Windows key = the macOS Control key; Ctrl+Tab cycles tabs. Text
  arrives through `WM_CHAR` (dead keys, IME results, AltGr, surrogate pairs);
  IME composition is shown by the system window, not inline.
- **HiDPI**: per monitor, live (`WM_DPICHANGED`).
- **Alerts**: TaskDialog (Save / Don't Save / Cancel, Esc = Cancel);
  text prompts use a small native dialog. Like `NSAlert`, main-thread tasks
  wait until a dialog is answered (also for file and print dialogs).
- **Clipboard**: `CF_UNICODETEXT`; LF is written as CRLF and read back as LF.
- **Printing**: the system print dialog (printer, page range, copies);
  pages are scaled to fit the printable area and centered.
- **Scrolling**: wheel and horizontal wheel in 48-point notches (Shift turns
  the vertical wheel horizontal); no precision-touchpad inertia.
- **File drops** from Explorer open the dropped documents.
