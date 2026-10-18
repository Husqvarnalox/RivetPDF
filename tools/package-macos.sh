#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Builds Rivet in Release and produces the macOS (arm64) distribution
# artifacts in dist/: Rivet.app (ad-hoc signed), .dmg, .zip, CLI tarball and
# SHA256SUMS. The artifacts are NOT Developer-ID signed and NOT notarized.
# See docs/RELEASING.md.
#
# Environment overrides:
#   RIVET_PDFIUM_ROOT   PDFium install prefix (default /Users/a55532/Projects/pdfium-install)
#   OPENSSL_ROOT_DIR    OpenSSL 3 prefix with static libs (default /opt/homebrew/opt/openssl@3)
#   MACOSX_DEPLOYMENT_TARGET  minimum macOS (default 26.0: src/app/QaScript.cpp uses
#                             std::from_chars(double), only available from macOS 26 in this SDK's libc++;
#                             the Homebrew OpenSSL .a is also built for 26.0)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PDFIUM_ROOT="${RIVET_PDFIUM_ROOT:-/Users/a55532/Projects/pdfium-install}"
OPENSSL_ROOT="${OPENSSL_ROOT_DIR:-/opt/homebrew/opt/openssl@3}"
MIN_OS="${MACOSX_DEPLOYMENT_TARGET:-26.0}"
BUILD_DIR="$ROOT/build/release-pkg"
DIST="$ROOT/dist"
ARCH=arm64
CLI_TARGETS=(rivet-pdf-encrypt rivet-pdf-signature-inspect rivet-signature-inspect)

[[ "$(uname -s)" == Darwin ]] || { echo "error: macOS only" >&2; exit 1; }
[[ -f "$PDFIUM_ROOT/include/fpdfview.h" ]] || { echo "error: PDFium not found at $PDFIUM_ROOT" >&2; exit 1; }
[[ -f "$OPENSSL_ROOT/lib/libcrypto.a" ]] || { echo "error: static OpenSSL not found at $OPENSSL_ROOT" >&2; exit 1; }

VERSION="$(sed -n '/^project(rivet/,/)/s/.*VERSION[[:space:]]*\([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -n1)"
[[ -n "$VERSION" ]] || { echo "error: cannot read project VERSION" >&2; exit 1; }
echo "==> Rivet $VERSION ($ARCH, macOS >= $MIN_OS)"

# Pin libcurl to the macOS SDK (headers + system libcurl.tbd) so a stray curl
# on CMAKE_PREFIX_PATH / framework paths (e.g. vendor SDKs) is never picked up.
SDK="$(xcrun --show-sdk-path)"
[[ -f "$SDK/usr/include/curl/curl.h" ]] || { echo "error: SDK lacks libcurl headers" >&2; exit 1; }

echo "==> Configure"
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=$ARCH \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_OS" \
    -DRIVET_BUILD_TESTS=OFF \
    -DRIVET_WITH_PDFIUM=ON -DRIVET_PDFIUM_ROOT="$PDFIUM_ROOT" \
    -DRIVET_ENABLE_CRYPTO=ON \
    -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT" -DOPENSSL_USE_STATIC_LIBS=TRUE \
    -DRIVET_ENABLE_NETWORK=ON \
    -DCURL_INCLUDE_DIR="$SDK/usr/include" -DCURL_LIBRARY="$SDK/usr/lib/libcurl.tbd" \
    -DRIVET_ENABLE_PKCS11=ON

echo "==> Build"
cmake --build "$BUILD_DIR" --target rivet "${CLI_TARGETS[@]}"

find_bin() { find "$BUILD_DIR" -type f -name "$1" -perm -u+x -not -path '*/CMakeFiles/*' | head -n1; }
APP_BIN="$(find_bin rivet)"
[[ -n "$APP_BIN" ]] || { echo "error: rivet binary not found" >&2; exit 1; }

echo "==> Verify binaries link only system libraries"
check_system_only() {
    local bad
    bad="$(otool -L "$1" | tail -n +2 | awk '{print $1}' | grep -Ev '^(/usr/lib/|/System/)' || true)"
    if [[ -n "$bad" ]]; then echo "error: $1 links non-system libraries:" >&2; echo "$bad" >&2; exit 1; fi
}
check_system_only "$APP_BIN"
for t in "${CLI_TARGETS[@]}"; do check_system_only "$(find_bin "$t")"; done

echo "==> Assemble app bundle"
rm -rf "$DIST"
mkdir -p "$DIST/Rivet.app/Contents/MacOS" "$DIST/Rivet.app/Contents/Resources"
APP="$DIST/Rivet.app"
cp "$APP_BIN" "$APP/Contents/MacOS/rivet"
strip -x "$APP/Contents/MacOS/rivet"
printf 'APPL????' > "$APP/Contents/PkgInfo"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key><string>en</string>
    <key>CFBundleExecutable</key><string>rivet</string>
    <key>CFBundleIdentifier</key><string>dev.rivet.Rivet</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>CFBundleName</key><string>Rivet</string>
    <key>CFBundleDisplayName</key><string>Rivet</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>$MIN_OS</string>
    <key>LSApplicationCategoryType</key><string>public.app-category.productivity</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSHighDPIRequired</key><true/>
    <key>NSPrincipalClass</key><string>NSApplication</string>
    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key><string>PDF Document</string>
            <key>CFBundleTypeRole</key><string>Editor</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>com.adobe.pdf</string></array>
            <key>CFBundleTypeExtensions</key><array><string>pdf</string></array>
        </dict>
        <dict>
            <key>CFBundleTypeName</key><string>Markdown Document</string>
            <key>CFBundleTypeRole</key><string>Editor</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key><array><string>net.daringfireball.markdown</string></array>
            <key>CFBundleTypeExtensions</key><array><string>md</string><string>markdown</string></array>
        </dict>
    </array>
</dict>
</plist>
PLIST
plutil -lint "$APP/Contents/Info.plist"

echo "==> Ad-hoc codesign (NOT Developer ID, NOT notarized)"
codesign --force --deep -s - "$APP"
codesign --verify --deep --strict --verbose=1 "$APP"

echo "==> DMG"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
DMG="$DIST/Rivet-$VERSION-macos-$ARCH.dmg"
hdiutil create -volname "Rivet $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG" >/dev/null

echo "==> ZIP"
ZIP="$DIST/Rivet-$VERSION-macos-$ARCH.zip"
ditto -c -k --keepParent "$APP" "$ZIP"

echo "==> CLI tarball"
CLI_DIR="$STAGE/rivet-cli-$VERSION"
mkdir -p "$CLI_DIR"
for t in "${CLI_TARGETS[@]}"; do
    cp "$(find_bin "$t")" "$CLI_DIR/$t"; strip -x "$CLI_DIR/$t"
    codesign --force -s - "$CLI_DIR/$t"
done
CLI_TGZ="$DIST/rivet-cli-$VERSION-macos-$ARCH.tar.gz"
tar -C "$STAGE" -czf "$CLI_TGZ" "rivet-cli-$VERSION"

echo "==> SHA256SUMS"
( cd "$DIST" && shasum -a 256 "$(basename "$DMG")" "$(basename "$ZIP")" "$(basename "$CLI_TGZ")" > SHA256SUMS )

echo "==> Done"
ls -l "$DIST"
echo "NOTE: ad-hoc signed, not notarized; see docs/RELEASING.md for the Gatekeeper first-run steps."
