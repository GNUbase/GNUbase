#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the render spike with every backend this machine may have: screenshots of both documents at dp ratio 1 and 2,
# and the benchmark. Writes them, the logs and summary.md into OUT. A backend that can't run is a result, not an
# error, so this fails only on bad arguments.
#
# usage: run-all.sh EXE OUT [NAME:ARGS]...
#   Each NAME:ARGS is a backend to try, e.g. "gl3:--backend gl3" or "vulkan:--backend gpu --gpu-driver vulkan".
#   Without any, it tries gl3 and gpu.
set -u
[ $# -ge 2 ] || {
	echo "usage: run-all.sh EXE OUT [NAME:ARGS]..." >&2
	exit 2
}
exe=$1
out=$2
shift 2
[ $# -gt 0 ] || set -- "gl3:--backend gl3" "gpu:--backend gpu"
mkdir -p "$out"

summary="$out/summary.md"
{
	echo "| Backend | Driver | Features | Benchmark: ms per frame (median, p95, CPU median) |"
	echo "|---|---|---|---|"
} > "$summary"

for entry in "$@"; do
	name=${entry%%:*}
	args=${entry#*:}
	log="$out/$name.log"
	: > "$log"
	features="failed"
	# shellcheck disable=SC2086 # args are words on purpose
	if "$exe" $args --screenshot "$out/$name-features-1x.png" >> "$log" 2>&1; then
		features="ok"
		"$exe" $args --scale 2 --screenshot "$out/$name-features-2x.png" >> "$log" 2>&1 || features="1x only"
		"$exe" $args --document benchmark --screenshot "$out/$name-benchmark.png" >> "$log" 2>&1
	fi
	benchmark="-"
	if [ "$features" != "failed" ] && "$exe" $args --document benchmark --benchmark 300 >> "$log" 2>&1; then
		benchmark=$(sed -n 's/.*"msPerFrame": \([0-9.]*\), "medianMs": \([0-9.]*\), "p95Ms": \([0-9.]*\), "cpuMedianMs": \([0-9.]*\).*/\1 (\2, \3, \4)/p' "$log")
	fi
	driver=$(sed -n 's/.*"driver": "\([^"]*\)".*/\1/p' "$log" | head -n 1)
	[ -n "$driver" ] || driver=$(grep -v '^$' "$log" | tail -n 1 | cut -c 1-160)
	echo "| $name | $driver | $features | $benchmark |" >> "$summary"
done
cat "$summary"
