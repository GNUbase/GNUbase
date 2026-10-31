// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"
#include "platform/audio/device.h"

#include <cstdint>
#include <memory>
#include <vector>

// Checks what every audio backend promises, for each of its devices (tests/device_tests.cpp): every device listed as
// available opens and starts. Devices listed as unavailable are skipped with a warning, as real hardware may be
// unplugged. With openFirst, every device is opened and started once before, since some backends learn only that way
// that a device is unavailable (ASIO).
void checkDeviceContract(gnubase::audio::Backend& backend, bool openFirst = false);

// A loopback to play a project through and record it back: a cable from an output of a device to one of its inputs, or
// a link inside a sound server's graph.
struct Loopback
{
	gnubase::engine::Json device; // audio.setDevice's parameters: backend, device and any others
	int output = 0;               // the device's channels, from 0
	int input = 0;
	// Frames the recording may lie off where it played, for converters and drivers that report their latency a few
	// frames wrong.
	int64_t tolerance = 0;
	// A link inside the server's graph: what an output plays comes back a buffer later (the device's block size),
	// whatever latency the server reports for the ports at the graph's ends.
	bool inGraph = false;
};

// Plays a burst of noise from an audio track on the loopback's output and records the input, through the engine API
// only, as a project would, and checks that the recording matches the burst in shape and level (within 6 dB) where it
// played, give or take the tolerance. Failures name the backend, the device and how far off the recording was. Returns
// how many frames later than where it played it was recorded, which recordShift in audio.setDevice corrects.
int64_t checkRoundTrip(std::vector<std::unique_ptr<gnubase::audio::Backend>> backends, const Loopback& loopback);
