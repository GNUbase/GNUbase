// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/rendersnapshot.h"
#include "engine/peaks/peaks.h"
#include "engine/schema.h"

#include <atomic>
#include <chrono>
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
	class Engine;
}

namespace gnubase::engine::peaks
{
	enum class State : uint8_t
	{
		Pending,    // waiting for the worker
		Generating, // the worker reads the audio file; the peaks so far can be read
		Ready,
		Failed,
		Recording, // a recorder appends to it
	};

	const char* stateName(State state);

	struct Status
	{
		State state = State::Pending;
		double progress = 0.0;  // 0 to 1 while generating
		uint64_t frames = 0;    // the frames that have peaks
		std::string error;      // why it failed
		std::string file;       // the peak file it was loaded from or written to, if any
		bool generated = false; // read from the audio file rather than a peak file
	};

	struct CacheSettings
	{
		std::filesystem::path fallbackFolder = defaultFallbackFolder();
		size_t chunkFrames = 65536; // what the worker reads from the audio file at a time
		// Tests: called on the worker before it reads each chunk.
		std::function<void()> beforeChunk;
	};

	class PeakCache;

	namespace detail
	{
		struct Entry;
	}

	// Appends a recording's frames to its peaks while the file grows, so its waveform can be drawn while it records.
	// Not for audio threads: the disk thread that writes the file calls append.
	class PeakRecorder
	{
	public:
		// Made by PeakCache::record.
		PeakRecorder(std::shared_ptr<detail::Entry> entry, std::filesystem::path fallbackFolder);
		~PeakRecorder();
		PeakRecorder(const PeakRecorder&) = delete;
		PeakRecorder& operator=(const PeakRecorder&) = delete;

		// frames sample frames, channels interleaved, as written to the file.
		void append(const float* samples, size_t frames);

		// Once the file is complete and closed: writes the peak file for it, so the peaks are not generated again.
		// The peaks stay readable. Throws PeakFileError if no peak file could be written.
		void finish();

	private:
		std::shared_ptr<detail::Entry> m_entry;
		std::filesystem::path m_fallbackFolder;
		bool m_finished = false;
	};

	// The waveform overviews of the project's media files. A background worker loads each file's peaks from its peak
	// file, or generates them from the audio file and writes the peak file, when the media file is added; a removed
	// media file's generation is cancelled. Peaks are read while they are generated. A peak file is used only while
	// its audio file has the size, modification time and header hash it was made from; a query notices a changed
	// file and generates its peaks again.
	class PeakCache
	{
	public:
		// notify announces events from the worker: media.peaksProgress, media.peaksReady and media.peaksFailed.
		using Notify = std::function<void(const std::string& name, const Json& data)>;

		explicit PeakCache(Notify notify, CacheSettings settings = {});
		~PeakCache();
		PeakCache(const PeakCache&) = delete;
		PeakCache& operator=(const PeakCache&) = delete;

		// Command thread: the project's media files are now these. Starts on new files, cancels removed ones.
		void sync(const std::vector<RenderMedia>& media);

		// The status of a media file's peaks, or nothing for an unknown media file.
		std::optional<Status> status(ObjectId media) const;

		// Columns of a media file's peaks (see peaks::columns), and their status. If its audio file changed since its
		// peaks were made, they are generated again. Nothing for an unknown media file.
		struct Query
		{
			Status status;
			Columns columns;
		};
		std::optional<Query> query(ObjectId media, uint64_t start, std::optional<uint64_t> frames, size_t pixels);

		// Starts a recording's peaks: the audio file at path is written while the recorder receives its frames.
		std::unique_ptr<PeakRecorder> record(const std::string& path, int channels, double sampleRate);

		// Tests: waits until the worker has nothing left to do.
		bool waitIdle(std::chrono::milliseconds timeout);

		// media.peaks and media.peakStatus.
		void registerCommands(Engine& engine);

	private:
		using Entry = detail::Entry;

		struct Media
		{
			std::string path;
			uint64_t length = 0;
		};

		void run();
		void process(const std::shared_ptr<Entry>& entry);
		bool load(const std::shared_ptr<Entry>& entry, const SourceStamp& stamp);
		void generate(const std::shared_ptr<Entry>& entry, const SourceStamp& stamp);
		void fail(const std::shared_ptr<Entry>& entry, const std::string& error, std::optional<SourceStamp> stamp);
		void announce(const std::shared_ptr<Entry>& entry, const std::string& name, Json data);
		// Holding m_mutex: a new entry for path, queued for the worker; cancels the one it replaces.
		void restart(const std::string& path);
		// Whether a query should start the entry again: its audio file changed, or appeared after it failed.
		static bool stale(const std::shared_ptr<Entry>& entry);

		Notify m_notify;
		CacheSettings m_settings;
		mutable std::mutex m_mutex; // guards everything below except the thread
		std::map<ObjectId, Media> m_media;
		std::map<std::string, std::shared_ptr<Entry>> m_entries; // by audio file path
		std::deque<std::shared_ptr<Entry>> m_queue;
		bool m_busy = false;
		bool m_stopping = false;
		std::atomic<bool> m_stop{false}; // m_stopping for the worker between chunks
		std::condition_variable m_wake;
		std::condition_variable m_idle;
		std::thread m_thread;
	};
}
