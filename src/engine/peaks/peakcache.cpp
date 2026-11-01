// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/peaks/peakcache.h"

#include "engine/audiofile/audiofile.h"
#include "engine/engine.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace gnubase::engine::peaks
{
	namespace detail
	{
		// One audio file's peaks. A changed file gets a new entry; the worker drops a cancelled one.
		struct Entry
		{
			explicit Entry(std::string path) : path(std::move(path)) {}

			const std::string path;
			std::atomic<bool> cancelled{false};
			std::atomic<bool> recording{false};

			mutable std::mutex mutex; // guards everything below
			State state = State::Pending;
			std::shared_ptr<const PeakData> data; // once complete
			std::unique_ptr<PeakBuilder> builder; // while generating or recording
			std::optional<SourceStamp> stamp;     // of the audio file the peaks were made from
			double progress = 0.0;
			std::string error;
			std::string file;
			bool generated = false;

			// Holding mutex.
			Status status() const
			{
				Status status;
				status.state = state;
				status.progress = state == State::Ready ? 1.0 : progress;
				if (data)
					status.frames = data->frames;
				else if (builder)
					status.frames = builder->data().frames;
				status.error = error;
				status.file = file;
				status.generated = generated;
				return status;
			}
		};
	}

	namespace
	{
		constexpr int g_progressSteps = 20; // progress events at most every 5 %
		// A temporary peak file this old was left by a writer that crashed; writing one takes moments.
		constexpr std::chrono::seconds g_staleTemporaryAge = std::chrono::minutes(10);

		// Throws if columns of these channels would be too many values.
		void checkSize(size_t pixels, const std::vector<int>& channels, int fileChannels)
		{
			const size_t count = channels.empty() ? static_cast<size_t>(fileChannels) : channels.size();
			if (pixels * count > g_maxColumnValues)
				throw std::invalid_argument(std::to_string(pixels) + " pixels of " + std::to_string(count) +
					" channels are more than " + std::to_string(g_maxColumnValues) +
					" values: ask for fewer pixels or pick channels");
		}

		// Writes the peak file next to the audio file, or else into the fallback folder. Returns where, or nothing.
		std::string store(const std::string& source, const SourceStamp& stamp, const PeakData& data,
			const std::filesystem::path& fallbackFolder)
		{
			const std::filesystem::path audio = std::filesystem::u8path(source);
			const std::filesystem::path beside = besidePath(audio);
			try
			{
				writePeakFile(beside, source, stamp, data);
				return beside.u8string();
			}
			catch (const PeakFileError& error)
			{
				logging::info("Peaks of {} go to the cache folder: {}", source, error.what());
			}
			const std::filesystem::path fallback = fallbackPath(fallbackFolder, audio);
			try
			{
				std::error_code ignored;
				std::filesystem::create_directories(fallbackFolder, ignored);
				writePeakFile(fallback, source, stamp, data);
				return fallback.u8string();
			}
			catch (const PeakFileError& error)
			{
				logging::warning("Can't store the peaks of {}: {}", source, error.what());
			}
			return {};
		}

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		Json integerSchema()
		{
			return {{"type", "integer"}};
		}

		Json stateSchema()
		{
			return {{"type", "string"}, {"enum", {"pending", "generating", "ready", "failed", "recording"}}};
		}

		Json toJson(ObjectId media, const Status& status)
		{
			return {{"media", media}, {"status", stateName(status.state)}, {"progress", status.progress},
				{"frames", status.frames}, {"file", status.file}, {"error", status.error},
				{"generated", status.generated}};
		}
	}

	const char* stateName(State state)
	{
		switch (state)
		{
		case State::Pending:
			return "pending";
		case State::Generating:
			return "generating";
		case State::Ready:
			return "ready";
		case State::Failed:
			return "failed";
		case State::Recording:
			return "recording";
		}
		return "pending";
	}

	PeakRecorder::PeakRecorder(PeakCache& cache, std::shared_ptr<detail::Entry> entry)
		: m_cache(cache), m_entry(std::move(entry))
	{
	}

	PeakRecorder::~PeakRecorder()
	{
		if (m_finished)
			return;
		// An unfinished recording keeps its peaks without a stamp, so a query generates them again from the file.
		const std::scoped_lock lock(m_entry->mutex);
		if (m_entry->builder)
		{
			m_entry->data = std::make_shared<const PeakData>(m_entry->builder->take());
			m_entry->builder.reset();
		}
		m_entry->state = State::Ready;
		m_entry->recording = false;
	}

	void PeakRecorder::append(const float* samples, size_t frames)
	{
		rt::assertNotRealtime("peaks::PeakRecorder::append");
		const std::scoped_lock lock(m_entry->mutex);
		if (m_entry->builder)
			m_entry->builder->append(samples, frames);
	}

	void PeakRecorder::finish()
	{
		rt::assertNotRealtime("peaks::PeakRecorder::finish");
		if (m_finished)
			return;
		{
			const std::scoped_lock lock(m_entry->mutex);
			if (m_entry->builder)
			{
				m_entry->data = std::make_shared<const PeakData>(m_entry->builder->take());
				m_entry->builder.reset();
			}
			m_entry->state = State::Ready;
			m_entry->generated = true;
		}
		m_finished = true;
		// Hashing the file and writing the peak file take a while: the worker does it, not the recorder's thread.
		m_cache.finishRecording(m_entry);
	}

	PeakCache::PeakCache(Notify notify, CacheSettings settings)
		: m_notify(std::move(notify)), m_settings(std::move(settings))
	{
		if (m_settings.chunkFrames == 0)
			throw std::invalid_argument("the peak cache needs a chunk size");
		m_thread = std::thread([this] { run(); });
	}

	PeakCache::~PeakCache()
	{
		{
			const std::scoped_lock lock(m_mutex);
			m_stopping = true;
			m_stop = true;
		}
		m_wake.notify_all();
		m_thread.join();
	}

	void PeakCache::sync(const std::vector<RenderMedia>& media)
	{
		const std::scoped_lock lock(m_mutex);
		std::map<ObjectId, Media> next;
		for (const RenderMedia& file : media)
			next[file.id] = {file.path, static_cast<uint64_t>(std::max<int64_t>(file.length, 0))};
		const bool same =
			std::equal(next.begin(), next.end(), m_media.begin(), m_media.end(), [](const auto& a, const auto& b)
				{ return a.first == b.first && a.second.path == b.second.path && a.second.length == b.second.length; });
		if (same)
			return;
		m_media = std::move(next);

		std::set<std::string> paths;
		for (const auto& [id, file] : m_media)
		{
			paths.insert(file.path);
			if (m_entries.count(file.path) == 0)
				restart(file.path);
		}
		for (auto it = m_entries.begin(); it != m_entries.end();)
		{
			if (paths.count(it->first) == 0 && !it->second->recording)
			{
				it->second->cancelled = true;
				it = m_entries.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	void PeakCache::restart(const std::string& path)
	{
		auto entry = std::make_shared<Entry>(path);
		auto& slot = m_entries[path];
		if (slot)
			slot->cancelled = true;
		slot = entry;
		m_queue.push_back({std::move(entry), false});
		m_wake.notify_one();
	}

	void PeakCache::finishRecording(std::shared_ptr<Entry> entry)
	{
		{
			const std::scoped_lock lock(m_mutex);
			m_queue.push_back({std::move(entry), true});
		}
		m_wake.notify_one();
	}

	std::optional<Status> PeakCache::status(ObjectId media) const
	{
		std::shared_ptr<Entry> entry;
		{
			const std::scoped_lock lock(m_mutex);
			const auto found = m_media.find(media);
			if (found == m_media.end())
				return std::nullopt;
			entry = m_entries.at(found->second.path);
		}
		const std::scoped_lock lock(entry->mutex);
		return entry->status();
	}

	bool PeakCache::stale(const std::shared_ptr<Entry>& entry)
	{
		if (entry->recording)
			return false;
		const std::filesystem::path file = std::filesystem::u8path(entry->path);
		const std::scoped_lock lock(entry->mutex);
		switch (entry->state)
		{
		case State::Ready:
			return !entry->stamp || !sameSizeAndTime(file, *entry->stamp);
		case State::Failed:
		{
			if (entry->stamp)
				return !sameSizeAndTime(file, *entry->stamp);
			std::error_code error;
			return std::filesystem::is_regular_file(file, error);
		}
		case State::Pending:
		case State::Generating:
		case State::Recording:
			break;
		}
		return false;
	}

	std::optional<PeakCache::Query> PeakCache::query(
		ObjectId media, uint64_t start, std::optional<uint64_t> frames, size_t pixels, const std::vector<int>& channels)
	{
		std::shared_ptr<Entry> entry;
		uint64_t length = 0;
		{
			const std::scoped_lock lock(m_mutex);
			const auto found = m_media.find(media);
			if (found == m_media.end())
				return std::nullopt;
			length = found->second.length;
			entry = m_entries.at(found->second.path);
			if (stale(entry))
			{
				restart(entry->path);
				entry = m_entries.at(found->second.path);
			}
		}

		Query result;
		std::shared_ptr<const PeakData> data;
		std::optional<ColumnSource> source;
		uint64_t count = 0;
		{
			const std::scoped_lock lock(entry->mutex);
			result.status = entry->status();
			data = entry->data;
			const uint64_t whole = length > 0 ? length : result.status.frames;
			count = frames.value_or(whole > start ? whole - start : 0);
			// Peaks that are still being built: only the few the columns need are copied under the lock, so a
			// recorder's append never waits for a query.
			if (!data && entry->builder && count > 0)
			{
				checkSize(pixels, channels, entry->builder->data().channels);
				source = columnSource(entry->builder->data(), start, count, pixels, channels);
			}
		}
		// Complete peaks are immutable.
		if (data && count > 0)
		{
			checkSize(pixels, channels, data->channels);
			source = columnSource(*data, start, count, pixels, channels);
		}
		if (source)
			result.columns = columns(*source, start, count, pixels);
		if (result.columns.pixels == 0)
		{
			// Nothing to show yet.
			result.columns.factor = defaultLevels()[0];
			result.columns.pixels = pixels;
		}
		return result;
	}

	std::unique_ptr<PeakRecorder> PeakCache::record(const std::string& path, int channels, double sampleRate)
	{
		auto entry = std::make_shared<Entry>(path);
		entry->recording = true;
		entry->state = State::Recording;
		entry->builder = std::make_unique<PeakBuilder>(channels, sampleRate);
		{
			const std::scoped_lock lock(m_mutex);
			auto& slot = m_entries[path];
			if (slot)
				slot->cancelled = true;
			slot = entry;
		}
		return std::make_unique<PeakRecorder>(*this, std::move(entry));
	}

	bool PeakCache::waitIdle(std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m_mutex);
		return m_idle.wait_for(lock, timeout, [this] { return m_queue.empty() && !m_busy; });
	}

	void PeakCache::run()
	{
		for (;;)
		{
			Job job;
			{
				std::unique_lock lock(m_mutex);
				m_busy = false;
				m_idle.notify_all();
				m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
				if (m_stopping)
					return;
				job = std::move(m_queue.front());
				m_queue.pop_front();
				m_busy = true;
			}
			// Once there is work, not at startup: a cache that is never used touches no folder.
			sweep(m_settings.fallbackFolder);
			try
			{
				if (job.finishRecording)
					storeRecording(job.entry);
				else
					process(job.entry);
			}
			catch (const std::exception& error)
			{
				// With the file's size and time, so it is only tried again once it changes.
				fail(job.entry, error.what(), sizeAndTime(std::filesystem::u8path(job.entry->path)));
				job.entry->recording = false;
			}
		}
	}

	void PeakCache::sweep(const std::filesystem::path& folder)
	{
		if (!m_swept.insert(folder).second)
			return;
		if (const size_t removed = removeStaleTemporaries(folder, g_staleTemporaryAge); removed > 0)
			logging::info("Removed {} temporary peak files left behind in {}", removed, folder.u8string());
	}

	void PeakCache::process(const std::shared_ptr<Entry>& entry)
	{
		if (entry->cancelled)
			return;
		const std::filesystem::path audio = std::filesystem::u8path(entry->path);
		sweep(audio.parent_path());
		const std::optional<SourceStamp> stamp = stampOf(audio);
		if (!stamp)
		{
			// An audio file that exists but can't be read, e.g. without permission or locked by another program, keeps
			// its size and time, so it is only tried again once it changes. One that doesn't exist has none: it is
			// tried again once it appears.
			fail(entry, "can't read " + entry->path, sizeAndTime(audio));
			return;
		}
		try
		{
			if (!load(entry, *stamp))
				generate(entry, *stamp);
		}
		catch (const std::exception& error)
		{
			// With the stamp, so the file is only tried again once it changes.
			fail(entry, error.what(), stamp);
		}
	}

	bool PeakCache::load(const std::shared_ptr<Entry>& entry, const SourceStamp& stamp)
	{
		const std::filesystem::path audio = std::filesystem::u8path(entry->path);
		for (const bool beside : {true, false})
		{
			const std::filesystem::path file =
				beside ? besidePath(audio) : fallbackPath(m_settings.fallbackFolder, audio);
			std::error_code error;
			if (!std::filesystem::is_regular_file(file, error))
				continue;
			try
			{
				PeakFile contents = readPeakFile(file);
				// A peak file next to its audio file moves with it; one in the cache folder must name it.
				if (contents.stamp != stamp || (!beside && contents.source != entry->path))
					continue;
				const uint64_t frames = contents.data.frames;
				{
					const std::scoped_lock lock(entry->mutex);
					entry->data = std::make_shared<const PeakData>(std::move(contents.data));
					entry->stamp = stamp;
					entry->state = State::Ready;
					entry->file = file.u8string();
					entry->generated = false;
				}
				announce(entry, "media.peaksReady", {{"frames", frames}, {"generated", false}});
				return true;
			}
			catch (const std::exception& problem)
			{
				// Damaged, foreign or unreadable: the peaks are generated again and replace it.
				logging::info("Ignoring the peak file {}: {}", file.u8string(), problem.what());
			}
		}
		return false;
	}

	void PeakCache::generate(const std::shared_ptr<Entry>& entry, const SourceStamp& stamp)
	{
		std::unique_ptr<AudioFileReader> reader;
		try
		{
			reader = AudioFileReader::open(entry->path);
		}
		catch (const AudioFileError& error)
		{
			fail(entry, error.what(), stamp);
			return;
		}
		const AudioFileInfo info = reader->info();
		{
			const std::scoped_lock lock(entry->mutex);
			entry->state = State::Generating;
			entry->builder = std::make_unique<PeakBuilder>(info.channels, info.sampleRate);
			entry->stamp = stamp;
		}

		std::vector<float> buffer(m_settings.chunkFrames * static_cast<size_t>(info.channels));
		const auto total = static_cast<uint64_t>(std::max<int64_t>(info.frames, 0));
		uint64_t done = 0;
		int step = 0;
		for (;;)
		{
			if (m_settings.beforeChunk)
				m_settings.beforeChunk();
			if (entry->cancelled || m_stop)
				return;
			const size_t frames = reader->read(buffer.data(), m_settings.chunkFrames);
			if (frames == 0)
				break;
			done += frames;
			{
				const std::scoped_lock lock(entry->mutex);
				entry->builder->append(buffer.data(), frames);
				entry->progress =
					total > 0 ? std::min(1.0, static_cast<double>(done) / static_cast<double>(total)) : 0.0;
			}
			if (done<total&& static_cast<int>(done * g_progressSteps / total)> step)
			{
				step = static_cast<int>(done * g_progressSteps / total);
				announce(entry, "media.peaksProgress",
					{{"progress", static_cast<double>(done) / static_cast<double>(total)}, {"frames", done}});
			}
		}

		std::shared_ptr<const PeakData> data;
		{
			const std::scoped_lock lock(entry->mutex);
			data = std::make_shared<const PeakData>(entry->builder->take());
			entry->data = data;
			entry->builder.reset();
		}
		reader.reset(); // ready peaks leave the audio file closed: Windows can't move or remove a file that is open
		if (entry->cancelled || m_stop)
			return;
		const std::string file = store(entry->path, stamp, *data, m_settings.fallbackFolder);
		{
			const std::scoped_lock lock(entry->mutex);
			entry->state = State::Ready;
			entry->progress = 1.0;
			entry->file = file;
			entry->generated = true;
		}
		announce(entry, "media.peaksReady", {{"frames", data->frames}, {"generated", true}});
	}

	void PeakCache::storeRecording(const std::shared_ptr<Entry>& entry)
	{
		std::shared_ptr<const PeakData> data;
		{
			const std::scoped_lock lock(entry->mutex);
			data = entry->data;
		}
		if (entry->cancelled || !data)
		{
			entry->recording = false;
			return;
		}
		const std::filesystem::path audio = std::filesystem::u8path(entry->path);
		sweep(audio.parent_path());
		const std::optional<SourceStamp> stamp = stampOf(audio);
		const std::string file = stamp ? store(entry->path, *stamp, *data, m_settings.fallbackFolder) : std::string();
		std::string error;
		if (!stamp)
			error = "can't read " + entry->path;
		else if (file.empty())
			error = "can't write the peaks of " + entry->path;
		{
			const std::scoped_lock lock(entry->mutex);
			entry->stamp = stamp;
			entry->file = file;
			entry->error = error;
		}
		entry->recording = false; // from now on a query notices when the file changes
		if (!error.empty())
			logging::warning("The peaks of the recording {} are not kept: {}", entry->path, error);
		announce(entry, "media.peaksReady", {{"frames", data->frames}, {"generated", true}});
	}

	void PeakCache::fail(
		const std::shared_ptr<Entry>& entry, const std::string& error, std::optional<SourceStamp> stamp)
	{
		{
			const std::scoped_lock lock(entry->mutex);
			entry->state = State::Failed;
			entry->error = error;
			entry->stamp = stamp;
			entry->builder.reset();
		}
		announce(entry, "media.peaksFailed", {{"error", error}});
	}

	void PeakCache::announce(const std::shared_ptr<Entry>& entry, const std::string& name, Json data)
	{
		std::vector<ObjectId> media;
		{
			const std::scoped_lock lock(m_mutex);
			const auto found = m_entries.find(entry->path);
			if (found == m_entries.end() || found->second != entry)
				return;
			for (const auto& [id, file] : m_media)
				if (file.path == entry->path)
					media.push_back(id);
		}
		for (const ObjectId id : media)
		{
			data["media"] = id;
			m_notify(name, data);
		}
	}

	void PeakCache::registerCommands(Engine& engine)
	{
		const Json columnsSchema = {{"type", "object"},
			{"properties", {{"channel", integerSchema()}, {"min", {{"type", "array"}}}, {"max", {{"type", "array"}}}}},
			{"required", {"channel", "min", "max"}}};
		engine.registerCommand(
			{"media.peaks", 1,
				"The waveform overview of a media file for drawing: frames sample frames of the file from start "
				"(default: the rest of the file), in pixels columns (up to 65536). Per channel (default: all of them; "
				"channels picks some, numbered from 0), the smallest and largest sample of each column, from -32767 "
				"to 32767 (full scale). Pixels times channels can be up to 262144. The columns come from the coarsest "
				"level of the file's peaks (level: 256, 4096 or 65536 frames per peak) that has a peak per column, so "
				"they cost the same at any length. status says whether the peaks are ready; while they are "
				"generated, only the first available frames have columns and the rest are 0. A file that changed "
				"since its peaks were made gets new ones; media.peaksProgress and media.peaksReady report how that "
				"goes.",
				{{"type", "object"},
					{"properties",
						{{"media", integerSchema()}, {"start", integerSchema()}, {"frames", integerSchema()},
							{"pixels", integerSchema()},
							{"channels", {{"type", "array"}, {"items", integerSchema()}}}}},
					{"required", {"media", "pixels"}}, {"additionalProperties", false}},
				{{"type", "object"},
					{"properties",
						{{"media", integerSchema()}, {"status", stateSchema()}, {"level", integerSchema()},
							{"available", integerSchema()},
							{"channels", {{"type", "array"}, {"items", columnsSchema}}}}},
					{"required", {"media", "status", "level", "available", "channels"}}},
				true},
			[this](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto media = parameters.at("media").get<ObjectId>();
				const auto start = parameters.value("start", int64_t{0});
				require(start >= 0, "start must not be negative");
				std::optional<uint64_t> frames;
				if (parameters.contains("frames"))
				{
					const auto count = parameters.at("frames").get<int64_t>();
					require(count > 0, "frames must be positive");
					frames = static_cast<uint64_t>(count);
				}
				const auto pixels = parameters.at("pixels").get<int64_t>();
				require(pixels > 0 && static_cast<uint64_t>(pixels) <= g_maxPixels,
					"pixels must be from 1 to " + std::to_string(g_maxPixels));
				std::vector<int> picked;
				if (parameters.contains("channels"))
				{
					for (const Json& channel : parameters.at("channels"))
					{
						const auto index = channel.get<int64_t>();
						require(index >= 0 && index < 256, "there is no channel " + std::to_string(index));
						picked.push_back(static_cast<int>(index));
					}
					require(!picked.empty(), "channels must name at least one channel");
				}
				const std::optional<Query> result =
					query(media, static_cast<uint64_t>(start), frames, static_cast<size_t>(pixels), picked);
				require(result.has_value(), "no media file " + std::to_string(media));
				const Columns& columns = result->columns;
				Json channels = Json::array();
				for (int channel = 0; channel < columns.channels; ++channel)
				{
					const auto first = static_cast<std::ptrdiff_t>(static_cast<size_t>(channel) * columns.pixels);
					const auto last = first + static_cast<std::ptrdiff_t>(columns.pixels);
					const int index = picked.empty() ? channel : picked[static_cast<size_t>(channel)];
					channels.push_back({{"channel", index},
						{"min", std::vector<int16_t>(columns.min.begin() + first, columns.min.begin() + last)},
						{"max", std::vector<int16_t>(columns.max.begin() + first, columns.max.begin() + last)}});
				}
				return Json{{"media", media}, {"status", stateName(result->status.state)}, {"level", columns.factor},
					{"available", result->status.frames}, {"channels", channels}};
			});

		const Json statusSchema = {{"type", "object"},
			{"properties",
				{{"media", integerSchema()}, {"status", stateSchema()}, {"progress", {{"type", "number"}}},
					{"frames", integerSchema()}, {"file", {{"type", "string"}}}, {"error", {{"type", "string"}}},
					{"generated", {{"type", "boolean"}}}}},
			{"required", {"media", "status", "progress", "frames", "file", "error", "generated"}}};
		engine.registerCommand(
			{"media.peakStatus", 1,
				"The state of the waveform overviews of a media file, or of every media file: pending, generating "
				"(with progress from 0 to 1), ready, failed (with the error) or recording; the frames that have "
				"peaks; the peak file they were loaded from or written to; whether they were generated from the audio "
				"file rather than loaded. Peak files are kept next to their audio file as <file name>.gbpeaks, or in "
				"fallbackFolder where that can't be written.",
				{{"type", "object"}, {"properties", {{"media", integerSchema()}}}, {"additionalProperties", false}},
				{{"type", "object"},
					{"properties",
						{{"media", {{"type", "array"}, {"items", statusSchema}}},
							{"fallbackFolder", {{"type", "string"}}}}},
					{"required", {"media", "fallbackFolder"}}},
				true},
			[this](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				std::vector<ObjectId> ids;
				if (parameters.contains("media"))
				{
					ids.push_back(parameters.at("media").get<ObjectId>());
				}
				else
				{
					const std::scoped_lock lock(m_mutex);
					for (const auto& entry : m_media)
						ids.push_back(entry.first);
				}
				Json list = Json::array();
				for (const ObjectId id : ids)
				{
					const std::optional<Status> found = status(id);
					require(found.has_value(), "no media file " + std::to_string(id));
					list.push_back(toJson(id, *found));
				}
				return Json{{"media", list}, {"fallbackFolder", m_settings.fallbackFolder.u8string()}};
			});
	}
}
