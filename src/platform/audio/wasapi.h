// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

#include <memory>

namespace gnubase::audio
{
	// WASAPI, Windows' own audio API. In shared mode (backend "wasapi") Windows mixes GNUbase with other programs and
	// converts the sample rate; in exclusive mode ("wasapi-exclusive") GNUbase has the device to itself, at its own
	// formats and with less latency.
	//
	// A device is an output endpoint, optionally with an input endpoint: "default" is the default output with the
	// default input, an output endpoint's id is that output alone, and "<output id>|<input id>" pairs the two. The
	// output's clock drives processing; the input follows it through an InputFifo, which compensates their drift. Block
	// sizes are free: each period of the output is processed in blocks of at most the block size; by default a block is
	// a period. The audio thread runs under MMCSS ("Pro Audio").
	//
	// The device asks for a reset, from a thread of its own, when one of its endpoints goes away, fails or hangs, when
	// in shared mode the format Windows mixes one of them at changes, and for "default" when the default endpoints
	// change.
	std::unique_ptr<Backend> makeWasapiBackend(bool exclusive);
}
