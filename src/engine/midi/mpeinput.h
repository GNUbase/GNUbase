// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/events.h"
#include "engine/schema.h"
#include "plugin/events.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <optional>
#include <string>

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
		// The three dimensions of the device's input movements, the page's sections, by their index in messages and
		// expressions: Horizontal/X, Pressure and Vertical/Y (#1674, pp. 1296-1298).
		enum Dimension : uint8_t
		{
			X,
			Pressure,
			Y,
		};
		// A dimension's MIDI Message: a controller's number (0-127), pitch bend or channel pressure (aftertouch).
		static constexpr uint8_t g_pitchBend = 128;
		static constexpr uint8_t g_aftertouch = 129;

		Mode mode = Mode::Off;
		int channels = 15;    // the notes' channels: a zone's member channels (at most 15) or the used channels (16)
		int firstChannel = 1; // channel rotation's first channel, 1-16; the used ones end at 16
		int pitchRange = 48;  // of the notes' pitch bend, in semitones, 1-96; tuning's range from any message
		bool tuning = true;   // Use for Tuning: X plays the notes' tuning over its own assignment, else that
		Initial initialX = Initial::Off; // Horizontal/X: off or center
		Initial initialY = Initial::Off; // Vertical/Y
		// Per dimension, the MIDI message it comes from and the note expression it plays (Note Expression
		// Assignment); none passes the message as it is. X's is its own, which it keeps while Use for Tuning
		// overrides it (#1749), and never tuning.
		std::array<uint8_t, 3> messages = {g_pitchBend, g_aftertouch, 74};
		std::array<std::optional<plugin::NoteExpression>, 3> expressions = {
			std::nullopt, plugin::NoteExpression::Pressure, plugin::NoteExpression::Brightness};

		// From an mpe value as midi.setPort and midi.portSettings have it; what it lacks keeps its default (or
		// defaults'), and a zone's channels are at most 15. X's own assignment tuning, which Use for Tuning is, is
		// none.
		static MpeSetup fromJson(const Json& json, const MpeSetup& defaults);
		static MpeSetup fromJson(const Json& json) { return fromJson(json, MpeSetup()); }
		Json toJson() const;
		// In 64 bits, for the audio thread, and back.
		uint64_t pack() const;
		static MpeSetup unpack(uint64_t packed);
		// Whether the notes on this MIDI channel, 0-15, have it to themselves: their messages of the dimensions are
		// their expression.
		bool notesChannel(uint8_t channel) const;
		// The note expression the dimension plays, by Use for Tuning for X.
		std::optional<plugin::NoteExpression> expression(size_t dimension) const;
		bool operator==(const MpeSetup& other) const { return pack() == other.pack(); }
	};

	// A known MPE input device, which a MIDI input whose name contains match is detected as: its page in the studio
	// setup has its name, and the input starts with its preset settings, the device's factory settings (p. 1295).
	struct MpeDevice
	{
		const char* name = "";
		const char* match = ""; // lower case
		MpeSetup setup;
	};
	// The device of a MIDI input of this name, or null.
	const MpeDevice* detectMpeDevice(const std::string& portName);

	// midi.setPort's mpe parameter.
	Json mpeSchema();

	// A MIDI Message (MpeSetup::messages) as JSON names it, pitchBend, aftertouch or cc0 to cc127, and back: none for
	// any other name.
	std::string messageName(uint8_t message);
	std::optional<uint8_t> messageNamed(const std::string& name);

	// Turns one port's MPE input into per-note expression, on the audio thread: on the notes' channels, the messages of
	// the dimensions (by default pitch bend with Use for Tuning, channel pressure and CC 74) are NoteExpression events
	// of the notes that sound there, the expressions assigned to them (tuning by the pitch range, pressure and
	// brightness); without a note they only set the channel's state. A released
	// note (or one the sustain pedal holds) keeps getting them while it fades, until the channel's next note-on, as MPE
	// has it. After a note-on come the note's initial values, each the channel's latest one or the fixed one Send
	// Initial Value gives, which the channel's other sounding notes then get too. Everything else passes as it is. No
	// allocation; the state is the port's channels.
	class MpeInput
	{
	public:
		// Adds what event plays to out, at its offset.
		void process(const MpeSetup& setup, const graph::ProcessEvent& event, graph::EventBuffer& out) noexcept;
		// How many events process adds for event at most, for a caller that checks for room first.
		size_t maxEvents(const graph::ProcessEvent& event) const noexcept;
		// Forgets the notes and values, e.g. when the setup changed.
		void reset() noexcept { m_channels = {}; }

	private:
		struct Channel
		{
			std::bitset<128> sounding;
			std::bitset<128> released; // since the channel's last note-on: their release still has its expression
			std::array<uint32_t, 3> values{}; // the dimensions' latest messages' values (MpeSetup::Dimension)
			std::bitset<3> known;             // which of values the channel has had
		};
		std::array<Channel, 16> m_channels{};
	};
}
