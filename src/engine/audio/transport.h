// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/processor.h"
#include "engine/time/tempomap.h"
#include "platform/rt/seqlockpair.h"
#include "platform/rt/spscqueue.h"

#include <cstdint>
#include <vector>

namespace gnubase::engine
{
	// Where playback starts.
	enum class StartMode : uint8_t
	{
		Cursor,     // the project cursor
		CycleStart, // the left locator
	};

	struct TransportSettings
	{
		time::Samples left = 0;  // the left locator
		time::Samples right = 0; // the right locator
		bool cycle = false;      // cycle playback between the locators, or skips them if the right comes first
		StartMode startMode = StartMode::Cursor;
		bool returnToStart = false; // on stop the cursor goes back to where playback started
		// Switches of the transport bar whose behaviour comes with their tickets: punching in and out at the locators
		// (#102) and the metronome click (#101).
		bool punchIn = false;
		bool punchOut = false;
		bool metronome = false;
	};

	// The project cursor, play and stop, recording, and cycle playback. The command thread sends requests; the audio
	// thread applies them at the start of its next block and computes where each frame of the block plays.
	//
	// Cycle playback: while the cursor is before the right locator, reaching the right locator jumps to the left one.
	// With the right locator after the left one that loops the range; with it before, it skips the range. Playback
	// from the right locator or after it plays on.
	class Transport
	{
	public:
		// Blocks are at most maxFrames long.
		explicit Transport(uint32_t maxFrames = 8192);

		// Command thread. Each returns false, and changes nothing, if too many requests are waiting.
		bool play();                                 // from the cursor, or the left locator in StartMode::CycleStart
		bool stop();                                 // ends recording; returns to the start if returnToStart is set
		bool record(bool on);                        // while playing: punches in or out; stopped, does nothing
		bool locate(time::Samples position);         // also the new start position for returnToStart
		bool set(const TransportSettings& settings); // new locators and modes

		// Command thread: the settings as requested and whether playback was requested.
		const TransportSettings& settings() const { return m_requested; }
		bool playing() const { return m_requestedPlaying; }
		bool recording() const { return m_requestedRecording; }

		// Any thread but the audio thread (the cue thread reads it while a device runs): the cursor, where the last
		// locate goes until the audio thread has applied it, then as of the last block.
		time::Samples position() const;

		// Audio thread, or the command thread while no audio thread runs: applies the waiting requests.
		void update() noexcept;

		// Audio thread: applies the waiting requests, then plays a block of up to maxFrames frames. The result stays
		// valid until the next call.
		const graph::TransportBlock& advance(uint32_t frames) noexcept;

	private:
		struct Request
		{
			enum class Type : uint8_t
			{
				Play,
				Stop,
				Locate,
				Set,
				Record,
			};

			Type type = Type::Play;
			time::Samples position = 0;
			TransportSettings settings;
			uint64_t locate = 0; // Locate: which one, counted from 1
			bool record = false; // Record: punch in, or out
		};

		bool send(const Request& request);
		void apply(const Request& request) noexcept;
		void locateTo(time::Samples position) noexcept;

		// Command thread.
		rt::SpscQueue<Request> m_requests;
		TransportSettings m_requested;
		bool m_requestedPlaying = false;
		bool m_requestedRecording = false;
		uint64_t m_locates = 0; // how many locates were sent

		// Audio thread.
		TransportSettings m_settings;
		bool m_playing = false;
		bool m_recording = false;
		time::Samples m_position = 0;
		time::Samples m_start = 0; // where playback started or was last located to
		uint64_t m_locate = 0;     // the last locate applied
		std::vector<graph::TransportSegment> m_segments;
		graph::TransportBlock m_block;

		// Written by the command thread: how many locates were sent and where the last one goes.
		rt::SeqLockPair<uint64_t, time::Samples> m_sent;
		// Written by the audio thread: m_locate and m_position after each block. Each pair is read whole, so the cursor
		// never mixes one locate's count with another's target, or one block's position with another's locate.
		rt::SeqLockPair<uint64_t, time::Samples> m_applied;
	};

	// Where the transport played each of the device's last frames, so recorded input finds where it belongs: with
	// the audio that went out the record offset before it came in (the device's input and output latency, see
	// docs/features/latency-and-monitoring.md). Kept per frame, so input lines up across cycle jumps, locates, starts
	// and stops too.
	class PlayedPositions
	{
	public:
		// Command thread: keeps the positions of the last frames frames.
		explicit PlayedPositions(size_t frames = 0) : m_positions(frames, -1) {}

		// Audio thread: the frames from device frame start on played where transport says.
		void add(uint64_t start, uint32_t frames, const graph::TransportBlock& transport) noexcept;

		// Audio thread: where the device frame played, or -1 if the transport stood then, or the frame isn't kept:
		// added longer ago than the last frames, or not yet.
		time::Samples at(int64_t frame) const noexcept;

	private:
		std::vector<time::Samples> m_positions; // a ring, by device frame
		int64_t m_end = 0;                      // the device frame after the last one added
	};
}
