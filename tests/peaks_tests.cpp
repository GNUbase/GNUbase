// SPDX-License-Identifier: GPL-3.0-or-later
// Waveform overviews (#83): peak levels, peak files, the background cache and media.peaks.
#include "engine/audiofile/audiofile.h"
#include "engine/engine.h"
#include "engine/peaks/peakcache.h"
#include "engine/peaks/peaks.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
namespace peaks = gnubase::engine::peaks;
namespace test = gnubase::test;
using Catch::Matchers::ContainsSubstring;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;

	// Noise with a slow sine under it, a few samples beyond full scale and a NaN, so the extremes are tested too.
	std::vector<float> testSignal(int channels, size_t frames, uint32_t seed = 1)
	{
		std::mt19937 random(seed);
		std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
		std::vector<float> samples(frames * static_cast<size_t>(channels));
		for (size_t frame = 0; frame < frames; ++frame)
			for (int channel = 0; channel < channels; ++channel)
				samples[(frame * static_cast<size_t>(channels)) + static_cast<size_t>(channel)] =
					static_cast<float>(0.6 * std::sin(2.0 * g_pi * static_cast<double>(frame) / (5000.0 + channel))) +
					noise(random);
		if (frames > 1000)
		{
			samples[100] = 1.5f;
			samples[201] = -2.0f;
			samples[302] = std::numeric_limits<float>::quiet_NaN();
		}
		return samples;
	}

	// Through a parameter: the tests want the same sequence every run.
	uint32_t seedOf(uint32_t seed)
	{
		return seed;
	}

	int16_t quantizeMin(float sample)
	{
		if (std::isnan(sample))
			return 0;
		return static_cast<int16_t>(std::clamp(std::floor(static_cast<double>(sample) * 32767.0), -32767.0, 32767.0));
	}

	int16_t quantizeMax(float sample)
	{
		if (std::isnan(sample))
			return 0;
		return static_cast<int16_t>(std::clamp(std::ceil(static_cast<double>(sample) * 32767.0), -32767.0, 32767.0));
	}

	// The smallest and largest value of a channel over frames [begin, end), the slow way.
	std::pair<int16_t, int16_t> scan(
		const std::vector<float>& samples, int channels, int channel, uint64_t begin, uint64_t end)
	{
		int16_t low = 32767;
		int16_t high = -32767;
		bool any = false;
		for (uint64_t frame = begin; frame < end; ++frame)
		{
			const float sample = samples[(frame * static_cast<uint64_t>(channels)) + static_cast<uint64_t>(channel)];
			if (std::isnan(sample))
				continue;
			low = std::min(low, quantizeMin(sample));
			high = std::max(high, quantizeMax(sample));
			any = true;
		}
		return any ? std::make_pair(low, high) : std::make_pair(int16_t{0}, int16_t{0});
	}

	// Every peak of every level against a brute-force scan of the samples.
	void checkAgainstScan(const peaks::PeakData& data, const std::vector<float>& samples, int channels)
	{
		const uint64_t frames = samples.size() / static_cast<size_t>(channels);
		REQUIRE(data.channels == channels);
		REQUIRE(data.frames == frames);
		for (const peaks::Level& level : data.levels)
		{
			CAPTURE(level.factor);
			REQUIRE(level.peaks == (frames + level.factor - 1) / level.factor);
			REQUIRE(level.values.size() == level.peaks * static_cast<uint64_t>(channels) * 2);
			for (uint64_t peak = 0; peak < level.peaks; ++peak)
				for (int channel = 0; channel < channels; ++channel)
				{
					const auto [low, high] = scan(samples, channels, channel, peak * level.factor,
						std::min<uint64_t>((peak + 1) * level.factor, frames));
					CAPTURE(peak, channel);
					REQUIRE(level.min(peak, channel, channels) == low);
					REQUIRE(level.max(peak, channel, channels) == high);
				}
		}
	}

	peaks::PeakData build(const std::vector<float>& samples, int channels, const std::vector<uint32_t>& levels,
		double sampleRate = 48000.0)
	{
		peaks::PeakBuilder builder(channels, sampleRate, levels);
		builder.append(samples.data(), samples.size() / static_cast<size_t>(channels));
		return builder.take();
	}

	void writeAudio(
		const std::filesystem::path& file, const std::vector<float>& samples, int channels, double sampleRate = 48000.0)
	{
		WriteOptions options;
		options.sampleRate = sampleRate;
		options.channels = channels;
		options.sampleFormat = SampleFormat::Float32;
		const auto writer = AudioFileWriter::create(file.u8string(), options);
		writer->write(samples.data(), samples.size() / static_cast<size_t>(channels));
		writer->close();
	}

	// Collects what a cache announces.
	class Announcements
	{
	public:
		peaks::PeakCache::Notify notify()
		{
			return [this](const std::string& name, const Json& data)
			{
				const std::scoped_lock lock(m_mutex);
				m_events.emplace_back(name, data);
				m_arrived.notify_all();
			};
		}

		// The data of the first event named name, waiting up to timeout for it; null if none came.
		Json waitFor(const std::string& name, std::chrono::milliseconds timeout = std::chrono::seconds(20))
		{
			std::unique_lock lock(m_mutex);
			Json found;
			m_arrived.wait_for(lock, timeout,
				[&]
				{
					const auto it = std::find_if(m_events.begin(), m_events.end(),
						[&name](const std::pair<std::string, Json>& event) { return event.first == name; });
					if (it == m_events.end())
						return false;
					found = it->second;
					m_events.erase(it);
					return true;
				});
			return found;
		}

		size_t count(const std::string& name)
		{
			const std::scoped_lock lock(m_mutex);
			return static_cast<size_t>(std::count_if(m_events.begin(), m_events.end(),
				[&name](const std::pair<std::string, Json>& event) { return event.first == name; }));
		}

	private:
		std::mutex m_mutex;
		std::condition_variable m_arrived;
		std::vector<std::pair<std::string, Json>> m_events;
	};

	RenderMedia mediaFile(ObjectId id, const std::filesystem::path& path, uint64_t length = 0)
	{
		RenderMedia media;
		media.id = id;
		media.path = path.u8string();
		media.length = static_cast<int64_t>(length);
		return media;
	}

	// Columns of the whole file, against the data it was made from.
	void checkColumns(const peaks::Columns& columns, const peaks::PeakData& data)
	{
		const peaks::Columns expected = peaks::columns(data, 0, data.frames, columns.pixels);
		CHECK(columns.factor == expected.factor);
		CHECK(columns.min == expected.min);
		CHECK(columns.max == expected.max);
	}

	// The next event named name in a client's queue, waiting up to timeout; null if none came.
	Json waitForEvent(
		Client& client, const std::string& name, std::chrono::milliseconds timeout = std::chrono::seconds(20))
	{
		const auto until = std::chrono::steady_clock::now() + timeout;
		while (std::chrono::steady_clock::now() < until)
		{
			const auto left =
				std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
			std::optional<Message> message = client.wait(std::max(left, std::chrono::milliseconds(1)));
			if (!message)
				break;
			if (const auto* event = std::get_if<Event>(&*message); event != nullptr && event->name == name)
				return event->data;
		}
		return nullptr;
	}

	peaks::Status statusOf(const peaks::PeakCache& cache, ObjectId media)
	{
		const std::optional<peaks::Status> status = cache.status(media);
		REQUIRE(status.has_value());
		return status.value_or(peaks::Status{});
	}

	peaks::PeakCache::Query queryOf(
		peaks::PeakCache& cache, ObjectId media, uint64_t start, std::optional<uint64_t> frames, size_t pixels)
	{
		std::optional<peaks::PeakCache::Query> query = cache.query(media, start, frames, pixels);
		REQUIRE(query.has_value());
		return std::move(query).value_or(peaks::PeakCache::Query{});
	}

	peaks::SourceStamp stampOf(const std::filesystem::path& file)
	{
		const std::optional<peaks::SourceStamp> stamp = peaks::stampOf(file);
		REQUIRE(stamp.has_value());
		return stamp.value_or(peaks::SourceStamp{});
	}

	Json run(Client& client, const std::string& command, const Json& parameters)
	{
		const Result result = client.call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}
}

TEST_CASE("every level of a peak builder matches a brute-force scan of the samples", "[unit]")
{
	const std::vector<float> stereo = testSignal(2, (3 * 65536) + 12345);
	checkAgainstScan(build(stereo, 2, peaks::defaultLevels()), stereo, 2);
	CHECK(peaks::defaultLevels() == std::vector<uint32_t>{256, 4096, 65536});

	const std::vector<float> mono = testSignal(1, 4000, 7);
	checkAgainstScan(build(mono, 1, {4, 16, 64, 128}), mono, 1);

	// Exact block lengths, a single frame and no frames.
	const std::vector<float> exact = testSignal(3, 4096, 3);
	checkAgainstScan(build(exact, 3, {16, 256}), exact, 3);
	const std::vector<float> one = {0.25f, -0.5f};
	checkAgainstScan(build(one, 2, {16, 256}), one, 2);
	const peaks::PeakData empty = build({}, 2, {16, 256});
	CHECK(empty.frames == 0);
	CHECK(empty.levels.size() == 2);
	CHECK(empty.levels[0].peaks == 0);

	CHECK_THROWS(peaks::PeakBuilder(2, 48000.0, {16, 24}));
	CHECK_THROWS(peaks::PeakBuilder(0, 48000.0, {16}));
}

TEST_CASE("appending in pieces gives complete peaks after every append, the same as at once", "[unit]")
{
	const std::vector<uint32_t> levels = {8, 32, 128};
	const std::vector<float> samples = testSignal(2, 5000, 11);
	peaks::PeakBuilder builder(2, 44100.0, levels);
	std::mt19937 random(seedOf(5));
	std::uniform_int_distribution<size_t> piece(0, 300);
	size_t done = 0;
	while (done < 5000)
	{
		const size_t frames = std::min<size_t>(piece(random), 5000 - done);
		builder.append(samples.data() + (done * 2), frames);
		done += frames;
		const std::vector<float> prefix(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(done * 2));
		checkAgainstScan(builder.data(), prefix, 2);
	}
	CHECK(builder.data() == build(samples, 2, levels, 44100.0));
	CHECK(builder.data().sampleRate == 44100.0);
}

TEST_CASE("columns come from the coarsest level with a peak per column and cover their frames", "[unit]")
{
	const std::vector<uint32_t> levels = {16, 256, 4096};
	const std::vector<float> samples = testSignal(2, 100000, 13);
	const peaks::PeakData data = build(samples, 2, levels);

	struct Case
	{
		uint64_t start;
		uint64_t frames;
		size_t pixels;
		uint32_t factor;
	};
	for (const Case& c : {Case{0, 100000, 20, 4096}, Case{0, 100000, 1000, 16}, Case{0, 100000, 300, 256},
			 Case{12345, 5000, 7, 256}, Case{99990, 10, 50, 16}, Case{50000, 3000, 3000, 16}})
	{
		CAPTURE(c.start, c.frames, c.pixels);
		const peaks::Columns columns = peaks::columns(data, c.start, c.frames, c.pixels);
		CHECK(columns.factor == c.factor);
		REQUIRE(columns.channels == 2);
		REQUIRE(columns.pixels == c.pixels);
		REQUIRE(columns.min.size() == 2 * c.pixels);
		CHECK(columns.covered == c.pixels);
		for (size_t i = 0; i < c.pixels; ++i)
		{
			const uint64_t begin = c.start + (i * c.frames / c.pixels);
			const uint64_t end = std::max(begin + 1, c.start + ((i + 1) * c.frames / c.pixels));
			// Widened to whole peaks of the level.
			const uint64_t from = begin / c.factor * c.factor;
			const uint64_t to = std::min<uint64_t>((end + c.factor - 1) / c.factor * c.factor, 100000);
			for (int channel = 0; channel < 2; ++channel)
			{
				CAPTURE(i, channel);
				const auto [low, high] = scan(samples, 2, channel, from, to);
				CHECK(columns.min[(static_cast<size_t>(channel) * c.pixels) + i] == low);
				CHECK(columns.max[(static_cast<size_t>(channel) * c.pixels) + i] == high);
			}
		}
	}

	// Beyond the frames that have peaks, columns are empty.
	const peaks::Columns beyond = peaks::columns(data, 50000, 100000, 10);
	CHECK(beyond.covered == 5);
	CHECK(beyond.min[9] == 0);
	CHECK(beyond.max[9] == 0);
	CHECK(beyond.max[4] != 0);
	CHECK_THROWS(peaks::columns(data, 0, 100, 0));
}

TEST_CASE("peak files round trip, and damaged ones are rejected", "[unit]")
{
	const test::TempDir dir;
	peaks::PeakFile contents;
	contents.source = (dir.path() / u8"Aufnahme ü.wav").u8string();
	contents.stamp = {123456789, -987654321, 0xfedcba9876543210ULL};
	const std::vector<float> samples = testSignal(2, 70000, 17);
	contents.data = build(samples, 2, peaks::defaultLevels());
	contents.data.sampleRate = 44100.0;

	const std::filesystem::path file = dir.path() / "take.gbpeaks";
	peaks::writePeakFile(file, contents.source, contents.stamp, contents.data);
	const peaks::PeakFile read = peaks::readPeakFile(file);
	CHECK(read.source == contents.source);
	CHECK(read.stamp == contents.stamp);
	CHECK(read.data == contents.data);
	CHECK(read.data.sampleRate == 44100.0);
	// Only the file itself: no temporary files left behind.
	CHECK(std::distance(std::filesystem::directory_iterator(dir.path()), std::filesystem::directory_iterator()) == 1);

	const std::string bytes = test::readText(file);
	const auto damage = [&](const std::string& changed, const std::string& reason)
	{
		CAPTURE(reason);
		const std::filesystem::path damaged = dir.path() / "damaged.gbpeaks";
		{
			std::ofstream out(damaged, std::ios::binary);
			out.write(changed.data(), static_cast<std::streamsize>(changed.size()));
		}
		CHECK_THROWS_AS(peaks::readPeakFile(damaged), peaks::PeakFileError);
	};
	damage(bytes.substr(0, bytes.size() - 1), "truncated");
	damage(bytes + "x", "too long");
	damage("XX" + bytes.substr(2), "not a peak file");
	std::string newer = bytes;
	newer[8] = 2; // the version
	damage(newer, "a newer version");
	damage("", "empty");
	CHECK_THROWS_AS(peaks::readPeakFile(dir.path() / "missing.gbpeaks"), peaks::PeakFileError);
}

TEST_CASE("an audio file's stamp changes with its size, its modification time and its header", "[unit]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "take.wav";
	writeAudio(file, testSignal(1, 48000), 1);
	const peaks::SourceStamp stamp = stampOf(file);
	CHECK(stamp.size == std::filesystem::file_size(file));
	CHECK(peaks::sameSizeAndTime(file, stamp));
	CHECK(peaks::stampOf(file) == stamp);
	CHECK_FALSE(peaks::stampOf(dir.path() / "missing.wav"));

	// Same size and time, other samples in the header's bytes.
	const auto time = std::filesystem::last_write_time(file);
	writeAudio(file, testSignal(1, 48000, 99), 1);
	std::filesystem::last_write_time(file, time);
	const peaks::SourceStamp changed = stampOf(file);
	CHECK(peaks::sameSizeAndTime(file, stamp));
	CHECK(changed.headerHash != stamp.headerHash);

	std::filesystem::last_write_time(file, time + std::chrono::seconds(5));
	CHECK_FALSE(peaks::sameSizeAndTime(file, stamp));
	writeAudio(file, testSignal(1, 48001), 1);
	std::filesystem::last_write_time(file, time);
	CHECK_FALSE(peaks::sameSizeAndTime(file, stamp));

	CHECK(peaks::besidePath(file) == dir.path() / "take.wav.gbpeaks");
	const std::filesystem::path fallback = peaks::fallbackPath(dir.path() / "cache", file);
	CHECK(fallback.parent_path() == dir.path() / "cache");
	CHECK(fallback.extension() == ".gbpeaks");
	CHECK(fallback != peaks::fallbackPath(dir.path() / "cache", dir.path() / "other.wav"));
}

TEST_CASE("the cache generates peaks in the background and loads the peak file next time", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "take.wav";
	const std::vector<float> samples = testSignal(2, 200000);
	writeAudio(file, samples, 2);
	peaks::CacheSettings settings;
	settings.fallbackFolder = dir.path() / "cache";

	{
		Announcements events;
		peaks::PeakCache cache(events.notify(), settings);
		CHECK_FALSE(cache.status(7));
		cache.sync({mediaFile(7, file, 200000)});
		const Json ready = events.waitFor("media.peaksReady");
		REQUIRE_FALSE(ready.is_null());
		CHECK(ready == Json{{"media", 7}, {"frames", 200000}, {"generated", true}});
		// 65536 frames at a time: a third, two thirds, then all of it.
		CHECK(events.count("media.peaksProgress") == 3);
		const Json progress = events.waitFor("media.peaksProgress");
		REQUIRE_FALSE(progress.is_null());
		CHECK(progress.at("media") == 7);
		CHECK(progress.at("frames") == 65536);

		const peaks::Status status = statusOf(cache, 7);
		CHECK(status.state == peaks::State::Ready);
		CHECK(status.frames == 200000);
		CHECK(status.generated);
		CHECK(status.file == peaks::besidePath(file).u8string());
		CHECK(std::filesystem::exists(peaks::besidePath(file)));

		const peaks::PeakFile written = peaks::readPeakFile(peaks::besidePath(file));
		checkAgainstScan(written.data, samples, 2);
		CHECK(written.stamp == peaks::stampOf(file));
		CHECK(written.source == file.u8string());

		const peaks::PeakCache::Query query = queryOf(cache, 7, 0, std::nullopt, 500);
		CHECK(query.status.state == peaks::State::Ready);
		CHECK(query.columns.covered == 500);
		checkColumns(query.columns, written.data);
		CHECK_FALSE(cache.query(8, 0, std::nullopt, 500));
	}

	// The next time, the peak file is used.
	Announcements events;
	peaks::PeakCache cache(events.notify(), settings);
	cache.sync({mediaFile(3, file)});
	const Json ready = events.waitFor("media.peaksReady");
	REQUIRE_FALSE(ready.is_null());
	CHECK(ready == Json{{"media", 3}, {"frames", 200000}, {"generated", false}});
	CHECK(events.count("media.peaksProgress") == 0);
	const peaks::Status status = statusOf(cache, 3);
	CHECK_FALSE(status.generated);

	// Two media files of the same audio file share its peaks.
	cache.sync({mediaFile(3, file), mediaFile(4, file)});
	REQUIRE(cache.waitIdle(std::chrono::seconds(10)));
	CHECK(statusOf(cache, 4).state == peaks::State::Ready);
}

TEST_CASE("peaks are generated again when the audio file changes", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "take.wav";
	writeAudio(file, testSignal(1, 30000, 1), 1);
	peaks::CacheSettings settings;
	settings.fallbackFolder = dir.path() / "cache";

	const auto generatedOnLoad = [&]
	{
		Announcements events;
		peaks::PeakCache cache(events.notify(), settings);
		cache.sync({mediaFile(1, file)});
		const Json ready = events.waitFor("media.peaksReady");
		REQUIRE_FALSE(ready.is_null());
		return ready.at("generated").get<bool>();
	};
	CHECK(generatedOnLoad());
	CHECK_FALSE(generatedOnLoad());

	SECTION("the size")
	{
		writeAudio(file, testSignal(1, 30001, 1), 1);
	}
	SECTION("the modification time")
	{
		std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(3));
	}
	SECTION("the header, at the same size and time")
	{
		const auto time = std::filesystem::last_write_time(file);
		writeAudio(file, testSignal(1, 30000, 2), 1);
		std::filesystem::last_write_time(file, time);
	}
	CHECK(generatedOnLoad());
	CHECK_FALSE(generatedOnLoad());
}

TEST_CASE("a query notices a changed audio file and generates its peaks again", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "take.wav";
	writeAudio(file, testSignal(1, 30000, 1), 1);
	peaks::CacheSettings settings;
	settings.fallbackFolder = dir.path() / "cache";
	Announcements events;
	peaks::PeakCache cache(events.notify(), settings);
	cache.sync({mediaFile(1, file)});
	REQUIRE_FALSE(events.waitFor("media.peaksReady").is_null());

	const std::vector<float> louder(30000, 0.75f);
	writeAudio(file, louder, 1);
	std::filesystem::last_write_time(file, std::filesystem::last_write_time(file) + std::chrono::seconds(3));
	const peaks::PeakCache::Query stale = queryOf(cache, 1, 0, 30000, 10);
	CHECK(stale.status.state != peaks::State::Ready);
	const Json ready = events.waitFor("media.peaksReady");
	REQUIRE_FALSE(ready.is_null());
	CHECK(ready.at("generated") == true);
	const peaks::PeakCache::Query fresh = queryOf(cache, 1, 0, 30000, 10);
	CHECK(fresh.status.state == peaks::State::Ready);
	CHECK(fresh.columns.max[0] == quantizeMax(0.75f));
	CHECK(fresh.columns.min[0] == quantizeMin(0.75f));
}

TEST_CASE("peaks that can't be stored next to the audio file go to the fallback folder", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "take.wav";
	writeAudio(file, testSignal(1, 10000), 1);
	std::filesystem::create_directory(peaks::besidePath(file)); // in the way of the peak file
	peaks::CacheSettings settings;
	settings.fallbackFolder = dir.path() / "cache";

	for (const bool generated : {true, false})
	{
		Announcements events;
		peaks::PeakCache cache(events.notify(), settings);
		cache.sync({mediaFile(1, file)});
		const Json ready = events.waitFor("media.peaksReady");
		REQUIRE_FALSE(ready.is_null());
		CHECK(ready.at("generated") == generated);
		CHECK(statusOf(cache, 1).file == peaks::fallbackPath(settings.fallbackFolder, file).u8string());
	}
	CHECK(std::filesystem::exists(peaks::fallbackPath(settings.fallbackFolder, file)));
}

TEST_CASE("an audio file that can't be read fails, and is tried again once it exists", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "later.wav";
	peaks::CacheSettings settings;
	settings.fallbackFolder = dir.path() / "cache";
	Announcements events;
	peaks::PeakCache cache(events.notify(), settings);
	cache.sync({mediaFile(1, file)});
	const Json failed = events.waitFor("media.peaksFailed");
	REQUIRE_FALSE(failed.is_null());
	CHECK(failed.at("media") == 1);
	CHECK_THAT(failed.at("error").get<std::string>(), ContainsSubstring("later.wav"));
	CHECK(statusOf(cache, 1).state == peaks::State::Failed);
	CHECK(queryOf(cache, 1, 0, 100, 10).status.state == peaks::State::Failed);

	writeAudio(file, testSignal(1, 1000), 1);
	cache.query(1, 0, 100, 10);
	REQUIRE_FALSE(events.waitFor("media.peaksReady").is_null());
}

TEST_CASE("removing a media file cancels the generation of its peaks", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "take.wav";
	writeAudio(file, testSignal(2, 100000), 2);

	// The worker waits before its second chunk until the media file is gone.
	std::mutex mutex;
	std::condition_variable changed;
	int chunks = 0;
	bool removed = false;
	peaks::CacheSettings settings;
	settings.fallbackFolder = dir.path() / "cache";
	settings.chunkFrames = 10000;
	settings.beforeChunk = [&]
	{
		std::unique_lock lock(mutex);
		++chunks;
		changed.notify_all();
		changed.wait(lock, [&] { return removed; });
	};

	Announcements events;
	peaks::PeakCache cache(events.notify(), settings);
	cache.sync({mediaFile(1, file)});
	{
		std::unique_lock lock(mutex);
		REQUIRE(changed.wait_for(lock, std::chrono::seconds(10), [&] { return chunks == 1; }));
	}
	CHECK(statusOf(cache, 1).state == peaks::State::Generating);
	cache.sync({});
	CHECK_FALSE(cache.status(1));
	{
		const std::scoped_lock lock(mutex);
		removed = true;
	}
	changed.notify_all();
	REQUIRE(cache.waitIdle(std::chrono::seconds(10)));
	CHECK(chunks == 1); // it read no further
	CHECK(events.count("media.peaksReady") == 0);
	CHECK_FALSE(std::filesystem::exists(peaks::besidePath(file)));
	CHECK_FALSE(std::filesystem::exists(settings.fallbackFolder));
}

TEST_CASE("a recording's peaks grow with every append and are written when it finishes", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "recording.wav";
	peaks::CacheSettings settings;
	settings.fallbackFolder = dir.path() / "cache";
	Announcements events;
	peaks::PeakCache cache(events.notify(), settings);

	const std::vector<float> samples = testSignal(2, 120000, 23);
	WriteOptions options;
	options.channels = 2;
	options.sampleFormat = SampleFormat::Float32;
	auto writer = AudioFileWriter::create(file.u8string(), options);
	const std::unique_ptr<peaks::PeakRecorder> recorder = cache.record(file.u8string(), 2, 48000.0);
	// The media file is added while it records, with its format given.
	cache.sync({mediaFile(5, file)});

	size_t done = 0;
	for (const size_t frames : {1000, 50000, 3, 68997})
	{
		writer->write(samples.data() + (done * 2), frames);
		recorder->append(samples.data() + (done * 2), frames);
		done += frames;
		const peaks::PeakCache::Query query = queryOf(cache, 5, 0, done, 100);
		CHECK(query.status.state == peaks::State::Recording);
		CHECK(query.status.frames == done);
		const std::vector<float> prefix(samples.begin(), samples.begin() + static_cast<std::ptrdiff_t>(done * 2));
		checkColumns(query.columns, build(prefix, 2, peaks::defaultLevels()));
	}
	writer->close();
	writer.reset();
	recorder->finish();
	CHECK(statusOf(cache, 5).state == peaks::State::Ready);
	const peaks::PeakFile written = peaks::readPeakFile(peaks::besidePath(file));
	checkAgainstScan(written.data, samples, 2);
	CHECK(written.stamp == peaks::stampOf(file));

	// Its peak file is used from now on.
	Announcements later;
	peaks::PeakCache next(later.notify(), settings);
	next.sync({mediaFile(5, file)});
	const Json ready = later.waitFor("media.peaksReady");
	REQUIRE_FALSE(ready.is_null());
	CHECK(ready.at("generated") == false);
}

TEST_CASE("media.peaks returns min and max columns of a media file", "[integration]")
{
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "take.wav";
	const std::vector<float> samples = testSignal(2, 100000, 29);
	writeAudio(file, samples, 2);

	Engine engine;
	Client client(engine);
	const Json added = run(client, "media.add", {{"path", file.u8string()}});
	const Json id = added.at("media").at("id");
	const Json ready = waitForEvent(client, "media.peaksReady");
	REQUIRE_FALSE(ready.is_null());
	CHECK(ready.at("media") == id);

	const Json result = run(client, "media.peaks", {{"media", id}, {"pixels", 50}});
	CHECK(result.at("media") == id);
	CHECK(result.at("status") == "ready");
	CHECK(result.at("level") == 256); // 2000 frames per column
	CHECK(result.at("available") == 100000);
	REQUIRE(result.at("channels").size() == 2);
	const peaks::Columns expected = peaks::columns(build(samples, 2, peaks::defaultLevels()), 0, 100000, 50);
	for (size_t channel = 0; channel < 2; ++channel)
	{
		const Json& columns = result.at("channels").at(channel);
		REQUIRE(columns.at("min").size() == 50);
		for (size_t i = 0; i < 50; ++i)
		{
			CHECK(columns.at("min").at(i) == expected.min[(channel * 50) + i]);
			CHECK(columns.at("max").at(i) == expected.max[(channel * 50) + i]);
		}
	}

	const Json part = run(client, "media.peaks", {{"media", id}, {"start", 1000}, {"frames", 2560}, {"pixels", 10}});
	CHECK(part.at("level") == 256);
	const Json status = run(client, "media.peakStatus", {{"media", id}});
	REQUIRE(status.at("media").size() == 1);
	CHECK(status.at("media").at(0).at("status") == "ready");
	CHECK(status.at("media").at(0).at("file") == peaks::besidePath(file).u8string());
	CHECK(run(client, "media.peakStatus", Json::object()).at("media").size() == 1);

	CHECK_THAT(client.call("media.peaks", {{"media", 999}, {"pixels", 10}}).error, ContainsSubstring("999"));
	CHECK_FALSE(client.call("media.peaks", {{"media", id}, {"pixels", 0}}).ok());
	CHECK_FALSE(client.call("media.peaks", {{"media", id}, {"frames", 0}, {"pixels", 10}}).ok());

	// Removing the media file forgets its peaks; undoing brings them back from the peak file.
	run(client, "media.remove", {{"media", id}});
	CHECK_FALSE(client.call("media.peaks", {{"media", id}, {"pixels", 10}}).ok());
	run(client, "history.undo", Json::object());
	const Json again = waitForEvent(client, "media.peaksReady");
	REQUIRE_FALSE(again.is_null());
	CHECK(again.at("generated") == false);
}

TEST_CASE(
	"the waveform of a one-hour file is answered in milliseconds once its peak file exists", "[integration][slow]")
{
#ifdef GNUBASE_SANITIZED
	SKIP("sanitizer builds are too slow to measure this");
#endif
	// One hour of mono 16-bit audio at 48 kHz: 345 MB.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "hour.wav";
	constexpr uint64_t hourFrames = 3600ULL * 48000;
	{
		WriteOptions options;
		options.channels = 1;
		options.sampleFormat = SampleFormat::Int16;
		const auto writer = AudioFileWriter::create(file.u8string(), options);
		// A 100 Hz sine that gets louder every second and starts again every minute.
		std::vector<float> sine(48000);
		for (size_t i = 0; i < sine.size(); ++i)
			sine[i] = static_cast<float>(std::sin(2.0 * g_pi * 100.0 * static_cast<double>(i) / 48000.0));
		std::vector<float> block(sine.size());
		for (uint64_t second = 0; second < 3600; ++second)
		{
			const auto gain = static_cast<float>((second % 60) + 1) / 60.0f;
			std::transform(sine.begin(), sine.end(), block.begin(), [gain](float sample) { return sample * gain; });
			writer->write(block.data(), block.size());
		}
		writer->close();
	}

	// The first time, the peaks are generated in the background.
	{
		Engine engine;
		Client client(engine);
		run(client, "media.add", {{"path", file.u8string()}});
		const Json ready = waitForEvent(client, "media.peaksReady", std::chrono::minutes(5));
		REQUIRE_FALSE(ready.is_null());
		CHECK(ready.at("generated") == true);
	}

	// Then the peak file is loaded, and every view of the hour is answered in milliseconds.
	Engine engine;
	Client client(engine);
	const auto added = std::chrono::steady_clock::now();
	const Json id = run(client, "media.add", {{"path", file.u8string()}}).at("media").at("id");
	const Json ready = waitForEvent(client, "media.peaksReady", std::chrono::seconds(30));
	REQUIRE_FALSE(ready.is_null());
	CHECK(ready.at("generated") == false);
	const auto loaded = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - added);
	WARN("loading the peak file of a one-hour file took " << loaded.count() << " ms");
	CHECK(loaded < std::chrono::seconds(2));

	struct View
	{
		uint64_t start;
		uint64_t frames;
	};
	// Each view's best of three, so a runner that is busy for a moment doesn't count as a slow query.
	std::chrono::microseconds slowest{0};
	for (const View view : {View{0, hourFrames}, View{hourFrames / 2, hourFrames / 10}, View{1000000, 480000},
			 View{hourFrames - 48000, 48000}, View{123456789, 2000}})
	{
		auto best = std::chrono::microseconds::max();
		for (int attempt = 0; attempt < 3; ++attempt)
		{
			const auto before = std::chrono::steady_clock::now();
			const Json result = run(client, "media.peaks",
				{{"media", id}, {"start", view.start}, {"frames", view.frames}, {"pixels", 2000}});
			best = std::min(
				best, std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - before));
			CHECK(result.at("status") == "ready");
			CHECK(result.at("channels").at(0).at("max").size() == 2000);
		}
		slowest = std::max(slowest, best);
	}
	WARN("the slowest view of a one-hour file took " << slowest.count() << " us");
	CHECK(slowest < std::chrono::milliseconds(100));
}
