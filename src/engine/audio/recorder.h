// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/transport.h"
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/schema.h"
#include "platform/audio/device.h"
#include "platform/rt/spscqueue.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
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

	// The format of the project's record files: 32-bit float WAV at its sample rate, until the project setup has a
	// record file type and bit depth (#78); then this function reads them.
	WriteOptions recordFormat(const Project& project);

	// Where and how a recording that starts writes its files.
	struct RecordSetup
	{
		std::filesystem::path folder;
		WriteOptions format; // the channels come from each track
		AudioRecordMode mode = AudioRecordMode::KeepHistory;
		int64_t preRecordFrames = 0; // audio pre-record, in device frames
		std::vector<RecordTrack> tracks;

		bool operator==(const RecordSetup& other) const
		{
			return folder == other.folder && format.format == other.format.format &&
				format.sampleFormat == other.format.sampleFormat && format.sampleRate == other.format.sampleRate &&
				mode == other.mode && preRecordFrames == other.preRecordFrames && tracks == other.tracks;
		}
	};

	// Records the input of record-enabled audio tracks while the transport records (docs/features/recording.md).
	// The audio thread hands each block's input, with the project position each frame belongs at, to the recorder's
	// disk thread through lock-free queues; it never waits, and a block that finds no free chunk is lost and counted.
	// The disk thread writes one file per track from where recording starts to where it ends, and a take per run of
	// consecutive positions: a cycle lap, or what was recorded between a punch in and a punch out. When the recording
	// ends it reports the files and their takes as recording.add takes them.
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
		// played the frame offset frames before it (recorded only if the transport recorded then).
		void process(const audio::DeviceBlock& block, const PlayedPositions& played, int64_t offset) noexcept;

		// Input frames lost because the disk thread fell behind.
		uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }

	private:
		// Consecutive device frames, recorded at consecutive positions from position, or not recorded (-1).
		struct Chunk
		{
			uint64_t deviceFrame = 0;
			time::Samples position = -1;
			uint32_t frames = 0;
			std::vector<float> samples; // every input channel, interleaved
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

		void run();
		// Disk thread, holding m_mutex.
		void handle(const Chunk& chunk);
		void begin(const Chunk& chunk);
		void write(const float* samples, uint32_t frames);
		void end();
		void keepHistory(const Chunk& chunk);
		void fail(const std::string& reason);
		// Audio thread: hands frames frames from offset in the block on, or false if no chunk is free.
		bool send(const audio::DeviceBlock& block, uint32_t offset, uint32_t frames, time::Samples position) noexcept;

		Notify m_notify;

		// Set by prepare, while no audio thread runs.
		int m_channels = 0;
		uint32_t m_maxFrames = 0;
		std::vector<Chunk> m_chunks;
		std::unique_ptr<rt::SpscQueue<Chunk*>> m_free;   // disk thread to audio thread
		std::unique_ptr<rt::SpscQueue<Chunk*>> m_filled; // audio thread to disk thread

		// Audio thread.
		bool m_sending = false; // the last chunk sent was recorded: the disk thread waits for the end
		std::atomic<bool> m_armed{false};
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

		std::thread m_thread;
	};
}
