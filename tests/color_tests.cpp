// SPDX-License-Identifier: GPL-3.0-or-later
// Color handling (#94, docs/features/colors.md): events and parts with colors of their own or their track's, the
// project's color set, and colors for new tracks (editing.autoTrackColor); in the app's project window the track
// list's and the event display's colors, the color menu and the project colors setup dialog.
#include "uiapp.h"

#include "engine/document/document.h"
#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase;
using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	ObjectId addTrack(Client& client, Json parameters)
	{
		return run(client, "track.add", std::move(parameters)).at("track").at("id").get<ObjectId>();
	}

	Json withId(const Json& list, ObjectId id)
	{
		const auto found = std::find_if(
			list.begin(), list.end(), [id](const Json& object) { return object.at("id").get<ObjectId>() == id; });
		REQUIRE(found != list.end());
		return *found;
	}

	Json colorOf(Client& client, const char* list, ObjectId id)
	{
		return withId(run(client, "project.get").at(list), id).at("color");
	}

	ObjectId addClip(Client& client)
	{
		const ObjectId media = run(client, "media.add",
			{{"path", "/audio/loop.wav"}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 192000}})
								   .at("media")
								   .at("id")
								   .get<ObjectId>();
		return run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	}

	// The last event of that name the client received, or null.
	Json lastEvent(Client& client, const std::string& name)
	{
		Json found;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message); event != nullptr && event->name == name)
				found = event->data;
		return found;
	}
}

TEST_CASE("audio events and MIDI parts have a color of their own, or their track's", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId audio = addTrack(client, {{"type", "audio"}, {"color", "#204060"}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	const ObjectId clip = addClip(client);
	const ObjectId event = run(client, "audioEvent.add", {{"track", audio}, {"clip", clip}, {"position", 0}})
							   .at("event")
							   .at("id")
							   .get<ObjectId>();
	const ObjectId part = run(client, "midiPart.add", {{"track", midi}, {"position", 0}, {"length", 3840}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();

	// By default they follow their track (reference manual p. 114): no color of their own.
	CHECK(colorOf(client, "audioEvents", event).is_null());
	CHECK(colorOf(client, "midiParts", part).is_null());

	// event.set colors both, also locked ones, and announces them.
	run(client, "event.set", {{"events", {part}}, {"lock", true}});
	run(client, "event.set", {{"events", {event, part}}, {"color", "#a0c0e0"}});
	CHECK(colorOf(client, "audioEvents", event) == "#a0c0e0");
	CHECK(colorOf(client, "midiParts", part) == "#a0c0e0");
	CHECK(lastEvent(client, "midiPart.changed").at("part").at("color") == "#a0c0e0");
	// A track's new color leaves them theirs.
	run(client, "track.set", {{"track", audio}, {"color", "#ffffff"}});
	CHECK(colorOf(client, "audioEvents", event) == "#a0c0e0");

	// Pieces of a split event keep its color.
	const Json pieces = run(client, "event.split", {{"events", {event}}, {"positions", {24000}}}).at("events");
	REQUIRE(pieces.size() == 1);
	CHECK(colorOf(client, "audioEvents", pieces[0].get<ObjectId>()) == "#a0c0e0");

	// Null: back to the track's color (p. 115). Undo puts the color back.
	run(client, "event.set", {{"events", {part}}, {"color", nullptr}});
	CHECK(colorOf(client, "midiParts", part).is_null());
	run(client, "history.undo");
	CHECK(colorOf(client, "midiParts", part) == "#a0c0e0");

	CHECK_THAT(reject(client, "event.set", {{"events", {part}}, {"color", "red"}}), ContainsSubstring("#rrggbb"));
	CHECK_THAT(reject(client, "event.set", {{"events", {part}}, {"color", "#12345g"}}), ContainsSubstring("#rrggbb"));
}

TEST_CASE("a project has a color set that project.set changes, with undo", "[unit]")
{
	Engine engine;
	Client client(engine);

	// New projects have the factory colors, each with a name.
	const Json factory = run(client, "project.get").at("colors");
	REQUIRE(factory.size() == 16);
	std::set<std::string> distinct;
	for (const Json& color : factory)
	{
		CHECK_FALSE(color.at("name").get<std::string>().empty());
		distinct.insert(color.at("color").get<std::string>());
	}
	CHECK(distinct.size() == factory.size());

	const Json colors = {{{"name", "Drums"}, {"color", "#ff0000"}}, {{"name", "Bass"}, {"color", "#00ff00"}}};
	run(client, "project.set", {{"colors", colors}});
	CHECK(run(client, "project.get").at("colors") == colors);
	CHECK(lastEvent(client, "project.changed").at("colors") == colors);
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Change project");

	run(client, "history.undo");
	CHECK(run(client, "project.get").at("colors") == factory);
	CHECK(lastEvent(client, "project.changed").at("colors") == factory);
	run(client, "history.redo");
	CHECK(run(client, "project.get").at("colors") == colors);

	CHECK_THAT(reject(client, "project.set", {{"colors", Json::array()}}), ContainsSubstring("at least one"));
	CHECK_THAT(reject(client, "project.set", {{"colors", {{{"name", "X"}, {"color", "#fff"}}}}}),
		ContainsSubstring("#rrggbb"));
	CHECK(run(client, "project.get").at("colors") == colors);
}

TEST_CASE("editing.autoTrackColor colors new tracks", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json colors = {{{"name", "A"}, {"color", "#aa0000"}}, {{"name", "B"}, {"color", "#00bb00"}},
		{{"name", "C"}, {"color", "#0000cc"}}};
	run(client, "project.set", {{"colors", colors}});
	const auto mode = [&client](const char* value)
	{ run(client, "settings.set", {{"key", "editing.autoTrackColor"}, {"value", value}}); };
	const auto added = [&client](Json parameters) { return colorOf(client, "tracks", addTrack(client, parameters)); };

	// The default (reference manual p. 1621): the track's default color.
	CHECK(added({{"type", "audio"}}).is_null());

	// The previous track's color: the one above the new track in the list.
	mode("previous");
	const ObjectId first = addTrack(client, {{"type", "audio"}, {"color", "#00bb00"}, {"index", 0}});
	CHECK(added({{"type", "audio"}, {"index", 1}}) == "#00bb00");
	CHECK(added({{"type", "audio"}, {"index", 0}}).is_null()); // none above it
	// A given color wins.
	CHECK(added({{"type", "audio"}, {"index", 1}, {"color", "#123456"}}) == "#123456");

	// The next color of the set after the previous track's, around at the end; the first after one that isn't in it.
	mode("previousPlusOne");
	run(client, "track.move", {{"track", first}, {"parent", 0}, {"index", 0}});
	CHECK(added({{"type", "midi"}, {"index", 1}}) == "#0000cc");
	CHECK(added({{"type", "midi"}, {"index", 2}}) == "#aa0000");
	run(client, "track.set", {{"track", first}, {"color", "#ffffff"}});
	CHECK(added({{"type", "midi"}, {"index", 1}}) == "#aa0000");

	// The color last applied to an event or part; the default before any.
	mode("lastApplied");
	CHECK(added({{"type", "audio"}}).is_null());
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	const ObjectId part = run(client, "midiPart.add", {{"track", midi}, {"position", 0}, {"length", 960}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
	run(client, "event.set", {{"events", {part}}, {"color", "#0000cc"}});
	run(client, "event.set", {{"events", {part}}, {"color", nullptr}}); // not a color applied
	CHECK(added({{"type", "audio"}}) == "#0000cc");

	// A random color of the set.
	mode("random");
	for (int i = 0; i < 10; ++i)
	{
		const Json color = added({{"type", "audio"}});
		CHECK(std::any_of(colors.begin(), colors.end(), [&color](const Json& c) { return c.at("color") == color; }));
	}

	// With editing.applyFolderColor a folder's color wins inside it.
	run(client, "settings.set", {{"key", "editing.applyFolderColor"}, {"value", true}});
	const ObjectId folder = addTrack(client, {{"type", "folder"}, {"color", "#334455"}});
	CHECK(added({{"type", "audio"}, {"parent", folder}}) == "#334455");
}

namespace
{
	// The project window of the app (test::UiApp), driven with ui::Automation as MCP's UI tools drive it.
	struct ProjectWindow
	{
		ProjectWindow() : display(app.appUi().showProject())
		{
			app.setDpRatio(1.0F);
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		void click(const std::string& selector)
		{
			app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		Rml::Element* find(const std::string& selector)
		{
			Rml::Context& context = app.main().context();
			for (int i = 0; i < context.GetNumDocuments(); ++i)
				if (Rml::Element* found = context.GetDocument(i)->QuerySelector(selector))
					return found;
			return nullptr;
		}

		// The colors of the events in the display's lanes, by lane.
		std::vector<std::vector<uint32_t>> laneColors()
		{
			std::vector<std::vector<uint32_t>> colors;
			if (auto* lanes = dynamic_cast<ui::EventLanes*>(find("event-lanes.events")))
				for (const ui::EventLanes::Lane& lane : lanes->lanes())
				{
					colors.emplace_back();
					for (const ui::TimeSpan& span : lane.events)
						colors.back().push_back(span.color);
				}
			return colors;
		}

		Json project() { return app.call("project.get"); }

		test::UiApp app{{1000, 500}};
		ui::EventDisplay& display;
	};
}

TEST_CASE("the track list and the event display show track and event colors", "[ui]")
{
	ProjectWindow window;
	const auto audio = window.app.call("track.add", {{"type", "audio"}, {"color", "#204060"}}).at("track").at("id");
	const auto midi = window.app.call("track.add", {{"type", "midi"}}).at("track").at("id");
	const auto part =
		window.app.call("midiPart.add", {{"track", midi}, {"position", 0}, {"length", 3840}}).at("part").at("id");
	window.app.call("midiPart.add", {{"track", midi}, {"position", 7680}, {"length", 3840}});
	const std::string strip = "#track-list listitem[value=\"" + audio.dump() + "\"] .track-color";
	REQUIRE(window.app.waitUntil([&] { return window.elements(strip).size() == 1; }));

	// The color strip has the track's color, which the style sheets don't know: code sets it as user data.
	CHECK(window.elements(strip).at(0).at("attributes").at("color") == "#204060");
	Rml::Element* element = window.find(strip);
	REQUIRE(element != nullptr);
	REQUIRE(element->GetProperty("background-color") != nullptr);
	CHECK(element->GetProperty("background-color")->Get<Rml::Colourb>() == Rml::Colourb(0x20, 0x40, 0x60, 255));

	// The parts follow their track's color (none: the style sheet's), until one has its own.
	REQUIRE(
		window.app.waitUntil([&] { return window.laneColors().size() == 2 && window.laneColors()[1].size() == 2; }));
	CHECK(window.laneColors()[1] == std::vector<uint32_t>{0, 0});
	window.app.call("track.set", {{"track", midi}, {"color", "#ff8000"}});
	REQUIRE(
		window.app.waitUntil([&] { return window.laneColors()[1] == std::vector<uint32_t>{0xff8000ff, 0xff8000ff}; }));
	window.app.call("event.set", {{"events", {part}}, {"color", "#00ff00"}});
	REQUIRE(
		window.app.waitUntil([&] { return window.laneColors()[1] == std::vector<uint32_t>{0x00ff00ff, 0xff8000ff}; }));

	// Back to the default color: the strip shows nothing of its own.
	window.app.call("track.set", {{"track", audio}, {"color", nullptr}});
	REQUIRE(window.app.waitUntil([&] { return window.elements(strip).at(0).at("attributes").at("color") == ""; }));
	CHECK(window.find(strip)->GetLocalProperty("background-color") == nullptr);
}

TEST_CASE("the color menu colors the selected events, or else the selected tracks", "[ui]")
{
	ProjectWindow window;
	window.app.call("project.set",
		{{"colors", {{{"name", "Warm"}, {"color", "#e04020"}}, {{"name", "Cold"}, {"color", "#2040e0"}}}}});
	const auto audio = window.app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	const auto midi = window.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto part = window.app.call("midiPart.add", {{"track", midi}, {"position", 0}, {"length", 3840}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return window.elements("#track-list listitem").size() == 2; }));

	// With tracks selected and no events: the colors of the set, and Set Track Color to Default.
	window.app.appUi().trackList()->select({audio, midi});
	window.click("#select-color");
	const Json items = window.elements("#color-menu menuitem");
	REQUIRE(items.size() == 3);
	CHECK(items[0].value("text", "") == "Set Track Color to Default");
	CHECK(items[1].value("text", "") == "Warm");
	CHECK(items[2].at("attributes").at("color") == "#2040e0");
	window.click("#color-menu menuitem[color=\"#2040e0\"]");
	REQUIRE(window.app.waitUntil([&] { return window.project().at("tracks")[1].at("color") == "#2040e0"; }));
	CHECK(window.project().at("tracks")[0].at("color") == "#2040e0");
	CHECK(window.app.call("history.get").at("steps").back().at("action") == "Colorize Tracks");

	// With an event selected, the color goes to it, and Set Event Color to Track takes it away again.
	window.display.select({part});
	window.click("#select-color");
	CHECK(window.elements("#color-menu menuitem").at(0).value("text", "") == "Set Event Color to Track");
	window.click("#color-menu menuitem[color=\"#e04020\"]");
	REQUIRE(window.app.waitUntil([&] { return window.project().at("midiParts")[0].at("color") == "#e04020"; }));
	CHECK(window.project().at("tracks")[1].at("color") == "#2040e0");
	window.click("#select-color");
	window.click("#color-menu menuitem[color=\"\"]");
	REQUIRE(window.app.waitUntil([&] { return window.project().at("midiParts")[0].at("color").is_null(); }));

	// Without events selected, Set Track Color to Default.
	window.display.select({});
	window.app.appUi().trackList()->select({midi});
	window.click("#select-color");
	window.click("#color-menu menuitem[color=\"\"]");
	REQUIRE(window.app.waitUntil([&] { return window.project().at("tracks")[1].at("color").is_null(); }));
	CHECK(window.project().at("tracks")[0].at("color") == "#2040e0");
}

TEST_CASE("the project colors setup dialog edits the project's color set", "[ui]")
{
	ProjectWindow window;
	const auto open = [&window] { return !window.elements("#project-colors-dialog").empty(); };
	window.app.keys().run("project.colorsSetup", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil(open));
	REQUIRE(window.elements("#project-colors > listitem").size() == 16);
	CHECK(window.elements("#color-picker").at(0).at("attributes").at("value") ==
		window.project().at("colors")[0].at("color"));

	// Remove the first color, duplicate the new first, and recolor the copy through the picker's red field.
	window.click("#project-colors > listitem[value=\"1\"]");
	window.click("#color-remove");
	window.click("#color-duplicate");
	CHECK(window.elements("#project-colors > listitem").size() == 16);
	auto* picker = window.find("#color-picker");
	picker->SetAttribute("value", "#123456");
	Rml::Dictionary parameters;
	parameters["value"] = Rml::String("#123456");
	picker->DispatchEvent(Rml::EventId::Change, parameters);
	window.click("#color-insert");
	CHECK(window.elements("#project-colors > listitem").size() == 17);
	window.click("#colors-apply");
	REQUIRE(window.app.waitUntil([&] { return !open(); }));

	const Json factory = window.app.call("project.factoryColors").at("colors");
	const Json colors = window.project().at("colors");
	REQUIRE(colors.size() == 17);
	CHECK(colors[0] == factory[1]);
	CHECK(colors[1].at("name") == factory[1].at("name"));
	CHECK(colors[1].at("color") == "#123456");
	CHECK(colors[2].at("color") == "#808080");
	CHECK(colors[3] == factory[2]);

	// One undo step; Reset to Factory Settings and Cancel change nothing.
	CHECK(window.app.call("history.get").at("steps").back().at("action") == "Change project");
	window.app.keys().run("project.colorsSetup", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil(open));
	window.click("#colors-factory");
	CHECK(window.elements("#project-colors > listitem").size() == 16);
	window.click("#colors-cancel");
	REQUIRE(window.app.waitUntil([&] { return !open(); }));
	CHECK(window.project().at("colors") == colors);
}
