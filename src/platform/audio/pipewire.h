// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

#include <memory>

namespace gnubase::audio
{
	// PipeWire, Linux's audio server (backend "pipewire"). GNUbase joins the graph as one filter node with an input
	// port per input channel and an output port per output channel, and links them to a sink's playback ports and a
	// source's capture ports itself.
	//
	// A device is a sink, optionally with a source: "default" is the default sink with the default source, a sink's
	// node name is that sink alone, and "<sink name>|<source name>" pairs the two. The graph's driver runs the node on
	// PipeWire's real-time thread. The node asks the graph for the device's sample rate (one of the allowed rates) and
	// for its block size as quantum; each cycle is processed in blocks of at most the block size, whatever quantum the
	// graph settles on. The device asks for a reset when its sink or source goes away, when the daemon goes away and,
	// for "default", when the default sink or source changes.
	std::unique_ptr<Backend> makePipeWireBackend();
}
