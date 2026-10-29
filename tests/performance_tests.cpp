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
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
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

namespace
{
	// Simulates a system that gives worker threads no real-time priority, for the lifetime of the object.
	struct RefuseRealtime
	{
		RefuseRealtime() { gnubase::rt::setWorkerPoolFaults({SIZE_MAX, true}); }
		~RefuseRealtime() { gnubase::rt::setWorkerPoolFaults({}); }
		RefuseRealtime(const RefuseRealtime&) = delete;
		RefuseRealtime& operator=(const RefuseRealtime&) = delete;
	};
}

TEST_CASE("audio.performance reports the load per block and thread", "[integration]")
{
	Engine engine;
	Client client(engine);
	const Json idle = run(client, "audio.performance");
	CHECK(idle.at("cycles") == 0);
	CHECK(idle.at("load") == 0.0);
	CHECK(idle.at("threads").empty());

	run(client, "audio.setDevice", {{"blockSize", 64}, {"captureSeconds", 1}, {"input", {{"processor", "noise"}}}});
	// Without real-time priority for workers (Linux without the privilege) only the audio thread processes.
	const size_t active =
		run(client, "audio.setProcessing", {{"threads", 3}}).at("processing").at("activeThreads").get<size_t>();
	CHECK(active >= 1);
	CHECK(active <= std::min<size_t>(3, gnubase::rt::hardwareThreads()));
	for (int i = 0; i < 8; ++i)
		run(client, "track.add", {{"type", "audio"}});
	run(client, "audio.render", {{"frames", 6400}});

	const Json measured = run(client, "audio.performance");
	CHECK(measured.at("cycles") == 100);
	CHECK(measured.at("load").get<double>() > 0.0);
	CHECK(measured.at("peak").get<double>() >= measured.at("load").get<double>());
	CHECK(measured.at("overloads").get<uint64_t>() <= 100);
	REQUIRE(measured.at("threads").size() == active);
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
	// Reading changes nothing.
	CHECK(run(client, "audio.performance").at("cycles") == 100);

	// As on a system that gives workers no real-time priority: the audio thread is the only thread measured.
	const RefuseRealtime refuse;
	const Json serial = run(client, "audio.setProcessing", {{"threads", 3}}).at("processing");
	run(client, "audio.render", {{"frames", 640}});
	CHECK(serial.at("activeThreads") == 1);
	CHECK(run(client, "audio.performance").at("threads").size() == 1);
}

namespace
{
	// The next event called name among the messages the client has received, if any.
	std::optional<Json> takeEvent(Client& client, const std::string& name)
	{
		while (const std::optional<Message> message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message); event != nullptr && event->name == name)
				return event->data;
		return std::nullopt;
	}

	const CommandInfo* findCommand(const std::vector<CommandInfo>& commands, const std::string& name)
	{
		const auto found = std::find_if(
			commands.begin(), commands.end(), [&name](const CommandInfo& info) { return info.name == name; });
		return found != commands.end() ? &*found : nullptr;
	}
}

TEST_CASE("audio.resetPerformance starts a new measurement and tells every client", "[integration]")
{
	Engine engine;
	Client client(engine);
	Client other(engine);
	const std::vector<CommandInfo> commands = engine.commands();
	const CommandInfo* query = findCommand(commands, "audio.performance");
	const CommandInfo* reset = findCommand(commands, "audio.resetPerformance");
	REQUIRE(query != nullptr);
	REQUIRE(reset != nullptr);
	CHECK(query->query);
	CHECK(!reset->query); // it changes what every client sees
	CHECK(!client.call("audio.performance", {{"reset", true}}).ok());

	run(client, "audio.setDevice", {{"blockSize", 64}, {"captureSeconds", 1}, {"input", {{"processor", "noise"}}}});
	run(client, "track.add", {{"type", "audio"}});
	run(client, "audio.render", {{"frames", 6400}});
	while (other.poll())
	{
	}

	// The result is the measurement that ended.
	const Json ended = run(client, "audio.resetPerformance");
	CHECK(ended.at("cycles") == 100);
	CHECK(ended.at("peak").get<double>() > 0.0);
	const std::optional<Json> event = takeEvent(other, "audio.performanceReset");
	REQUIRE(event.has_value());
	if (event)
		CHECK(event->at("cycles") == 100);

	// With the device stopped the reset is immediate; while it runs, it takes effect with the next block.
	const Json fresh = run(client, "audio.performance");
	CHECK(fresh.at("cycles") == 0);
	CHECK(fresh.at("peak") == 0.0);
	CHECK(fresh.at("overloads") == 0);
	CHECK(fresh.at("load").get<double>() > 0.0); // the average goes on
	run(client, "audio.render", {{"frames", 640}});
	CHECK(run(client, "audio.performance").at("cycles") == 10);
}

TEST_CASE("audio.performance starts over with a new device or new processing threads", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}, {"captureSeconds", 1}, {"input", {{"processor", "noise"}}}});
	run(client, "audio.setProcessing", {{"threads", 2}});
	run(client, "track.add", {{"type", "audio"}});
	run(client, "track.add", {{"type", "audio"}});
	run(client, "audio.render", {{"frames", 6400}});
	REQUIRE(run(client, "audio.performance").at("cycles") == 100);

	// The old device's numbers don't stay up while the new one hasn't processed a block.
	run(client, "audio.setDevice", {{"blockSize", 128}, {"captureSeconds", 1}});
	const Json device = run(client, "audio.performance");
	CHECK(device.at("cycles") == 0);
	CHECK(device.at("load") == 0.0);
	CHECK(device.at("peak") == 0.0);
	CHECK(device.at("overloads") == 0);
	CHECK(device.at("threads").empty());

	// Nor do the old threads' averages when the number of threads changes.
	run(client, "audio.render", {{"frames", 1280}});
	REQUIRE(run(client, "audio.performance").at("cycles") == 10);
	run(client, "audio.setProcessing", {{"threads", 1}});
	const Json processing = run(client, "audio.performance");
	CHECK(processing.at("cycles") == 0);
	CHECK(processing.at("load") == 0.0);
	CHECK(processing.at("threads").empty());
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
