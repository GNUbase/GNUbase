// SPDX-License-Identifier: GPL-3.0-or-later
// Input monitoring (#100): the monitor button, the automatic modes for record-enabled tracks, through the graph and
// through the device's direct monitoring.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Json;
using gnubase::engine::ObjectId;
using gnubase::engine::Result;

namespace
{
	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	bool silent(const std::vector<float>& samples)
	{
		return std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0.0f; });
	}

	// The dummy device with a sine on its inputs and an audio track on the main input bus, which every new audio
	// track gets.
	struct Setup
	{
		explicit Setup(bool direct = false, double frequency = 440.0)
		{
			run(client, "audio.setDevice",
				{{"blockSize", 64}, {"captureSeconds", 1}, {"directMonitoring", direct},
					{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", frequency}, {"Level", 0.5}}}}}});
			channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
		}

		// Renders ten blocks and returns what the first output played.
		std::vector<float> render()
		{
			run(client, "audio.render", {{"frames", 640}});
			const Json samples =
				run(client, "audio.capture", {{"channel", 0}, {"start", rendered}, {"frames", 640}}).at("samples");
			rendered += 640;
			return samples.get<std::vector<float>>();
		}

		// Whether the track hears its input, after the block in which monitoring may glide in or out.
		bool hears()
		{
			const std::vector<float> samples = render();
			return !silent(std::vector<float>(samples.begin() + 64, samples.end()));
		}

		Engine engine;
		Client client{engine};
		ObjectId channel = 0;
		int64_t rendered = 0;
	};
}

TEST_CASE("the monitor button gates an audio track's input", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	CHECK(!setup.hears()); // a new track doesn't monitor

	Json channel = run(client, "channel.set", {{"channel", setup.channel}, {"monitor", true}}).at("channel");
	CHECK(channel.at("monitor") == true);
	CHECK(channel.at("recordEnable") == false);
	CHECK(setup.hears());
	run(client, "transport.play");
	CHECK(setup.hears());
	run(client, "transport.record");
	CHECK(setup.hears());
	run(client, "transport.stop");

	run(client, "channel.set", {{"channel", setup.channel}, {"monitor", false}});
	CHECK(!setup.hears());
}

TEST_CASE("each automatic monitoring mode monitors record-enabled tracks in its transport states", "[integration]")
{
	const bool direct = GENERATE(false, true);
	INFO((direct ? "direct monitoring" : "monitoring through the graph"));
	struct Mode
	{
		const char* name;
		std::array<bool, 3> hears; // stopped, playing, recording
	};
	for (const Mode& mode : {Mode{"manual", {false, false, false}}, Mode{"whileRecordEnabled", {true, true, true}},
			 Mode{"whileRecording", {false, false, true}}, Mode{"tapeMachine", {true, false, true}}})
	{
		INFO("mode " << mode.name);
		Setup setup(direct);
		Client& client = setup.client;
		run(client, "settings.set", {{"key", "audio.autoMonitoring"}, {"value", mode.name}});
		// state: 0 record-enabled, 1 neither button, 2 the monitor button too
		for (int state = 0; state < 3; ++state)
		{
			INFO("state " << state);
			run(client, "channel.set",
				{{"channel", setup.channel}, {"recordEnable", state != 1}, {"monitor", state == 2}});
			const auto expect = [&mode, state](size_t transport)
			{ return state == 2 || (state == 0 && mode.hears[transport]); };
			CHECK(setup.hears() == expect(0));
			run(client, "transport.play");
			CHECK(setup.hears() == expect(1));
			run(client, "transport.record");
			CHECK(setup.hears() == expect(2));
			run(client, "transport.record", {{"record", false}}); // punch out: plays on
			CHECK(setup.hears() == expect(1));
			run(client, "transport.stop");
			CHECK(setup.hears() == expect(0));
		}
	}
}

TEST_CASE("the automatic monitoring mode is a user setting, tape machine style by default", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	CHECK(run(client, "settings.get", {{"key", "audio.autoMonitoring"}}).at("value") == "tapeMachine");
	run(client, "channel.set", {{"channel", setup.channel}, {"recordEnable", true}});
	CHECK(setup.hears()); // stopped, record-enabled

	// A new mode applies at once, while the project stays as it is.
	run(client, "settings.set", {{"key", "audio.autoMonitoring"}, {"value", "manual"}});
	CHECK(!setup.hears());
	run(client, "settings.reset", {{"key", "audio.autoMonitoring"}});
	CHECK(setup.hears());
	CHECK_THAT(reject(client, "settings.set", {{"key", "audio.autoMonitoring"}, {"value", "always"}}),
		ContainsSubstring("audio.autoMonitoring"));
}

TEST_CASE("monitoring glides in and out over a block instead of clicking", "[integration]")
{
	Setup setup(false, 10.0); // almost constant: a hard switch would jump by up to the level
	Client& client = setup.client;
	setup.render();
	for (const bool monitor : {true, false})
	{
		run(client, "channel.set", {{"channel", setup.channel}, {"monitor", monitor}});
		const std::vector<float> samples = setup.render();
		float jump = 0.0f;
		for (size_t i = 1; i < samples.size(); ++i)
			jump = std::max(jump, std::abs(samples[i] - samples[i - 1]));
		CHECK(jump < 0.01f);
		CHECK(silent(std::vector<float>(samples.begin() + 64, samples.end())) == !monitor);
	}
}

TEST_CASE("monitor and record enable are audio track settings saved with the project", "[integration]")
{
	Engine engine;
	Client client(engine);
	const Json midi = run(client, "track.add", {{"type", "midi"}});
	CHECK_THAT(reject(client, "channel.set", {{"channel", midi.at("channel").at("id")}, {"monitor", true}}),
		ContainsSubstring("only audio tracks"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", midi.at("channel").at("id")}, {"recordEnable", true}}),
		ContainsSubstring("only audio tracks"));

	const ObjectId channel = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channel}, {"monitor", true}, {"recordEnable", true}});
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "monitoring.dawproject").u8string();
	run(client, "project.save", {{"path", file}});
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	const Json channels = run(client, "project.get").at("channels");
	const auto loaded = std::find_if(
		channels.begin(), channels.end(), [channel](const Json& item) { return item.at("id") == channel; });
	REQUIRE(loaded != channels.end());
	CHECK(loaded->at("monitor") == true);
	CHECK(loaded->at("recordEnable") == true);
}
