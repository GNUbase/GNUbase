// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/miditap.h"
#include "engine/audio/transport.h"
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/graph/events.h"
#include "engine/schema.h"
#include "platform/audio/device.h"
#include "platform/rt/spscqueue.h"

#include <array>
#include <atomic>
#include <bitset>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	// A record-enabled audio track: the device input each channel of its file records, or -1 for silence.
	struct RecordTrack
	{
		ObjectId track = 0;
		std::string name; // the file is named after it
		std::vector<int> ports;

		bool operator==(const RecordTrack& other) const
		{
			return track == other.track && name == other.name && ports == other.ports;
		}
	};

	// A record-enabled MIDI or instrument track: what its channel's MIDI input plays is recorded. chordsOnly: a track
	// that only monitors while the chord track is record-enabled, whose notes only make chord events (reference
	// manual p. 1345).
	struct MidiRecordTrack
	{
		ObjectId track = 0;
		ObjectId channel = 0;
		bool chordsOnly = false;

		bool operator==(const MidiRecordTrack& other) const
		{
			return track == other.track && channel == other.channel && chordsOnly == other.chordsOnly;
		}
	};

	// The MIDI messages that MIDI recording leaves out (the user setting midi.recordFilter, reference manual p. 1613):
	// by type, by MIDI channel and by controller number. They still play.
	struct MidiRecordFilter
	{
		std::bitset<10> types;    // by graph::ProcessEvent::Type
		std::bitset<16> channels; // by MIDI channel, 0-15
		std::bitset<128> controllers;

		// From the user setting: {types: [...], channels: [1-16, ...], controllers: [0-127, ...]}.
		static MidiRecordFilter fromJson(const Json& json);
		bool records(const graph::ProcessEvent& event) const;
		bool operator==(const MidiRecordFilter& other) const
		{
			return types == other.types && channels == other.channels && controllers == other.controllers;
		}
	};

	// The type names of midi.recordFilter, by graph::ProcessEvent::Type; note-ons and note-offs are both notes.
	inline constexpr std::array<const char*, 7> g_midiFilterTypes = {
		"notes", "controllers", "pitchBend", "channelPressure", "polyPressure", "programs", "sysEx"};

	// Turns MIDI input at project positions into the notes and controller events recording.add takes: a note-on and
	// the next note-off of its pitch and channel make a note, a note-on of a pitch that sounds ends it first. Each
	// note and controller event says which take it was recorded in ("take"), for the MIDI cycle record modes.
	//
	// With MPE input (setMpe, docs/features/note-expression.md), the pitch bend, channel pressure and CC 74 of
	// channels 2 to 16 are the expression of the notes on their channel: points of their tuning, pressure and
	// brightness in project samples, and each channel's latest values are the initial ones of its next note.
	class MidiTakeBuilder
	{
	public:
		// MPE input with this pitch range in semitones, or 0 for none (the default).
		void setMpe(int pitchRange) { m_mpeRange = pitchRange; }
		void add(time::Samples position, const graph::ProcessEvent& event);
		// What comes from now on belongs to the take with this index; end the notes that sound first.
		void setTake(size_t take) { m_take = take; }
		// Ends the notes that sound at position.
		void endNotes(time::Samples position);
		bool empty() const { return m_notes.empty() && m_controllers.empty(); }
		// The latest position seen, or -1.
		time::Samples last() const { return m_last; }
		const Json& notes() const { return m_notes; }
		const Json& controllers() const { return m_controllers; }

	private:
		struct Sounding
		{
			time::Samples position = -1; // -1: not sounding
			uint16_t velocity = 0;
			Json expressions; // MPE: its expression points so far, positions in project samples
		};

		// MPE: whether the event is expression of a member channel's notes; then adds it to them.
		bool addExpression(time::Samples position, const graph::ProcessEvent& event);

		std::array<Sounding, size_t{16} * 128> m_sounding{};
		int m_mpeRange = 0;
		// MPE: each channel's latest tuning, pressure and brightness, null before the first.
		std::array<std::array<Json, 3>, 16> m_mpeState{};
		Json m_notes = Json::array();
		Json m_controllers = Json::array();
		time::Samples m_last = -1;
		size_t m_take = 0;
	};

	namespace settings
	{
		class Store;
	}

	// The pitch range of MPE input: midi.mpePitchRange with the user setting midi.mpeInput on, 0 with it off.
	int mpePitchRange(const settings::Store& store);

	// The format of the project's record files: the project setup's record file type and bit depth at the project's
	// sample rate. A Broadcast Wave file gets a bext chunk; the recorder fills in what belongs to each file.
	WriteOptions recordFormat(const Project& project);

	// Where and how a recording that starts writes its files.
	struct RecordSetup
	{
		uint64_t project = 0; // the project it records for (ProjectModel::openedRevision)
		std::filesystem::path folder;
		WriteOptions format; // the channels come from each track
		AudioRecordMode mode = AudioRecordMode::KeepHistory;
		int64_t preRecordFrames = 0; // audio pre-record, in device frames
		std::vector<RecordTrack> tracks;
		// MIDI recording (docs/features/midi-recording.md).
		std::vector<MidiRecordTrack> midiTracks;
		MidiRecordMode midiMode = MidiRecordMode::NewParts;
		MidiCycleRecordMode midiCycleMode = MidiCycleRecordMode::Mix;
		MidiRecordFilter midiFilter;
		bool snapToBars = true;   // midi.snapPartsToBars
		int64_t catchFrames = 0;  // midi.recordCatchRange, in device frames
		size_t retrospective = 0; // midi.retrospectiveEvents: what each track's retrospective buffer holds
		int64_t pauseFrames = 0;  // a pause after which input in stop mode starts the buffer afresh
		int mpePitchRange = 0;    // with midi.mpeInput on, midi.mpePitchRange; 0 without MPE input

		bool operator==(const RecordSetup& other) const
		{
			const auto timeReference = [](const WriteOptions& options)
			{ return options.broadcast ? std::optional(options.broadcast->timeReference) : std::nullopt; };
			return project == other.project && folder == other.folder && format.format == other.format.format &&
				format.sampleFormat == other.format.sampleFormat && format.sampleRate == other.format.sampleRate &&
				timeReference(format) == timeReference(other.format) && mode == other.mode &&
				preRecordFrames == other.preRecordFrames && tracks == other.tracks && midiTracks == other.midiTracks &&
				midiMode == other.midiMode && midiCycleMode == other.midiCycleMode && midiFilter == other.midiFilter &&
				snapToBars == other.snapToBars && catchFrames == other.catchFrames &&
				retrospective == other.retrospective && pauseFrames == other.pauseFrames &&
				mpePitchRange == other.mpePitchRange;
		}
	};

	// MIDI input that a track's retrospective buffer kept (midi.insertRetrospective).
	struct RetrospectiveEvent
	{
		uint64_t deviceFrame = 0;
		time::Samples position = -1; // where the transport played it, or -1 in stop mode
		graph::ProcessEvent event;   // its offset is unused
	};

	// Records the input of record-enabled tracks while the transport records (docs/features/recording.md,
	// docs/features/midi-recording.md). The audio thread hands each block's audio input, with the project position
	// each frame belongs at, and the MIDI input of MIDI and instrument tracks to the recorder's disk thread through
	// lock-free queues; it never waits, and a block that finds no free chunk is lost and counted. The disk thread
	// writes one file per audio track from where recording starts to where it ends, and a take per run of consecutive
	// positions: a cycle lap, or what was recorded between a punch in and a punch out. MIDI tracks record their input
	// the same way, without the record offset. When the recording ends it reports the files and their takes, and the
	// MIDI tracks' notes and controller events, as recording.add takes them. MIDI input that isn't recorded goes into
	// each track's retrospective buffer.
	class Recorder
	{
	public:
		// Disk thread: "recording.finished" with recording.add's parameters, or "recording.failed" with a reason.
		using Notify = std::function<void(const std::string& name, const Json& data)>;

		explicit Recorder(Notify notify);
		~Recorder(); // ends a recording that is still open, once its files are created
		Recorder(const Recorder&) = delete;
		Recorder& operator=(const Recorder&) = delete;

		// Command thread, while no audio thread calls process: blocks of up to maxFrames frames of channels input
		// channels from now on. Ends a recording that is still open, once its files are created.
		void prepare(int channels, uint32_t maxFrames);

		// Command thread: what the next recording records and where it goes. The one that runs keeps its own, and so
		// does one the audio thread started before. Doesn't wait for the disk thread, which can be listing the record
		// folder and creating the files.
		void setup(RecordSetup setup);

		// Audio thread: a block of the device's input, each frame of which belongs where played says the transport
		// played the frame offset frames before it (recorded only if the transport recorded then), and what the
		// sequencers' MIDI input plays in the block, recorded where the transport plays it.
		void process(const audio::DeviceBlock& block, const PlayedPositions& played, int64_t offset,
			const MidiTap::Sequencers& sequencers) noexcept;

		// Input frames lost because the disk thread fell behind.
		uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }

		// Command thread: what the retrospective buffer of the channel holds, oldest first. The next input to it
		// starts the buffer afresh. Doesn't wait while the disk thread lists the record folder and creates a
		// recording's files, and leaves a recording's start to it: the input sent after the chunk that starts the
		// recording isn't in it yet then.
		std::vector<RetrospectiveEvent> retrospective(ObjectId channel);
		// Command thread: empties the channel's retrospective buffer, or every one for 0. Like retrospective, input
		// still behind a recording's start comes in afterwards.
		void clearRetrospective(ObjectId channel);

	private:
		// MIDI input in a chunk: the channel it came to and where in the chunk.
		struct MidiInput
		{
			ObjectId channel = 0;
			uint32_t offset = 0;       // frames from the chunk's device frame
			time::Samples played = -1; // where the transport played it, or -1
			graph::ProcessEvent event;
		};

		// Consecutive device frames, recorded at consecutive positions from position, or not recorded (-1).
		struct Chunk
		{
			uint64_t deviceFrame = 0;
			time::Samples position = -1;
			uint32_t frames = 0;
			std::vector<float> samples; // audio: every input channel, interleaved
			bool midi = false;          // MIDI input instead
			std::vector<MidiInput> events;
			size_t eventCount = 0;
			uint64_t setup = 0; // the latest setup when it was sent (m_setupCount)
		};

		// A file being written.
		struct File
		{
			RecordTrack track;
			std::string path;
			std::unique_ptr<AudioFileWriter> writer;
		};

		struct Take
		{
			time::Samples position = 0;
			time::Samples offset = 0; // in the file
			time::Samples length = 0;
		};

		struct Retrospective
		{
			std::deque<RetrospectiveEvent> events;
			bool taken = false; // retrospective() returned it: the next input starts afresh
			// Its events before this device frame went into a recording (a track that only monitors while the chord
			// track records): the catch range of the next one leaves them out.
			uint64_t recordedUntil = 0;
		};

		// Releases m_mutex while a recording's files are created, with m_opening set.
		class Unlocked
		{
		public:
			explicit Unlocked(Recorder& recorder);
			~Unlocked();
			Unlocked(const Unlocked&) = delete;
			Unlocked& operator=(const Unlocked&) = delete;
			Unlocked(Unlocked&&) = delete;
			Unlocked& operator=(Unlocked&&) = delete;

		private:
			Recorder& m_recorder;
		};

		void run();
		// Disk thread, or the command thread, holding m_mutex.
		// Handles the chunks the audio thread sent, or false if there were none. Without mayBegin it stops before a
		// chunk that starts a recording: that one's folder listing and files are the disk thread's.
		bool drain(bool mayBegin);
		void handle(const Chunk& chunk);
		void begin(const Chunk& chunk);
		void write(const float* samples, uint32_t frames);
		void end();
		void keepHistory(const Chunk& chunk);
		void fail(const std::string& reason);
		void handleMidi(const Chunk& chunk);
		void beginMidi(const Chunk& chunk);
		void endMidi();
		void keepRetrospective(const MidiInput& input, uint64_t deviceFrame);
		// Makes the latest setup up to number the next recording's.
		void applySetups(uint64_t number);
		// Audio thread: hands frames frames from offset in the block on, or false if no chunk is free.
		bool send(const audio::DeviceBlock& block, uint32_t offset, uint32_t frames, time::Samples position) noexcept;
		void processMidi(const audio::DeviceBlock& block, const PlayedPositions& played,
			const MidiTap::Sequencers& sequencers) noexcept;
		bool sendMidi(const audio::DeviceBlock& block, const PlayedPositions& played,
			const MidiTap::Sequencers& sequencers, uint32_t offset, uint32_t frames, time::Samples position) noexcept;

		Notify m_notify;

		// Set by prepare, while no audio thread runs.
		int m_channels = 0;
		uint32_t m_maxFrames = 0;
		std::vector<Chunk> m_chunks;
		std::vector<Chunk> m_midiChunks;
		std::unique_ptr<rt::SpscQueue<Chunk*>> m_free;     // disk thread to audio thread
		std::unique_ptr<rt::SpscQueue<Chunk*>> m_midiFree; // the same for MIDI chunks
		std::unique_ptr<rt::SpscQueue<Chunk*>> m_filled;   // audio thread to disk thread, both kinds in order

		// Audio thread.
		bool m_sending = false;        // the last chunk sent was recorded: the disk thread waits for the end
		bool m_midiSending = false;    // the same for MIDI
		time::Samples m_midiNext = -1; // where the recorded MIDI run that was handed over last goes on, or -1
		uint64_t m_midiNextFrame = 0;  // ... at this device frame
		std::atomic<bool> m_armed{false};
		std::atomic<bool> m_midiArmed{false};
		std::atomic<uint64_t> m_dropped{0};
		std::atomic<uint64_t> m_latestSetup{0}; // the number of the latest setup, which the chunks get

		// Setups not yet applied, numbered from 1 on, oldest first: the command thread adds them, the disk thread
		// applies them as the chunks sent after them come.
		std::mutex m_setupMutex;
		std::deque<std::pair<uint64_t, RecordSetup>> m_setups;
		uint64_t m_setupCount = 0;

		// Disk thread, and the command thread while it holds m_mutex.
		std::mutex m_mutex;
		std::condition_variable m_wake;
		bool m_stopping = false;
		bool m_opening = false;    // begin creates the files without m_mutex: the chunks behind wait
		RecordSetup m_next;        // for the next recording
		uint64_t m_nextNumber = 0; // its number
		RecordSetup m_current;     // the recording's
		bool m_recording = false;
		std::vector<File> m_files;
		std::vector<Take> m_takes;
		time::Samples m_written = 0;       // frames in each file
		uint64_t m_deviceEnd = 0;          // the device frame after the last one recorded
		std::vector<float> m_history;      // pre-record: the last frames not recorded, a ring of all channels
		size_t m_historyFrames = 0;        // valid frames in it
		size_t m_historyStart = 0;         // the oldest frame's index
		uint64_t m_historyEnd = 0;         // the device frame after its newest one
		std::vector<float> m_trackSamples; // one track's frames, interleaved
		// MIDI recording: the setup it started with, its takes and what each track recorded, by channel.
		RecordSetup m_midiCurrent;
		bool m_midiRecording = false;
		std::vector<Take> m_midiTakes; // their offsets are the device frames they started at
		std::map<ObjectId, MidiTakeBuilder> m_midiBuilders;
		// MIDI recordings that ended while an audio recording still runs (its input comes later by the record
		// offset): recording.add gets them with it, as one undo step.
		Json m_pendingMidi = Json::array();
		RecordSetup m_pendingMidiSetup;
		std::map<ObjectId, Retrospective> m_retrospective; // by channel

		std::thread m_thread;
	};
}
