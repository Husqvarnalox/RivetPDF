#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Linux (X11) build + scripted QA pass inside a throwaway Ubuntu container.
# Run from the repository root on the host:
#
#   docker run --rm -v "$PWD":/src:ro -v "$PWD/build/linux-qa":/out ubuntu:24.04 \
#       bash /src/tools/qa/linux-qa.sh
#
# The source tree is mounted read-only; everything is built under /work and
# the logs and screenshots are written to /out. PDFium is OFF (no Linux
# PDFium binary is wired up), so the PDF scenarios cannot run here; the
# Linux pass covers the shell chrome, Markdown and the harness itself.
set -u
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/out/apt.log 2>&1
apt-get install -y -qq cmake ninja-build g++ pkg-config libx11-dev libcairo2-dev libfreetype-dev \
    libharfbuzz-dev libfontconfig-dev libpng-dev libjpeg-dev libcups2-dev xvfb fonts-dejavu-core \
    >>/out/apt.log 2>&1 || { echo "apt failed"; tail -20 /out/apt.log; exit 1; }

cp -a /src /work
cd /work
rm -rf build
cmake -S . -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS=-Werror -DRIVET_BUILD_TESTS=ON \
    >/out/configure.log 2>&1 || { tail -30 /out/configure.log; exit 1; }
cmake --build build/linux -j4 >/out/build.log 2>&1
build_status=$?
grep -E "warning:|error:" /out/build.log | head -40
echo "build exit $build_status"
[ "$build_status" -eq 0 ] || exit 1

Xvfb :99 -screen 0 1920x1200x24 >/out/xvfb.log 2>&1 &
export DISPLAY=:99
sleep 1
ctest --test-dir build/linux --output-on-failure >/out/ctest.log 2>&1
echo "ctest exit $? ($(grep -E 'tests passed|tests failed' /out/ctest.log))"

export QA_FIX=/work/build/qa-fixtures QA_WORK=/work/build/qa-work RIVET_QA_OUT=/out/shots
tools/qa/make-fixtures.sh >/out/fixtures.log 2>&1 || echo "fixtures failed"
tools/qa/run-qa.sh build/linux/src/platform/linux/rivet tools/qa/scenarios/10-welcome-resize.rivetqa \
    tools/qa/scenarios/12-linux-smoke.rivetqa
exit 0
