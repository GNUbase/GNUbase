// SPDX-License-Identifier: GPL-3.0-or-later
// Track heights and fold states in the engine (#491, docs/features/track-list.md): trackLayout.get and
// trackLayout.set keep them with the project, saved with it, not as undo steps, and a file's values are checked.
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <limits>
#include <map>
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

		uint64_t track(const std::string& type)
		{
			return call("track.add", {{"type", type}}).at("track").at("id").get<uint64_t>();
		}

		Json layout() { return call("trackLayout.get"); }

		engine::Engine engine;
		engine::Client client{engine};
	};

	Json height(uint64_t track, double dp)
	{
		return {{"track", track}, {"height", dp}};
	}
}

TEST_CASE("track heights and folded folders are kept with the project, saved with it but not undo steps", "[unit]")
{
	Fixture f;
	std::mutex mutex;
	std::vector<Json> changes;
	const engine::Listener listener(f.engine,
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "trackLayout.changed")
				changes.push_back(event.data);
		});
	CHECK(f.layout() == Json{{"heights", Json::array()}, {"folded", Json::array()}});
	const uint64_t folder = f.track("folder");
	const uint64_t audio = f.track("audio");
	const uint64_t other = f.track("folder");

	// Heights by track, those given change and the others stay; folded replaces the folded folders.
	CHECK(f.call("trackLayout.set", {{"heights", {height(audio, 80.0)}}, {"folded", {folder}}}) ==
		Json{{"heights", {height(audio, 80.0)}}, {"folded", {folder}}});
	CHECK(f.call("trackLayout.set", {{"heights", {height(folder, 30.0)}}}) ==
		Json{{"heights", {height(folder, 30.0), height(audio, 80.0)}}, {"folded", {folder}}});
	{
		const std::scoped_lock lock(mutex);
		REQUIRE(changes.size() == 2);
		CHECK(changes[1] == f.layout());
	}
	CHECK(f.call("project.get").at("trackLayout") == f.layout());

	// Unknown tracks, folded tracks that aren't folders and heights out of range are refused.
	CHECK_THAT(f.fails("trackLayout.set", {{"heights", {height(9999, 50.0)}}}), ContainsSubstring("no track"));
	CHECK_THAT(f.fails("trackLayout.set", {{"folded", {audio}}}), ContainsSubstring("not a folder"));
	CHECK(!f.client.call("trackLayout.set", {{"heights", {height(audio, 10.0)}}}).ok());
	CHECK(f.layout().at("folded") == Json{folder});

	// Not undo steps: a set leaves the history where it is, so undo right after it takes back the last folder, which
	// then leaves the layout, and redo brings it back.
	const Json history = f.call("history.get").at("position");
	f.call("trackLayout.set", {{"folded", {folder, other}}, {"heights", {height(other, 100.0)}}});
	CHECK(f.call("history.get").at("position") == history);
	f.call("history.undo");
	CHECK(f.call("project.get").at("tracks").size() == 2);
	CHECK(f.layout() == Json{{"heights", {height(folder, 30.0), height(audio, 80.0)}}, {"folded", {folder}}});
	f.call("history.redo");
	CHECK(f.layout().at("folded") == Json{folder, other});

	// Saved with the project and loaded again; a new project starts without.
	const test::TempDir dir;
	const std::string file = (dir.path() / "layout.dawproject").u8string();
	f.call("project.save", {{"path", file}});
	f.call("project.new");
	CHECK(f.layout() == Json{{"heights", Json::array()}, {"folded", Json::array()}});
	f.call("project.load", {{"path", file}});
	CHECK(f.layout() ==
		Json{{"heights", {height(folder, 30.0), height(audio, 80.0), height(other, 100.0)}},
			{"folded", {folder, other}}});
}

TEST_CASE("track heights from a file are kept in range and folded tracks that aren't folders are left out", "[unit]")
{
	Fixture f;
	const uint64_t folder = f.track("folder");
	const uint64_t audio = f.track("audio");
	const uint64_t midi = f.track("midi");
	const test::TempDir dir;
	const std::string file = (dir.path() / "layout.dawproject").u8string();
	f.call("trackLayout.set", {{"heights", {height(folder, 50.0)}}, {"folded", {folder}}});
	f.call("project.save", {{"path", file}});

	// A hand-edited file: heights below and above the range, one that isn't a number and ones of tracks that aren't
	// there, and folded an audio track, an unknown track and an id that isn't one.
	std::map<std::string, std::string> entries = test::readZip(file);
	Json extension = Json::parse(entries.at("gnubase.json"));
	Json& layout = extension.at("project").at("trackLayout");
	layout["heights"] = {height(folder, 1.0), height(audio, 1e9), {{"track", midi}, {"height", "tall"}},
		{{"track", -1}, {"height", 50.0}}, height(9999, 50.0)};
	layout["folded"] = {audio, folder, 9999, "x", folder};
	entries["gnubase.json"] = extension.dump();
	test::writeZip(file, entries);

	f.call("project.load", {{"path", file}});
	const Json expected = {{"heights", {height(folder, 20.0), height(audio, 400.0)}}, {"folded", {folder}}};
	CHECK(f.layout() == expected);
	CHECK(f.call("project.get").at("trackLayout") == expected);
}

// #1131: the engine keeps a removed track's height and fold state for undo, which brings the track back, so the
// clients hear of the layout then.
TEST_CASE("undo and redo announce the layout when a track with a height or fold state comes back or goes", "[unit]")
{
	Fixture f;
	std::mutex mutex;
	std::vector<Json> changes;
	const engine::Listener listener(f.engine,
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "trackLayout.changed")
				changes.push_back(event.data);
		});
	const auto announced = [&]
	{
		const std::scoped_lock lock(mutex);
		return changes;
	};
	const auto forget = [&]
	{
		const std::scoped_lock lock(mutex);
		changes.clear();
	};
	const uint64_t folder = f.track("folder");
	const uint64_t audio = f.track("audio");
	const uint64_t plain = f.track("audio");
	f.call("trackLayout.set", {{"heights", {height(audio, 80.0)}}, {"folded", {folder}}});
	const Json full = f.layout();

	// Removing the folder leaves it out of the layout, without a set, and its state stays for undo.
	f.call("track.remove", {{"track", folder}});
	const Json without = f.layout();
	CHECK(without == Json{{"heights", {height(audio, 80.0)}}, {"folded", Json::array()}});
	forget();
	f.call("history.undo");
	REQUIRE(announced().size() == 1);
	CHECK(announced()[0] == full);
	CHECK(f.layout() == full);

	// And redo takes it away again.
	forget();
	f.call("history.redo");
	REQUIRE(announced().size() == 1);
	CHECK(announced()[0] == without);

	// A step that changes none announces nothing, and several steps at once announce the layout once.
	f.call("track.set", {{"track", plain}, {"name", "Other"}});
	forget();
	f.call("history.undo");
	f.call("history.redo");
	CHECK(announced().empty());
	f.call("history.undo", {{"steps", 2}});
	REQUIRE(announced().size() == 1);
	CHECK(announced()[0] == full);
	forget();
	f.call("history.redo", {{"steps", 2}});
	REQUIRE(announced().size() == 1);
	CHECK(announced()[0] == without);
}

// #1131: a set that changes nothing is no change of the project: the revision stays, so the project doesn't count as
// modified, and nothing is announced.
TEST_CASE("trackLayout.set with the layout the project has changes nothing", "[unit]")
{
	Fixture f;
	std::mutex mutex;
	int changes = 0;
	const engine::Listener listener(f.engine,
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "trackLayout.changed")
				++changes;
		});
	const uint64_t folder = f.track("folder");
	const uint64_t audio = f.track("audio");
	const auto revision = [&f] { return f.call("project.revision").at("revision").get<uint64_t>(); };
	const Json set = {{"heights", {height(audio, 80.0)}}, {"folded", {folder}}};
	const Json layout = f.call("trackLayout.set", set);
	const uint64_t first = revision();
	{
		const std::scoped_lock lock(mutex);
		CHECK(changes == 1);
	}

	// The same layout again, its parts, and an empty set return it, and change nothing.
	CHECK(f.call("trackLayout.set", set) == layout);
	CHECK(f.call("trackLayout.set", {{"heights", {height(audio, 80.0)}}}) == layout);
	CHECK(f.call("trackLayout.set", {{"folded", {folder}}}) == layout);
	CHECK(f.call("trackLayout.set", Json::object()) == layout);
	CHECK(revision() == first);
	{
		const std::scoped_lock lock(mutex);
		CHECK(changes == 1);
	}

	// A change still advances it, announces it and is saved: a height, and a folded folder less.
	f.call("trackLayout.set", {{"heights", {height(audio, 81.0)}}});
	CHECK(revision() == first + 1);
	f.call("trackLayout.set", {{"folded", Json::array()}});
	CHECK(revision() == first + 2);
	{
		const std::scoped_lock lock(mutex);
		CHECK(changes == 3);
	}
}

// #1131: in-process clients, such as Lua's 0/0, can send a NaN, which no comparison with the range catches.
TEST_CASE("trackLayout.set refuses a height that is not a number", "[unit]")
{
	Fixture f;
	const uint64_t audio = f.track("audio");
	CHECK_THAT(f.fails("trackLayout.set", {{"heights", {height(audio, std::numeric_limits<double>::quiet_NaN())}}}),
		ContainsSubstring("must be at least 20"));
	CHECK_THAT(f.fails("trackLayout.set", {{"heights", {height(audio, std::numeric_limits<double>::infinity())}}}),
		ContainsSubstring("must be at most 400"));
	CHECK(f.layout() == Json{{"heights", Json::array()}, {"folded", Json::array()}});
}

// #1131: JSON has no NaN or Infinity, and the parser refuses a number that overflows, so a file never reaches the
// check for them in trackLayoutFromJson; a Json built in memory does.
TEST_CASE("trackLayoutFromJson leaves out heights that are not finite and brings the others into range", "[unit]")
{
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double infinity = std::numeric_limits<double>::infinity();
	const engine::TrackLayout layout = engine::trackLayoutFromJson(
		Json{{"heights", {height(1, nan), height(2, infinity), height(3, -infinity), height(4, 90.0), height(5, 1e9)}},
			{"folded", {uint64_t{7}, uint64_t{7}, uint64_t{3}}}});
	CHECK(layout.heights == std::map<uint64_t, double>{{4, 90.0}, {5, 400.0}});
	CHECK(layout.folded == std::vector<uint64_t>{3, 7});
}
