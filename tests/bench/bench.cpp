// SPDX-License-Identifier: GPL-3.0-or-later
// Engine benchmarks (#212, #24). Each case renders a project offline through the engine API and reports how many
// times faster than real time it ran: workers (scaling with threads), tracks (scaling with track count),
// blockSizes (32 to 2048 frames) and transposedEvents (audio events that follow the project root key, each through a
// real-time stretcher of its own on the disk threads, #1555). Build a Release configuration for meaningful numbers.
// CI runs it with --quick and keeps the JSON as an artifact.
//
//   gnubase-bench [--quick] [--case <name>] [--json <file>]
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "platform/rt/workerpool.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// windows.h first
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Json;
using gnubase::engine::ObjectId;

namespace
{
	constexpr int64_t g_ticksPerQuarter = 1720320;

	// A render in a Debug build can take longer than the 10 s a call waits by default.
	Json call(Client& client, const std::string& command, const Json& parameters = Json::object(),
		std::chrono::milliseconds timeout = std::chrono::minutes(10))
	{
		gnubase::engine::Result result = client.call(command, parameters, timeout);
		if (!result.ok())
			throw std::runtime_error(command + ": " + result.error);
		return std::move(result.value);
	}

	struct Options
	{
		bool quick = false;
		std::string only;    // a case name, or all
		std::string json;    // where to write the results, or nowhere
		std::string program; // this program, which transposedEvents starts again for each render
	};

	// A project of instrument tracks, each playing chords of the test synth for the whole render.
	struct Song
	{
		int tracks = 32;
		int voices = 8; // notes per chord
		int blockSize = 256;
		double seconds = 4.0;
		int threads = 1;
	};

	struct Rendered
	{
		double speed = 0.0;    // how many times faster than real time
		int activeThreads = 1; // the threads that processed: fewer than asked without real-time priority
		Json limitedBy;        // why they were fewer, as audio.setProcessing says
	};

	Rendered render(const Song& song)
	{
		Engine engine;
		Client client(engine);
		const auto frames = static_cast<int64_t>(song.seconds * 48000.0);
		call(client, "audio.setDevice", {{"blockSize", song.blockSize}, {"inputChannels", 0}, {"captureSeconds", 1}});
		const Json processing = call(client, "audio.setProcessing", {{"threads", song.threads}}).at("processing");
		Rendered rendered;
		rendered.activeThreads = processing.at("activeThreads").get<int>();
		rendered.limitedBy = processing.at("limitedBy");
		const int64_t quarters = (frames / 24000) + 1;
		for (int t = 0; t < song.tracks; ++t)
		{
			const auto track = call(client, "track.add", {{"type", "instrument"}}).at("track").at("id").get<ObjectId>();
			ObjectId channel = 0;
			const Json project = call(client, "project.get");
			for (const Json& item : project.at("tracks"))
				if (item.at("id").get<ObjectId>() == track)
					channel = item.at("channel").get<ObjectId>();
			call(client, "channel.set", {{"channel", channel}, {"instrument", "testSynth"}});
			const Json part = call(
				client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", quarters * g_ticksPerQuarter}});
			Json notes = Json::array();
			for (int v = 0; v < song.voices; ++v)
				notes.push_back({{"position", 0}, {"length", quarters * g_ticksPerQuarter},
					{"pitch", 36 + (((t * 5) + (v * 7)) % 60)}, {"velocity", 0.2}});
			call(client, "note.add", {{"part", part.at("part").at("id")}, {"notes", notes}});
		}
		call(client, "transport.play");
		const auto start = std::chrono::steady_clock::now();
		call(client, "audio.render", {{"frames", frames}});
		const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
		rendered.speed = song.seconds / took.count();
		return rendered;
	}

	// Speed with 1, 2, 4 ... threads up to the hardware's: independent tracks should scale close to linearly. Where
	// workers get no real-time priority (Linux without the privilege) only the audio thread processes, and the
	// speedup stays flat: activeThreads and limitedBy say so.
	Json workers(const Options& options)
	{
		Json results = Json::array();
		const auto hardware = static_cast<int>(gnubase::rt::hardwareThreads());
		std::vector<int> counts;
		for (int threads = 1; threads < hardware; threads *= 2)
			counts.push_back(threads);
		counts.push_back(hardware);
		double single = 0.0;
		for (const int threads : counts)
		{
			Song song;
			song.tracks = 64;
			song.seconds = options.quick ? 1.0 : 4.0;
			song.threads = threads;
			const Rendered rendered = render(song);
			if (threads == 1)
				single = rendered.speed;
			const double speedup = rendered.speed / single;
			std::printf("workers  threads %3d  active %3d  %8.2fx real time  speedup %5.2f  efficiency %3.0f%%\n",
				threads, rendered.activeThreads, rendered.speed, speedup, 100.0 * speedup / rendered.activeThreads);
			results.push_back({{"threads", threads}, {"activeThreads", rendered.activeThreads},
				{"limitedBy", rendered.limitedBy}, {"realTimeFactor", rendered.speed}, {"speedup", speedup}});
		}
		return results;
	}

	// Speed with more and more tracks, on every hardware thread: the cost of a track should stay the same.
	Json tracks(const Options& options)
	{
		Json results = Json::array();
		for (const int count : {8, 16, 32, 64, 128})
		{
			Song song;
			song.tracks = count;
			song.seconds = options.quick ? 1.0 : 4.0;
			song.threads = static_cast<int>(gnubase::rt::hardwareThreads());
			const double speed = render(song).speed;
			const double perTrack = 1000.0 * song.blockSize / 48000.0 / speed / count; // ms of a block per track
			std::printf(
				"tracks   tracks %4d  %8.2fx real time  %7.4f ms per track and block\n", count, speed, perTrack);
			results.push_back({{"tracks", count}, {"realTimeFactor", speed}, {"msPerTrackBlock", perTrack}});
		}
		return results;
	}

	// Speed at device blocks from 32 to 2048 frames, on every hardware thread: small blocks cost more per sample.
	Json blockSizes(const Options& options)
	{
		Json results = Json::array();
		for (int blockSize = 32; blockSize <= 2048; blockSize *= 2)
		{
			Song song;
			song.tracks = 64;
			song.blockSize = blockSize;
			song.seconds = options.quick ? 1.0 : 4.0;
			song.threads = static_cast<int>(gnubase::rt::hardwareThreads());
			const double speed = render(song).speed;
			std::printf("blocks   frames %4d  %8.2fx real time\n", blockSize, speed);
			results.push_back({{"blockSize", blockSize}, {"realTimeFactor", speed}});
		}
		return results;
	}

	// The memory the process holds now, in bytes: its working set or resident set; 0 where it can't tell.
	size_t residentBytes()
	{
#ifdef _WIN32
		PROCESS_MEMORY_COUNTERS counters{};
		return GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) != 0 ? counters.WorkingSetSize
																						   : 0;
#elif defined(__APPLE__)
		mach_task_basic_info_data_t info{};
		mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
		// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): task_info's C interface takes an int array.
		return task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) ==
				KERN_SUCCESS
			? static_cast<size_t>(info.resident_size)
			: 0;
#else
		std::ifstream statm("/proc/self/statm");
		size_t pages = 0;
		size_t resident = 0;
		return statm >> pages >> resident ? resident * static_cast<size_t>(sysconf(_SC_PAGESIZE)) : 0;
#endif
	}

	struct Transposed
	{
		double speed = 0.0;     // how many times faster than real time
		double megabytes = 0.0; // what the process holds more while it plays than before
	};

	// Audio tracks that each play an event of a mono file, in C, for the whole render: in a project without a root key
	// they play their file as it is; in one in D each follows the root key 2 semitones up, through a real-time
	// stretcher of its own on the disk threads (#1368), which offline rendering waits for before each block.
	Transposed renderTransposed(int tracks, double seconds, const std::string& file, bool follow)
	{
		Engine engine;
		Client client(engine);
		const auto frames = static_cast<int64_t>(seconds * 48000.0);
		call(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 1}});
		call(client, "audio.setProcessing", {{"threads", static_cast<int>(gnubase::rt::hardwareThreads())}});
		const Json media = call(client, "media.add", {{"path", file}}).at("media");
		const Json clip = call(client, "clip.add", {{"media", media.at("id")}}).at("clip");
		if (follow)
			call(client, "project.set", {{"rootKey", "D"}});
		for (int t = 0; t < tracks; ++t)
		{
			const Json track = call(client, "track.add", {{"type", "audio"}, {"channels", 1}}).at("track");
			const Json event =
				call(client, "audioEvent.add", {{"track", track.at("id")}, {"clip", clip.at("id")}, {"position", 0}})
					.at("event");
			call(client, "audioEvent.set", {{"event", event.at("id")}, {"rootKey", "C"}});
		}
		const size_t before = residentBytes();
		call(client, "transport.play");
		const auto start = std::chrono::steady_clock::now();
		call(client, "audio.render", {{"frames", frames}});
		const std::chrono::duration<double> took = std::chrono::steady_clock::now() - start;
		const size_t after = residentBytes();
		return {seconds / took.count(), (static_cast<double>(after) - static_cast<double>(before)) / (1024.0 * 1024.0)};
	}

	// renderTransposed in a process of its own, this program started with --render-transposed, so that no render
	// reuses memory that an earlier one freed without the process growing, which its megabytes would then miss.
	Transposed renderTransposedAlone(
		const Options& options, int tracks, double seconds, const std::string& file, bool follow)
	{
		const std::filesystem::path out =
			std::filesystem::temp_directory_path() / "gnubase-bench-transposed-result.json";
		const auto quoted = [](const std::string& text) { return '"' + text + '"'; };
		std::string command = quoted(std::filesystem::absolute(std::filesystem::u8path(options.program)).u8string()) +
			" --render-transposed " + std::to_string(tracks) + " " + std::to_string(seconds) + " " + quoted(file) +
			(follow ? " 1 " : " 0 ") + quoted(out.u8string());
#ifdef _WIN32
		command = quoted(command); // cmd /c takes away the outer quotes
#endif
		if (std::system(command.c_str()) != 0)
			throw std::runtime_error("transposedEvents: the render in a process of its own failed");
		Json result;
		std::ifstream(out) >> result;
		std::error_code ignored;
		std::filesystem::remove(out, ignored);
		return {result.at("speed").get<double>(), result.at("megabytes").get<double>()};
	}

	// What audio events that follow the root key cost against the same events played as they are: the speed of the
	// render and the memory their stretchers take, per event, each render in a process of its own.
	Json transposedEvents(const Options& options)
	{
		const double seconds = options.quick ? 1.0 : 4.0;
		const std::filesystem::path file = std::filesystem::temp_directory_path() / "gnubase-bench-transposed.wav";
		{
			gnubase::engine::WriteOptions format;
			format.sampleFormat = gnubase::engine::SampleFormat::Float32;
			format.channels = 1;
			auto writer = gnubase::engine::AudioFileWriter::create(file.u8string(), format);
			std::vector<float> samples(static_cast<size_t>((seconds + 1.0) * 48000.0));
			for (size_t i = 0; i < samples.size(); ++i)
				samples[i] = static_cast<float>(
					0.25 * std::sin(2.0 * 3.14159265358979 * 220.0 * static_cast<double>(i) / 48000.0));
			writer->write(samples.data(), samples.size());
			writer->close();
		}
		Json results = Json::array();
		for (const int count : {16, 64})
		{
			const Transposed plain = renderTransposedAlone(options, count, seconds, file.u8string(), false);
			const Transposed shifted = renderTransposedAlone(options, count, seconds, file.u8string(), true);
			const double perEvent = (shifted.megabytes - plain.megabytes) / count;
			std::printf("transposedEvents  events %3d  as they are %8.2fx real time  following the root key %8.2fx "
						"real time  %6.2f MB more per event\n",
				count, plain.speed, shifted.speed, perEvent);
			results.push_back({{"events", count}, {"realTimeFactor", plain.speed},
				{"transposedRealTimeFactor", shifted.speed}, {"megabytesPerTransposedEvent", perEvent}});
		}
		std::error_code ignored;
		std::filesystem::remove(file, ignored);
		return results;
	}

	struct Case
	{
		const char* name;
		Json (*run)(const Options&);
	};

	const Case g_cases[] = {
		{"workers", &workers},
		{"tracks", &tracks},
		{"blockSizes", &blockSizes},
		{"transposedEvents", &transposedEvents},
	};

	int benchmark(int argc, char** argv)
	{
		Options options;
		options.program = argv[0];
		// One render of transposedEvents (renderTransposedAlone): tracks, seconds, file, follow (0 or 1), result file.
		if (argc == 7 && std::string(argv[1]) == "--render-transposed")
		{
			const Transposed rendered =
				renderTransposed(std::stoi(argv[2]), std::stod(argv[3]), argv[4], std::string(argv[5]) == "1");
			std::ofstream(argv[6]) << Json{{"speed", rendered.speed}, {"megabytes", rendered.megabytes}}.dump();
			return 0;
		}
		for (int i = 1; i < argc; ++i)
		{
			const std::string argument = argv[i];
			if (argument == "--quick")
			{
				options.quick = true;
			}
			else if (argument == "--case" && i + 1 < argc)
			{
				options.only = argv[++i];
			}
			else if (argument == "--json" && i + 1 < argc)
			{
				options.json = argv[++i];
			}
			else
			{
				std::cerr << "usage: gnubase-bench [--quick] [--case <name>] [--json <file>]\n";
				return 2;
			}
		}
		Json results = Json::object();
		results["hardwareThreads"] = gnubase::rt::hardwareThreads();
		results["quick"] = options.quick;
		bool ran = false;
		for (const Case& c : g_cases)
		{
			if (!options.only.empty() && options.only != c.name)
				continue;
			ran = true;
			results["cases"][c.name] = c.run(options);
		}
		if (!ran)
		{
			std::cerr << "no benchmark " << options.only << "\n";
			return 2;
		}
		if (!options.json.empty())
			std::ofstream(options.json) << results.dump(2) << "\n";
		return 0;
	}
}

int main(int argc, char** argv)
{
	try
	{
		return benchmark(argc, argv);
	}
	catch (const std::exception& error)
	{
		std::fputs(error.what(), stderr);
		std::fputs("\n", stderr);
		return 1;
	}
	catch (...)
	{
		return 1;
	}
}
