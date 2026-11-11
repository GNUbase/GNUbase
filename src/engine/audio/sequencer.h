// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/rendersnapshot.h"
#include "engine/graph/processor.h"
#include "engine/midi/effects.h"
#include "engine/processors/midieffects.h"
#include "platform/rt/snapshotpublisher.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::engine
{
	// The events of one kind, in time order, for chasing: a run of an index's events, or of an order vector.
	struct ChaseLane
	{
		uint16_t kind = 0; // channel * 128 + pitch or number; programs and pitch bends follow (MidiSequencer::g_chased)
		uint32_t first = 0;
		uint32_t last = 0;
	};

	// Controller events that can be chased (control changes, program changes and pitch bends), sorted by kind, then
	// position, so a chase finds each kind's last event by binary search.
	struct ChaseIndex
	{
		std::vector<RenderController> controllers;
		std::vector<ChaseLane> lanes; // runs of controllers, by kind
	};

	struct ChasePart
	{
		RenderPart range;
		ChaseIndex index; // the part's own controller events
	};

	// A track's controller events for chasing (makeChaseTrack): all of them, and those of each MIDI part.
	struct ChaseTrack
	{
		ChaseIndex all;
		std::vector<ChasePart> parts; // sorted by start
	};

	// Command thread: the chase index of a track's controller events (sorted by position, each with the index of its
	// part in parts) and MIDI parts (sorted by start).
	ChaseTrack makeChaseTrack(const std::vector<RenderController>& controllers, const std::vector<RenderPart>& parts);

	// A MIDI effect a track's sequencer runs (MidiSequence::effects).
	struct RunningEffect
	{
		std::shared_ptr<processors::MidiEffect> effect;
		uint64_t serial = 0;    // the instance's: another instance has another one
		size_t slot = 0;        // its slot in its section
		bool elsewhere = false; // a send that plays to another instrument track (MidiSequence::sendOutputs)
		bool send = false;      // a MIDI send, not an insert
	};

	class MidiSequencer;

	// What a MIDI or instrument track plays, in the project's samples.
	struct MidiSequence
	{
		std::vector<RenderNote> notes;             // sorted by start, then pitch
		std::vector<RenderController> controllers; // sorted by position
		// The tracks whose controller events a jump chases here: an instrument track's own and those of the MIDI
		// tracks routed to it, so they are chased by time across them; none for a MIDI track routed to an instrument.
		std::vector<ChaseTrack> chase;
		std::vector<uint32_t> noteOrder;  // filled in by setSequence: indices into notes with a length, by pitch, then
										  // start
		std::vector<ChaseLane> noteLanes; // filled in by setSequence: runs of noteOrder, by channel and pitch
		midi::Modifiers modifiers;        // for the notes played into the track (input); notes has them applied
		std::vector<LiveHarmony> liveInput; // Live Input: what the notes played into the track move to, by position
		midi::EffectRack inserts;           // the track's MIDI inserts and sends (docs/features/midi-effects.md)
		midi::EffectRack sends;
		// Per send slot, the instrument track's channel its effect plays to (a MidiSendTap takes it there), or 0
		// for the track's own output.
		std::array<uint64_t, midi::g_effectSlots> sendOutputs{};
		// Sends before modifiers that change notes: the notes of the parts without the modifiers (RenderChannel).
		std::vector<RenderNote> preNotes;
		// Filled in by setSequence: the inserts that play, in order, then the sends that play. The first inputCount
		// inserts are above the divider: they play the track's input alone (MidiSequencer::processInput).
		std::vector<RunningEffect> effects;
		size_t inputCount = 0;
		size_t insertCount = 0;
		// Filled in by setSequence when sends play before modifiers that change notes: a sequencer that plays the
		// track's events without them, for those sends.
		std::shared_ptr<MidiSequencer> pre;
		bool sendBuffers = false; // filled in by setSequence: MidiSequencer::sendOutput has room, from now on
		uint64_t serial = 0;      // filled in by setSequence: counts the sequences it was given
	};

	// Which events a jump chases (the midi.chase* user settings): the controller events in effect where playback
	// lands, the last of each kind before it, and the notes that sound there.
	struct MidiChase
	{
		bool notes = false;
		bool controllers = false; // control changes, each number by itself
		bool programs = false;
		bool pitchBend = false;
		bool beyondParts = true; // controller events from the whole track, not just the part under the position
	};

	// Plays a track's MIDI parts as events at their sample offsets, after the events it receives (from MIDI tracks
	// routed to an instrument track). Where events meet at one sample, note-offs come first, then controller events,
	// then note-ons; a note that starts while the same pitch sounds ends it first.
	//
	// It remembers which notes sound, and ends them where they end, where the transport stops and where playback
	// jumps: a start, a locate or a cycle jump. The engine keeps one sequencer per channel across graph rebuilds, so a
	// note removed while it sounds still ends.
	//
	// Where playback jumps, it chases what MidiChase chooses, at the jump's offset: after the note-offs, the last
	// controller event of each kind before the position (MidiSequence::chase), in time order, unless one of its kind is
	// at the position itself, on any of the tracks chased, and plays there; then the controller events at the position;
	// then the notes that sound there as they would have if playback had run up to it: of each pitch the last one that
	// started, with its velocity, until its end, or for a sample if it is an articulation map's key switch that ended
	// (RenderNote::keySwitch); then the notes that start there. A locate while stopped chases the
	// controller events alone, up to and including those at the cursor. A chase takes a binary search per kind of
	// event.
	//
	// The track's own events, those of its input and of its parts, go through its MIDI inserts in order, each effect
	// playing what the one before it played, and each MIDI send plays its effect on them beside that, before or after
	// the inserts. When the effects that play change (one added, removed, replaced, switched off or bypassed), it ends
	// the notes it sounds at the next block, those the effects played too, so that none hangs: the effects that leave
	// can't end theirs, and those that arrive didn't see the note-ons.
	//
	// When the block's event buffer is full, the sequencer skips the rest of the segment: notes that should have
	// ended in it end at the start of the next block that has room, notes that should have started don't play, and
	// the rest of a chase follows in the next blocks, the notes that sound at their position.
	class MidiSequencer : public graph::Processor
	{
	public:
		static constexpr size_t g_notes = size_t{16} * 128; // a note per MIDI channel and pitch
		// What is chased by itself: a control change per channel and number, a program and a pitch bend per channel.
		static constexpr size_t g_chased = g_notes + (size_t{16} * 2);

		MidiSequencer();

		// Command thread: what to play from the next block on. Indexes the notes for chasing, and makes the MIDI
		// effects its slots hold, keeping those that play in the same slot as before.
		void setSequence(std::unique_ptr<MidiSequence> sequence);

		// Command thread: what jumps chase from the next block on. Nothing until set.
		void setChase(const MidiChase& chase) noexcept;

		void process(const graph::ProcessContext& context) noexcept override;

		// Audio thread, before the track's input is recorded (the audio engine's block, after the input
		// transformers): runs the inserts above the divider on the input, which then holds what they played, so the
		// track records and plays that (reference manual p. 1105).
		void processInput(uint32_t frames, const graph::TransportBlock& transport) noexcept;

		// Audio thread, after process: what the MIDI send in a slot played in the block, where it plays to another
		// instrument track; empty otherwise.
		const graph::EventBuffer& sendOutput(size_t slot) const noexcept { return m_sendOut[slot % m_sendOut.size()]; }

		// Audio thread: whether the note, channel * 128 + pitch, sounds.
		bool sounds(uint16_t index) const noexcept;

		// Audio thread: the sounding notes, as channel * 128 + pitch, ascending.
		const uint16_t* soundingBegin() const noexcept { return m_sounding.data(); }
		const uint16_t* soundingEnd() const noexcept { return m_sounding.data() + m_soundingCount; }

		// Audio thread: whether the note sounds, from the track's parts, held through its input (a note-on came in
		// and no note-off yet; a stop ends those too) or played by its MIDI effects.
		bool holds(uint16_t index) const noexcept
		{
			return m_inputHeld[index] || m_effectsHeld[index] || sounds(index);
		}

		// Audio thread: forgets the notes it holds without ending them, after the instrument they sounded on ended
		// them (InstrumentChannel::releaseNotesOf).
		void forget() noexcept
		{
			m_soundingCount = 0;
			m_inputHeld.fill(false);
			m_effectsHeld.fill(false);
		}

		// Audio thread, before the first block of a graph, after its releases: the serial of the instrument channel its
		// track plays on in that graph (InstrumentChannel::serial), or 0. When that changes (the instrument replaced,
		// the instrument track removed, routed from nowhere), the notes its input holds sounded on another instrument
		// or none, so it forgets them: they mustn't keep the same notes of other sources from ending on this one
		// (#391). Its parts' notes still end. Serials are never reused, unlike the addresses of freed channels.
		void playOn(uint64_t instrument) noexcept
		{
			if (instrument == m_instrument)
				return;
			m_inputHeld.fill(false);
			m_effectsHeld.fill(false);
			m_instrument = instrument;
		}

		// Audio thread: the serial of the instrument channel it played on in the graph that ran last (playOn).
		uint64_t playsOn() const noexcept { return m_instrument; }

		// Audio thread: the events played into the track in this block (midi.inject), as from its MIDI input. They go
		// out after the events it receives and before its parts' events, also while the transport stands, their notes
		// as the track's MIDI modifiers play them; a note-off and poly pressure go where their note-on went, also
		// after the modifiers changed. The audio engine fills them before the graph processes a block.
		graph::EventBuffer& input() noexcept { return m_input; }

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		// The track's own events: its input's and, by the transport, its parts'.
		void playOwn(
			const MidiSequence& sequence, const graph::ProcessContext& context, graph::EventBuffer& out) noexcept;
		// Runs an effect on in, with the parameter events of its automation (m_effectEvents), into to.
		void runEffect(const RunningEffect& running, const graph::EventBuffer& in, graph::EventBuffer& to,
			uint32_t frames, const graph::TransportBlock& transport, bool late) noexcept;
		// Runs the effects on the events in m_own and adds what they play to out.
		void runEffects(
			const MidiSequence& sequence, const graph::ProcessContext& context, graph::EventBuffer& out) noexcept;
		// When the sequence is new and other effects play: ends every note it sounds, also those its sends played to
		// other instrument tracks.
		void takeEffects(const MidiSequence& sequence, graph::EventBuffer& out) noexcept;
		// An event of its input as the modifiers play it; false if left out. A second note-on for a held key whose
		// note goes elsewhere this time sets ends to where the first one went, which still sounds: that note ends
		// first, so that no note hangs.
		bool modifyInput(
			const MidiSequence& sequence, int64_t position, graph::ProcessEvent& event, uint16_t& ends) noexcept;
		void play(
			const MidiSequence& sequence, const graph::TransportSegment& segment, graph::EventBuffer& out) noexcept;
		// Each returns false if the event buffer was full; then the note didn't start or end (though noteOn may have
		// ended the same pitch first).
		bool noteOn(const RenderNote& note, uint32_t offset, graph::EventBuffer& out) noexcept;
		bool noteOff(uint16_t index, uint32_t offset, graph::EventBuffer& out) noexcept;
		// Notes it can't end for a full buffer end as soon as there is room.
		void endAll(uint32_t offset, graph::EventBuffer& out) noexcept;
		// Finds the controller events to chase at a position; sendChased sends them, false if the buffer filled first.
		// Stopped, it takes the events at the position too.
		void findChased(const MidiSequence& sequence, int64_t position, uint8_t chase, bool stopped) noexcept;
		void consider(const ChaseIndex& index, uint16_t track, int64_t position, uint8_t chase, bool stopped) noexcept;
		bool sendChased(uint32_t offset, graph::EventBuffer& out) noexcept;
		// Starts the notes that sound at a position; false if the buffer was full before it started them all.
		bool chaseNotes(
			const MidiSequence& sequence, int64_t position, uint32_t offset, graph::EventBuffer& out) noexcept;

		// A controller event to chase.
		struct Chased
		{
			int64_t position = std::numeric_limits<int64_t>::min(); // where it was; the minimum: none of its kind
			bool played = false; // one of its kind is at the position and plays there: not chased
			uint32_t value = 0;
			uint16_t track = 0; // in MidiSequence::chase
			uint16_t kind = 0;
			ControllerType type = ControllerType::Control;
			uint8_t number = 0;
			uint8_t channel = 0;
		};

		rt::SnapshotPublisher<MidiSequence> m_sequence;
		std::atomic<uint8_t> m_chase{0};            // MidiChase as flags
		std::array<int64_t, g_notes> m_ends{};      // where each sounding note ends
		std::array<uint16_t, g_notes> m_sounding{}; // the sounding notes, as channel * 128 + pitch, in order
		size_t m_soundingCount = 0;
		int64_t m_next = -1;       // where playback goes on without a jump; -1 while stopped
		int64_t m_cursor = -1;     // while stopped, the position whose controller events were sent last; -1 if unknown
		bool m_chaseNotes = false; // whether the last jump has yet to chase the notes
		// The controller events to chase: while finding, by kind; then the first m_chasedCount in time order, of
		// which the first m_chasedSent were sent.
		std::array<Chased, g_chased> m_chased{};
		size_t m_chasedCount = 0;
		size_t m_chasedSent = 0;
		std::array<uint32_t, g_notes> m_chasedNotes{}; // while chasing: the notes to start, as indices into notes
		graph::EventBuffer m_input;
		std::array<bool, g_notes> m_inputHeld{}; // notes its input sent a note-on for and no note-off since
		// Where each note of its input went after the modifiers, as channel * 128 + pitch; g_notes: left out.
		std::array<uint16_t, g_notes> m_inputTo{};
		std::array<bool, g_notes> m_inputDown{}; // the keys of its input, as channel * 128 + pitch, that are held
		uint64_t m_random = 0;                   // the random modifiers' seed for the next note of its input
		uint64_t m_instrument = 0;               // playOn: where its notes went in the graph that runs

		// Command thread: the effect in each slot, the inserts' then the sends', while it plays.
		struct SlotEffect
		{
			std::string id;
			std::shared_ptr<processors::MidiEffect> effect;
			uint64_t serial = 0;
			uint64_t output = 0; // sends: MidiSequence::sendOutputs
			bool input = false;  // inserts: above the divider
			uint8_t source = 0;  // sends: where their events come from (setSequence)
		};
		std::array<SlotEffect, 2 * midi::g_effectSlots> m_slots;
		uint64_t m_effectSerial = 0;   // the last RunningEffect::serial given
		uint64_t m_sequenceSerial = 0; // the last MidiSequence::serial given
		double m_sampleRate = 0.0;     // prepare's, for new effects; 0: not prepared yet
		uint32_t m_maxFrames = 0;
		// Audio thread: allocated by setSequence before the first sequence with effects.
		graph::EventBuffer m_own;    // the track's own events, for the effects
		graph::EventBuffer m_chainA; // what the effects play, in turns
		graph::EventBuffer m_chainB;
		// The block's parameter events for the effects (midi::g_effectEvents), which its automation sends among its
		// events in, and an effect's events in with its own of them.
		graph::EventBuffer m_effectEvents;
		graph::EventBuffer m_effectIn;
		uint64_t m_serial = 0;                                   // the sequence it took last
		std::array<uint64_t, 2 * midi::g_effectSlots> m_chain{}; // the serials of the effects that played
		size_t m_chainCount = 0;                                 // how many
		std::array<bool, g_notes> m_effectsHeld{}; // notes the effects played a note-on for and no note-off since
		MidiChase m_chaseSetting;                  // setChase's, for the sequencer of the sends before the modifiers
		std::shared_ptr<MidiSequencer> m_pre;      // command thread: MidiSequence::pre while sends need it
		graph::EventBuffer m_preOut;               // audio thread: what it played in the block
		graph::EventBuffer m_none;                 // nothing, for its events in
		// Per send slot, what it played in the block to another instrument track, and the notes it holds there.
		std::array<graph::EventBuffer, midi::g_effectSlots> m_sendOut;
		std::array<std::array<bool, g_notes>, midi::g_effectSlots> m_sendHeld{};
	};

	// A MIDI send's way to another instrument track (graph::NodeKind::MidiSend): it outputs what the send's effect
	// played in the block (MidiSequencer::sendOutput), after the track's sequencer, and remembers the notes it holds
	// there, so that the instrument ends them when the send goes (InstrumentChannel::releaseNotesOf).
	class MidiSendTap : public graph::Processor
	{
	public:
		MidiSendTap(std::shared_ptr<MidiSequencer> source, size_t slot);

		void process(const graph::ProcessContext& context) noexcept override;

		// Audio thread: whether a note-on for the note, channel * 128 + pitch, went through and no note-off since.
		bool holds(uint16_t index) const noexcept { return m_held[index]; }
		void forget() noexcept { m_held.fill(false); }

	private:
		std::shared_ptr<MidiSequencer> m_source;
		size_t m_slot = 0;
		std::array<bool, MidiSequencer::g_notes> m_held{};
	};

	// An instrument track's channel: its own parts' events and those of the MIDI tracks routed to it, played by its
	// instrument (none: silence). When the transport stops, it ends every note the instrument still holds.
	class InstrumentChannel : public graph::Processor
	{
	public:
		// The audio engine gives each channel it makes a new serial, for MidiSequencer::playOn.
		InstrumentChannel(std::shared_ptr<MidiSequencer> sequencer, std::shared_ptr<graph::Processor> instrument,
			uint64_t serial = 0);

		void process(const graph::ProcessContext& context) noexcept override;

		uint64_t serial() const noexcept { return m_serial; }

		// Audio thread, before the first block of the graph without a MIDI track that was routed here (removed or
		// rerouted), and before the sequencers' playOn: ends the notes that track's sequencer started here, from its
		// parts or its input, at the next block, except those this channel's own sequencer or one of the staying
		// tracks' also holds. A staying track counts only if it played here in the graph that ran last: one that
		// arrives in this graph holds notes from somewhere else. The departed sequencer forgets them.
		void releaseNotesOf(
			MidiSequencer& departed, const std::vector<std::shared_ptr<MidiSequencer>>& staying) noexcept;
		// The same for another track's MIDI send that played here and goes: the notes it holds here end.
		void releaseNotesOf(MidiSendTap& departed, const std::vector<std::shared_ptr<MidiSequencer>>& staying) noexcept;

		// Audio thread, after the block's process: the events its instrument got (midi.capture).
		const graph::EventBuffer& played() const noexcept { return m_events; }

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		// Ends the held notes that departed holds and no other source does.
		template <typename Departed>
		void release(const Departed& departed, const std::vector<std::shared_ptr<MidiSequencer>>& staying) noexcept;

		std::shared_ptr<MidiSequencer> m_sequencer;
		std::shared_ptr<graph::Processor> m_instrument;
		uint64_t m_serial = 0;
		graph::EventBuffer m_events;
		std::array<bool, MidiSequencer::g_notes> m_held{};   // notes the instrument got a note-on for and no note-off
		std::array<bool, MidiSequencer::g_notes> m_ending{}; // held notes to end as soon as the buffer has room
		size_t m_endingCount = 0;                            // how many m_ending are set
		bool m_playing = false;                              // in the last block
	};
}
