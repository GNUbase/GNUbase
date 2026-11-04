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

	// A record-enabled MIDI or instrument track: what its channel's MIDI input plays is recorded.
	struct MidiRecordTrack
	{
		ObjectId track = 0;
		ObjectId channel = 0;

		bool operator==(const MidiRecordTrack& other) const { return track == other.track && channel == other.channel; }
	};

	// The MIDI messages that MIDI recording leaves out (the user setting midi.recordFilter, reference manual p. 1613):
	// by type, by MIDI channel and by controller number. They still play.
	struct MidiRecordFilter
	{
		std::bitset<9> types;     // by graph::ProcessEvent::Type
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
	class MidiTakeBuilder
	{
	public:
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
		};

		std::array<Sounding, size_t{16} * 128> m_sounding{};
		Json m_notes = Json::array();
		Json m_controllers = Json::array();
		time::Samples m_last = -1;
		size_t m_take = 0;
	};

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
				retrospective == other.retrospective && pauseFrames == other.pauseFrames;
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
		~Recorder(); // ends a recording that is still open
		Recorder(const Recorder&) = delete;
		Recorder& operator=(const Recorder&) = delete;

		// Command thread, while no audio thread calls process: blocks of up to maxFrames frames of channels input
		// channels from now on. Ends a recording that is still open.
		void prepare(int channels, uint32_t maxFrames);

		// Command thread: what the next recording records and where it goes. The one that runs keeps its own.
		void setup(RecordSetup setup);

		// Audio thread: a block of the device's input, each frame of which belongs where played says the transport
		// played the frame offset frames before it (recorded only if the transport recorded then), and what the
		// sequencers' MIDI input plays in the block, recorded where the transport plays it.
		void process(const audio::DeviceBlock& block, const PlayedPositions& played, int64_t offset,
			const MidiTap::Sequencers& sequencers) noexcept;

		// Input frames lost because the disk thread fell behind.
		uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }

		// Command thread: what the retrospective buffer of the channel holds, oldest first. The next input to it
		// starts the buffer afresh.
		std::vector<RetrospectiveEvent> retrospective(ObjectId channel);
		// Command thread: empties the channel's retrospective buffer, or every one for 0.
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
		};

		void run();
		// Disk thread, or the command thread, holding m_mutex.
		bool drain(); // handles the chunks the audio thread sent; false if there were none
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

		// Disk thread, and the command thread while it holds m_mutex.
		std::mutex m_mutex;
		std::condition_variable m_wake;
		bool m_stopping = false;
		RecordSetup m_next;    // for the next recording
		RecordSetup m_current; // the recording's
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
