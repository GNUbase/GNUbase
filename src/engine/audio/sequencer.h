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
	};

	// Plays a track's MIDI parts as events at their sample offsets, after the events it receives (from MIDI tracks
	// routed to an instrument track). Where events meet at one sample, note-offs come first, then controller events,
	// then note-ons; a note that starts while the same pitch sounds ends it first.
	//
	// It remembers which notes sound, and ends them where they end, where the transport stops and where playback
	// jumps: a locate or a cycle jump. After a jump only notes that start from there on play. The engine keeps one
	// sequencer per channel across graph rebuilds, so a note removed while it sounds still ends.
	class MidiSequencer : public graph::Processor
	{
	public:
		MidiSequencer();

		// Command thread: what to play from the next block on.
		void setSequence(std::unique_ptr<const MidiSequence> sequence);

		void process(const graph::ProcessContext& context) noexcept override;

	private:
		static constexpr size_t g_notes = size_t{16} * 128; // a note per MIDI channel and pitch

		void play(
			const MidiSequence& sequence, const graph::TransportSegment& segment, graph::EventBuffer& out) noexcept;
		void noteOn(const RenderNote& note, uint32_t offset, graph::EventBuffer& out) noexcept;
		void noteOff(uint16_t index, uint32_t offset, graph::EventBuffer& out) noexcept;
		void endAll(uint32_t offset, graph::EventBuffer& out) noexcept;

		rt::SnapshotPublisher<MidiSequence> m_sequence;
		std::array<int64_t, g_notes> m_ends{};      // where each sounding note ends
		std::array<uint16_t, g_notes> m_sounding{}; // the sounding notes, as channel * 128 + pitch, in order
		size_t m_soundingCount = 0;
		int64_t m_next = -1; // where playback goes on without a jump; -1 while stopped
	};

	// An instrument track's channel: its own parts' events and those of the MIDI tracks routed to it, played by its
	// instrument (none: silence).
	class InstrumentChannel : public graph::Processor
	{
	public:
		InstrumentChannel(std::shared_ptr<MidiSequencer> sequencer, std::shared_ptr<graph::Processor> instrument);

		void process(const graph::ProcessContext& context) noexcept override;

		// Any thread: ends every note the instrument holds at the next block, e.g. when a MIDI track routed here was
		// removed or rerouted while its notes sounded.
		void releaseNotes() { m_release.store(true, std::memory_order_release); }

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		std::shared_ptr<MidiSequencer> m_sequencer;
		std::shared_ptr<graph::Processor> m_instrument;
		graph::EventBuffer m_events;
		std::array<bool, size_t{16} * 128> m_held{}; // notes the instrument got a note-on for and no note-off yet
		std::atomic<bool> m_release{false};
	};
}
