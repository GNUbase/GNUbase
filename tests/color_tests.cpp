// SPDX-License-Identifier: GPL-3.0-or-later
// Color handling (#94, docs/features/colors.md): events and parts with colors of their own or their track's, the
// project's color set, and colors for new tracks (editing.autoTrackColor); in the app's project window the track
// list's and the event display's colors, the color menu and the project colors setup dialog.
#include "uiapp.h"

#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/docking.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
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
	CHECK_THAT(reject(client, "event.set", {{"events", {part}}, {"color", ""}}), ContainsSubstring("#rrggbb"));
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

TEST_CASE("editing.defaultColors is the color set of new projects", "[unit]")
{
	// Save Color Set as Default in the project colors setup's Options tab (reference manual p. 118).
	Engine engine;
	Client client(engine);
	const Json factory = run(client, "project.factoryColors").at("colors");
	CHECK(run(client, "settings.get", {{"key", "editing.defaultColors"}}).at("value") == Json::array());

	const Json colors = {{{"name", "Drums"}, {"color", "#ff0000"}}, {{"name", "Bass"}, {"color", "#00ff00"}}};
	run(client, "settings.set", {{"key", "editing.defaultColors"}, {"value", colors}});
	CHECK(run(client, "project.get").at("colors") == factory); // the open project keeps its set
	run(client, "project.new");
	CHECK(run(client, "project.get").at("colors") == colors);

	CHECK_THAT(reject(client, "settings.set",
				   {{"key", "editing.defaultColors"}, {"value", {{{"name", "X"}, {"color", "#fff"}}}}}),
		ContainsSubstring("editing.defaultColors"));

	// An empty set is the factory set again.
	run(client, "settings.set", {{"key", "editing.defaultColors"}, {"value", Json::array()}});
	run(client, "project.new");
	CHECK(run(client, "project.get").at("colors") == factory);
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
	const auto added = [&client](Json parameters)
	{ return colorOf(client, "tracks", addTrack(client, std::move(parameters))); };

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
	// It's the color the user last applied, also in the next project.
	run(client, "project.new");
	CHECK(added({{"type", "audio"}}) == "#0000cc");
	run(client, "project.set", {{"colors", colors}});

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

TEST_CASE("MIDI functions keep their parts' colors and give the tracks they add the color for new tracks", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set",
		{{"colors", {{{"name", "A"}, {"color", "#aa0000"}}, {{"name", "B"}, {"color", "#00bb00"}}}}});
	run(client, "settings.set", {{"key", "editing.autoTrackColor"}, {"value", "previousPlusOne"}});
	const ObjectId track = addTrack(client, {{"type", "midi"}, {"color", "#aa0000"}});
	const auto addPart = [&client, track](int position, const char* color)
	{
		const auto part = run(client, "midiPart.add", {{"track", track}, {"position", position}, {"length", 960}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
		run(client, "note.add",
			{{"part", part},
				{"notes",
					{{{"position", 0}, {"length", 10}, {"pitch", 60}, {"velocity", 0.5}},
						{{"position", 10}, {"length", 10}, {"pitch", 62}, {"velocity", 0.5}}}}});
		run(client, "event.set", {{"events", {part}}, {"color", color}});
		return part;
	};

	// Dissolve Part: the new parts keep the part's color, the new tracks after it get the next colors of the set.
	const Json dissolved =
		run(client, "midi.dissolve", {{"parts", {addPart(0, "#123456")}}, {"by", "pitch"}, {"to", "tracks"}});
	REQUIRE(dissolved.at("tracks").size() == 2);
	CHECK(colorOf(client, "tracks", dissolved.at("tracks")[0].get<ObjectId>()) == "#00bb00");
	CHECK(colorOf(client, "tracks", dissolved.at("tracks")[1].get<ObjectId>()) == "#aa0000");
	CHECK(lastEvent(client, "track.added").at("track").at("color") == "#aa0000");
	REQUIRE(dissolved.at("parts").size() == 2);
	for (const Json& part : dissolved.at("parts"))
		CHECK(colorOf(client, "midiParts", part.get<ObjectId>()) == "#123456");

	// Bounce MIDI: the part has the first part's color.
	const ObjectId second = addPart(3840, "#654321");
	const ObjectId third = addPart(7680, "#abcdef");
	const Json bounced = run(client, "midi.bounce", {{"parts", {third, second}}}).at("parts");
	REQUIRE(bounced.size() == 1);
	CHECK(colorOf(client, "midiParts", bounced[0].get<ObjectId>()) == "#654321");

	// Merge MIDI in Loop: the new track at the end gets the color after the last track's.
	const Json merged = run(client, "midi.merge", {{"start", 0}, {"end", 480000}, {"tracks", {track}}}).at("tracks");
	REQUIRE(merged.size() == 1);
	CHECK(colorOf(client, "tracks", merged[0].get<ObjectId>()) == "#00bb00");
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

		test::UiApp app{{1024, 500}}; // with room for the toolbar's color menu
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

	// Back to the default color: the strip shows nothing of its own, and loses the attribute that themes select
	// (#1314).
	window.app.call("track.set", {{"track", audio}, {"color", nullptr}});
	REQUIRE(window.app.waitUntil(
		[&] { return !window.elements(strip).at(0).value("attributes", Json::object()).contains("color"); }));
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

	// At 1024 pixels the toolbar has no room for its color section after the quantize section, which goes here.
	window.app.appUi().docking().setSectionHidden("project", "toolbar", "quantize", true);
	window.app.shell().step(std::chrono::milliseconds(0));

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

TEST_CASE("Alt-click on a track's color strip or the inspector's Basics header colors that one track", "[ui]")
{
	ProjectWindow window;
	window.app.appUi().docking().open("inspector");
	window.app.appUi().docking().setSectionHidden("project", "toolbar", "quantize", true); // room for #select-color
	window.app.call("project.set",
		{{"colors", {{{"name", "Warm"}, {"color", "#e04020"}}, {{"name", "Cold"}, {"color", "#2040e0"}}}}});
	const auto audio = window.app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	const auto midi = window.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto part = window.app.call("midiPart.add", {{"track", midi}, {"position", 0}, {"length", 3840}})
						  .at("part")
						  .at("id")
						  .get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return window.elements("#track-list listitem").size() == 2; }));
	const auto altClick = [&window](const std::string& selector)
	{
		window.app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {"alt"});
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto open = [&window]
	{
		const Rml::Element* menu = window.find("#color-menu");
		return menu != nullptr && menu->IsClassSet("open");
	};

	// The Choose Track Color pane on the strip of a track that isn't selected, with an event selected elsewhere: its
	// color goes to that track alone, not to the selected track or event (reference manual p. 113).
	window.app.appUi().trackList()->select({audio});
	window.display.select({part});
	altClick("#track-list listitem[value=\"" + std::to_string(midi) + "\"] .track-color");
	REQUIRE(open());
	const Json items = window.elements("#color-menu menuitem");
	REQUIRE(items.size() == 3);
	CHECK(items[0].value("text", "") == "Set Track Color to Default");
	window.click("#color-menu menuitem[color=\"#2040e0\"]");
	REQUIRE(window.app.waitUntil([&] { return window.project().at("tracks")[1].at("color") == "#2040e0"; }));
	CHECK(window.project().at("tracks")[0].at("color").is_null());
	CHECK(window.project().at("midiParts")[0].at("color").is_null());
	CHECK(window.app.call("history.get").at("steps").back().at("action") == "Colorize Tracks");

	// The toolbar's menu colors the selection again afterwards.
	window.display.select({});
	window.app.appUi().trackList()->select({audio});
	window.click("#select-color");
	window.click("#color-menu menuitem[color=\"#e04020\"]");
	REQUIRE(window.app.waitUntil([&] { return window.project().at("tracks")[0].at("color") == "#e04020"; }));
	CHECK(window.project().at("tracks")[1].at("color") == "#2040e0");

	// The row left of the track's name opens the pane as well; Set Track Color to Default.
	Rml::Element* name = window.find("#track-list listitem[value=\"" + std::to_string(midi) + "\"] .track-name");
	REQUIRE(name != nullptr);
	window.app.ui().click(std::nullopt,
		{"", 0,
			ui::Point{static_cast<int>(name->GetAbsoluteLeft()) - 2,
				static_cast<int>(name->GetAbsoluteTop() + (name->GetBox().GetSize().y / 2))}},
		ui::MouseButton::Left, 1, {"alt"});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(open());
	window.click("#color-menu menuitem[color=\"\"]");
	REQUIRE(window.app.waitUntil([&] { return window.project().at("tracks")[1].at("color").is_null(); }));
	CHECK(window.project().at("tracks")[0].at("color") == "#e04020");

	// The inspector's Basics header colors the track the inspector shows, and Alt-click doesn't close the section.
	window.app.appUi().trackList()->select({midi});
	window.app.shell().step(std::chrono::milliseconds(0));
	altClick(".inspector [section=basics] .section-header");
	REQUIRE(open());
	CHECK_FALSE(window.find(".inspector [section=basics]")->IsClassSet("collapsed"));
	window.click("#color-menu menuitem[color=\"#2040e0\"]");
	REQUIRE(window.app.waitUntil([&] { return window.project().at("tracks")[1].at("color") == "#2040e0"; }));
	CHECK(window.project().at("tracks")[0].at("color") == "#e04020");

	// The theme stretches a selected, colored track's strip across its row: right of the name it opens no pane. The
	// track list learns the color from the engine's event, after project.get already answers with it.
	const std::string row = "#track-list listitem[value=\"" + std::to_string(midi) + "\"]";
	REQUIRE(window.app.waitUntil([&] { return window.find(row + " .track-color[color=\"#2040e0\"]") != nullptr; }));
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE_FALSE(open());
	Rml::Element* strip = window.find(row + " .track-color");
	name = window.find(row + " .track-name");
	REQUIRE(strip != nullptr);
	REQUIRE(name != nullptr);
	const float y = strip->GetAbsoluteTop() + (strip->GetBox().GetSize().y / 2);
	int x = -1; // the first point right of the name where the strip is on top
	for (int at = static_cast<int>(name->GetAbsoluteLeft() + name->GetBox().GetSize().x);
		at < static_cast<int>(strip->GetAbsoluteLeft() + strip->GetBox().GetSize().x) && x < 0; ++at)
		if (strip->GetContext()->GetElementAtPoint(Rml::Vector2f(static_cast<float>(at), y)) == strip)
			x = at;
	REQUIRE(x >= 0);
	window.app.ui().click(std::nullopt, {"", 0, ui::Point{x, static_cast<int>(y)}}, ui::MouseButton::Left, 1, {"alt"});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(open());
}

TEST_CASE("Alt-click on the Basics header of an inspector in a window of its own colors its track", "[ui]")
{
	ProjectWindow window;
	ui::Docking& docking = window.app.appUi().docking();
	docking.open("inspector");
	docking.setFloating("inspector", true);
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(docking.windowOf("inspector") != nullptr);
	const ui::WindowId panel = docking.windowOf("inspector")->id();
	window.app.call("project.set",
		{{"colors", {{{"name", "Warm"}, {"color", "#e04020"}}, {{"name", "Cold"}, {"color", "#2040e0"}}}}});
	window.app.call("track.add", {{"type", "audio"}});
	const auto midi = window.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	REQUIRE(window.app.waitUntil([&] { return window.elements("#track-list listitem").size() == 2; }));
	window.app.appUi().trackList()->select({midi});
	window.app.shell().step(std::chrono::milliseconds(0));
	const auto click = [&window, panel](const std::string& selector, const std::vector<std::string>& modifiers)
	{
		window.app.ui().click(panel, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, modifiers);
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto elements = [&window, panel](const std::string& selector)
	{ return window.app.ui().dom(panel, selector, 0).at("elements"); };

	// The Choose Track Color pane opens in the inspector's window, for the track it shows, and the section stays open
	// (reference manual p. 113).
	click(".inspector [section=basics] .section-header", {"alt"});
	REQUIRE(elements("#color-menu.open").size() == 1);
	CHECK(elements("#color-menu menuitem").size() == 3);
	click("#color-menu menuitem[color=\"#2040e0\"]", {});
	REQUIRE(window.app.waitUntil([&] { return window.project().at("tracks")[1].at("color") == "#2040e0"; }));
	CHECK(window.project().at("tracks")[0].at("color").is_null());
	CHECK(elements(".inspector [section=basics].collapsed").empty());
}

TEST_CASE("the color tool's button shows its color, the wheel steps it, and Ctrl-click opens the pane", "[ui]")
{
	ProjectWindow window;
	window.app.call("project.set",
		{{"colors", {{{"name", "Warm"}, {"color", "#e04020"}}, {{"name", "Cold"}, {"color", "#2040e0"}}}}});
	const auto midi = window.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	// A bar: two seconds at 120 BPM, 200 dp.
	window.app.call("midiPart.add", {{"track", midi}, {"position", 0}, {"length", 4 * time::g_ticksPerQuarter}});
	REQUIRE(window.app.waitUntil([&] { return !window.laneColors().empty() && !window.laneColors()[0].empty(); }));
	const auto strip = [&window]() -> std::optional<Rml::Colourb>
	{
		Rml::Element* found = window.find("button[tool=color] .tool-color");
		const Rml::Property* color = found != nullptr ? found->GetLocalProperty("background-color") : nullptr;
		return color != nullptr ? std::optional<Rml::Colourb>(color->Get<Rml::Colourb>()) : std::nullopt;
	};

	// The toolbar's button chooses the tool and shows its color, the color set's first (reference manual p. 114).
	window.click("button[tool=color]");
	CHECK(window.display.tool() == ui::Tool::Color);
	REQUIRE(window.app.waitUntil([&] { return strip() == Rml::Colourb(0xe0, 0x40, 0x20, 255); }));
	// The wheel over it steps through the set.
	window.app.ui().wheel(std::nullopt, {"button[tool=color]", 0, std::nullopt}, 0.0F, 1.0F, {});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.display.toolColor() == "#2040e0");
	CHECK(strip() == Rml::Colourb(0x20, 0x40, 0xe0, 255));

	// Ctrl/Cmd and a click on the part opens the Choose Event Color pane for it, and a click colors it with the tool.
	const auto clickPart = [&window](const std::vector<std::string>& modifiers)
	{
		// A second in, the middle of the part's bar.
		Rml::Element* lanes = window.find("event-lanes.events");
		const double x = (1.0 - window.display.view().start) / window.display.view().scale;
		window.app.ui().click(std::nullopt,
			{"", 0,
				ui::Point{
					static_cast<int>(lanes->GetAbsoluteLeft() + x), static_cast<int>(lanes->GetAbsoluteTop()) + 10}},
			ui::MouseButton::Left, 1, modifiers);
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	clickPart({"ctrl"});
	REQUIRE(window.find("#color-menu")->IsClassSet("open"));
	CHECK(window.elements("#color-menu menuitem").at(0).value("text", "") == "Set Event Color to Track");
	window.click("#color-menu menuitem[color=\"#e04020\"]");
	REQUIRE(window.app.waitUntil([&] { return window.project().at("midiParts")[0].at("color") == "#e04020"; }));
	window.display.select({});
	window.app.shell().step(std::chrono::milliseconds(0));
	clickPart({});
	REQUIRE(window.app.waitUntil([&] { return window.project().at("midiParts")[0].at("color") == "#2040e0"; }));
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

TEST_CASE("the project colors setup dialog renames colors and resets one to its factory color", "[ui]")
{
	ProjectWindow window;
	const auto open = [&window] { return !window.elements("#project-colors-dialog").empty(); };
	window.app.keys().run("project.colorsSetup", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil(open));
	// Types a name over the old one in the name field.
	const auto rename = [&window](const std::string& name)
	{
		window.click("#color-name");
		window.app.ui().key(std::nullopt, "Mod+A");
		window.app.ui().text(std::nullopt, name);
		window.app.shell().step(std::chrono::milliseconds(0));
	};

	// The name field renames the selected color, and its row in the list with it.
	window.click("#project-colors > listitem[value=\"3\"]");
	rename("Kick");
	CHECK(window.elements("#project-colors > listitem[value=\"3\"]").at(0).value("text", "") == "Kick");

	// Reset Color gives the selected color the factory name and color in its place.
	const Json factory = window.app.call("project.factoryColors").at("colors");
	window.click("#project-colors > listitem[value=\"2\"]");
	rename("Snare");
	window.click("#color-picker .color-field");
	CHECK(window.elements("#color-picker").at(0).at("attributes").at("value") != factory[1].at("color"));
	window.click("#color-reset");
	CHECK(window.elements("#project-colors > listitem[value=\"2\"]").at(0).value("text", "") ==
		factory[1].at("name").get<std::string>());
	CHECK(window.elements("#color-picker").at(0).at("attributes").at("value") == factory[1].at("color"));

	window.click("#colors-apply");
	REQUIRE(window.app.waitUntil([&] { return !open(); }));
	const Json colors = window.project().at("colors");
	REQUIRE(colors.size() == factory.size());
	CHECK(colors[1] == factory[1]);
	CHECK(colors[2].at("name") == "Kick");
	CHECK(colors[2].at("color") == factory[2].at("color"));
}

TEST_CASE("the project colors setup dialog's options save the set as the default and reset to it", "[ui]")
{
	ProjectWindow window;
	const auto open = [&window] { return !window.elements("#project-colors-dialog").empty(); };
	const auto colorsSetup = [&]
	{
		window.app.keys().run("project.colorsSetup", Json::object(), window.app.main(), nullptr);
		REQUIRE(window.app.waitUntil(open));
	};
	const auto defaultColors = [&window]
	{ return window.app.call("settings.get", {{"key", "editing.defaultColors"}}).at("value"); };
	const Json factory = window.app.call("project.factoryColors").at("colors");

	// Save Color Set as Default (reference manual p. 118) saves the set being edited, not the project's.
	colorsSetup();
	window.click("#project-colors > listitem[value=\"1\"]");
	window.click("#color-remove");
	window.click("#colors-options-tab");
	window.click("#colors-save-default");
	Json saved = factory;
	saved.erase(0);
	CHECK(defaultColors() == saved);
	window.click("#colors-cancel");
	REQUIRE(window.app.waitUntil([&] { return !open(); }));
	CHECK(window.project().at("colors") == factory);

	// New projects get it.
	window.app.call("project.new");
	CHECK(window.project().at("colors") == saved);

	// Reset Color Set to Default takes it in place of the set being edited.
	window.app.call("project.set", {{"colors", factory}});
	colorsSetup();
	window.click("#colors-options-tab");
	window.click("#colors-reset-default");
	CHECK(window.elements("#project-colors > listitem").size() == saved.size());
	window.click("#colors-apply");
	REQUIRE(window.app.waitUntil([&] { return !open(); }));
	CHECK(window.project().at("colors") == saved);
}

TEST_CASE("the project colors setup dialog's presets make sets of 8 to 32 basic colors in tints", "[ui]")
{
	ProjectWindow window;
	const auto open = [&window] { return !window.elements("#project-colors-dialog").empty(); };
	window.app.keys().run("project.colorsSetup", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil(open));
	window.click("#colors-presets-tab");
	const auto choose = [&window](const char* id, const char* value)
	{
		auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(window.find(std::string("#") + id));
		REQUIRE(select != nullptr);
		select->SetValue(value);
		window.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto names = [&window]
	{
		std::vector<std::string> list;
		for (const Json& item : window.elements("#project-colors > listitem"))
			list.push_back(item.value("text", ""));
		return list;
	};
	const Json factory = window.app.call("project.factoryColors").at("colors");

	// 8 basic colors around the factory set's hues, its grey last; the new set shows on the tab too (p. 117).
	choose("preset-basic", "8");
	REQUIRE(names().size() == 8);
	CHECK(window.elements("#preset-colors .color-swatch").size() == 8);
	CHECK(names().front() == factory[0].at("name").get<std::string>());
	CHECK(names().back() == factory[15].at("name").get<std::string>());
	const std::vector<std::string> eight = names();
	CHECK(std::set<std::string>(eight.begin(), eight.end()).size() == 8); // no name twice

	// A color changed in the picker changes on the tab as well.
	auto* picker = window.find("#color-picker");
	REQUIRE(picker != nullptr);
	picker->SetAttribute("value", "#123456");
	Rml::Dictionary parameters;
	parameters["value"] = Rml::String("#123456");
	picker->DispatchEvent(Rml::EventId::Change, parameters);
	const Rml::Property* swatch = window.find("#preset-colors .color-swatch")->GetLocalProperty("background-color");
	REQUIRE(swatch != nullptr);
	CHECK(swatch->Get<Rml::Colourb>() == Rml::Colourb(0x12, 0x34, 0x56, 255));

	// Reset to Factory Settings sets the tab back to 16 basic colors in one tint, so choosing 8 again makes 8.
	window.click("#colors-factory");
	REQUIRE(names().size() == 16);
	CHECK(window.find("#preset-basic")->GetAttribute<Rml::String>("value", "") == "16");
	CHECK(window.find("#preset-tints")->GetAttribute<Rml::String>("value", "") == "1");
	CHECK(window.find("#preset-sort")->GetAttribute<Rml::String>("value", "") == "basic");
	choose("preset-basic", "8");
	CHECK(names() == eight);

	// Two tints each, sorted by basic color: each color, then its light tint; by tint: the basic colors first.
	choose("preset-tints", "2");
	REQUIRE(names().size() == 16);
	CHECK(names()[1] == "Light " + names()[0]);
	choose("preset-sort", "tint");
	CHECK(names()[8] == "Light " + names()[0]);

	// 16 basic colors in one tint are the factory set; 32 in 4 tints are 128 colors.
	choose("preset-basic", "32");
	choose("preset-tints", "4");
	CHECK(names().size() == 128);
	choose("preset-basic", "16");
	choose("preset-tints", "1");
	window.click("#colors-apply");
	REQUIRE(window.app.waitUntil([&] { return !open(); }));
	CHECK(window.project().at("colors") == factory);

	// Apply takes the new set: 24 basic colors, all of them valid and different.
	window.app.keys().run("project.colorsSetup", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil(open));
	window.click("#colors-presets-tab");
	choose("preset-basic", "24");
	window.click("#colors-apply");
	REQUIRE(window.app.waitUntil([&] { return !open(); }));
	const Json colors = window.project().at("colors");
	REQUIRE(colors.size() == 24);
	std::set<std::string> distinct;
	for (const Json& color : colors)
		distinct.insert(color.at("color").get<std::string>());
	CHECK(distinct.size() == 24);
	CHECK(colors[0] == factory[0]);
	CHECK(colors[23] == factory[15]);
}

TEST_CASE("the project colors setup dialog shows the missing project colors and Apply replaces them", "[ui]")
{
	ProjectWindow window;
	const Json factory = window.app.call("project.factoryColors").at("colors");
	const std::string kept = "#010203"; // not in the set: no color replaces it
	const auto colored = [&window](const std::string& color)
	{ return window.app.call("track.add", {{"type", "midi"}, {"color", color}}).at("track").at("id"); };
	colored(factory[1].at("color").get<std::string>());
	colored(kept);
	const auto plain = window.app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const auto part =
		window.app.call("midiPart.add", {{"track", plain}, {"position", 0}, {"length", 1920}}).at("part").at("id");
	window.app.call("event.set", {{"events", Json::array({part})}, {"color", factory[2].at("color")}});

	const auto open = [&window] { return !window.elements("#project-colors-dialog").empty(); };
	window.app.keys().run("project.colorsSetup", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil(open));
	window.click("#colors-presets-tab");
	CHECK(window.elements("#missing-colors .color-swatch").empty()); // the project's own set

	// 8 basic colors keep the factory set's first and last; the others are missing, each with the color of the new
	// set nearest to it to replace it (reference manual p. 118).
	auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(window.find("#preset-basic"));
	REQUIRE(select != nullptr);
	select->SetValue("8");
	window.app.shell().step(std::chrono::milliseconds(0));
	const Json missing = window.elements("#missing-colors .color-swatch");
	REQUIRE(missing.size() == 14);
	CHECK(missing[0].at("attributes").at("color") == factory[1].at("color"));
	const auto replacement = [&missing](size_t index)
	{ return std::stoi(missing[index].at("attributes").at("replacement").get<std::string>()); };

	// The mouse over a missing color highlights the color replacing it.
	window.app.ui().move(std::nullopt, {"#missing-colors .color-swatch", 0, std::nullopt});
	window.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(window.elements("#preset-colors .color-swatch.replacement").size() == 1);
	CHECK(window.find("#preset-colors .color-swatch.replacement") ==
		window.find("#preset-colors")->GetChild(replacement(0) - 1));
	window.app.ui().move(std::nullopt, {"#colors-presets-tab", 0, std::nullopt});
	window.app.shell().step(std::chrono::milliseconds(0));
	CHECK(window.elements("#preset-colors .color-swatch.replacement").empty());

	// Apply gives the tracks and parts in a missing color the color replacing it, in the same undo step.
	window.click("#colors-apply");
	REQUIRE(window.app.waitUntil([&] { return !open(); }));
	const Json project = window.project();
	const Json& colors = project.at("colors");
	REQUIRE(colors.size() == 8);
	CHECK(project.at("tracks")[0].at("color") == colors[static_cast<size_t>(replacement(0) - 1)].at("color"));
	CHECK(project.at("tracks")[1].at("color") == kept);
	CHECK(project.at("tracks")[2].at("color").is_null());
	CHECK(project.at("midiParts")[0].at("color") == colors[static_cast<size_t>(replacement(1) - 1)].at("color"));
	CHECK(window.app.call("history.get").at("steps").back().at("action") == "Change project");
	window.app.call("history.undo");
	CHECK(window.project().at("colors") == factory);
	CHECK(window.project().at("tracks")[0].at("color") == factory[1].at("color"));
	CHECK(window.project().at("midiParts")[0].at("color") == factory[2].at("color"));
}

TEST_CASE("the project colors setup dialog gives a color changed in the picker's tracks its new color", "[ui]")
{
	ProjectWindow window;
	const Json factory = window.app.call("project.factoryColors").at("colors");
	const std::string old = factory[1].at("color").get<std::string>();
	window.app.call("track.add", {{"type", "midi"}, {"color", old}});

	const auto open = [&window] { return !window.elements("#project-colors-dialog").empty(); };
	window.app.keys().run("project.colorsSetup", Json::object(), window.app.main(), nullptr);
	REQUIRE(window.app.waitUntil(open));
	window.click("#project-colors > listitem[value=\"2\"]");
	auto* picker = window.find("#color-picker");
	REQUIRE(picker != nullptr);
	picker->SetAttribute("value", "#123456");
	Rml::Dictionary parameters;
	parameters["value"] = Rml::String("#123456");
	picker->DispatchEvent(Rml::EventId::Change, parameters);

	// The old color is missing at once, replaced by the color it was changed to, not by the nearest other one.
	window.click("#colors-presets-tab");
	const Json missing = window.elements("#missing-colors .color-swatch");
	REQUIRE(missing.size() == 1);
	CHECK(missing[0].at("attributes").at("color") == old);
	CHECK(missing[0].at("attributes").at("replacement") == "2");

	window.click("#colors-apply");
	REQUIRE(window.app.waitUntil([&] { return !open(); }));
	CHECK(window.project().at("colors")[1].at("color") == "#123456");
	CHECK(window.project().at("tracks")[0].at("color") == "#123456");
}
