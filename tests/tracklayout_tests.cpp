// SPDX-License-Identifier: GPL-3.0-or-later
// Track heights and fold states in the engine (#491, docs/features/track-list.md): trackLayout.get and
// trackLayout.set keep them with the project, saved with it, not as undo steps, and a file's values are checked.
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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
