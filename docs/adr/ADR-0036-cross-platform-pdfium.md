# ADR-0036: Cross-platform PDFium builds (macOS, Windows, Linux)

Status: Accepted (Linux/Windows link sets and CI unverified at time of writing)

Date: 2026-10-17

## Context

PDFium-ON builds existed only on macOS, from a hand-built archive. The
portable layers compile everywhere, but without a reproducible PDFium the
Windows and Linux products had no PDF engine. Requirements: one pinned
PDFium, identical capability flags on every OS (no accidental PDF semantic
differences), the ADR-0007 security policy everywhere, no network access from
Rivet's own build.

## Decision

1. **One recipe, checked in** (`tools/pdfium/`): `PDFIUM_REVISION` (the
   existing macOS revision `40ecb4f4...`), `args.common.gn` (V8 off, XFA off,
   Skia off, AGG, static complete lib, release, `use_custom_libcxx=false`),
   thin `args.<os>.gn` files that only select `target_os` and
   build-environment args, source patches, and two drivers
   (`build-pdfium.sh`, `build-pdfium.ps1`). The drivers fetch depot_tools and
   the pinned revision, apply patches, build, and install
   `include/` + `lib/` + `PDFIUM_BUILD_INFO`. Capability flags live only in
   the common file.
2. **Static CRT on MSVC.** GN's non-component build yields `/MT`; Rivet adopts
   `/MT` (release CRT in every configuration) whenever PDFium is ON
   (`cmake/RivetMsvcRuntime.cmake`). The PDFium-OFF Windows job keeps `/MD`.
3. **System libraries** are attached to `PDFium::PDFium` per OS in
   `cmake/FindPDFium.cmake` (Apple frameworks verified; Linux `dl;m` and
   Windows GDI/COM/crypto sets are deliberate over-approximations).
4. **Policy enforcement at configure time**: if `PDFIUM_BUILD_INFO` exists,
   configuration fails when V8, XFA or Skia was enabled.
5. **CI**: `linux-pdfium` (GCC) and `windows-msvc-pdfium` build PDFium via the
   scripts on a cache miss (`actions/cache`, key hashed from `tools/pdfium/**`),
   then configure PDFium ON with warnings-as-errors, build, run ctest and the
   `rivet-smoke-open` smoke (open, render page 1, save a copy through the
   assembly, reopen).
6. **Linux toolchain**: PDFium is built by its bundled clang against host
   libstdc++ (`use_sysroot=false`), so the archive links into GCC- and
   Clang-built Rivet. PDFium itself does not support GCC builds.

## Consequences

- Updating PDFium means changing `PDFIUM_REVISION` (and re-checking patches);
  CI rebuilds the cache automatically.
- ARM64 (Windows, Linux) and a universal macOS archive (`lipo`) are
  documented, not gated.
- Windows Debug + PDFium uses the release static CRT; STL debug iterators are
  therefore off in that configuration.
- Not verified on a real Windows or Linux machine when this ADR was written:
  the GN args at this revision for those targets, the system-library lists,
  `/MT` compatibility of the Win32 shell, the cold-build time of the CI jobs.
  The first CI runs are the verification; expect small follow-up fixes.
- The cached build depends on the runner's toolchain only through the host
  libstdc++ headers (Linux) and the Visual Studio/Windows SDK (Windows); a
  runner image change that breaks compatibility needs a cache-key bump (edit
  any file in `tools/pdfium/`).
