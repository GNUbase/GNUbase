#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts a headless PipeWire daemon with WirePlumber, a null sink and a null source, for the PipeWire tests in
# tests/device_tests.cpp on machines without a sound server, such as CI runners. Needs the pipewire, pipewire-bin and
# wireplumber packages. Prints the XDG_RUNTIME_DIR the daemon listens in, as NAME=value for $GITHUB_ENV.
# Usage: tools/start-pipewire.sh >> "$GITHUB_ENV"
set -euo pipefail

runtime=${XDG_RUNTIME_DIR:-$(mktemp -d)}
export XDG_RUNTIME_DIR=$runtime
mkdir -p "$runtime"
chmod 700 "$runtime"
log=${TMPDIR:-/tmp}/gnubase-pipewire.log

waitFor() { # a command, tried for up to ten seconds
	for _ in $(seq 100); do
		"$@" > /dev/null 2>&1 && return
		sleep 0.1
	done
	echo "PipeWire didn't start, see $log" >&2
	exit 1
}

# Without a session bus, WirePlumber gets one of its own.
session=()
[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] || session=(dbus-run-session --)
setsid nohup pipewire > "$log" 2>&1 &
waitFor pw-cli info 0
setsid nohup "${session[@]}" wireplumber >> "$log" 2>&1 &

for node in "gnubase-test-sink Audio/Sink" "gnubase-test-source Audio/Source/Virtual"; do
	set -- $node
	pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=$1 media.class=$2
		audio.position=[ FL FR ] object.linger=true }" > /dev/null
done
waitFor sh -c 'pw-metadata 0 default.audio.sink | grep -q gnubase-test-sink'
waitFor sh -c 'pw-metadata 0 default.audio.source | grep -q gnubase-test-source'
echo "XDG_RUNTIME_DIR=$runtime"
