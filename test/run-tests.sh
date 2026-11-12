#!/usr/bin/env bash
# Run the obs-optical-flow test harnesses against an installed OBS Studio
# (Linux: system libobs; macOS: /Applications/OBS.app).
#
# Usage (from the unpacked tests-* folder):  ./run-tests.sh
# Hybrid-GPU Linux laptops: run once plain and once via `prime-run ./run-tests.sh`.
set -uo pipefail
cd "$(dirname "$0")"

fail=0
for t in effect-test flow-test debug-test perf-test; do
	echo
	echo "===== $t [opengl] ====="
	if "./$t" --backend opengl --effect data/effects/optical-flow.effect; then
		echo "-> PASS"
	else
		echo "-> FAIL ($?)"
		fail=1
	fi
done
exit $fail
