// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/rendersnapshot.h"
#include "engine/schema.h"
#include "platform/audio/device.h"
#include "platform/hosttime.h"
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
	class CommandContext;
	class Engine;

	namespace settings
	{
		class Store;
	}

	// The ports of every MIDI backend, with their settings: the name they are shown as, whether they are visible, and
	// whether an input belongs to "all MIDI inputs". The settings are the user setting midi.portSettings, so they last
	// across restarts and wait for ports that aren't connected. The first midi command lists the ports and opens the
	// visible inputs, so an engine that never uses MIDI leaves the hardware alone. Their messages reach the audio
	// thread, which places them in its blocks. A port that disappears is listed as not present and opens again when it
	// comes back. Only the command thread changes ports; outputs open once something sends to them.
	class MidiPorts
	{
	public:
		// The user settings are the engine's; only the command thread reads them.
		MidiPorts(const settings::Store& settings, std::vector<std::unique_ptr<gnubase::midi::Backend>> backends);
		~MidiPorts(); // closes the ports
		MidiPorts(const MidiPorts&) = delete;
		MidiPorts& operator=(const MidiPorts&) = delete;

		// Registers midi.ports, midi.setPort, midi.refresh and midi.monitor. When a backend's ports change, the engine
		// refreshes them on its own.
		void registerCommands(Engine& engine);

		// Lists the ports again, opens new and returning visible inputs, closes the others. True if anything changed.
		bool refresh();

		// After a command changed midi.portSettings: the listed ports follow their settings, opening or closing inputs,
		// without listing the hardware again, and the command emits them as midi.changed. A port that appeared
		// meanwhile waits for the midi.refresh its backend posted, which reports it; this never adds ports, so
		// midi.setPort keeps its port across the change. Nothing while unlisted.
		void applySettings(CommandContext& context);

		// Audio thread, once per block: the messages that arrived for this block go into allInputs if their port
		// belongs to all MIDI inputs, and to the monitor (midi.monitor) at their frame position. While the chord pads'
		// remote is active, a port's note on a remote key plays the pad's pitches into pads instead (#174), the
		// highest on MIDI channel 1, the next on channel 2 and so on, over the chords that sound as padKeys' overlap
		// says (midi::PadVoices); its note-off ends them, or with latch its next note-on. With filter, the remote
		// keys without a chord play nothing (#1089). pads also gets what playPads plays.
		void take(const audio::DeviceBlock& block, double sampleRate, audio::MidiBuffer& allInputs,
			audio::MidiBuffer& pads, const ChordPadKeys& padKeys) noexcept;

		// Any thread but an audio thread: a message from the on-screen keyboard (midi.keyboard), which belongs to all
		// MIDI inputs and arrives now, as a port's would. False if the input queue is full.
		bool play(const audio::MidiMessage& message) { return m_queue.push(g_keyboardSlot, message, hostTime()); }
		// The same for the chord pads' notes (chordPads.trigger), which go where the chord pads play (take's pads).
		bool playPads(const audio::MidiMessage& message) { return m_queue.push(g_padsSlot, message, hostTime()); }

		// What the chord pads' remote keys did on the audio thread (#1089), for the command thread to apply
		// (chordPads.remoteInput): a key that triggered or released its pad, a chord modifier's key, and while
		// listenForPads is on, every port's note.
		struct PadEvent
		{
			enum class Kind : uint8_t
			{
				Triggered,
				Released,
				Modifier,
				Note,
			};
			Kind kind = Kind::Note;
			uint8_t key = 0;
			uint8_t velocity = 0; // 0 for a note-off
		};
		// One thread, the audio engine's cue thread: the next event the audio thread found. False if there is none.
		bool takePadEvent(PadEvent& event) { return m_padEvents.tryPop(event); }
		// Command thread: while on, every port's note goes to takePadEvent as it plays on, and no remote key triggers
		// a pad (Assign from MIDI Input, MIDI Learn: chordPads.listen).
		void listenForPads(bool on) { m_padListen.store(on, std::memory_order_relaxed); }

		// Drops the input that waits for the audio thread. Only while no audio thread takes.
		void clear() { m_queue.clear(); }

		// Inputs that can be open at once.
		static constexpr size_t g_maxInputs = 256;

	private:
		static constexpr int g_keyboardSlot = -1; // the on-screen keyboard's in the input queue
		static constexpr int g_padsSlot = -2;     // the chord pads'
		struct Port
		{
			gnubase::midi::Backend* backend = nullptr;
			gnubase::midi::PortInfo info;
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

		// A port's message on a chord pad's remote key goes into pads as the pad's notes, or nowhere for a remote key
		// that Filter Notes keeps; false for others.
		bool playPad(const audio::MidiMessage& message, const ChordPadKeys& keys, audio::MidiBuffer& pads) noexcept;
		void open(Port& port);
		void listOnce();
		bool listHardware();   // adds new ports, marks the others present or not; true if that changed any
		bool followSettings(); // opens and closes inputs, sets the all-inputs flags; true if an input opened or closed
		size_t find(const Json& parameters) const;             // the port's index in m_ports
		Json setPort(Engine& engine, CommandContext& context); // midi.setPort
		static std::string key(const Port& port);              // in midi.portSettings: backend/in or out/id
		Json setting(const Port& port) const;                  // its entry there; empty while it has the defaults
		bool visible(const Port& port) const { return setting(port).value("visible", true); }
		bool inAllInputs(const Port& port) const { return port.info.input && setting(port).value("inAllInputs", true); }
		Json toJson(const Port& port) const;
		Json list() const;

		const settings::Store& m_settings;
		std::vector<std::unique_ptr<gnubase::midi::Backend>> m_backends;
		std::vector<Port> m_ports;
		bool m_listed = false;
		int m_nextSlot = 0;
		gnubase::midi::InputQueue m_queue;
		std::array<std::atomic<bool>, g_maxInputs> m_allInputs{};
		rt::SpscQueue<Received> m_monitor;
		// Audio thread: the pitches each remote key's note-on played, until its note-off.
		midi::PadVoices m_padVoices;
		rt::SpscQueue<PadEvent> m_padEvents{512};
		std::atomic<bool> m_padListen{false};
		// Per input slot, chordPadPortHash of its port's key, for the MIDI input that the remote keys come from.
		std::array<std::atomic<uint64_t>, g_maxInputs> m_slotHashes{};
		std::atomic<size_t> m_monitorDropped{0};
	};
}
