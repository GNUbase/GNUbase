// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/rendersnapshot.h"
#include "engine/graph/processor.h"
#include "platform/rt/snapshotpublisher.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
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
	// started, with its velocity, until its end; then the notes that start there. A locate while stopped chases the
	// controller events alone, up to and including those at the cursor. A chase takes a binary search per kind of
	// event.
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

		// Command thread: what to play from the next block on. Indexes the notes for chasing.
		void setSequence(std::unique_ptr<MidiSequence> sequence);

		// Any thread: what jumps chase from the next block on. Nothing until set.
		void setChase(const MidiChase& chase) noexcept;

		void process(const graph::ProcessContext& context) noexcept override;

		// Audio thread: whether the note, channel * 128 + pitch, sounds.
		bool sounds(uint16_t index) const noexcept;

		// Audio thread: the sounding notes, as channel * 128 + pitch, ascending.
		const uint16_t* soundingBegin() const noexcept { return m_sounding.data(); }
		const uint16_t* soundingEnd() const noexcept { return m_sounding.data() + m_soundingCount; }

		// Audio thread: forgets the sounding notes without ending them, after the instrument they sounded on ended
		// them (InstrumentChannel::releaseNotesOf).
		void forget() noexcept { m_soundingCount = 0; }

	private:
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
	};

	// An instrument track's channel: its own parts' events and those of the MIDI tracks routed to it, played by its
	// instrument (none: silence). When the transport stops, it ends every note the instrument still holds.
	class InstrumentChannel : public graph::Processor
	{
	public:
		InstrumentChannel(std::shared_ptr<MidiSequencer> sequencer, std::shared_ptr<graph::Processor> instrument);

		void process(const graph::ProcessContext& context) noexcept override;

		// Audio thread, before the first block of the graph without a MIDI track that was routed here (removed or
		// rerouted): ends the notes that track's sequencer started here at the next block, except those this
		// channel's own sequencer or one of the staying tracks' also holds. The departed sequencer forgets them.
		void releaseNotesOf(
			MidiSequencer& departed, const std::vector<std::shared_ptr<MidiSequencer>>& staying) noexcept;

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		std::shared_ptr<MidiSequencer> m_sequencer;
		std::shared_ptr<graph::Processor> m_instrument;
		graph::EventBuffer m_events;
		std::array<bool, MidiSequencer::g_notes> m_held{};   // notes the instrument got a note-on for and no note-off
		std::array<bool, MidiSequencer::g_notes> m_ending{}; // held notes to end as soon as the buffer has room
		size_t m_endingCount = 0;                            // how many m_ending are set
		bool m_playing = false;                              // in the last block
	};
}
