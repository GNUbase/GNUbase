// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

#include <memory>

namespace gnubase::audio
{
	// CoreAudio, macOS's audio API (backend "coreaudio"). A device is a HAL device with its inputs and outputs on one
	// clock, aggregate devices included, which combine several devices into one in Audio MIDI Setup. Its id is the
	// device's UID; an empty id opens the default output device. Opening sets the device's sample rate and the
	// process's I/O buffer size; closing sets the sample rate it had back. Each I/O cycle is processed in blocks of at
	// most the block size, so any block size works; by default a block is an I/O cycle. The device asks for a reset
	// when it goes away, its channels change or another program changes its sample rate. Its audio thread's workgroup
	// is there for the engine's worker threads to join (Device::workgroup).
	std::unique_ptr<Backend> makeCoreAudioBackend();
}
