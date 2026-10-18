# Releasing Rivet (alpha)

macOS artifacts are built by hand on a Mac (below). Linux x64 and Windows x64
artifacts are built by CI only (see "Linux and Windows" at the end).

## Build the artifacts

    tools/package-macos.sh

Requirements: macOS on Apple Silicon, Xcode command line tools, CMake >= 3.28,
Ninja, a PDFium install (`RIVET_PDFIUM_ROOT`, default
`/Users/a55532/Projects/pdfium-install`, see `docs/BUILDING_PDFIUM.md`), and
OpenSSL 3 with static libraries (`OPENSSL_ROOT_DIR`, default
`/opt/homebrew/opt/openssl@3`).

The script:

1. Configures and builds a Release build in `build/release-pkg` (arm64,
   tests OFF, PDFium ON, crypto ON with static OpenSSL, PKCS#11 ON (the module
   is `dlopen`ed at run time), network ON using the macOS SDK's system libcurl
   8.x).
2. Fails unless the binaries link only `/usr/lib` and `/System` libraries.
3. Assembles `dist/Rivet.app` (version taken from `project(rivet VERSION ...)`
   in `CMakeLists.txt`, bundle id `dev.rivet.Rivet`, PDF and Markdown document
   types) and ad-hoc signs it (`codesign -s -`), then verifies it with
   `codesign --verify --deep --strict`.
4. Produces `Rivet-<version>-macos-arm64.dmg` (with an `/Applications`
   symlink), `Rivet-<version>-macos-arm64.zip`,
   `rivet-cli-<version>-macos-arm64.tar.gz` (`rivet-pdf-encrypt`,
   `rivet-pdf-signature-inspect`, `rivet-signature-inspect`) and `SHA256SUMS`
   in `dist/` (git-ignored; never commit artifacts).

The minimum macOS version is 26.0 (override with `MACOSX_DEPLOYMENT_TARGET`):
`src/app/QaScript.cpp` uses floating-point `std::from_chars`, which this SDK's
libc++ only provides from macOS 26, and the Homebrew OpenSSL archives are built
for 26.0.

## Signing status

The artifacts are **ad-hoc signed only**. They are not signed with a Developer
ID and are **not notarized**. On first run Gatekeeper will block a downloaded
copy. Either right-click the app and choose Open, or run:

    xattr -dr com.apple.quarantine /Applications/Rivet.app

## Not provided

- Installers for Windows and Linux (only a zip / tar.gz of the binaries).
- Intel Macs: the prebuilt PDFium is arm64-only, so only arm64 is supported.

## Linux and Windows (CI only)

`.github/workflows/release.yml` builds both artifact sets; there is no
supported local path other than running the scripts it calls:

- `tools/package-linux.sh` (GCC, Release, PDFium ON, crypto ON, network ON,
  PKCS#11 ON, tests OFF; needs `RIVET_PDFIUM_ROOT`) produces
  `Rivet-<version>-linux-x64.tar.gz`, `rivet-cli-<version>-linux-x64.tar.gz` and
  `SHA256SUMS` in `dist/`. It prints `ldd` for every binary and fails if a
  binary has a direct `DT_NEEDED` entry outside an allow-list (glibc, libstdc++,
  libgcc_s, X11, cairo, freetype, harfbuzz, fontconfig, libpng16, libjpeg, cups,
  curl, libcrypto.so.3). Binaries are stripped.
- `tools/package-windows.ps1` (PowerShell 7, MSVC x64 developer shell, static
  `/MT` CRT, static PDFium, vcpkg `x64-windows-static` OpenSSL, crypto ON,
  network and PKCS#11 OFF as in CI; needs `RIVET_PDFIUM_ROOT` and
  `OPENSSL_ROOT_DIR`) produces `Rivet-<version>-windows-x64.zip`,
  `rivet-cli-<version>-windows-x64.zip` and `SHA256SUMS`. It runs
  `dumpbin /dependents` and fails if a binary imports a DLL that is not in
  System32 / an API set, or is a C/C++ runtime or third-party DLL.

The PDFium cache keys and steps are the same as in `ci.yml`, so a release
build normally restores the PDFium that CI already built; a cold build takes
tens of minutes.

### Cutting a release

1. Bump `project(rivet VERSION ...)` in `CMakeLists.txt` and merge to `main`
   once `ci` is green.
2. Tag and push: `git tag v0.1.0 && git push origin v0.1.0`. The workflow
   builds both platforms, uploads them as workflow artifacts
   (`rivet-linux-x64`, `rivet-windows-x64`) and attaches the files to the
   release for that tag with `gh release upload --clobber`. If the release does
   not exist it is created as a **prerelease**.
3. The `publish` job also uploads `SHA256SUMS-linux-windows.txt` covering the
   Linux and Windows assets. It never touches the `SHA256SUMS` of the macOS
   artifacts: build and upload those yourself with `tools/package-macos.sh`
   and `gh release upload`.
4. To rebuild or retry: Actions > release > Run workflow with the `tag` input
   (the tag must already exist). Running it with an empty tag only produces
   downloadable workflow artifacts and creates no release.

### Limitations (honest)

- Neither artifact set has been run on a real desktop by this pipeline: the
  scripts verify the build and the binaries' library dependencies, nothing
  more. Treat Linux (X11/cairo) and Windows (Win32/Direct2D) builds as
  untested alpha.
- Windows: not Authenticode-signed (SmartScreen will warn). The build has no
  network (libcurl) support and no PKCS#11 provider.
- Linux: not signed. Built on the `ubuntu-latest` runner image, so the glibc
  and libstdc++ baseline is whatever that image ships (currently Ubuntu 24.04
  at the time of writing; check the runner image); older distributions may
  fail to start with `GLIBC_*` / `GLIBCXX_*` errors. The binary needs these
  shared libraries installed: X11, cairo, freetype, harfbuzz, fontconfig,
  libpng16, libjpeg, libcups, libcurl and libcrypto.so.3 (OpenSSL 3 is linked
  dynamically). X11 only (XWayland on Wayland). PDFium is linked statically.
  PKCS#11 modules are `dlopen`ed from a user-chosen path.
- x64 only; no ARM builds, no installers, no AppImage/deb/rpm/MSI.
- The packaging scripts and `release.yml` were written without a CI run; the
  first tag build may need fixes.
