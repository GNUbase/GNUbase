// SPDX-License-Identifier: GPL-3.0-or-later
// Engine benchmarks (#212, #24). Each case renders a project offline through the engine API and reports how many
// times faster than real time it ran: workers (scaling with threads), tracks (scaling with track count) and
// blockSizes (32 to 2048 frames). Build a Release configuration for meaningful numbers. CI runs it with --quick and
// keeps the JSON as an artifact.
//
//   gnubase-bench [--quick] [--case <name>] [--json <file>]
#include "engine/document/document.h"
#include "engine/engine.h"
#include "platform/rt/workerpool.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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

	Json call(Client& client, const std::string& command, const Json& parameters = Json::object())
	{
		gnubase::engine::Result result = client.call(command, parameters);
		if (!result.ok())
			throw std::runtime_error(command + ": " + result.error);
		return std::move(result.value);
	}

	struct Options
	{
		bool quick = false;
		std::string only; // a case name, or all
		std::string json; // where to write the results, or nowhere
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

	struct Case
	{
		const char* name;
		Json (*run)(const Options&);
	};

	const Case g_cases[] = {
		{"workers", &workers},
		{"tracks", &tracks},
		{"blockSizes", &blockSizes},
	};

	int benchmark(int argc, char** argv)
	{
		Options options;
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
