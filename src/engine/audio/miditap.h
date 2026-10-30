// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/sequencer.h"
#include "engine/document/document.h"
#include "platform/audio/device.h"
#include "platform/rt/spscqueue.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	// MIDI for tests and remote control, at frame positions on the audio device: messages injected into MIDI and
	// instrument tracks (midi.inject), which play them as if they came from their MIDI input, and what instruments
	// played (midi.capture). The command thread injects and takes; the audio thread delivers and captures.
	class MidiTap
	{
	public:
		// A MIDI 1.0 channel message for a track's channel (injected) or from an instrument's channel (captured).
		struct Message
		{
			ObjectId channel = 0;
			uint64_t position = 0;
			audio::MidiMessage message; // its offset is ignored
		};

		using Sequencers = std::vector<std::pair<ObjectId, std::shared_ptr<MidiSequencer>>>;      // by channel id
		using Instruments = std::vector<std::pair<ObjectId, std::shared_ptr<InstrumentChannel>>>; // by channel id

		// Messages that can wait for the audio thread, and captured messages kept between two takes.
		static constexpr size_t g_size = 4096;

		MidiTap();

		// Command thread. False if too many messages wait for the audio thread already.
		bool inject(const Message& message) { return m_injected.tryPush(message); }

		// Command thread: what instruments played since the last call, oldest first, and how many messages didn't
		// fit.
		std::vector<Message> take(size_t& dropped);

		// While no audio thread runs: drops the injected messages that wait, e.g. for a device that starts again at
		// position 0.
		void clear();

		// Audio thread, before the graph processes the block at position: gives each sequencer the injected messages
		// for it, at their offsets; messages for positions that have passed play at the block's start, those for
		// channels that are gone are dropped. A sequencer whose input is full gets the rest in the next block.
		void deliver(uint64_t position, uint32_t frames, const Sequencers& sequencers) noexcept;

		// Audio thread, after the graph processed the block at position: keeps what each instrument got.
		void capture(uint64_t position, const Instruments& instruments) noexcept;

	private:
		rt::SpscQueue<Message> m_injected{g_size};
		std::vector<Message> m_waiting; // audio thread: injected messages for later blocks, in the order they came
		size_t m_waitingCount = 0;
		rt::SpscQueue<Message> m_captured{g_size};
		std::atomic<size_t> m_dropped{0};
	};
}
