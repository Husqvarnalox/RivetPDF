#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Builds Rivet in Release and produces the Linux x86_64 distribution
# artifacts in dist/: Rivet-<ver>-linux-x64.tar.gz, rivet-cli-<ver>-linux-x64.tar.gz
# and SHA256SUMS. Intended for the ubuntu-latest CI runner (GCC); the binaries
# link the system's shared X11/cairo/... libraries (see the README in the
# tarball and docs/RELEASING.md). Not tested on a real desktop by this script.
#
# Build dependencies (Debian/Ubuntu package names):
#   cmake ninja-build pkg-config g++ libx11-dev libcairo2-dev libfreetype-dev
#   libharfbuzz-dev libfontconfig-dev libpng-dev libjpeg-dev libcups2-dev
#   libcurl4-openssl-dev libssl-dev
#
# Environment:
#   RIVET_PDFIUM_ROOT   PDFium install prefix (required; tools/pdfium/build-pdfium.sh --install)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

PDFIUM_ROOT="${RIVET_PDFIUM_ROOT:-}"
BUILD_DIR="$ROOT/build/release-pkg-linux"
DIST="$ROOT/dist"
ARCH=x64
CLI_TARGETS=(rivet-pdf-encrypt rivet-pdf-signature-inspect rivet-signature-inspect)

[[ "$(uname -s)" == Linux ]] || { echo "error: Linux only" >&2; exit 1; }
[[ "$(uname -m)" == x86_64 ]] || { echo "error: x86_64 only" >&2; exit 1; }
[[ -n "$PDFIUM_ROOT" && -f "$PDFIUM_ROOT/include/fpdfview.h" ]] \
    || { echo "error: set RIVET_PDFIUM_ROOT to a PDFium install prefix" >&2; exit 1; }

VERSION="$(sed -n '/^project(rivet/,/)/s/.*VERSION[[:space:]]*\([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -n1)"
[[ -n "$VERSION" ]] || { echo "error: cannot read project VERSION" >&2; exit 1; }
echo "==> Rivet $VERSION (linux-$ARCH)"

echo "==> Configure"
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DRIVET_BUILD_TESTS=OFF \
    -DRIVET_WITH_PDFIUM=ON -DRIVET_PDFIUM_ROOT="$PDFIUM_ROOT" \
    -DRIVET_ENABLE_CRYPTO=ON \
    -DRIVET_ENABLE_NETWORK=ON \
    -DRIVET_ENABLE_PKCS11=ON

echo "==> Build"
cmake --build "$BUILD_DIR" --target rivet "${CLI_TARGETS[@]}"

find_bin() { find "$BUILD_DIR" -type f -name "$1" -perm -u+x -not -path '*/CMakeFiles/*' | head -n1; }
APP_BIN="$(find_bin rivet)"
[[ -n "$APP_BIN" ]] || { echo "error: rivet binary not found (X11/cairo dev packages missing? CMake skips the executable then)" >&2; exit 1; }
for t in "${CLI_TARGETS[@]}"; do
    [[ -n "$(find_bin "$t")" ]] || { echo "error: $t not found" >&2; exit 1; }
done

# Direct DT_NEEDED entries must be within this allow-list (glibc/libstdc++ and
# the libraries src/platform/linux, src/net and src/crypto link). Transitive
# dependencies of those libraries are resolved by the system package manager.
ALLOWED_RE='^(libc\.so\.6|libm\.so\.6|libdl\.so\.2|libpthread\.so\.0|librt\.so\.1|libstdc\+\+\.so\.6|libgcc_s\.so\.1|ld-linux-x86-64\.so\.2|libX11\.so\.6|libcairo\.so\.2|libfreetype\.so\.6|libharfbuzz\.so\.0|libfontconfig\.so\.1|libpng16\.so\.16|libjpeg\.so\.[0-9]+|libcups\.so\.2|libcurl\.so\.4|libcurl-gnutls\.so\.4|libcrypto\.so\.3)$'

echo "==> Verify shared-library dependencies"
check_deps() {
    local bin="$1" needed bad
    echo "--- ldd $(basename "$bin")"
    ldd "$bin"
    if ldd "$bin" | grep -q 'not found'; then echo "error: unresolved libraries in $bin" >&2; exit 1; fi
    needed="$(readelf -d "$bin" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')"
    bad="$(printf '%s\n' "$needed" | grep -Ev "$ALLOWED_RE" || true)"
    if [[ -n "$bad" ]]; then
        echo "error: $bin needs libraries outside the allow-list:" >&2; echo "$bad" >&2; exit 1
    fi
}
check_deps "$APP_BIN"
for t in "${CLI_TARGETS[@]}"; do check_deps "$(find_bin "$t")"; done

echo "==> Assemble"
rm -rf "$DIST"
mkdir -p "$DIST"
STAGE="$(mktemp -d)"; trap 'rm -rf "$STAGE"' EXIT
APP_DIR="$STAGE/Rivet-$VERSION-linux-$ARCH"
mkdir -p "$APP_DIR"
cp "$APP_BIN" "$APP_DIR/rivet"
for t in "${CLI_TARGETS[@]}"; do cp "$(find_bin "$t")" "$APP_DIR/$t"; done
strip --strip-unneeded "$APP_DIR"/rivet "$APP_DIR"/rivet-*
cp LICENSE "$APP_DIR/LICENSE"
cat > "$APP_DIR/README.txt" <<README
Rivet $VERSION - Linux x86_64 (alpha)

Run ./rivet [file.pdf]. The command-line tools (rivet-pdf-encrypt,
rivet-pdf-signature-inspect, rivet-signature-inspect) sit next to it.

Built on the GitHub ubuntu-latest runner (GCC, glibc of that image): it needs
a glibc and libstdc++ at least that new. Not tested on a real desktop by the
release pipeline. X11 only (Wayland desktops use XWayland).

Runtime shared libraries (Debian/Ubuntu package names; install what is missing):
  libx11-6 libcairo2 libfreetype6 libharfbuzz0b libfontconfig1 libpng16-16
  libjpeg-turbo8 (or libjpeg8) libcups2 libcurl4 libssl3 (libcrypto.so.3)
PDFium is linked statically. A PKCS#11 module for hardware tokens is loaded at
run time from a path you choose; none is bundled.

The files are not signed. Verify the download against SHA256SUMS.
License: MPL-2.0 (see LICENSE).
README

CLI_DIR="$STAGE/rivet-cli-$VERSION-linux-$ARCH"
mkdir -p "$CLI_DIR"
for t in "${CLI_TARGETS[@]}"; do cp "$APP_DIR/$t" "$CLI_DIR/$t"; done
cp LICENSE "$CLI_DIR/LICENSE"
cat > "$CLI_DIR/README.txt" <<README
Rivet command-line tools $VERSION - Linux x86_64 (alpha)

rivet-pdf-encrypt, rivet-pdf-signature-inspect, rivet-signature-inspect.
Needs the system libstdc++ and libcrypto.so.3 (libssl3) at runtime; built on
the GitHub ubuntu-latest runner. Not signed; verify against SHA256SUMS.
License: MPL-2.0 (see LICENSE).
README

APP_TGZ="Rivet-$VERSION-linux-$ARCH.tar.gz"
CLI_TGZ="rivet-cli-$VERSION-linux-$ARCH.tar.gz"
tar -C "$STAGE" -czf "$DIST/$APP_TGZ" "Rivet-$VERSION-linux-$ARCH"
tar -C "$STAGE" -czf "$DIST/$CLI_TGZ" "rivet-cli-$VERSION-linux-$ARCH"

echo "==> SHA256SUMS"
( cd "$DIST" && sha256sum "$APP_TGZ" "$CLI_TGZ" > SHA256SUMS && cat SHA256SUMS )

echo "==> Done"
ls -l "$DIST"
