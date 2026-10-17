#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Reproducibly builds the static PDFium Rivet links against (macOS, Linux) and
# assembles the layout cmake/FindPDFium.cmake expects:
#
#   <install>/include/*.h
#   <install>/lib/libpdfium.a
#   <install>/PDFIUM_BUILD_INFO       (revision, args hash, security flags)
#
# Usage:
#   tools/pdfium/build-pdfium.sh --install DIR [--arch arm64|x64] [--work DIR]
#                                [--depot-tools DIR] [--jobs N]
#
#   --arch   target CPU (default: the host CPU). macOS: arm64 or x64 (x86_64
#            accepted); Linux: x64 (arm64 is supported by GN but not gated).
#   --work   checkout/build scratch (default: ./.pdfium-work). Reused between
#            runs; keep it outside the repository or gitignored.
#   --depot-tools  existing depot_tools checkout (default: <work>/depot_tools,
#            cloned if missing; override the ref with RIVET_DEPOT_TOOLS_REF).
#
# Pinned inputs (all checked in next to this script):
#   PDFIUM_REVISION   PDFium git commit
#   args.common.gn + args.<os>.gn   GN arguments (+ generated target_cpu)
#   patches/*.patch   source patches applied on top of the revision
#
# This script contacts the network (googlesource) by design; Rivet's own CMake
# build never does.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
install_dir=""
work_dir="$PWD/.pdfium-work"
depot_dir=""
arch=""
jobs=""

while [ $# -gt 0 ]; do
  case "$1" in
    --install) install_dir="$2"; shift 2 ;;
    --work) work_dir="$2"; shift 2 ;;
    --depot-tools) depot_dir="$2"; shift 2 ;;
    --arch) arch="$2"; shift 2 ;;
    --jobs) jobs="$2"; shift 2 ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$install_dir" ] || { echo "--install DIR is required" >&2; exit 2; }

case "$(uname -s)" in
  Darwin) os=macos; libname=libpdfium.a ;;
  Linux)  os=linux; libname=libpdfium.a ;;
  *) echo "unsupported host for this script (use build-pdfium.ps1 on Windows)" >&2; exit 2 ;;
esac

host_cpu="$(uname -m)"
[ -n "$arch" ] || arch="$host_cpu"
case "$arch" in
  x64|x86_64|amd64) cpu=x64 ;;
  arm64|aarch64) cpu=arm64 ;;
  *) echo "unsupported --arch: $arch" >&2; exit 2 ;;
esac

revision="$(tr -d '[:space:]' < "$here/PDFIUM_REVISION")"
mkdir -p "$work_dir" "$install_dir"
work_dir="$(cd "$work_dir" && pwd)"
install_dir="$(cd "$install_dir" && pwd)"
[ -n "$depot_dir" ] || depot_dir="$work_dir/depot_tools"

sha256() { if command -v sha256sum >/dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

# --- depot_tools -----------------------------------------------------------
if [ ! -d "$depot_dir/.git" ] && [ ! -x "$depot_dir/gclient" ]; then
  git clone --depth 1 ${RIVET_DEPOT_TOOLS_REF:+--branch "$RIVET_DEPOT_TOOLS_REF"} \
    https://chromium.googlesource.com/chromium/tools/depot_tools.git "$depot_dir"
fi
export PATH="$depot_dir:$PATH"
export DEPOT_TOOLS_UPDATE=0   # reproducible: do not self-update mid-run

# --- source at the pinned revision ----------------------------------------
cat > "$work_dir/.gclient" <<EOF
solutions = [
  {
    "name": "pdfium",
    "url": "https://pdfium.googlesource.com/pdfium.git@${revision}",
    "deps_file": "DEPS",
    "managed": False,
    "custom_deps": {},
    "custom_vars": {},
  },
]
EOF
(cd "$work_dir" && { gclient sync --no-history --revision "pdfium@${revision}" || gclient sync --revision "pdfium@${revision}"; })

src="$work_dir/pdfium"
actual="$(git -C "$src" rev-parse HEAD)"
[ "$actual" = "$revision" ] || { echo "checkout is $actual, expected $revision" >&2; exit 1; }

# --- patches (idempotent) ---------------------------------------------------
for patch in "$here"/patches/*.patch; do
  if git -C "$src" apply --reverse --check "$patch" 2>/dev/null; then
    echo "patch already applied: $(basename "$patch")"
  else
    git -C "$src" apply --check "$patch"
    git -C "$src" apply "$patch"
    echo "applied patch: $(basename "$patch")"
  fi
done

# --- gn args + build --------------------------------------------------------
out="$src/out/rivet-${os}-${cpu}"
mkdir -p "$out"
{
  cat "$here/args.common.gn"
  echo
  cat "$here/args.$os.gn"
  echo "target_cpu = \"$cpu\""
} > "$out/args.gn"

(cd "$src" && gn gen "$out")
(cd "$src" && autoninja ${jobs:+-j "$jobs"} -C "$out" pdfium)

lib="$out/obj/$libname"
[ -f "$lib" ] || { echo "expected library not produced: $lib" >&2; exit 1; }

# --- install layout ---------------------------------------------------------
rm -rf "${install_dir:?}/include" "${install_dir:?}/lib"
mkdir -p "$install_dir/include" "$install_dir/lib"
cp "$src"/public/*.h "$install_dir/include/"
cp "$lib" "$install_dir/lib/$libname"

args_hash="$(sha256 "$out/args.gn" | cut -d' ' -f1)"
flag() { sed -n "s/^$1 *= *\(.*\)$/\1/p" "$out/args.gn" | tail -1; }
cat > "$install_dir/PDFIUM_BUILD_INFO" <<EOF
revision=$revision
target_os=$os
target_cpu=$cpu
args_sha256=$args_hash
pdf_enable_v8=$(flag pdf_enable_v8)
pdf_enable_xfa=$(flag pdf_enable_xfa)
pdf_use_skia=$(flag pdf_use_skia)
use_custom_libcxx=$(flag use_custom_libcxx)
EOF

echo "PDFium $revision ($os/$cpu) installed to $install_dir"
echo "Configure Rivet with: -DRIVET_WITH_PDFIUM=ON -DRIVET_PDFIUM_ROOT=$install_dir"
