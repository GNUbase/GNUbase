// SPDX-License-Identifier: GPL-3.0-or-later
// The add track dialog (#80, docs/features/add-track.md) in the app's UI, driven with ui::Automation as MCP's UI tools
// drive it: every track type it adds makes the same project as the same tracks added with MCP's track_add.
#include "uiapp.h"

#include "engine/engine.h"
#include "mcp/server.h"
#include "ui/automation.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <optional>
#include <string>

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
	mcp.call("track_add", {{"type", "ruler"}, {"timeFormat", "timecode"}});

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
