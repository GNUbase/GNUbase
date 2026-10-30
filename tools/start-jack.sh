#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts a JACK server (jackd2) with its dummy driver at 48 kHz and 256 frames, for the JACK tests in
# tests/device_tests.cpp on machines without audio hardware, such as CI runners. Needs the jackd2 package.
# Usage: tools/start-jack.sh
set -euo pipefail

log=${TMPDIR:-/tmp}/gnubase-jackd.log
setsid nohup jackd --no-realtime -d dummy -r 48000 -p 256 > "$log" 2>&1 &
if ! jack_wait --wait --timeout 10 > /dev/null 2>&1; then
	echo "jackd didn't start:" >&2
	cat "$log" >&2
	exit 1
fi
