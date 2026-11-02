// SPDX-License-Identifier: GPL-3.0-or-later
// What the mixers keep in the project (#106, docs/features/mixer-window.md): each mixer's hidden channels and channel
// types (mixer.get, mixer.set), the channels' notes and the tracks' pictures.
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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

		// The new track and its channel.
		std::pair<uint64_t, uint64_t> track(const std::string& type)
		{
			const Json added = call("track.add", {{"type", type}});
			return {added.at("track").at("id").get<uint64_t>(), added.at("channel").at("id").get<uint64_t>()};
		}

		Json view(const std::string& mixer) { return call("mixer.get", {{"mixer", mixer}}); }

		engine::Engine engine;
		engine::Client client{engine};
	};
}

TEST_CASE("each mixer hides channels and channel types of its own, saved with the project but not undo steps", "[unit]")
{
	Fixture f;
	std::mutex mutex;
	std::vector<Json> changes;
	const engine::Listener listener(f.engine,
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "mixer.changed")
				changes.push_back(event.data);
		});
	const Json none = {{"mixer", "lower"}, {"hidden", Json::array()}, {"hiddenTypes", Json::array()}};
	CHECK(f.view("lower") == none);
	const uint64_t audio = f.track("audio").first;
	const uint64_t groupChannel = f.track("group").second;
	uint64_t input = 0;
	const Json project = f.call("project.get");
	for (const Json& bus : project.at("busses"))
		if (bus.at("direction") == "input")
			input = bus.at("id").get<uint64_t>();

	// Channels and busses by id, sorted and once each; types in the filter's order.
	const Json set = f.call("mixer.set",
		{{"mixer", "window1"}, {"hidden", {groupChannel, input, groupChannel}}, {"hiddenTypes", {"output", "midi"}}});
	CHECK(set == Json{{"mixer", "window1"}, {"hidden", {input, groupChannel}}, {"hiddenTypes", {"midi", "output"}}});
	CHECK(f.view("window1") == set);
	CHECK(f.view("lower") == none); // the others keep theirs
	CHECK(f.call("mixer.set", {{"mixer", "window1"}, {"hiddenTypes", {"audio"}}}).at("hidden") ==
		Json{input, groupChannel}); // what isn't given stays
	{
		const std::scoped_lock lock(mutex);
		REQUIRE(changes.size() == 2);
		CHECK(changes[1].at("hiddenTypes") == Json{"audio"});
	}
	CHECK(f.call("project.get").at("mixers").at("window1").at("hiddenTypes") == Json{"audio"});
	CHECK_THAT(f.fails("mixer.set", {{"mixer", "window1"}, {"hidden", {audio}}}), ContainsSubstring("no channel"));
	CHECK_THAT(f.fails("mixer.set", {{"mixer", "window5"}}), ContainsSubstring("mixer"));
	CHECK_THAT(
		f.fails("mixer.set", {{"mixer", "lower"}, {"hiddenTypes", {"folder"}}}), ContainsSubstring("hiddenTypes"));

	// Not an undo step: undo takes back the group track, whose channel then no longer shows as hidden.
	f.call("history.undo");
	CHECK(f.view("window1").at("hidden") == Json{input});

	// Saved with the project and loaded again.
	f.call("history.redo");
	const test::TempDir dir;
	const std::string file = (dir.path() / "mixers.dawproject").u8string();
	f.call("project.save", {{"path", file}});
	f.call("project.new");
	CHECK(f.view("window1").at("hiddenTypes").empty());
	f.call("project.load", {{"path", file}});
	CHECK(
		f.view("window1") == Json{{"mixer", "window1"}, {"hidden", {input, groupChannel}}, {"hiddenTypes", {"audio"}}});
}

TEST_CASE("channels have notes and tracks a picture, as undo steps saved with the project", "[unit]")
{
	Fixture f;
	const auto [track, channel] = f.track("audio");
	CHECK(f.call("channel.set", {{"channel", channel}, {"notes", "Take 3 is the one"}}).at("channel").at("notes") ==
		"Take 3 is the one");
	CHECK(f.call("track.set", {{"track", track}, {"picture", "pictures/guitar.png"}}).at("track").at("picture") ==
		"pictures/guitar.png");
	f.call("history.undo");
	CHECK(f.call("project.get").at("tracks").at(0).at("picture") == "");
	f.call("history.redo");

	// A copy of the track has them too.
	const Json copy = f.call("track.duplicate", {{"track", track}}).at("track");
	CHECK(copy.at("picture") == "pictures/guitar.png");

	const test::TempDir dir;
	const std::string file = (dir.path() / "notes.dawproject").u8string();
	f.call("project.save", {{"path", file}});
	f.call("project.load", {{"path", file}});
	const Json project = f.call("project.get");
	CHECK(project.at("tracks").at(0).at("picture") == "pictures/guitar.png");
	for (const Json& item : project.at("channels"))
		CHECK(item.at("notes") == "Take 3 is the one");
}
