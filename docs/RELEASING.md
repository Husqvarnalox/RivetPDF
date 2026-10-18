# Releasing Rivet (macOS, alpha)

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

- Windows and Linux installers: those desktop shells are not implemented.
- Intel Macs: the prebuilt PDFium is arm64-only, so only arm64 is supported.
