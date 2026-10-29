// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"
#include "platform/audio/device.h"
#include "platform/midi/inputqueue.h"
#include "platform/midi/port.h"
#include "platform/rt/spscqueue.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::engine
{
	class Engine;

	// The ports of every MIDI backend, with their settings: the name they are shown as, whether they are visible, and
	// whether an input belongs to "all MIDI inputs". The first midi command lists the ports and opens the visible
	// inputs, so an engine that never uses MIDI leaves the hardware alone. Their messages reach the audio thread, which
	// places them in its blocks. A port that disappears keeps its settings and opens again when it comes back. Only the
	// command thread changes ports; outputs open once something sends to them.
	class MidiPorts
	{
	public:
		explicit MidiPorts(std::vector<std::unique_ptr<gnubase::midi::Backend>> backends);
		~MidiPorts(); // closes the ports
		MidiPorts(const MidiPorts&) = delete;
		MidiPorts& operator=(const MidiPorts&) = delete;

		// Registers midi.ports, midi.setPort, midi.refresh and midi.monitor. When a backend's ports change, the engine
		// refreshes them on its own.
		void registerCommands(Engine& engine);

		// Lists the ports again, opens new and returning visible inputs, closes the others. True if anything changed.
		bool refresh();

		// Changes a port's settings as midi.setPort does and returns the port. Lists the ports first if they never
		// were.
		Json setPort(const Json& parameters);

		// Audio thread, once per block: the messages that arrived for this block go into allInputs if their port
		// belongs to all MIDI inputs, and to the monitor (midi.monitor) at their frame position.
		void take(const audio::DeviceBlock& block, double sampleRate, audio::MidiBuffer& allInputs) noexcept;

		// Drops the input that waits for the audio thread. Only while no audio thread takes.
		void clear() { m_queue.clear(); }

		// Inputs that can be open at once.
		static constexpr size_t g_maxInputs = 256;

	private:
		struct Port
		{
			gnubase::midi::Backend* backend = nullptr;
			gnubase::midi::PortInfo info;
			std::string showAs; // empty: the system name
			bool visible = true;
			bool inAllInputs = true;
			bool present = true;
			int slot = -1; // an input's index in m_allInputs and in the input queue
			std::unique_ptr<gnubase::midi::Input> input;
		};

		// What the audio thread received, for the monitor.
		struct Received
		{
			int slot = 0;
			uint64_t position = 0;
			audio::MidiMessage message;
		};

		void open(Port& port);
		void listOnce();
		Port& find(const Json& parameters);
		static Json toJson(const Port& port);
		Json list() const;

		std::vector<std::unique_ptr<gnubase::midi::Backend>> m_backends;
		std::vector<Port> m_ports;
		bool m_listed = false;
		int m_nextSlot = 0;
		gnubase::midi::InputQueue m_queue;
		std::array<std::atomic<bool>, g_maxInputs> m_allInputs{};
		rt::SpscQueue<Received> m_monitor;
		std::atomic<size_t> m_monitorDropped{0};
	};
}
