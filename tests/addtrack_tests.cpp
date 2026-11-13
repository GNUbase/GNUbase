// SPDX-License-Identifier: GPL-3.0-or-later
// The add track dialog (#80, docs/features/add-track.md) in the app's UI, driven with ui::Automation as MCP's UI tools
// drive it: every track type it adds makes the same project as the same tracks added with MCP's track_add.
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "mcp/server.h"
#include "ui/automation.h"
#include "ui/shell.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	void click(test::UiApp& app, const std::string& selector, int index = 0, int count = 1)
	{
		app.ui().click(std::nullopt, {selector, index, std::nullopt}, ui::MouseButton::Left, count, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// Opens a drop-down list, moves that many options down from the selected one (up when negative) and closes it.
	// Its options aren't in the DOM that the UI tools see.
	void choose(test::UiApp& app, const std::string& select, int down)
	{
		click(app, select);
		for (int i = 0; i < (down < 0 ? -down : down); ++i)
			app.ui().key(std::nullopt, down < 0 ? "Up" : "Down");
		click(app, select);
	}

	bool open(test::UiApp& app)
	{
		return !elements(app, "#add-track-dialog").empty();
	}

	// A page of the dialog, by its track type.
	void page(test::UiApp& app, const std::string& type)
	{
		click(app, "#type-" + type);
		REQUIRE(elements(app, "#track-types").at(0).at("attributes").at("value") == type);
	}

	void setCount(test::UiApp& app, int count)
	{
		click(app, "#track-count", 0, 2);
		app.ui().text(std::nullopt, std::to_string(count));
		app.ui().key(std::nullopt, "Enter");
	}

	void setName(test::UiApp& app, const std::string& name)
	{
		click(app, "#track-name");
		app.ui().text(std::nullopt, name);
	}

	uint64_t idOf(const Json& added)
	{
		return added.at("track").at("id").get<uint64_t>();
	}

	std::vector<std::string> names(test::UiApp& app)
	{
		std::vector<std::string> result;
		const Json project = app.call("project.get");
		for (const Json& track : project.at("tracks"))
			result.push_back(track.at("name").get<std::string>());
		return result;
	}

	// Selects the tracks once the track list shows them.
	void select(test::UiApp& app, const std::vector<uint64_t>& tracks)
	{
		REQUIRE(app.waitUntil(
			[&]
			{
				app.appUi().trackList()->select(tracks);
				return app.appUi().trackList()->selection() == tracks;
			}));
	}

	// A double click near the bottom of the element, below the tracks.
	void doubleClickBelow(test::UiApp& app, const std::string& selector)
	{
		const Json box = elements(app, selector).at(0).at("box");
		const auto x = static_cast<int>(box.at("x").get<double>() + (box.at("width").get<double>() / 2));
		const auto y = static_cast<int>(box.at("y").get<double>() + box.at("height").get<double>() - 60);
		app.ui().click(std::nullopt, {"", 0, ui::Point{x, y}}, ui::MouseButton::Left, 2, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// Tracks added with MCP's tools, as a client of an in-process server, through JSON text.
	class Mcp
	{
	public:
		explicit Mcp(engine::Engine& engine) : m_server(engine)
		{
			request("initialize",
				{{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()},
					{"clientInfo", {{"name", "gnubase-tests"}, {"version", "1"}}}});
		}

		Json call(const std::string& tool, const Json& arguments = Json::object())
		{
			const Json result = request("tools/call", {{"name", tool}, {"arguments", arguments}}).at("result");
			INFO(tool << ": " << result.dump());
			REQUIRE(result.at("isError") == false);
			return result.value("structuredContent", Json::object());
		}

	private:
		Json request(const std::string& method, const Json& params)
		{
			const std::string text =
				Json{{"jsonrpc", "2.0"}, {"id", ++m_id}, {"method", method}, {"params", params}}.dump();
			const std::optional<Json> response = m_server.handle(Json::parse(text));
			REQUIRE(response.has_value());
			return Json::parse(response.value_or(Json()).dump());
		}

		mcp::Server m_server;
		int m_id = 0;
	};
}

TEST_CASE("the add track dialog adds every track type as MCP's track_add does", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	click(app, "#add-track");
	REQUIRE(app.waitUntil([&app] { return open(app); }));

	// Two mono audio tracks called Vox; the audio page shows first.
	CHECK(elements(app, "#track-format").at(0).at("value") == "stereo");
	CHECK(elements(app, "#track-input").size() == 1);
	setName(app, "Vox");
	choose(app, "#track-format", -1); // mono, before stereo
	setCount(app, 2);
	click(app, "#add-track-confirm");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));

	// The other types with Keep Dialog Open, each page with its own settings.
	click(app, "#add-track");
	REQUIRE(app.waitUntil([&app] { return open(app); }));
	click(app, "#keep-open");
	page(app, "group");
	CHECK(elements(app, "#track-input").empty());
	click(app, "#add-track-confirm");
	page(app, "folder");
	CHECK(elements(app, "#track-output").at(0).at("attributes").contains("disabled"));
	click(app, "#track-group-channel");
	CHECK_FALSE(elements(app, "#track-output").at(0).at("attributes").contains("disabled"));
	click(app, "#add-track-confirm");
	page(app, "instrument");
	choose(app, "#track-instrument", 1); // the first built-in instrument after No Instrument
	const std::string instrument = elements(app, "#track-instrument").at(0).at("value").get<std::string>();
	CHECK(!instrument.empty());
	click(app, "#add-track-confirm");
	page(app, "midi");
	choose(app, "#track-output", 1); // the instrument track's channel, after Not Connected
	setCount(app, 3);
	click(app, "#add-track-confirm");
	page(app, "effect");
	choose(app, "#track-output", 1); // the group channel: No Bus, the main output, Group 01, Folder 01
	click(app, "#add-track-confirm");
	page(app, "marker");
	setName(app, "Song");
	click(app, "#add-track-confirm");
	page(app, "ruler");
	CHECK(elements(app, "#track-name").empty());
	choose(app, "#track-time-format", 2); // timecode, after Bars+Beats and Seconds
	// A time offset that isn't a timecode keeps the dialog open with a message; a second is two beats.
	click(app, "#track-time-offset");
	app.ui().text(std::nullopt, "x");
	click(app, "#add-track-confirm");
	CHECK_THAT(elements(app, "#add-track-message").at(0).value("text", ""), ContainsSubstring("time offset"));
	click(app, "#track-time-offset");
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "00:00:01:00");
	click(app, "#add-track-confirm");
	CHECK(open(app));
	CHECK(elements(app, "#add-track-message").at(0).value("text", "").empty());
	click(app, "#add-track-cancel");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));

	// The same tracks through MCP, in a project of their own.
	engine::Engine engine;
	Mcp mcp(engine);
	const Json busses = mcp.call("project_get").at("busses");
	const Json input = busses.at(0).at("id");
	const Json output = busses.at(1).at("id");
	for (int i = 0; i < 2; ++i)
		mcp.call("track_add",
			{{"type", "audio"}, {"name", "Vox"}, {"format", "mono"}, {"input", input}, {"output", output}});
	const Json group = mcp.call("track_add", {{"type", "group"}, {"format", "stereo"}, {"output", output}});
	mcp.call("track_add", {{"type", "folder"}, {"groupChannel", true}, {"format", "stereo"}, {"output", output}});
	const Json synth = mcp.call("track_add", {{"type", "instrument"}, {"instrument", instrument}, {"output", output}});
	for (int i = 0; i < 3; ++i)
		mcp.call("track_add", {{"type", "midi"}, {"output", synth.at("channel").at("id")}});
	mcp.call("track_add", {{"type", "effect"}, {"format", "stereo"}, {"output", group.at("channel").at("id")}});
	mcp.call("track_add", {{"type", "marker"}, {"name", "Song"}});
	mcp.call("track_add",
		{{"type", "ruler"}, {"timeFormat", "timecode"}, {"timeOffset", 2 * time::g_ticksPerQuarter},
			{"timeOffsetOn", true}});

	const Json expected = mcp.call("project_get");
	const Json actual = app.call("project.get");
	CHECK(actual.at("tracks") == expected.at("tracks"));
	CHECK(actual.at("channels") == expected.at("channels"));
	CHECK(actual.at("activeMarkerTrack") == expected.at("activeMarkerTrack"));
	// Count tracks are one undo step: four undo the ruler, marker, effect and three MIDI tracks.
	for (int i = 0; i < 4; ++i)
		app.call("history.undo");
	CHECK(app.call("project.get").at("tracks").size() == actual.at("tracks").size() - 6);
}

TEST_CASE("the add track dialog shows what the engine refuses and stays open", "[ui]")
{
	test::UiApp app({900, 640});
	for (int i = 0; i < 10; ++i)
		app.call("track.add", {{"type", "marker"}});
	app.keys().run("project.addTrack", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return open(app); }));
	page(app, "marker");
	click(app, "#add-track-confirm");
	CHECK(open(app));
	CHECK_THAT(elements(app, "#add-track-message").at(0).value("text", ""), ContainsSubstring("at most 10"));
	CHECK(app.call("project.get").at("tracks").size() == 10);
	app.ui().key(std::nullopt, "Escape");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));
}

TEST_CASE("Group and Effect Track to Selected Tracks route the selected tracks into the new track", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const Json first = app.call("track.add", {{"type", "audio"}});
	const Json second = app.call("track.add", {{"type", "audio"}});
	app.call("track.add", {{"type", "audio"}});
	// Selected once the track list shows them.
	REQUIRE(app.waitUntil(
		[&]
		{
			app.appUi().trackList()->select(
				{first.at("track").at("id").get<uint64_t>(), second.at("track").at("id").get<uint64_t>()});
			return app.appUi().trackList()->selection().size() == 2;
		}));
	const auto channel = [&app](const Json& added)
	{
		const Json channels = app.call("project.get").at("channels");
		const auto found = std::find_if(channels.begin(), channels.end(),
			[&added](const Json& item) { return item.at("id") == added.at("channel").at("id"); });
		REQUIRE(found != channels.end());
		return *found;
	};

	// Effect Track to Selected Tracks opens the effect page; into the project's FX folder.
	app.keys().run("track.addEffectToSelected", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return open(app); }));
	CHECK(elements(app, "#track-types").at(0).at("attributes").at("value") == "effect");
	click(app, "#track-project-folder");
	click(app, "#add-track-confirm");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));
	Json project = app.call("project.get");
	const Json& effect = project.at("tracks").back();
	CHECK(effect.at("type") == "effect");
	CHECK(channel(first).at("sends")[0].at("destination") == effect.at("channel"));
	CHECK(channel(second).at("sends")[0].at("on") == true);
	for (const Json& track : project.at("tracks"))
		if (track.at("id") == effect.at("parent"))
			CHECK(track.at("name") == "FX");

	// Group Track to Selected Tracks: they play into the new group, below the lower one.
	app.keys().run("track.addGroupToSelected", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return open(app); }));
	click(app, "#add-track-confirm");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));
	project = app.call("project.get");
	CHECK(project.at("tracks").at(2).at("type") == "group");
	CHECK(channel(first).at("output") == project.at("tracks").at(2).at("channel"));
	CHECK(channel(second).at("output") == project.at("tracks").at(2).at("channel"));

	// Count 2: the tracks play into the first of the new groups, and with Keep Dialog Open the next Add Track routes
	// nothing either.
	const size_t before = project.at("tracks").size();
	app.keys().run("track.addGroupToSelected", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return open(app); }));
	setCount(app, 2);
	click(app, "#keep-open");
	click(app, "#add-track-confirm");
	REQUIRE(app.waitUntil([&] { return app.call("project.get").at("tracks").size() == before + 2; }));
	const Json grouped = app.call("project.get").at("tracks").at(2).at("channel");
	CHECK(channel(first).at("output") == grouped);
	CHECK(channel(second).at("output") == grouped);
	click(app, "#add-track-confirm");
	REQUIRE(app.waitUntil([&] { return app.call("project.get").at("tracks").size() == before + 3; }));
	CHECK(channel(first).at("output") == grouped);
	CHECK(channel(second).at("output") == grouped);
	CHECK(elements(app, "#add-track-message").at(0).value("text", "").empty());
	click(app, "#add-track-cancel");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));
}

TEST_CASE("the add track dialog adds below the selected track, in its folder or below it", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const uint64_t folder = idOf(app.call("track.add", {{"type", "folder"}}));
	const uint64_t inside = idOf(app.call("track.add", {{"type", "audio"}, {"parent", folder}}));
	app.call("track.add", {{"type", "audio"}, {"parent", folder}});
	app.call("track.add", {{"type", "audio"}});
	select(app, {inside});

	// Two MIDI tracks below the selected track, in its folder, in the order they were added.
	click(app, "#add-track");
	REQUIRE(app.waitUntil([&app] { return open(app); }));
	page(app, "midi");
	setCount(app, 2);
	click(app, "#keep-open");
	click(app, "#add-track-confirm");
	REQUIRE(app.waitUntil([&app] { return names(app).size() == 6; }));
	CHECK(
		names(app) == std::vector<std::string>{"Folder 01", "Audio 01", "MIDI 01", "MIDI 02", "Audio 02", "Audio 03"});
	// With Keep Dialog Open the next tracks go below those, and a marker track, which stays at the top level, below
	// the folder.
	page(app, "audio");
	click(app, "#add-track-confirm");
	REQUIRE(app.waitUntil([&app] { return names(app).size() == 7; }));
	page(app, "marker");
	click(app, "#add-track-confirm");
	REQUIRE(app.waitUntil([&app] { return names(app).size() == 8; }));
	CHECK(names(app) ==
		std::vector<std::string>{
			"Folder 01", "Audio 01", "MIDI 01", "MIDI 02", "Audio 04", "Audio 02", "Markers", "Audio 03"});
	const Json tracks = app.call("project.get").at("tracks");
	CHECK(tracks[4].at("parent") == folder);
	CHECK(tracks[6].at("parent") == 0);
	click(app, "#add-track-cancel");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));
}

TEST_CASE("a double click in the empty area adds a track like the selected one below it, or a MIDI track", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const Json group = app.call("track.add", {{"type", "group"}}).at("track").at("channel");
	const Json synth = app.call("track.add", {{"type", "instrument"}}).at("track").at("channel");
	const uint64_t audio = idOf(app.call("track.add", {{"type", "audio"}, {"input", 0}, {"output", group}}));
	const uint64_t midi = idOf(app.call("track.add", {{"type", "midi"}, {"output", synth}}));
	app.call("track.add", {{"type", "audio"}});
	const auto channel = [&app](const Json& track)
	{
		const Json channels = app.call("project.get").at("channels");
		const auto found = std::find_if(channels.begin(), channels.end(),
			[&track](const Json& item) { return item.at("id") == track.at("channel"); });
		REQUIRE(found != channels.end());
		return *found;
	};

	// A selected audio track: an audio track with its input (none) and output (the group) below it, from the track
	// list's empty area.
	select(app, {audio});
	doubleClickBelow(app, "#track-list");
	REQUIRE(app.waitUntil([&app] { return names(app).size() == 6; }));
	Json tracks = app.call("project.get").at("tracks");
	CHECK(tracks[3].at("type") == "audio");
	CHECK(channel(tracks[3]).at("input") == 0);
	CHECK(channel(tracks[3]).at("output") == group);

	// A selected MIDI track: a MIDI track into the same instrument below it, from the event display's.
	select(app, {midi});
	doubleClickBelow(app, "#event-display");
	REQUIRE(app.waitUntil([&app] { return names(app).size() == 7; }));
	tracks = app.call("project.get").at("tracks");
	CHECK(tracks[5].at("type") == "midi");
	CHECK(channel(tracks[5]).at("output") == synth);

	// None: a MIDI track at the end.
	select(app, {});
	doubleClickBelow(app, "#track-list");
	REQUIRE(app.waitUntil([&app] { return names(app).size() == 8; }));
	CHECK(names(app).back() == "MIDI 03");
}

TEST_CASE("with Keep Dialog Open the next tracks stay below the selection after a project folder track", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const uint64_t first = idOf(app.call("track.add", {{"type", "audio"}}));
	app.call("track.add", {{"type", "audio"}});
	select(app, {first});

	// An effect track into the project's FX folder, then with Keep Dialog Open an audio track: below the selected
	// track, not into the folder.
	click(app, "#add-track");
	REQUIRE(app.waitUntil([&app] { return open(app); }));
	click(app, "#keep-open");
	page(app, "effect");
	click(app, "#track-project-folder");
	click(app, "#add-track-confirm");
	REQUIRE(app.waitUntil([&app] { return names(app).size() == 4; }));
	page(app, "audio");
	click(app, "#add-track-confirm");
	REQUIRE(app.waitUntil([&app] { return names(app).size() == 5; }));
	CHECK(names(app) == std::vector<std::string>{"Audio 01", "Audio 03", "Audio 02", "FX", "FX 01"});
	const Json tracks = app.call("project.get").at("tracks");
	CHECK(tracks[1].at("parent") == 0);
	CHECK(tracks[4].at("parent") == tracks[3].at("id"));
	click(app, "#add-track-cancel");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));
}

TEST_CASE("a double click with a selected MIDI track copies its MIDI input, in one undo step", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const Json synth = app.call("track.add", {{"type", "instrument"}});
	const Json midi = app.call("track.add", {{"type", "midi"}, {"output", synth.at("track").at("channel")}});
	app.call("channel.set", {{"channel", midi.at("track").at("channel")}, {"midiInput", "none"}});
	select(app, {idOf(midi)});
	const auto channel = [&app](const Json& track)
	{
		const Json channels = app.call("project.get").at("channels");
		const auto found = std::find_if(channels.begin(), channels.end(),
			[&track](const Json& item) { return item.at("id") == track.at("channel"); });
		REQUIRE(found != channels.end());
		return *found;
	};

	// The new MIDI track below has the selected track's MIDI input (none) and output (the instrument).
	doubleClickBelow(app, "#track-list");
	REQUIRE(app.waitUntil([&app] { return names(app).size() == 3; }));
	const Json tracks = app.call("project.get").at("tracks");
	CHECK(tracks[2].at("type") == "midi");
	REQUIRE(app.waitUntil([&] { return channel(tracks[2]).at("midiInput") == "none"; }));
	CHECK(channel(tracks[2]).at("output") == synth.at("track").at("channel"));

	// track.add and the channel.set of the MIDI input are one undo step.
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return names(app).size() == 2; }));
}

TEST_CASE("gnubase.ui.command runs in the window of the document that called it", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	ui::WindowSpec spec;
	spec.size = {400, 300};
	spec.hidden = true;
	const ui::WindowId second = app.shell().open(spec).id();
	Rml::ElementDocument* document = app.shell().find(second)->context().LoadDocumentFromMemory(
		R"(<rml><head><style>body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; font-family: Inter; }
		button { display: block; width: 100dp; height: 30dp; }</style></head>
		<body><button id="call" onclick="gnubase.ui.command{command = 'project.addTrack'}">Add</button>
		<button id="nested" onclick="gnubase.ui.runLua{code = [[gnubase.ui.command{command = 'project.addTrack'}]]}">
		Run</button></body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	app.shell().step(std::chrono::milliseconds(0));
	app.ui().click(second, {"#call", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	app.shell().step(std::chrono::milliseconds(0));
	const auto there = [&app, second] { return !app.ui().dom(second, "#add-track-dialog", 0).at("elements").empty(); };
	REQUIRE(app.waitUntil(there));
	CHECK_FALSE(open(app)); // not in the main window
	app.ui().click(second, {"#add-track-cancel", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&there] { return !there(); }));

	// Lua that the document runs, as the script console runs what is typed into it, runs in the main window.
	app.ui().click(second, {"#nested", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(app.waitUntil([&app] { return open(app); }));
	CHECK_FALSE(there());
	click(app, "#add-track-cancel");
	REQUIRE(app.waitUntil([&app] { return !open(app); }));
	app.shell().close(second);
}
