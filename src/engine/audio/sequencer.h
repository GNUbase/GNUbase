// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/rendersnapshot.h"
#include "engine/graph/processor.h"
#include "platform/rt/snapshotpublisher.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace gnubase::engine
{
	// What a MIDI or instrument track plays, in the project's samples.
	struct MidiSequence
	{
		std::vector<RenderNote> notes;             // sorted by start, then pitch
		std::vector<RenderController> controllers; // sorted by position
		std::vector<RenderPart> parts;             // sorted by start
		std::vector<int64_t> reach;                // the latest end of notes[0] to notes[i], filled in by setSequence
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
	// Where playback jumps, it chases what MidiChase chooses, at the jump's offset: after the note-offs, the controller
	// events in effect there, in the order they were sent; then the controller events at that position itself; then
	// the notes that sound there, which end where they end; then the notes that start there. A locate while stopped
	// chases the controller events alone. Chasing looks back from the position through the track's (or the part's)
	// controller events, so it takes time in proportion to their number.
	//
	// When the block's event buffer is full, the sequencer skips the rest of the segment: notes that should have
	// ended in it end at the start of the next block that has room, notes that should have started don't play, and a
	// chase that didn't fit takes place at the next block's position.
	class MidiSequencer : public graph::Processor
	{
	public:
		static constexpr size_t g_notes = size_t{16} * 128; // a note per MIDI channel and pitch
		// What is chased by itself: a control change per channel and number, a program and a pitch bend per channel.
		static constexpr size_t g_chased = g_notes + (size_t{16} * 2);

		MidiSequencer();

		// Command thread: what to play from the next block on.
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
		// Each returns false if the event buffer was full before it sent everything.
		bool chaseControllers(const MidiSequence& sequence, int64_t position, uint32_t offset, uint8_t chase,
			graph::EventBuffer& out) noexcept;
		bool chaseNotes(
			const MidiSequence& sequence, int64_t position, uint32_t offset, graph::EventBuffer& out) noexcept;

		rt::SnapshotPublisher<MidiSequence> m_sequence;
		std::atomic<uint8_t> m_chase{0};            // MidiChase as flags
		std::array<int64_t, g_notes> m_ends{};      // where each sounding note ends
		std::array<uint16_t, g_notes> m_sounding{}; // the sounding notes, as channel * 128 + pitch, in order
		size_t m_soundingCount = 0;
		int64_t m_next = -1;   // where playback goes on without a jump; -1 while stopped
		int64_t m_cursor = -1; // while stopped, the position whose controller events were sent last; -1 if unknown
		uint8_t m_pending = 0; // what the last jump has yet to chase, as flags with the chase's beyondParts
		std::array<bool, g_chased> m_chased{};  // while chasing: what was found
		std::array<size_t, g_chased> m_found{}; // while chasing: the controller events found, the latest first
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
