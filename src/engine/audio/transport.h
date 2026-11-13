// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/arranger.h"
#include "engine/graph/processor.h"
#include "engine/time/tempomap.h"
#include "platform/rt/seqlockpair.h"
#include "platform/rt/spscqueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace gnubase::engine
{
	using time::g_maxPosition; // the largest project position (engine/time/tempomap.h)

	// Where playback starts.
	enum class StartMode : uint8_t
	{
		Cursor,     // the project cursor
		CycleStart, // the left locator
	};

	// What a recording does to the events of its track that it overlaps (reference manual p. 345), as recording.add
	// takes it. MIDI recording has its own modes, MidiRecordMode.
	enum class AudioRecordMode : uint8_t
	{
		KeepHistory,         // they stay
		CycleHistoryReplace, // the recording replaces them; every lap of a cycle recording stays
		Replace,             // each take replaces them and the takes before it
	};

	// The modes' names, by AudioRecordMode.
	inline constexpr std::array<const char*, 3> g_audioRecordModes = {"keepHistory", "cycleHistoryReplace", "replace"};

	// What a MIDI recording does to the MIDI parts of its track that it overlaps (reference manual p. 352).
	enum class MidiRecordMode : uint8_t
	{
		NewParts, // they stay; the recording is a part of its own
		Merge,    // the recorded events go into the first part they overlap
		Replace,  // ... and replace the events of the parts they overlap in the recorded range
	};

	// The modes' names, by MidiRecordMode.
	inline constexpr std::array<const char*, 3> g_midiRecordModes = {"newParts", "merge", "replace"};

	// What a MIDI cycle recording makes of its laps (reference manual pp. 352-353), as recording.add takes it.
	enum class MidiCycleRecordMode : uint8_t
	{
		Mix,        // every lap goes into one part
		Overwrite,  // input in a lap replaces what the laps before recorded from there to the lap's end
		KeepLast,   // the last complete lap that recorded anything replaces the others
		Stacked,    // a part per lap, each on a lane of its own; all but the last are muted
		MixStacked, // the same, none muted
	};

	// The modes' names, by MidiCycleRecordMode.
	inline constexpr std::array<const char*, 5> g_midiCycleRecordModes = {
		"mix", "overwrite", "keepLast", "stacked", "mixStacked"};

	struct TransportSettings
	{
		time::Samples left = 0;  // the left locator
		time::Samples right = 0; // the right locator
		bool cycle = false;      // cycle playback between the locators, or skips them if the right comes first
		StartMode startMode = StartMode::Cursor;
		bool returnToStart = false; // on stop the cursor goes back to where playback started
		bool punchIn = false;       // reaching the left locator while playing starts recording
		bool punchOut = false;      // reaching the right locator while recording ends it; playback goes on
		bool stopAfterPunchOut =
			false; // ... unless this is set (audio.stopAfterPunchOut): then playback stops, after the post-roll
		bool preRoll = false; // playback starts preRollAmount before the cursor
		time::Samples preRollAmount = 0;
		bool postRoll = false; // playback stops postRollAmount after an automatic punch out
		time::Samples postRollAmount = 0;
		// The transport bar's switch for the metronome click, whose behaviour comes with #101.
		bool metronome = false;
		AudioRecordMode audioRecordMode = AudioRecordMode::KeepHistory;
		MidiRecordMode midiRecordMode = MidiRecordMode::NewParts;
		MidiCycleRecordMode midiCycleRecordMode = MidiCycleRecordMode::Mix;
		// Lock Record (reference manual p. 356): the commands refuse to stop or punch out a recording unless the stop
		// is confirmed, and recording ignores the automatic punch out.
		bool lockRecord = false;
	};

	// The project cursor, play and stop, recording, and cycle playback. The command thread sends requests; the audio
	// thread applies them at the start of its next block and computes where each frame of the block plays.
	//
	// A count-in delays a recording from stop: for its length the blocks count in and don't play, then playback starts
	// at the exact frame where it ends. Stop ends a count-in; the cursor stays.
	//
	// Cycle playback: while the cursor is before the right locator, reaching the right locator jumps to the left one.
	// With the right locator after the left one that loops the range; with it before, it skips the range. Playback
	// from the right locator or after it plays on.
	//
	// Punching in and out at the locators (reference manual pp. 318-319, 336-338) happens on the audio thread at the
	// exact sample, and so does stopping after an automatic punch out, at the first block that starts at or after
	// the right locator plus the post-roll. The command thread takes such changes over with sync().
	//
	// Arranger mode (#167): with a plan that plays, playback follows the active chain instead of the cycle, jumping
	// at the exact sample where a step's event ends (arrangerNext), and stops there after a step that pauses.
	class Transport
	{
	public:
		// Blocks are at most maxFrames long.
		explicit Transport(uint32_t maxFrames = 8192);

		// Command thread. Each returns false, and changes nothing, if too many requests are waiting.
		bool play(); // from startPosition: the cursor, or the left locator in StartMode::CycleStart, less the pre-roll
		bool stop(); // ends recording or a count-in; returns to the start if returnToStart is set
		// While playing: punches in, or with punchIn before the left locator lets the punch in start recording there;
		// off punches out. Stopped, on starts playback as play does and recording where play would start without the
		// pre-roll, or at the left locator with punchIn if that comes later; off does nothing. With a count-in (#101),
		// recording from stop counts in first, and playback starts at the exact sample where it ends. While it counts
		// in, on with a count-in starts it again (after a locate or a new pre-roll), on without one changes nothing,
		// and off ends it as stop does.
		bool record(bool on, const graph::CountIn& countIn = {});
		// Positions and locators are clamped to 0 to g_maxPosition.
		bool locate(time::Samples position);         // also the new start position for returnToStart
		bool set(const TransportSettings& settings); // new locators and modes
		// Arranger playback: makes state the one playing (after a locate to where it plays), or with pending lets its
		// step or event take over at the next jump point.
		bool arrange(const ArrangerState& state, bool pending);
		// Any thread but the audio thread: where arranger playback is, as of the last block or request.
		ArrangerState arranger() const;

		// Command thread: the settings as requested and whether playback was requested.
		const TransportSettings& settings() const { return m_requested; }
		bool playing() const { return m_requestedPlaying; }
		// While counting in, playing is true, and recording is whether recording starts where playback starts.
		bool recording() const { return m_requestedRecording; }
		bool countingIn() const { return m_requestedCountingIn; }
		// Command thread: where play starts playback.
		time::Samples startPosition() const;
		time::Samples playStart() const; // where play starts playback without the pre-roll
		// Command thread: where the last playback started, without the pre-roll (0 before any).
		time::Samples playbackStart() const { return m_playbackStart; }
		// Where recording from stop starts, for playback that starts at start before the pre-roll and at from with
		// it: at start, or the left locator with punch in if that comes later. In a cycle that would jump back before
		// reaching that, at from, or with punch in at from or the left locator, whichever comes later; in one that
		// skips the range it lies in, at the left locator, where playback lands. A locate while recording waits uses
		// it too, with start and from where the locate goes.
		static time::Samples recordStart(const TransportSettings& settings, time::Samples start, time::Samples from);

		// Command thread: once the audio thread has applied every request, takes over whether it plays and records,
		// which punching in and out and stopping after a punch out change on their own. Returns whether that changed
		// anything.
		bool sync();
		// Command thread: how many requests were sent. Any thread: how many of them the audio thread has applied. It
		// publishes the cursor and the count-in first: a thread that sees a request applied sees where it went.
		uint64_t sentRequests() const { return m_sentRequests; }
		uint64_t appliedRequests() const { return m_appliedRequests.load(std::memory_order_acquire); }
		// Any thread: counts the blocks and requests after which the audio thread played or recorded differently, or
		// applied a request: a thread that sees it change has the command thread sync.
		uint64_t changes() const { return m_changes.load(std::memory_order_acquire); }

		// Any thread but the audio thread (the cue thread reads it while a device runs): the cursor, where the last
		// locate goes until the audio thread has applied it, then as of the last block.
		time::Samples position() const;
		// Any thread but the audio thread: while the audio thread counts in, where playback starts after it, the
		// place to cue; -1 otherwise.
		time::Samples countInStart() const { return m_countInStart.load(std::memory_order_acquire); }

		// Audio thread, or the command thread while no audio thread runs: applies the waiting requests.
		void update() noexcept;

		// Audio thread: applies the waiting requests, then plays a block of up to maxFrames frames, in arranger mode
		// along the plan. The result stays valid until the next call.
		const graph::TransportBlock& advance(uint32_t frames, const ArrangerPlan* plan = nullptr) noexcept;

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
				Arrange,
			};

			Type type = Type::Play;
			time::Samples position = 0;
			TransportSettings settings;
			uint64_t locate = 0;      // Locate: which one, counted from 1
			bool record = false;      // Record: punch in, or out
			graph::CountIn countIn{}; // Record from stop
			ArrangerState arranger{}; // Arrange; record: pending
		};

		bool send(const Request& request);
		void apply(const Request& request) noexcept;
		void locateTo(time::Samples position) noexcept;
		void start() noexcept;
		void startRecording() noexcept;
		void autoStop() noexcept;
		void publish(bool changed) noexcept;

		// Command thread.
		rt::SpscQueue<Request> m_requests;
		TransportSettings m_requested;
		bool m_requestedPlaying = false;
		bool m_requestedRecording = false;
		bool m_requestedCountingIn = false;
		uint64_t m_locates = 0;      // how many locates were sent
		uint64_t m_sentRequests = 0; // how many requests were sent
		time::Samples m_playbackStart = 0;

		// Audio thread.
		TransportSettings m_settings;
		bool m_playing = false;
		bool m_recording = false;
		bool m_countingIn = false; // before a recording from stop
		graph::CountIn m_countIn;
		time::Samples m_countedIn = 0; // samples of the count-in done
		time::Samples m_position = 0;
		time::Samples m_start = 0;     // where playback started or was last located to
		bool m_inPreRoll = false;      // the pre-roll plays: the position hasn't reached m_start yet
		time::Samples m_recordAt = -1; // a recording from stop starts here, after the pre-roll
		time::Samples m_stopAt = -1;   // after an automatic punch out: where playback stops
		uint64_t m_locate = 0;         // the last locate applied
		uint8_t m_published = 0;       // what m_state says
		std::vector<graph::TransportSegment> m_segments;
		graph::TransportBlock m_block;
		ArrangerState m_arranger;
		const ArrangerPlan* m_plan = nullptr; // the plan advance last played, only compared
		ArrangerState m_publishedArranger;

		// Written by the command thread: how many locates were sent and where the last one goes.
		rt::SeqLockPair<uint64_t, time::Samples> m_sent;
		// Written by the audio thread: m_locate and m_position after each block. Each pair is read whole, so the cursor
		// never mixes one locate's count with another's target, or one block's position with another's locate.
		rt::SeqLockPair<uint64_t, time::Samples> m_applied;
		// Written by the audio thread: how many requests it applied, whether it plays (1), records (2) and counts in
		// (4), changes(), and countInStart().
		std::atomic<uint64_t> m_appliedRequests{0};
		std::atomic<time::Samples> m_countInStart{-1};
		std::atomic<uint8_t> m_state{0};
		std::atomic<uint64_t> m_changes{0};
		// Written by the audio thread: m_arranger, packed (arranger()).
		rt::SeqLockPair<int64_t, int64_t> m_arrangerShared;
	};

	// Where the transport played each of the device's last frames, so recorded input finds where it belongs: with
	// the audio that went out the record offset before it came in (the device's input and output latency, see
	// docs/features/latency-and-monitoring.md). Kept per frame, so input lines up across cycle jumps, locates, starts
	// and stops too.
	class PlayedPositions
	{
	public:
		// Command thread: keeps the positions of the last frames frames.
		explicit PlayedPositions(size_t frames = 0) : m_positions(frames, -1), m_recording(frames, 0) {}

		// Audio thread: the frames from device frame start on played where transport says.
		void add(uint64_t start, uint32_t frames, const graph::TransportBlock& transport) noexcept;

		// Audio thread: where the device frame played, or -1 if the transport stood then, or the frame isn't kept:
		// added longer ago than the last frames, or not yet.
		time::Samples at(int64_t frame) const noexcept;
		// Audio thread: the same, but -1 also if the transport didn't record then.
		time::Samples recordedAt(int64_t frame) const noexcept;

	private:
		std::vector<time::Samples> m_positions; // a ring, by device frame
		std::vector<uint8_t> m_recording;       // the same ring: 1 where the transport recorded
		int64_t m_end = 0;                      // the device frame after the last one added
	};
}
