// SPDX-License-Identifier: GPL-3.0-or-later
// Track visibility in the engine (#166, docs/features/track-visibility.md): the tracks the project window hides
// (trackVisibility.get, trackVisibility.set), the visibility configurations, and the mixers that follow it.
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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

// #906: a hidden folder hides the tracks inside it everywhere: trackVisibility.set takes them into hidden, the mixers
// that follow hide their channels, and hiding the folder's group channel in a mixer that follows hides them too.
TEST_CASE("a hidden folder hides the tracks inside it in the mixers that follow, both ways", "[unit]")
{
	Fixture f;
	// A track and a folder in a folder with a group channel, and a track in the nested one.
	const Json band = f.call("track.add", {{"type", "folder"}, {"groupChannel", true}, {"name", "Band"}});
	const uint64_t folder = band.at("track").at("id").get<uint64_t>();
	const uint64_t group = band.at("channel").at("id").get<uint64_t>();
	const auto [guitar, guitarChannel] = f.track("audio");
	f.call("track.move", {{"track", guitar}, {"parent", folder}, {"index", 0}});
	const uint64_t nested = f.call("track.add", {{"type", "folder"}, {"parent", folder}, {"index", 1}})
								.at("track")
								.at("id")
								.get<uint64_t>();
	const auto [keys, keysChannel] = f.track("instrument");
	f.call("track.move", {{"track", keys}, {"parent", nested}, {"index", 0}});

	// trackVisibility.set with the folder alone takes the tracks inside it into hidden, nested ones too, and the
	// lower zone's mixer hides their channels.
	f.call("trackVisibility.set", {{"hidden", {folder}}});
	CHECK(f.hidden() == sorted({folder, nested, guitar, keys}));
	CHECK(f.mixerHidden("lower") == sorted({group, guitarChannel, keysChannel}));

	// A synced window follows the same way.
	f.call("trackVisibility.set", {{"sync", "window1"}, {"hidden", Json::array()}});
	REQUIRE(f.hidden().empty());
	REQUIRE(f.mixerHidden("lower").empty());
	REQUIRE(f.mixerHidden("window1").empty());
	f.call("trackVisibility.set", {{"hidden", {folder}}});
	CHECK(f.mixerHidden("window1") == sorted({group, guitarChannel, keysChannel}));

	// Hiding the folder's group channel in a mixer that follows hides the tracks inside it too, and the mixers that
	// follow hide their channels.
	f.call("trackVisibility.set", {{"hidden", Json::array()}});
	REQUIRE(f.mixerHidden("window1").empty());
	f.call("mixer.set", {{"mixer", "window1"}, {"hidden", {group}}});
	CHECK(f.hidden() == sorted({folder, nested, guitar, keys}));
	CHECK(f.mixerHidden("window1") == sorted({group, guitarChannel, keysChannel}));
	CHECK(f.mixerHidden("lower") == sorted({group, guitarChannel, keysChannel}));

	// Showing an inside track's channel while its folder stays hidden shows it not: the folder hides it again.
	f.call("mixer.set", {{"mixer", "lower"}, {"hidden", sorted({group, keysChannel})}});
	CHECK(f.hidden() == sorted({folder, nested, guitar, keys}));
	CHECK(f.mixerHidden("lower") == sorted({group, guitarChannel, keysChannel}));

	// Showing the folder's group channel again shows the tracks inside it too, and the mixers that follow show their
	// channels (#1366).
	f.call("mixer.set", {{"mixer", "window1"}, {"hidden", sorted({guitarChannel, keysChannel})}});
	CHECK(f.hidden().empty());
	CHECK(f.mixerHidden("window1").empty());
	CHECK(f.mixerHidden("lower").empty());

	// A configuration with the folder keeps the tracks inside it with it, and applying it hides them.
	f.call("trackVisibility.set", {{"sync", ""}, {"hidden", Json::array()}});
	f.call("trackVisibility.set",
		{{"configurations", {{{"name", "Without Band"}, {"hidden", {folder}}}}}, {"configuration", "Without Band"}});
	CHECK(f.hidden() == sorted({folder, nested, guitar, keys}));
	CHECK(f.visibility().at("configurations") ==
		Json{{{"name", "Without Band"}, {"hidden", sorted({folder, nested, guitar, keys})}}});

	// A track that joins the folder later is the configuration's too, which doesn't show as modified once it is applied
	// (#1366, #1475).
	f.call("trackVisibility.set", {{"hidden", Json::array()}});
	const uint64_t drums =
		f.call("track.add", {{"type", "audio"}, {"parent", folder}, {"index", 0}}).at("track").at("id").get<uint64_t>();
	const Json applied = f.call("trackVisibility.set", {{"configuration", "Without Band"}});
	CHECK(applied.at("hidden") == sorted({folder, nested, guitar, keys, drums}));
	CHECK(applied.at("modified") == false);
	CHECK(applied.at("configurations") ==
		Json{{{"name", "Without Band"}, {"hidden", sorted({folder, nested, guitar, keys, drums})}}});
}

// #906: what a file has for the track visibility is checked on load as trackVisibility.set checks it: an unknown sync,
// names that are empty or there twice, an active name that isn't there and ids that aren't tracks don't come in, and
// a hidden folder comes in with the tracks inside it.
TEST_CASE("the track visibility from a file is checked like trackVisibility.set checks it", "[unit]")
{
	Fixture f;
	const Json band = f.call("track.add", {{"type", "folder"}, {"groupChannel", true}});
	const uint64_t folder = band.at("track").at("id").get<uint64_t>();
	const uint64_t group = band.at("channel").at("id").get<uint64_t>();
	const auto [guitar, guitarChannel] = f.track("audio");
	f.call("track.move", {{"track", guitar}, {"parent", folder}, {"index", 0}});
	const test::TempDir dir;
	const std::string file = (dir.path() / "visibility.dawproject").u8string();
	f.call("project.save", {{"path", file}});

	// A hand-edited file: a sync that isn't a mixer window, configurations without a name and with a name twice, an
	// active one that isn't there, ids that aren't tracks and that aren't ids, and a folder without its inside.
	std::map<std::string, std::string> entries = test::readZip(file);
	Json extension = Json::parse(entries.at("gnubase.json"));
	Json& visibility = extension.at("project").at("trackVisibility");
	visibility["hidden"] = {folder, 9999, "x"};
	visibility["sync"] = "bogus";
	visibility["configurations"] = {{{"name", ""}, {"hidden", Json::array()}}, {{"name", "One"}, {"hidden", {folder}}},
		{{"name", "One"}, {"hidden", Json::array()}}, {{"name", "Two"}, {"hidden", {9999, guitar}}}};
	visibility["configuration"] = "Nope";
	entries["gnubase.json"] = extension.dump();
	test::writeZip(file, entries);

	f.call("project.load", {{"path", file}});
	CHECK(f.visibility().at("sync") == "");
	CHECK(f.hidden() == sorted({folder, guitar}));
	CHECK(f.visibility().at("configurations") ==
		Json{{{"name", "One"}, {"hidden", sorted({folder, guitar})}}, {{"name", "Two"}, {"hidden", Json{guitar}}}});
	CHECK(f.visibility().at("configuration") == "");
	// The unknown sync didn't make a mixer view of its own, which the project would save.
	CHECK(!f.call("project.get").at("mixers").contains("bogus"));
	// The lower zone's mixer follows the loaded visibility, with the tracks that joined it from their folder (#1366).
	CHECK(f.mixerHidden("lower") == sorted({group, guitarChannel}));

	// Values of the wrong JSON types are left out, not refused.
	CHECK_NOTHROW(engine::trackVisibilityFromJson(
		Json{{"hidden", 5}, {"sync", 3}, {"configurations", "x"}, {"configuration", 7}}));
	const engine::TrackVisibility none = engine::trackVisibilityFromJson(
		Json{{"hidden", "x"}, {"sync", 3}, {"configurations", {7, {{"name", 1}}}}, {"configuration", 7}});
	CHECK(none.hidden.empty());
	CHECK(none.sync.empty());
	CHECK(none.configurations.size() == 2);
	CHECK(none.configurations.at(0).name.empty());
	CHECK(none.configuration.empty());
}

// #1475: a track that joins a hidden folder later, by track.add or track.move, hides with it right away: it joins
// hidden and the configurations with the folder, and the mixers that follow hide its channel, so the active
// configuration doesn't show as modified, nor after a mixer.set on a mixer that follows.
TEST_CASE("a track that joins a hidden folder later hides with it right away", "[unit]")
{
	Fixture f;
	const Json band = f.call("track.add", {{"type", "folder"}, {"groupChannel", true}, {"name", "Band"}});
	const uint64_t folder = band.at("track").at("id").get<uint64_t>();
	const uint64_t group = band.at("channel").at("id").get<uint64_t>();
	f.call("trackVisibility.set",
		{{"configurations", {{{"name", "Without Band"}, {"hidden", {folder}}}}}, {"configuration", "Without Band"}});
	REQUIRE(f.hidden() == Json{folder});

	const Json added = f.call("track.add", {{"type", "audio"}, {"parent", folder}, {"index", 0}});
	const uint64_t drums = added.at("track").at("id").get<uint64_t>();
	const uint64_t drumsChannel = added.at("channel").at("id").get<uint64_t>();
	CHECK(f.hidden() == sorted({folder, drums}));
	CHECK(f.visibility().at("modified") == false);
	CHECK(f.visibility().at("configurations").at(0).at("hidden") == sorted({folder, drums}));
	CHECK(f.mixerHidden("lower") == sorted({group, drumsChannel}));

	const auto [bass, bassChannel] = f.track("audio");
	f.call("track.move", {{"track", bass}, {"parent", folder}, {"index", 0}});
	CHECK(f.hidden() == sorted({folder, drums, bass}));
	CHECK(f.visibility().at("modified") == false);
	CHECK(f.mixerHidden("lower") == sorted({group, drumsChannel, bassChannel}));

	// A mixer.set on a mixer that follows that changes only its filter changes nothing of the track visibility.
	f.call("mixer.set", {{"mixer", "lower"}, {"hiddenTypes", {"effect"}}});
	CHECK(f.hidden() == sorted({folder, drums, bass}));
	CHECK(f.visibility().at("modified") == false);

	// Undoing the move takes the track out of the folder, and it shows again with its channel; redoing it hides it
	// again (#1533).
	f.call("history.undo");
	CHECK(f.hidden() == sorted({folder, drums}));
	CHECK(f.mixerHidden("lower") == sorted({group, drumsChannel}));
	f.call("history.redo");
	CHECK(f.hidden() == sorted({folder, drums, bass}));
	CHECK(f.visibility().at("modified") == false);
}

// #1475: applying a configuration keeps the ids of removed tracks in it, so that undoing the removal brings the track
// back into it, hidden: announced, and with its channel hidden in the mixers that follow.
TEST_CASE("applying a configuration keeps the tracks that are removed for undo", "[unit]")
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
	const auto [b, bChannel] = f.track("audio");
	f.call("trackVisibility.set", {{"configurations", {{{"name", "Without B"}, {"hidden", {b}}}}}});
	f.call("track.remove", {{"track", b}});
	const Json applied = f.call("trackVisibility.set", {{"configuration", "Without B"}});
	CHECK(applied.at("hidden").empty());
	CHECK(applied.at("modified") == false);
	{
		const std::scoped_lock lock(mutex);
		changes.clear();
	}

	f.call("history.undo");
	const Json visibility = f.visibility();
	CHECK(visibility.at("configurations").at(0).at("hidden") == Json{b});
	CHECK(visibility.at("hidden") == Json{b});
	CHECK(visibility.at("modified") == false);
	{
		const std::scoped_lock lock(mutex);
		REQUIRE(changes.size() == 1);
		CHECK(changes[0].at("hidden") == Json{b});
	}
	CHECK(f.mixerHidden("lower") == Json{bChannel});

	// The lower zone's mixer hides the channel, so a mixer.set that changes only its filter leaves the track hidden.
	f.call("mixer.set", {{"mixer", "lower"}, {"hiddenTypes", {"effect"}}});
	CHECK(f.hidden() == Json{b});
	CHECK(f.visibility().at("modified") == false);
}

// #1475: an undo step that puts a track back into a folder hidden since hides it with the folder, with its channel.
TEST_CASE("a track that an undo step puts into a hidden folder hides with it", "[unit]")
{
	Fixture f;
	const Json band = f.call("track.add", {{"type", "folder"}, {"groupChannel", true}, {"name", "Band"}});
	const uint64_t folder = band.at("track").at("id").get<uint64_t>();
	const uint64_t group = band.at("channel").at("id").get<uint64_t>();
	const Json added = f.call("track.add", {{"type", "audio"}, {"parent", folder}, {"index", 0}});
	const uint64_t drums = added.at("track").at("id").get<uint64_t>();
	const uint64_t drumsChannel = added.at("channel").at("id").get<uint64_t>();
	f.call("track.move", {{"track", drums}, {"parent", 0}, {"index", 0}});
	f.call("trackVisibility.set",
		{{"configurations", {{{"name", "Without Band"}, {"hidden", {folder}}}}}, {"configuration", "Without Band"}});
	REQUIRE(f.hidden() == Json{folder});

	f.call("history.undo");
	CHECK(f.hidden() == sorted({folder, drums}));
	CHECK(f.visibility().at("configurations").at(0).at("hidden") == sorted({folder, drums}));
	CHECK(f.visibility().at("modified") == false);
	CHECK(f.mixerHidden("lower") == sorted({group, drumsChannel}));
}

// #1533: a track that leaves a hidden folder leaves hidden and the configurations with the folder, as the folder
// brought it in: by track.move, or by an undo or redo step.
TEST_CASE("a track that leaves a hidden folder shows again", "[unit]")
{
	Fixture f;
	const uint64_t folder = f.call("track.add", {{"type", "folder"}}).at("track").at("id").get<uint64_t>();
	const auto [drums, drumsChannel] = f.track("audio");
	const uint64_t other = f.track("audio").first;

	// A configuration that hides the folder and isn't the active one: a track dragged into the folder and back by
	// undo isn't the configuration's, so applying it later hides the folder alone.
	f.call("trackVisibility.set",
		{{"configurations",
			 {{{"name", "Without Band"}, {"hidden", {folder}}}, {{"name", "Without Other"}, {"hidden", {other}}}}},
			{"configuration", "Without Other"}});
	f.call("track.move", {{"track", drums}, {"parent", folder}, {"index", 0}});
	CHECK(f.visibility().at("configurations").at(0).at("hidden") == sorted({folder, drums}));
	f.call("history.undo");
	CHECK(f.visibility().at("configurations").at(0).at("hidden") == Json{folder});
	CHECK(f.call("trackVisibility.set", {{"configuration", "Without Band"}}).at("hidden") == Json{folder});
	CHECK(f.mixerHidden("lower").empty());

	// A track moved out of a folder that is hidden then: undoing the move hides it with the folder, redoing it shows
	// it again, as before the undo.
	f.call("track.move", {{"track", drums}, {"parent", folder}, {"index", 0}});
	REQUIRE(f.hidden() == sorted({folder, drums}));
	f.call("trackVisibility.set", {{"hidden", Json::array()}});
	f.call("track.move", {{"track", drums}, {"parent", 0}, {"index", 0}});
	f.call("trackVisibility.set", {{"hidden", {folder}}});
	f.call("history.undo");
	CHECK(f.hidden() == sorted({folder, drums}));
	CHECK(f.mixerHidden("lower") == Json{drumsChannel});
	f.call("history.redo");
	CHECK(f.hidden() == Json{folder});
	CHECK(f.mixerHidden("lower").empty());

	// Moved out by track.move, it shows too; a track hidden on its own stays hidden wherever it goes.
	f.call("track.move", {{"track", drums}, {"parent", folder}, {"index", 0}});
	REQUIRE(f.hidden() == sorted({folder, drums}));
	f.call("track.move", {{"track", drums}, {"parent", 0}, {"index", 0}});
	CHECK(f.hidden() == Json{folder});
	f.call("trackVisibility.set", {{"hidden", {other}}});
	f.call("track.move", {{"track", other}, {"parent", folder}, {"index", 0}});
	f.call("track.move", {{"track", other}, {"parent", 0}, {"index", 0}});
	CHECK(f.hidden() == Json{other});
}

// #1539: after an edit, the mixers that follow sync, and trackVisibility.changed goes out when what
// trackVisibility.get returns changed: a hidden folder that gets a group channel later, a hidden track removed.
TEST_CASE("an edit that changes what the track visibility hides announces it and syncs the mixers", "[unit]")
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
	const uint64_t folder = f.call("track.add", {{"type", "folder"}}).at("track").at("id").get<uint64_t>();
	const auto [b, bChannel] = f.track("audio");
	f.call("trackVisibility.set",
		{{"configurations", {{{"name", "Hidden"}, {"hidden", {folder, b}}}}}, {"configuration", "Hidden"}});
	REQUIRE(f.mixerHidden("lower") == Json{bChannel});

	// The folder's new group channel is hidden in the lower zone's mixer, so a mixer.set there leaves the folder
	// hidden.
	f.call("track.set", {{"track", folder}, {"groupChannel", true}});
	const Json lower = f.mixerHidden("lower");
	CHECK(lower.size() == 2);
	CHECK(std::find(lower.begin(), lower.end(), Json(bChannel)) != lower.end());
	f.call("mixer.set", {{"mixer", "lower"}, {"hiddenTypes", {"effect"}}});
	CHECK(f.hidden() == sorted({folder, b}));
	CHECK(f.visibility().at("modified") == false);

	// Removing a hidden track changes what trackVisibility.get returns, which is announced.
	{
		const std::scoped_lock lock(mutex);
		changes.clear();
	}
	f.call("track.remove", {{"track", b}});
	{
		const std::scoped_lock lock(mutex);
		REQUIRE(changes.size() == 1);
		CHECK(changes[0].at("hidden") == Json{folder});
	}
}

// #1539: applying a configuration in a transaction, after a track joined one of its folders, hides that track too.
TEST_CASE("a configuration applied in a transaction hides the tracks that joined its folders", "[unit]")
{
	Fixture f;
	const uint64_t folder = f.call("track.add", {{"type", "folder"}}).at("track").at("id").get<uint64_t>();
	f.call("trackVisibility.set", {{"configurations", {{{"name", "Without Band"}, {"hidden", {folder}}}}}});
	const Json result = f.call("history.transaction",
		{{"description", "Add and hide"},
			{"commands",
				{{{"command", "track.add"}, {"parameters", {{"type", "audio"}, {"parent", folder}, {"index", 0}}}},
					{{"command", "trackVisibility.set"}, {"parameters", {{"configuration", "Without Band"}}}}}}});
	const uint64_t drums = result.at("results").at(0).at("track").at("id").get<uint64_t>();
	CHECK(result.at("results").at(1).at("hidden") == sorted({folder, drums}));
	CHECK(f.hidden() == sorted({folder, drums}));
}
