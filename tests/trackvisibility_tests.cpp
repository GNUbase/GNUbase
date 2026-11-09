// SPDX-License-Identifier: GPL-3.0-or-later
// Track visibility in the engine (#166, docs/features/track-visibility.md): the tracks the project window hides
// (trackVisibility.get, trackVisibility.set), the visibility configurations, and the mixers that follow it.
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	struct Fixture
	{
		Json call(const std::string& command, Json parameters = Json::object())
		{
			const engine::Result result = client.call(command, std::move(parameters));
			INFO(command << " failed: " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, Json parameters)
		{
			const engine::Result result = client.call(command, std::move(parameters));
			REQUIRE(!result.ok());
			return result.error;
		}

		// The new track and its channel (0 for none).
		std::pair<uint64_t, uint64_t> track(const std::string& type)
		{
			const Json added = call("track.add", {{"type", type}});
			const Json channel = added.value("channel", Json());
			return {
				added.at("track").at("id").get<uint64_t>(), channel.is_object() ? channel.at("id").get<uint64_t>() : 0};
		}

		Json visibility() { return call("trackVisibility.get"); }
		Json hidden() { return visibility().at("hidden"); }
		Json mixerHidden(const std::string& mixer) { return call("mixer.get", {{"mixer", mixer}}).at("hidden"); }

		uint64_t bus(const std::string& direction)
		{
			const Json project = call("project.get");
			for (const Json& bus : project.at("busses"))
				if (bus.at("direction") == direction)
					return bus.at("id").get<uint64_t>();
			FAIL("no " << direction << " bus");
		}

		engine::Engine engine;
		engine::Client client{engine};
	};

	Json sorted(std::vector<uint64_t> ids)
	{
		std::sort(ids.begin(), ids.end());
		return ids;
	}
}

TEST_CASE("track visibility hides tracks, saved with the project but not an undo step", "[unit]")
{
	Fixture f;
	std::mutex mutex;
	std::vector<Json> changes;
	const engine::Listener listener(f.engine,
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "trackVisibility.changed")
				changes.push_back(event.data);
		});
	CHECK(f.visibility() ==
		Json{{"hidden", Json::array()}, {"sync", ""}, {"configurations", Json::array()}, {"configuration", ""},
			{"modified", false}});
	const uint64_t a = f.track("audio").first;
	const uint64_t marker = f.track("marker").first;
	const uint64_t b = f.track("audio").first;

	// Tracks by id, sorted and once each, of any type.
	CHECK(f.call("trackVisibility.set", {{"hidden", {b, marker, b}}}).at("hidden") == Json{marker, b});
	CHECK(f.hidden() == Json{marker, b});
	{
		const std::scoped_lock lock(mutex);
		REQUIRE(changes.size() == 1);
		CHECK(changes[0].at("hidden") == Json{marker, b});
	}
	CHECK(f.call("project.get").at("trackVisibility").at("hidden") == Json{marker, b});
	CHECK_THAT(f.fails("trackVisibility.set", {{"hidden", {a, 9999}}}), ContainsSubstring("no track"));
	CHECK(f.hidden() == Json{marker, b});

	// Not an undo step: undo takes back the last track, which then no longer shows as hidden.
	f.call("history.undo");
	CHECK(f.hidden() == Json{marker});
	f.call("history.redo");
	CHECK(f.hidden() == Json{marker, b});

	// Saved with the project and loaded again, with the configurations.
	f.call("trackVisibility.set", {{"configurations", {{{"name", "Mix"}, {"hidden", {a}}}}}});
	const test::TempDir dir;
	const std::string file = (dir.path() / "visibility.dawproject").u8string();
	f.call("project.save", {{"path", file}});
	f.call("project.new");
	CHECK(f.hidden().empty());
	f.call("project.load", {{"path", file}});
	CHECK(f.hidden() == Json{marker, b});
	CHECK(f.visibility().at("configurations") == Json{{{"name", "Mix"}, {"hidden", {a}}}});
}

TEST_CASE("the lower zone's mixer and the synced mixer window follow the track visibility, and back", "[unit]")
{
	Fixture f;
	const auto [a, aChannel] = f.track("audio");
	const auto [b, bChannel] = f.track("instrument");
	const uint64_t marker = f.track("marker").first;
	const uint64_t output = f.bus("output");
	f.call("mixer.set", {{"mixer", "window1"}, {"hidden", {output}}});

	// The lower zone's mixer always follows: a hidden track's channel is hidden there; busses keep their state.
	f.call("mixer.set", {{"mixer", "lower"}, {"hidden", {output}}});
	f.call("trackVisibility.set", {{"hidden", {a, marker}}});
	CHECK(f.mixerHidden("lower") == sorted({aChannel, output}));
	CHECK(f.mixerHidden("window1") == Json{output}); // not synced
	f.call("mixer.set", {{"mixer", "lower"}, {"hidden", {bChannel}}});
	CHECK(f.hidden() == Json{b, marker}); // tracks without a channel keep theirs

	// Sync Visibility of Project and Mixer: the window takes the tracks' visibility at once.
	f.call("trackVisibility.set", {{"sync", "window1"}});
	CHECK(f.visibility().at("sync") == "window1");
	CHECK(f.mixerHidden("window1") == sorted({bChannel, output}));
	// Its Visibility tab hides tracks too, which the lower zone's mixer follows.
	f.call("mixer.set", {{"mixer", "window1"}, {"hidden", {aChannel}}});
	CHECK(f.hidden() == Json{a, marker});
	CHECK(f.mixerHidden("lower") == Json{aChannel});

	// Only one window at a time: the first one no longer follows.
	f.call("trackVisibility.set", {{"sync", "window2"}});
	f.call("trackVisibility.set", {{"hidden", Json::array()}});
	CHECK(f.mixerHidden("window1") == Json{aChannel});
	CHECK(f.mixerHidden("window2").empty());
	CHECK(f.mixerHidden("lower").empty());
	f.call("mixer.set", {{"mixer", "window1"}, {"hidden", {bChannel}}});
	CHECK(f.hidden().empty());
	f.call("trackVisibility.set", {{"sync", ""}});
	f.call("mixer.set", {{"mixer", "window2"}, {"hidden", {bChannel}}});
	CHECK(f.hidden().empty());
	CHECK_THAT(f.fails("trackVisibility.set", {{"sync", "lower"}}), ContainsSubstring("sync"));
}

TEST_CASE("visibility configurations keep sets of hidden tracks to switch between", "[unit]")
{
	Fixture f;
	const uint64_t a = f.track("audio").first;
	const uint64_t b = f.track("audio").first;

	// Add Configuration: the tracks hidden now, under a name, which becomes the active one.
	f.call("trackVisibility.set", {{"hidden", {a}}});
	Json set = f.call("trackVisibility.set",
		{{"configurations", {{{"name", "Without A"}, {"hidden", {a}}}}}, {"configuration", "Without A"}});
	CHECK(set.at("configuration") == "Without A");
	CHECK(set.at("modified") == false);

	// A change shows as modified until Update Configuration takes it.
	set = f.call("trackVisibility.set", {{"hidden", {a, b}}});
	CHECK(set.at("configuration") == "Without A");
	CHECK(set.at("modified") == true);
	f.call("trackVisibility.set", {{"configurations", {{{"name", "Without A"}, {"hidden", {a, b}}}}}});
	CHECK(f.visibility().at("modified") == false);

	// Choosing a configuration applies its hidden tracks.
	f.call("trackVisibility.set",
		{{"configurations",
			 {{{"name", "Without A"}, {"hidden", {a, b}}}, {{"name", "All"}, {"hidden", Json::array()}}}},
			{"configuration", "All"}});
	CHECK(f.hidden().empty());
	set = f.call("trackVisibility.set", {{"configuration", "Without A"}});
	CHECK(set.at("hidden") == Json{a, b});
	CHECK(set.at("modified") == false);

	// Names are unique and not empty; the active one is one of them.
	CHECK_THAT(f.fails("trackVisibility.set",
				   {{"configurations",
					   {{{"name", "X"}, {"hidden", Json::array()}}, {{"name", "X"}, {"hidden", Json::array()}}}}}),
		ContainsSubstring("X"));
	CHECK_THAT(f.fails("trackVisibility.set", {{"configurations", {{{"name", ""}, {"hidden", Json::array()}}}}}),
		ContainsSubstring("name"));
	CHECK_THAT(f.fails("trackVisibility.set", {{"configuration", "Nope"}}), ContainsSubstring("Nope"));
	CHECK_THAT(f.fails("trackVisibility.set", {{"configurations", {{{"name", "Y"}, {"hidden", {9999}}}}}}),
		ContainsSubstring("no track"));

	// Delete Configuration: without the active one, none is active; the tracks stay as they are.
	set = f.call("trackVisibility.set", {{"configurations", {{{"name", "All"}, {"hidden", Json::array()}}}}});
	CHECK(set.at("configuration") == "");
	CHECK(set.at("hidden") == Json{a, b});

	// A configuration's tracks that are gone don't show, nor count as a change.
	f.call("trackVisibility.set",
		{{"configurations", {{{"name", "All"}, {"hidden", Json::array()}}, {{"name", "B"}, {"hidden", {b}}}}},
			{"configuration", "B"}});
	f.call("track.remove", {{"track", b}});
	CHECK(f.visibility().at("configurations").at(1).at("hidden").empty());
	CHECK(f.visibility().at("modified") == false);
}
