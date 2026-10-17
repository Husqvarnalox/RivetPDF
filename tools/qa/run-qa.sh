#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Runs scripted QA scenarios (tools/qa/scenarios/*.rivetqa) against a built
# Rivet binary. Screenshots land in $RIVET_QA_OUT (default build/qa-shots),
# logs next to them as <scenario>.log.
#
#   tools/qa/run-qa.sh <rivet-binary> [scenario ...]
#
# Environment: QA_FIX (fixture dir, default build/qa-fixtures; create it with
# tools/qa/make-fixtures.sh), RIVET_QA_OUT, QA_TIMEOUT (seconds per scenario,
# default 300), QA_WRAP (a command prefix such as "xvfb-run -a"),
# RIVET_APP_DATA (application data; default: a fresh build/qa-appdata/<scenario>
# per scenario, so runs never touch the user's preferences or Recent list).
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
binary="${1:?usage: run-qa.sh <rivet-binary> [scenario ...]}"
shift
export QA_ROOT="$root"
export QA_FIX="${QA_FIX:-$root/build/qa-fixtures}"
export QA_WORK="${QA_WORK:-$root/build/qa-work}"
export RIVET_QA_OUT="${RIVET_QA_OUT:-$root/build/qa-shots}"
timeout_s="${QA_TIMEOUT:-300}"
mkdir -p "$RIVET_QA_OUT" "$QA_WORK"

if [ "$#" -eq 0 ]; then
    set -- "$here"/scenarios/*.rivetqa
fi

status=0
for scenario in "$@"; do
    name="$(basename "$scenario" .rivetqa)"
    log="$RIVET_QA_OUT/$name.log"
    echo "== $name"
    # shellcheck disable=SC2086
    if command -v timeout >/dev/null 2>&1; then
        limit=(timeout "$timeout_s")
    else
        limit=(perl -e 'alarm shift; exec @ARGV' "$timeout_s") # macOS has no timeout(1)
    fi
    appdata="${RIVET_APP_DATA:-}"
    if [ -z "$appdata" ]; then
        appdata="$root/build/qa-appdata/$name"
        rm -rf "$appdata"
    fi
    RIVET_APP_DATA="$appdata" RIVET_QA_SCRIPT="$scenario" "${limit[@]}" ${QA_WRAP:-} "$binary" >"$log" 2>&1
    code=$?
    grep -E "^\[qa\] (line [0-9]+: (FAIL|ERROR)|ERROR|done|.*parse error)" "$log"
    echo "   exit code $code (log: $log)"
    [ "$code" -ne 0 ] && status=1
done
exit "$status"
