// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/midi/logical.h"
#include "engine/processors/builtin.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

// GNUbase's built-in MIDI effects (#135): processors that change the events going through them and add new ones,
// for the MIDI insert and send slots of MIDI and instrument tracks (midi/effects.h). They run inside the engine, on
// the track's sequencer (docs/features/midi-effects.md).
namespace gnubase::engine::processors
{
	// A MIDI effect: it processes events only and leaves the audio as it is. Each one ends the notes it started when
	// the notes that made it start them end, so a track that ends its notes leaves none of the effect's hanging.
	class MidiEffect : public graph::Processor
	{
	public:
		static constexpr size_t g_notes = size_t{16} * 128; // a note per MIDI channel and pitch

		// Audio thread: sets a parameter, as a parameter event would; values beyond its range are clamped.
		virtual void setParameter(uint32_t id, double value) noexcept = 0;

		// Takes parameter events at their offsets, hands the other events to handle in order and calls advance
		// before each one, and at the end of the block, with the offset it has reached.
		void process(const graph::ProcessContext& context) noexcept override;

	protected:
		// Off the audio thread: id names the effect in midiEffects(), whose parameter ranges setParameter uses.
		explicit MidiEffect(const std::string& id);

		// A parameter's value clamped to its range and, if stepped, rounded; the default if it isn't finite.
		double clamped(uint32_t id, double value) const noexcept;

		virtual void handle(const graph::ProcessEvent& event, const graph::ProcessContext& context) noexcept = 0;
		virtual void advance(uint32_t /*to*/, const graph::ProcessContext& /*context*/) noexcept {}
		virtual void endBlock(uint32_t /*frames*/) noexcept {}

	private:
		const BuiltinInfo* m_info = nullptr;
	};

	// Plays the notes held one after another at a note value of the tempo, from the first note-on on, over one to
	// four octaves: up, down, up and down (without playing the top and bottom notes twice) or in the order played.
	// Each note lasts the gate's share of a step and has the velocity of the note it comes from. Other events pass.
	class Arpeggiator final : public MidiEffect
	{
	public:
		enum Parameter : uint8_t
		{
			Mode,    // Up, Down, UpDown, AsPlayed
			Rate,    // 1/4, 1/8, 1/16, 1/32
			Gate,    // % of a step
			Octaves, // 1 to 4
		};

		enum Modes : uint8_t
		{
			Up,
			Down,
			UpDown,
			AsPlayed,
		};

		static constexpr size_t g_held = 16; // notes held at a time; further ones are left out

		Arpeggiator();
		void setParameter(uint32_t id, double value) noexcept override;
		void reset() noexcept override;

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;
		void handle(const graph::ProcessEvent& event, const graph::ProcessContext& context) noexcept override;
		void advance(uint32_t to, const graph::ProcessContext& context) noexcept override;
		void endBlock(uint32_t frames) noexcept override { m_time += frames; }

	private:
		struct Note
		{
			uint8_t channel = 0;
			uint8_t key = 0;
			uint16_t velocity = 0;
		};

		// Both return false when the output is full and they must be called again; a step whose note-on doesn't fit
		// is left out.
		bool step(uint32_t offset, double frames, graph::EventBuffer& out) noexcept;
		bool end(uint32_t offset, graph::EventBuffer& out) noexcept;
		// The frames of a step that starts at offset in the block, at the tempo there.
		double stepFrames(uint32_t offset, const graph::TransportBlock& transport) const noexcept;

		double m_sampleRate = 48000.0;
		uint8_t m_mode = Up;
		uint8_t m_rate = 2;
		double m_gate = 50.0;
		int m_octaves = 1;
		std::array<Note, g_held> m_held{}; // in the order they were played
		size_t m_heldCount = 0;
		int64_t m_time = 0; // frames before this block
		bool m_running = false;
		double m_next = 0.0; // the frame of the next step
		uint64_t m_step = 0; // steps since the first note-on
		bool m_sounding = false;
		Note m_playing;
		int64_t m_end = std::numeric_limits<int64_t>::max(); // the frame its note ends
	};

	// Plays a chord for every note: the note and the intervals of the chord chosen above it. Where the chords of two
	// notes share a pitch, it plays once and ends with the last of them. Other events pass.
	class ChordGenerator final : public MidiEffect
	{
	public:
		enum Parameter : uint8_t
		{
			Chord,
		};

		ChordGenerator();
		void setParameter(uint32_t id, double value) noexcept override;
		void reset() noexcept override;

	protected:
		void handle(const graph::ProcessEvent& event, const graph::ProcessContext& context) noexcept override;

	private:
		static constexpr uint8_t g_none = 0xFF;

		void release(uint16_t index, const graph::ProcessEvent& event, graph::EventBuffer& out) noexcept;

		uint8_t m_chord = 0;
		std::array<uint8_t, g_notes> m_chordOf{};   // per note played, the chord it started, or g_none
		std::array<uint16_t, g_notes> m_sounding{}; // per note out, how many chords hold it
	};

	// Rules to apply to events (midi/logical.h), as views of their lines, so that they can be made on the audio thread.
	struct LogicalRulesView
	{
		const midi::FilterLine* filters = nullptr;
		size_t filterCount = 0;
		midi::LogicalFunction function = midi::LogicalFunction::Transform;
		const midi::ActionLine* actions = nullptr;
		size_t actionCount = 0;
	};

	// Applies rules to events one at a time, as the transformer effect and each module of an input transformer do. A
	// note-off goes where the note-on went, so a note changed by the rules ends where it sounds, and so does the poly
	// pressure of a held note unless the rules find it; a note-on that became another type of event loses its note-off.
	// Where two held notes play one note, it plays once and ends with the last of them. In a full output, note-offs
	// take the places of other events.
	// Velocities stay from 1 to 127, and a controller turned into a note plays a note-on for values above 0 and a
	// note-off for 0. SysEx passes.
	class LogicalStage
	{
	public:
		LogicalStage();
		void reset() noexcept;
		// Audio thread: adds what event becomes to out.
		void handle(const graph::ProcessEvent& event, const LogicalRulesView& rules, graph::EventBuffer& out) noexcept;

	private:
		static constexpr uint16_t g_none = 0xFFFF;

		struct Held
		{
			uint16_t to = g_none; // where the note-on went, as channel * 128 + pitch, besides itself if it passed
			bool passed = false;  // the note-on itself passed
			bool down = false;    // a note-on came and no note-off yet
		};

		// Whether held, a note-on of source, plays note; source's note-on itself counts if it passed.
		static bool uses(const Held& held, uint16_t source, uint16_t note) noexcept;
		// Counts a note-on of note less; true when none is left and the note ends.
		bool release(uint16_t note) noexcept;

		std::array<Held, MidiEffect::g_notes> m_held{};
		std::array<uint16_t, MidiEffect::g_notes> m_sounding{}; // per note out, how many held notes play it
		midi::LogicalRandom m_random;
	};

	// A transformer-style effect: the notes that meet its condition (all, or those with a pitch, velocity or MIDI
	// channel from 1 to 16 from the minimum to the maximum) are transposed, get a velocity set or added, move to
	// another channel, or are deleted, by the value. It is a rule of the rule editor's kind with a fixed shape and
	// plays like an input transformer module (LogicalStage). Other events pass.
	class Transformer final : public MidiEffect
	{
	public:
		enum Parameter : uint8_t
		{
			Condition, // AllNotes, Pitch, Velocity, Channel
			Minimum,
			Maximum,
			Action, // Transpose, SetVelocity, AddVelocity, SetChannel, Delete
			Value,  // -127 to 127
		};

		enum Conditions : uint8_t
		{
			AllNotes,
			Pitch,
			Velocity,
			Channel,
		};

		enum Actions : uint8_t
		{
			Transpose,
			SetVelocity,
			AddVelocity,
			SetChannel,
			Delete,
		};

		Transformer();
		void setParameter(uint32_t id, double value) noexcept override;
		void reset() noexcept override;

	protected:
		void handle(const graph::ProcessEvent& event, const graph::ProcessContext& context) noexcept override;

	private:
		// Makes the rule of the parameters.
		void update() noexcept;

		uint8_t m_condition = AllNotes;
		int m_minimum = 0;
		int m_maximum = 127;
		uint8_t m_action = Transpose;
		int m_value = 0;
		std::array<midi::FilterLine, 2> m_filters{};
		midi::ActionLine m_change;
		LogicalRulesView m_rules;
		LogicalStage m_stage;
	};

	// A MIDI or instrument track's input transformer (midi::InputTransformer): applies the rules of its modules that
	// are on, in order, to what the track's MIDI input plays, before the track plays and records it. It stays with the
	// track while the rules change, so the notes they changed end where they sound.
	class InputTransformerProcessor
	{
	public:
		// Off the audio thread: capacity is that of the track's input.
		explicit InputTransformerProcessor(size_t capacity);

		// Audio thread: changes the events in place. Stages without rules pass events on and still end the notes
		// they changed.
		void process(graph::EventBuffer& events, const std::vector<midi::LogicalRules>& modules) noexcept;

	private:
		std::array<LogicalStage, midi::g_inputTransformerModules> m_stages;
		graph::EventBuffer m_first;
		graph::EventBuffer m_second;
	};

	// Every built-in MIDI effect, sorted by id. Their parameter ids count from 0.
	const std::vector<BuiltinInfo>& midiEffects();

	// The built-in MIDI effect with this id, or nullptr.
	const BuiltinInfo* findMidiEffect(const std::string& id);

	// A MIDI effect's parameter's key in a slot's parameters (channel.set midiInserts): its name, starting in lower
	// case.
	std::string parameterKey(const ParameterInfo& parameter);

	// A new instance of a built-in MIDI effect with its parameters at their defaults. Throws std::invalid_argument for
	// an unknown id.
	std::shared_ptr<MidiEffect> createMidiEffect(const std::string& id);
}
