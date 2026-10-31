// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

#include <memory>
#include <string>

namespace gnubase::audio
{
	// JACK (backend "jack"): a running JACK server, jackd2's or PipeWire's JACK layer. GNUbase never starts one. Its
	// one device, "default", is a JACK client named "gnubase" with an input port per physical capture port and an
	// output port per physical playback port, which it connects to them while it runs. The server's sample rate is the
	// only rate; opening at another block size asks the server for it, and closing gives the server its size back. Each
	// cycle is processed in blocks of at most the block size, whatever the server's buffer size is. The device asks
	// for a reset when the server goes away, changes its sample rate or buffer size, a physical port goes away or the
	// latency changes, and counts the server's xruns as overloads.
	//
	// libjack is loaded when the backend is first used, so GNUbase runs where it isn't installed; library is the one to
	// load, e.g. PipeWire's.
	std::unique_ptr<Backend> makeJackBackend(std::string library = "libjack.so.0");
}
