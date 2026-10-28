// SPDX-License-Identifier: GPL-3.0-or-later
// Engine performance data (#24): processing load, peaks, overloads, per-thread load and the disk streams' load.
#include "engine/audio/performance.h"
#include "engine/engine.h"
#include "platform/rt/workerpool.h"
#include "testutils/testutils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace gnubase::engine;
using Catch::Approx;

TEST_CASE("the performance meter averages the load and keeps peaks and overloads until reset", "[unit]")
{
	PerformanceMeter meter;
	const double block = 0.005;
	const double threads[] = {0.004, 0.002};
	for (int i = 0; i < 1000; ++i)
		meter.cycle(0.0025, block, threads, 2);
	CHECK(meter.data().load == Approx(0.5).epsilon(0.01)); // settled after five seconds of blocks
	CHECK(meter.data().peak == Approx(0.5));
	CHECK(meter.data().overloads == 0);
	CHECK(meter.data().cycles == 1000);
	CHECK(meter.data().threads == 2);
	CHECK(meter.data().threadLoad[0] == Approx(0.8).epsilon(0.01));
	CHECK(meter.data().threadLoad[1] == Approx(0.4).epsilon(0.01));

	meter.cycle(0.0075, block, threads, 2); // one block over its time
	CHECK(meter.data().peak == Approx(1.5));
	CHECK(meter.data().overloads == 1);
	CHECK(meter.data().load < 0.6); // one block barely moves the average

	meter.reset();
	CHECK(meter.data().peak == 0.0);
	CHECK(meter.data().overloads == 0);
	CHECK(meter.data().cycles == 0);
	CHECK(meter.data().load > 0.4); // the average goes on
	meter.cycle(0.001, block, threads, 1);
	CHECK(meter.data().peak == Approx(0.2));
	CHECK(meter.data().threads == 1);
}

TEST_CASE("the performance meter ignores threads beyond its maximum and empty blocks", "[unit]")
{
	PerformanceMeter meter;
	std::vector<double> threads(PerformanceData::g_maxThreads + 10, 0.001);
	meter.cycle(0.001, 0.0, threads.data(), threads.size());
	CHECK(meter.data().cycles == 0);
	meter.cycle(0.001, 0.002, threads.data(), threads.size());
	CHECK(meter.data().threads == PerformanceData::g_maxThreads);
	CHECK(meter.data().cycles == 1);
}

namespace
{
	Json run(Client& client, const std::string& command, const Json& parameters = Json::object())
	{
		const Result result = client.call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}
}

TEST_CASE("audio.performance reports the load per block and thread, and resets", "[integration]")
{
	Engine engine;
	Client client(engine);
	const Json idle = run(client, "audio.performance");
	CHECK(idle.at("cycles") == 0);
	CHECK(idle.at("load") == 0.0);
	CHECK(idle.at("threads").empty());

	run(client, "audio.setDevice", {{"blockSize", 64}, {"captureSeconds", 1}, {"input", {{"processor", "noise"}}}});
	run(client, "audio.setProcessing", {{"threads", 3}});
	for (int i = 0; i < 8; ++i)
		run(client, "track.add", {{"type", "audio"}});
	run(client, "audio.render", {{"frames", 6400}});

	const Json measured = run(client, "audio.performance", {{"reset", true}});
	CHECK(measured.at("cycles") == 100);
	CHECK(measured.at("load").get<double>() > 0.0);
	CHECK(measured.at("peak").get<double>() >= measured.at("load").get<double>());
	CHECK(measured.at("overloads").get<uint64_t>() <= 100);
	REQUIRE(measured.at("threads").size() == std::min<size_t>(3, gnubase::rt::hardwareThreads()));
	// A thread's time on nodes lies within the block's processing time. A thread can show 0: nodes this small can
	// finish within one tick of the clock.
	for (const Json& load : measured.at("threads"))
	{
		CHECK(load.get<double>() >= 0.0);
		CHECK(load.get<double>() <= measured.at("peak").get<double>() + 1e-9);
	}
	CHECK(measured.at("diskLoad").get<double>() >= 0.0);
	CHECK(measured.at("diskLoad").get<double>() <= 1.0);
	CHECK(measured.at("diskDropouts") == 0);

	// The reset takes effect with the next block.
	run(client, "audio.render", {{"frames", 640}});
	CHECK(run(client, "audio.performance").at("cycles") == 10);
}

TEST_CASE("audio.performance reports how far the disk streams have run empty", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "long.wav").u8string();
	gnubase::test::Audio audio;
	audio.channels = 2;
	audio.samples.assign(size_t{2} * 48000 * 4, 0.1f);
	gnubase::test::writeWav(path, audio);

	Engine engine;
	Client client(engine);
	const Json track = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}});
	const Json media = run(client, "media.add", {{"path", path}});
	const Json clip = run(client, "clip.add", {{"media", media.at("media").at("id")}});
	run(client, "audioEvent.add",
		{{"track", track.at("track").at("id")}, {"clip", clip.at("clip").at("id")}, {"position", 0}});
	run(client, "audio.setDevice", {{"blockSize", 256}, {"captureSeconds", 1}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 48000}});

	// Offline rendering waits for full streams before every block, so the last block left one block's worth empty
	// at most.
	const Json performance = run(client, "audio.performance");
	CHECK(performance.at("diskLoad").get<double>() >= 0.0);
	CHECK(performance.at("diskLoad").get<double>() < 0.5);
	CHECK(performance.at("diskDropouts") == 0);
}
