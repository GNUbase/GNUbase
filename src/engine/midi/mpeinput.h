// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/events.h"
#include "engine/schema.h"

#include <array>
#include <bitset>
#include <cstdint>

namespace gnubase::engine::midi
{
	// How a MIDI input port's MPE input is set up (#1514, reference manual pp. 1295-1298,
	// docs/features/note-expression.md): its entry's mpe in the user setting midi.portSettings, which midi.setPort
	// changes and midi.ports lists.
	struct MpeSetup
	{
		// Multi-Channel Input Mode: off, MPE's lower zone (master channel 1, the notes on 2 and up), its upper zone
		// (master channel 16, the notes on 15 and down) or channel rotation (the notes on the used channels, no
		// master).
		enum class Mode : uint8_t
		{
			Off,
			Lower,
			Upper,
			Rotation,
		};
		// Send Initial Value: what each note's expression starts at. Off: the channel's latest value, what the device
		// sent before the note-on (absolute input); the others a fixed value (relative input).
		enum class Initial : uint8_t
		{
			Off,
			Min,
			Center,
			Max,
		};

		Mode mode = Mode::Off;
		int channels = 15;    // the notes' channels: a zone's member channels (at most 15) or the used channels
		int firstChannel = 1; // channel rotation's first channel, 1-16; the used ones end at 16
		int pitchRange = 48;  // of the notes' pitch bend, in semitones, 1-96
		bool tuning = true;   // Use for Tuning: the notes' pitch bend is their tuning, else it stays pitch bend
		Initial initialX = Initial::Off; // Horizontal/X, pitch bend: off or center (no bend)
		Initial initialY = Initial::Off; // Vertical/Y, CC 74

		// From an mpe value as midi.setPort and midi.portSettings have it; what it lacks keeps its default.
		static MpeSetup fromJson(const Json& json);
		Json toJson() const;
		// In 32 bits, for the audio thread, and back.
		uint32_t pack() const;
		static MpeSetup unpack(uint32_t packed);
		// Whether the notes on this MIDI channel, 0-15, have it to themselves: their pitch bend, channel pressure and
		// CC 74 are their expression.
		bool notesChannel(uint8_t channel) const;
		bool operator==(const MpeSetup& other) const { return pack() == other.pack(); }
	};

	// midi.setPort's mpe parameter.
	Json mpeSchema();

	// Turns one port's MPE input into per-note expression, on the audio thread: on the notes' channels, pitch bend
	// (with Use for Tuning), channel pressure and CC 74 are NoteExpression events of the notes that sound there, their
	// tuning by the pitch range, pressure and brightness; without a note they only set the channel's state. After a
	// note-on come the note's initial values, each the channel's latest one or the fixed one Send Initial Value gives.
	// Everything else passes as it is. No allocation; the state is the port's channels.
	class MpeInput
	{
	public:
		// Adds what event plays to out, at its offset.
		void process(const MpeSetup& setup, const graph::ProcessEvent& event, graph::EventBuffer& out) noexcept;
		// Forgets the notes and values, e.g. when the setup changed.
		void reset() noexcept { m_channels = {}; }

	private:
		struct Channel
		{
			std::bitset<128> sounding;
			std::array<double, 3> values{}; // tuning, pressure, brightness
			std::bitset<3> known;           // which of values the channel has had
		};
		std::array<Channel, 16> m_channels{};
	};
}
