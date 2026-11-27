// SPDX-License-Identifier: GPL-3.0-or-later
// What the mixers keep in the project (#106, docs/features/mixer-window.md): each mixer's hidden channels and channel
// types (mixer.get, mixer.set), the channels' notes and the tracks' pictures.
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
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
	// A picture's path is absolute, relative ones to the working folder, so the mixer finds it from anywhere.
	const std::string picture = (std::filesystem::current_path() / "pictures" / "guitar.png").u8string();
	CHECK(f.call("track.set", {{"track", track}, {"picture", "pictures/guitar.png"}}).at("track").at("picture") ==
		picture);
	f.call("history.undo");
	CHECK(f.call("project.get").at("tracks").at(0).at("picture").get<std::string>().empty());
	f.call("history.redo");

	// A copy of the track has them too.
	const Json copy = f.call("track.duplicate", {{"track", track}}).at("track");
	CHECK(copy.at("picture") == picture);
	const Json channels = f.call("project.get").at("channels");
	CHECK(std::count_if(channels.begin(), channels.end(),
			  [](const Json& item) { return item.value("notes", "") == "Take 3 is the one"; }) == 2);

	const test::TempDir dir;
	const std::string file = (dir.path() / "notes.dawproject").u8string();
	f.call("project.save", {{"path", file}});
	f.call("project.load", {{"path", file}});
	const Json project = f.call("project.get");
	CHECK(project.at("tracks").at(0).at("picture") == picture); // missing, so referred to where it was
	for (const Json& item : project.at("channels"))
		CHECK(item.at("notes") == "Take 3 is the one");
}

TEST_CASE("a track's picture goes with its project file", "[unit]")
{
	Fixture f;
	const uint64_t track = f.track("audio").first;
	const test::TempDir dir;
	const std::filesystem::path picture = dir.path() / "guitar.png";
	std::ofstream(picture, std::ios::binary) << "a picture";
	f.call("track.set", {{"track", track}, {"picture", picture.u8string()}});
	const auto loaded = [&f]
	{ return std::filesystem::u8path(f.call("project.get").at("tracks").at(0).at("picture").get<std::string>()); };

	// Saved into the file, as media files are: once the picture is gone, the load extracts it next to the file.
	const std::filesystem::path file = dir.path() / "pictures.dawproject";
	f.call("project.save", {{"path", file.u8string()}});
	std::filesystem::remove(picture);
	f.call("project.load", {{"path", file.u8string()}});
	CHECK(loaded().parent_path() == dir.path() / "pictures media" / "pictures");
	CHECK(test::readText(loaded()) == "a picture");
	// Not unused: cleaning up the media folder keeps it.
	CHECK(f.call("media.cleanup").at("files").empty());

	// Without embedding, a picture in the file's folder is referred to relative to the file, so the folder can move.
	f.call("project.save", {{"path", file.u8string()}, {"embedMedia", false}});
	const test::TempDir moved;
	std::filesystem::copy(dir.path(), moved.path(), std::filesystem::copy_options::recursive);
	std::filesystem::remove_all(dir.path() / "pictures media");
	f.call("project.load", {{"path", (moved.path() / "pictures.dawproject").u8string()}});
	CHECK(test::readText(loaded()) == "a picture");

	// While embedding, a missing picture in the file's folder is referred to by its absolute path: a relative one
	// could name another track's picture that the same save embeds, here as pictures/guitar.png.
	const std::filesystem::path missing = dir.path() / "pictures" / "guitar.png";
	f.call(
		"track.set", {{"track", f.call("project.get").at("tracks").at(0).at("id")}, {"picture", missing.u8string()}});
	const test::TempDir elsewhere;
	std::ofstream(elsewhere.path() / "guitar.png", std::ios::binary) << "another picture";
	f.call("track.set", {{"track", f.track("audio").first}, {"picture", (elsewhere.path() / "guitar.png").u8string()}});
	f.call("project.save", {{"path", file.u8string()}});
	f.call("project.load", {{"path", file.u8string()}});
	const Json tracks = f.call("project.get").at("tracks");
	CHECK(std::filesystem::u8path(tracks.at(0).at("picture").get<std::string>()) == missing);
	CHECK(test::readText(std::filesystem::u8path(tracks.at(1).at("picture").get<std::string>())) == "another picture");
}

TEST_CASE("each mixer undoes and redoes its last ten visibility changes", "[unit]")
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
	std::vector<uint64_t> channels;
	for (int i = 0; i < 12; ++i)
		channels.push_back(f.track("audio").second);
	const auto hidden = [&f](const std::string& mixer) { return f.view(mixer).at("hidden"); };
	const auto undo = [&f](const std::string& mixer)
	{ return f.call("mixer.undoVisibility", {{"mixer", mixer}}).at("hidden"); };

	// Nothing to undo or redo yet: the view stays.
	CHECK(undo("window1").empty());
	CHECK(f.call("mixer.redoVisibility", {{"mixer", "window1"}}).at("hidden").empty());
	CHECK(changes.empty());

	// Hidden channels and the type filter are visibility changes; a set that changes nothing isn't one.
	f.call("mixer.set", {{"mixer", "window1"}, {"hidden", {channels[0]}}});
	f.call("mixer.set", {{"mixer", "window1"}, {"hiddenTypes", {"output"}}});
	f.call("mixer.set", {{"mixer", "window1"}, {"hidden", {channels[0]}}});
	f.call("mixer.set", {{"mixer", "lower"}, {"hidden", {channels[1]}}}); // the other mixers have their own
	CHECK(f.call("mixer.undoVisibility", {{"mixer", "window1"}}).at("hiddenTypes").empty());
	CHECK(undo("window1") == Json::array());
	CHECK(undo("window1") == Json::array()); // nothing more
	CHECK(hidden("lower") == Json{channels[1]});
	{
		const std::scoped_lock lock(mutex);
		REQUIRE(changes.size() == 6);
		CHECK(changes.back() == f.view("window1"));
	}

	// Redo brings them back, until a new change.
	CHECK(f.call("mixer.redoVisibility", {{"mixer", "window1"}}).at("hidden") == Json{channels[0]});
	f.call("mixer.set", {{"mixer", "window1"}, {"hidden", {channels[2]}}});
	CHECK(f.call("mixer.redoVisibility", {{"mixer", "window1"}}).at("hidden") == Json{channels[2]});

	// Ten changes back at most.
	for (const uint64_t channel : channels)
		f.call("mixer.set", {{"mixer", "window2"}, {"hidden", {channel}}});
	for (int i = 0; i < 12; ++i)
		undo("window2");
	CHECK(hidden("window2") == Json{channels[1]});
	CHECK_THAT(f.fails("mixer.undoVisibility", {{"mixer", "window5"}}), ContainsSubstring("mixer"));
}
