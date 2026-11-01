// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

#include <atomic>
#include <cstdint>
#include <memory>

namespace gnubase::audio
{
	// Finds the dropouts of the graph's driver in its clock: a driver adds up how long its xruns lasted
	// (spa_io_clock::xrun, which ALSA's drivers do), so every increase is one. The first cycle after restart() and a
	// clock of another driver only set the baseline.
	class XrunCounter
	{
	public:
		// Before the node runs again. Any thread.
		void restart() noexcept { m_known.store(false, std::memory_order_release); }

		// Every cycle, with the driver's clock id and xrun duration. True if the driver dropped out since the last
		// cycle. The audio thread.
		bool cycle(uint32_t clock, uint64_t xrun) noexcept
		{
			const bool dropped = m_known.exchange(true, std::memory_order_acq_rel) && clock == m_clock && xrun > m_xrun;
			m_clock = clock;
			m_xrun = xrun;
			return dropped;
		}

	private:
		std::atomic<bool> m_known{false};
		uint32_t m_clock = 0;
		uint64_t m_xrun = 0;
	};

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
