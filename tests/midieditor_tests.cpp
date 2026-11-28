// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI editors' common framework (#126, docs/features/midi-editors.md) in the app, driven with ui::Automation as
// MCP's UI tools drive it: the editor window and the lower zone's editor opened for several parts on several tracks,
// the active part, the part editing mode, the Visibility tab, the global tracks and Keep Editor Contents.
// tests/editors_tests.cpp checks the engine's editor views.
#include "engine/time/tempomap.h"
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "ui/midieditor.h"
#include "ui/rmlui.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	using Window = std::optional<ui::WindowId>;

	Json elements(test::UiApp& app, Window window, const std::string& selector)
	{
		return app.ui().dom(window, selector, 0).at("elements");
	}

	void click(test::UiApp& app, Window window, const std::string& selector, int count = 1, bool shift = false)
	{
		app.ui().click(window, {selector, 0, std::nullopt}, ui::MouseButton::Left, count,
			shift ? std::vector<std::string>{"shift"} : std::vector<std::string>{});
		app.shell().step(std::chrono::milliseconds(0));
	}

	bool has(const Json& element, const std::string& name)
	{
		const Json names = element.value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	// The ids in an attribute of the elements, in order.
	std::vector<uint64_t> ids(test::UiApp& app, Window window, const std::string& selector, const char* attribute)
	{
		std::vector<uint64_t> found;
		for (const Json& element : elements(app, window, selector))
			found.push_back(std::stoull(element.at("attributes").at(attribute).get<std::string>()));
		return found;
	}

	std::vector<uint64_t> parts(const Json& view)
	{
		return view.at("parts").get<std::vector<uint64_t>>();
	}

	Json view(test::UiApp& app, const std::string& editor)
	{
		return app.call("editor.get", {{"editor", editor}});
	}

	uint64_t addTrack(test::UiApp& app, const std::string& name)
	{
		return app.call("track.add", {{"type", "midi"}, {"name", name}}).at("track").at("id").get<uint64_t>();
	}

	uint64_t addPart(test::UiApp& app, uint64_t track, int64_t position, const std::string& name)
	{
		return app.call("midiPart.add", {{"track", track}, {"position", position}, {"length", 1920}, {"name", name}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	}

	// Two MIDI tracks with three parts, shown in the project window.
	struct Fixture
	{
		explicit Fixture(test::UiApp& app) : app(app), display(app.appUi().showProject())
		{
			keys = addTrack(app, "Keys");
			bass = addTrack(app, "Bass");
			keys2 = addPart(app, keys, 3840, "Keys B");
			keys1 = addPart(app, keys, 0, "Keys A");
			bass1 = addPart(app, bass, 0, "Bass A");
			REQUIRE(app.waitUntil([&] { return display.project().at("midiParts").size() == 3; }));
		}

		// Selects the events in the project window, then lets the editors follow.
		void select(std::vector<uint64_t> events)
		{
			display.select(std::move(events));
			app.shell().step(std::chrono::milliseconds(0));
		}

		test::UiApp& app;
		ui::EventDisplay& display;
		uint64_t keys = 0;
		uint64_t bass = 0;
		uint64_t keys1 = 0;
		uint64_t keys2 = 0;
		uint64_t bass1 = 0;
	};
}

TEST_CASE("the editor window opens for several parts on several tracks and edits the active part", "[ui]")
{
	test::UiApp app({1200, 700});
	Fixture f(app);

	// Open/Close Editor (Mod+E, Return, p. 1189) with parts of two tracks selected, in the window with Double-click
	// opens Editor in a Window (p. 1603).
	app.call("settings.set", {{"key", "editing.doubleClickOpensEditorIn"}, {"value", "window"}});
	CHECK(app.keys().keysFor("editor.openClose").size() == 2);
	CHECK(elements(app, std::nullopt, "#edit-menu menuitem[command=\"editor.openClose\"]").size() == 1);
	f.select({f.bass1, f.keys2, f.keys1});
	app.keys().run("editor.openClose", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	const Window window = app.appUi().editorWindowId();
	// The parts by track and position; the first is active.
	REQUIRE(app.waitUntil(
		[&] { return ids(app, window, ".editor-part", "part") == std::vector<uint64_t>{f.keys1, f.keys2, f.bass1}; }));
	CHECK(ids(app, window, ".editor-part.active", "part") == std::vector<uint64_t>{f.keys1});
	CHECK(elements(app, window, "#editor-active-part").at(0).at("text") == "Keys A");
	// Activate Part for Editing (p. 1143): the parts under their tracks' names.
	const Json items = elements(app, window, "#editor-active-part-menu menuitem");
	REQUIRE(items.size() == 5);
	CHECK(items.at(0).at("text") == "Keys");
	CHECK(items.at(1).at("text") == "Keys A");
	CHECK(items.at(2).at("text") == "Keys B");
	CHECK(items.at(3).at("text") == "Bass");
	CHECK(items.at(4).at("text") == "Bass A");
	click(app, window, "#editor-active-part");
	click(app, window, "#editor-active-part-menu menuitem[active-part=\"" + std::to_string(f.bass1) + "\"]");
	REQUIRE(app.waitUntil([&] { return view(app, "window").at("activePart") == f.bass1; }));
	REQUIRE(app.waitUntil(
		[&] { return ids(app, window, ".editor-part.active", "part") == std::vector<uint64_t>{f.bass1}; }));

	// Part Editing Mode (pp. 1142-1143): all parts on the active track are editable.
	CHECK(ids(app, window, ".editor-part.editable", "part").size() == 3);
	click(app, window, "#editor-editing-mode");
	click(app, window, "#editor-editing-mode-menu menuitem[editing-mode=activeTrack]");
	REQUIRE(app.waitUntil(
		[&] { return ids(app, window, ".editor-part.editable", "part") == std::vector<uint64_t>{f.bass1}; }));
	// A click on a part activates it, and with it its track.
	click(app, window, ".editor-part[part=\"" + std::to_string(f.keys2) + "\"]");
	REQUIRE(app.waitUntil(
		[&] { return ids(app, window, ".editor-part.editable", "part") == std::vector<uint64_t>{f.keys1, f.keys2}; }));
	CHECK(view(app, "window").at("activePart") == f.keys2);

	// The Visibility tab (pp. 1148-1153) in the window's left zone: a check per track, the active one marked.
	const std::string bassRow = ".editor-visibility-track[track=\"" + std::to_string(f.bass) + "\"]";
	const std::string keysRow = ".editor-visibility-track[track=\"" + std::to_string(f.keys) + "\"]";
	REQUIRE(app.waitUntil([&] { return elements(app, window, ".editor-visibility-track").size() == 2; }));
	CHECK(has(elements(app, window, keysRow).at(0), "active"));
	CHECK(has(elements(app, window, bassRow + " .editor-visibility-check").at(0), "checked"));
	click(app, window, bassRow + " .editor-visibility-check");
	REQUIRE(app.waitUntil([&] { return parts(view(app, "window")) == std::vector<uint64_t>{f.keys1, f.keys2}; }));
	REQUIRE(app.waitUntil(
		[&] { return !has(elements(app, window, bassRow + " .editor-visibility-check").at(0), "checked"); }));
	// Showing a track activates it; a double click on its name shows it exclusively.
	click(app, window, bassRow + " .editor-visibility-check");
	REQUIRE(app.waitUntil([&] { return view(app, "window").at("activeTrack") == f.bass; }));
	click(app, window, keysRow + " .editor-visibility-name", 2);
	REQUIRE(app.waitUntil([&] { return parts(view(app, "window")) == std::vector<uint64_t>{f.keys1, f.keys2}; }));
	// A track not all of whose parts show has a rectangle, and a click shows them all.
	app.call("editor.set", {{"editor", "window"}, {"parts", {f.keys1}}});
	REQUIRE(app.waitUntil(
		[&] { return has(elements(app, window, keysRow + " .editor-visibility-check").at(0), "partial"); }));
	click(app, window, keysRow + " .editor-visibility-check");
	REQUIRE(app.waitUntil([&] { return parts(view(app, "window")) == std::vector<uint64_t>{f.keys1, f.keys2}; }));

	// Global tracks above the events (pp. 1144-1146), from the inspector's Global Tracks section.
	const uint64_t markers = app.call("marker.add", Json::object()).at("marker").at("track").get<uint64_t>();
	app.appUi().docking().open("editorInspector");
	const std::string toggle = ".editor-global-toggle[track=\"" + std::to_string(markers) + "\"]";
	REQUIRE(app.waitUntil([&] { return elements(app, window, toggle).size() == 1; }));
	CHECK(elements(app, window, ".editor-global-track").empty());
	click(app, window, toggle);
	REQUIRE(app.waitUntil(
		[&] { return ids(app, window, ".editor-global-track", "track") == std::vector<uint64_t>{markers}; }));
	CHECK(has(elements(app, window, toggle).at(0), "checked"));
	// The signature track is one too (#729).
	app.call("track.add", {{"type", "signature"}});
	REQUIRE(app.waitUntil([&] { return elements(app, window, ".editor-global-toggle").size() == 2; }));

	// Open/Close Editor without a part selected closes it; closing its window does too.
	f.select({});
	app.keys().run("editor.openClose", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() == nullptr; }));
	CHECK(view(app, "window").at("open") == false);
	f.select({f.bass1});
	app.keys().run("editor.openClose", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	app.shell().close(app.appUi().editorWindowId());
	REQUIRE(app.waitUntil([&] { return view(app, "window").at("open") == false; }));
	// An engine client opens it too, and a new project closes it.
	app.call("editor.open", {{"editor", "window"}, {"parts", {f.keys1}}});
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	app.call("project.new");
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() == nullptr; }));
}

TEST_CASE("the lower zone's editor follows the project window's selection unless it keeps its contents", "[ui]")
{
	test::UiApp app({1200, 700});
	Fixture f(app);
	// Whether the editor window shows the part, once it has its view.
	const auto windowShows = [&app](uint64_t part)
	{
		return app.waitUntil([&app, part]
			{ return ids(app, app.appUi().editorWindowId(), ".editor-part", "part") == std::vector<uint64_t>{part}; });
	};
	f.select({f.keys1, f.bass1});
	// From the project zone's focus, the lower zone's editor takes the keyboard focus when it opens (p. 100).
	REQUIRE(app.appUi().docking().focus("project", "main"));
	REQUIRE(has(elements(app, std::nullopt, "#main-zone").at(0), "focused"));
	app.keys().run("window.lowerZoneEditor", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("editor"); }));
	CHECK(app.appUi().lowerZoneEditor() != nullptr);
	CHECK(app.waitUntil([&] { return has(elements(app, std::nullopt, "#lower-zone").at(0), "focused"); }));
	CHECK(!has(elements(app, std::nullopt, "#main-zone").at(0), "focused"));
	REQUIRE(app.waitUntil(
		[&]
		{
			return ids(app, std::nullopt, "#lower-zone .editor-part", "part") ==
				std::vector<uint64_t>{f.keys1, f.bass1};
		}));
	// The lower zone's editor has its Visibility tab in the project window's left zone, and no global tracks.
	app.appUi().docking().open("visibility");
	REQUIRE(
		app.waitUntil([&] { return elements(app, std::nullopt, "#left-zone .editor-visibility-track").size() == 2; }));
	CHECK(elements(app, std::nullopt, "#lower-zone .editor-global-tracks").empty());

	// Editor Content Follows Event Selection (p. 1153).
	f.select({f.keys2});
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{f.keys2}; }));
	// Keep Editor Contents: the selection no longer reaches it.
	click(app, std::nullopt, "#lower-zone #editor-keep-contents");
	REQUIRE(app.waitUntil([&] { return view(app, "lower").at("keepContents") == true; }));
	REQUIRE(app.waitUntil([&] { return has(elements(app, std::nullopt, "#editor-keep-contents").at(0), "checked"); }));
	f.select({f.bass1});
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	CHECK(parts(view(app, "lower")) == std::vector<uint64_t>{f.keys2});

	// Open in Separate Window (p. 1196): the window takes the parts, the lower zone's editor closes.
	click(app, std::nullopt, "#lower-zone #editor-move");
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	CHECK(parts(view(app, "window")) == std::vector<uint64_t>{f.keys2});
	// The Editor tab stays in front and shows the selection again, without Keep Editor Contents (pp. 87-88, #625).
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{f.bass1}; }));
	CHECK(view(app, "lower").at("keepContents") == false);
	// And back: Open in Lower Zone, once the window shows the parts (before it has its view from the engine, it
	// leaves clicks alone).
	REQUIRE(windowShows(f.keys2));
	click(app, app.appUi().editorWindowId(), "#editor-move");
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() == nullptr; }));
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{f.keys2}; }));
	CHECK(app.appUi().docking().showing("editor"));

	// Another tab in front of it leaves it open; hiding the lower zone closes it.
	app.appUi().docking().open("mixer");
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	CHECK(view(app, "lower").at("open") == true);
	// Behind the Mixer it still follows the selection, and the Mixer stays in front.
	f.select({f.keys1});
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{f.keys1}; }));
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.appUi().docking().showing("mixer"));
	CHECK_FALSE(app.appUi().docking().showing("editor"));
	// Opening it explicitly brings the Editor tab forward from behind the Mixer: Open Editor in Lower Zone ...
	app.keys().run("window.lowerZoneEditor", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("editor"); }));
	// ... and the editor window's Open in Lower Zone (p. 1198).
	app.appUi().docking().open("mixer");
	app.call("editor.open", {{"editor", "window"}, {"parts", {f.keys1}}});
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	CHECK_FALSE(app.appUi().docking().showing("editor"));
	REQUIRE(windowShows(f.keys1));
	click(app, app.appUi().editorWindowId(), "#editor-move");
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() == nullptr; }));
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("editor"); }));
	app.appUi().docking().setVisible("project", "lower", false);
	REQUIRE(app.waitUntil([&] { return view(app, "lower").at("open") == false; }));
}

TEST_CASE(
	"Return and a double click on a part open it in the lower zone's editor, which takes the keyboard focus", "[ui]")
{
	test::UiApp app({1200, 700});
	Fixture f(app);
	app.setDpRatio(1.0F);
	// A part from 4 to 6 seconds (two bars at 120 BPM) on the Bass track, and two points on it.
	const uint64_t part = app.call("midiPart.add",
								 {{"track", f.bass}, {"position", 8 * time::g_ticksPerQuarter},
									 {"length", 4 * time::g_ticksPerQuarter}, {"name", "Bass B"}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
	REQUIRE(app.waitUntil([&] { return f.display.project().at("midiParts").size() == 4; }));
	app.shell().step(std::chrono::milliseconds(0));
	const std::vector<uint64_t>& lanes = f.display.laneTracks();
	const auto lane = static_cast<size_t>(std::find(lanes.begin(), lanes.end(), f.bass) - lanes.begin());
	REQUIRE(lane < lanes.size());
	const auto at = [&](double seconds)
	{
		Rml::Element& area = f.display.eventArea();
		const double x = area.GetAbsoluteLeft() + ((seconds - f.display.view().start) / f.display.view().scale);
		const double y = area.GetAbsoluteTop() + ((f.display.laneTops()[lane] + f.display.laneTops()[lane + 1]) / 2.0) -
			f.display.view().top;
		return ui::Target{"", 0, ui::Point{static_cast<int>(x), static_cast<int>(y)}};
	};
	const auto focused = [&app](const char* zone) { return has(elements(app, std::nullopt, zone).at(0), "focused"); };
	// Hiding the lower zone closes its editor; while the Editor tab shows, it would show the selection again (#625).
	// First the UI has the open view: one still on its way brings the tab forward again.
	const auto hideLower = [&app]
	{
		REQUIRE(app.waitUntil(
			[&]
			{
				const ui::MidiEditor* lower = app.appUi().lowerZoneEditor();
				return lower != nullptr && lower->view().value("open", false);
			}));
		app.appUi().docking().setVisible("project", "lower", false);
		REQUIRE(app.waitUntil([&] { return view(app, "lower").at("open") == false; }));
	};

	// Return (Open/Close Editor) opens the selected part in the lower zone's editor by default (reference manual pp.
	// 87, 1603), which shows the zone and takes the keyboard focus (p. 100).
	app.ui().click(std::nullopt, at(4.5), ui::MouseButton::Left, 1, {});
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(f.display.selection() == std::vector<uint64_t>{part});
	REQUIRE(focused("#main-zone"));
	app.ui().key(std::nullopt, "Return");
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{part}; }));
	CHECK(app.appUi().docking().showing("editor"));
	CHECK(app.waitUntil([&] { return focused("#lower-zone"); }));
	CHECK(app.appUi().editorWindow() == nullptr);

	// A double click on the part does the same.
	hideLower();
	REQUIRE(app.appUi().docking().focus("project", "main"));
	app.ui().click(std::nullopt, at(5.0), ui::MouseButton::Left, 2, {});
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{part}; }));
	CHECK(app.appUi().docking().showing("editor"));
	CHECK(app.waitUntil([&] { return focused("#lower-zone"); }));
	CHECK(app.appUi().editorWindow() == nullptr);

	// Return with only an audio part selected opens it in the lower zone's audio part editor (p. 692).
	const uint64_t audio = app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
	const uint64_t audioPart =
		app.call("audioPart.add", {{"track", audio}, {"position", 0}, {"length", 4 * time::g_ticksPerQuarter}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	REQUIRE(app.waitUntil([&] { return f.display.project().at("audioParts").size() == 1; }));
	hideLower();
	REQUIRE(app.appUi().docking().focus("project", "main"));
	f.select({audioPart});
	app.ui().key(std::nullopt, "Return");
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{audioPart}; }));
	CHECK(view(app, "lower").at("content") == "audioPart");
	CHECK(app.waitUntil([&] { return focused("#lower-zone"); }));
	CHECK(app.appUi().editorWindow() == nullptr);

	// With Double-click opens Editor in a Window (p. 1603), both open the editor window and leave the focus in the
	// main zone.
	app.call("settings.set", {{"key", "editing.doubleClickOpensEditorIn"}, {"value", "window"}});
	hideLower();
	REQUIRE(app.appUi().docking().focus("project", "main"));
	f.select({part});
	app.ui().key(std::nullopt, "Return");
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	CHECK(parts(view(app, "window")) == std::vector<uint64_t>{part});
	CHECK(view(app, "lower").at("open") == false);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(focused("#main-zone"));
	app.call("editor.close", {{"editor", "window"}});
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() == nullptr; }));
	app.ui().click(std::nullopt, at(5.5), ui::MouseButton::Left, 2, {});
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	CHECK(view(app, "lower").at("open") == false);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(focused("#main-zone"));
}

TEST_CASE("the Visibility tab indents and folds folders and marks the tracks that can't show", "[ui]")
{
	test::UiApp app({1200, 700});
	Fixture f(app);
	const uint64_t folder =
		app.call("track.add", {{"type", "folder"}, {"name", "Folder"}}).at("track").at("id").get<uint64_t>();
	const uint64_t pads = app.call("track.add", {{"type", "midi"}, {"name", "Pads"}, {"parent", folder}})
							  .at("track")
							  .at("id")
							  .get<uint64_t>();
	const uint64_t strings = app.call("track.add", {{"type", "midi"}, {"name", "Strings"}, {"parent", folder}})
								 .at("track")
								 .at("id")
								 .get<uint64_t>();
	const uint64_t pads1 = addPart(app, pads, 0, "Pads A");
	const uint64_t strings1 = addPart(app, strings, 0, "Strings A");
	REQUIRE(app.waitUntil([&] { return f.display.project().at("midiParts").size() == 5; }));
	app.call("editor.open", {{"editor", "window"}, {"parts", {f.keys1, pads1, strings1}}});
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	const Window window = app.appUi().editorWindowId();
	const auto row = [](uint64_t track) { return ".editor-visibility-track[track=\"" + std::to_string(track) + "\"]"; };
	const auto rowOf = [&](uint64_t track) { return elements(app, window, row(track)).at(0); };
	const auto checkOf = [&](uint64_t track)
	{ return elements(app, window, row(track) + " .editor-visibility-check").at(0); };
	const auto shows = [&](const std::vector<uint64_t>& shown) { return parts(view(app, "window")) == shown; };
	REQUIRE(app.waitUntil([&] { return elements(app, window, ".editor-visibility-track").size() == 5; }));

	// A folder's tracks are indented under it, and the folder has a triangle (p. 1151). Every row has the triangle's
	// part, shown only on folders, so the rows line up.
	CHECK(rowOf(pads).at("attributes").at("level") == "1");
	CHECK(rowOf(f.keys).at("attributes").at("level") == "0");
	CHECK(has(rowOf(folder), "parent"));
	CHECK(has(rowOf(folder), "expanded"));
	CHECK_FALSE(has(rowOf(f.keys), "parent"));
	CHECK(elements(app, window, ".editor-visibility-track > .tree-toggle").size() == 5);
	// A click on the folder's name collapses it in place, one on the triangle expands it again.
	click(app, window, row(folder) + " .editor-visibility-name");
	REQUIRE(app.waitUntil([&] { return has(rowOf(pads), "hidden") && has(rowOf(strings), "hidden"); }));
	CHECK_FALSE(has(rowOf(folder), "expanded"));
	CHECK_FALSE(has(rowOf(f.keys), "hidden"));
	CHECK(shows({f.keys1, pads1, strings1}));
	click(app, window, row(folder) + " .tree-toggle");
	REQUIRE(app.waitUntil([&] { return !has(rowOf(pads), "hidden"); }));
	CHECK(has(rowOf(folder), "expanded"));
	// A double click on the folder's name shows its tracks exclusively, and leaves it expanded.
	click(app, window, row(folder) + " .editor-visibility-name", 2);
	REQUIRE(app.waitUntil([&] { return shows({pads1, strings1}); }));
	CHECK(has(rowOf(folder), "expanded"));
	app.call("editor.set", {{"editor", "window"}, {"parts", {f.keys1, pads1, strings1}}});
	REQUIRE(app.waitUntil([&] { return shows({f.keys1, pads1, strings1}); }));

	// Tracks that hold locked parts can't show (p. 1151): marked, and their checks show nothing, but hide what shows.
	app.call("event.set", {{"events", {f.bass1, f.keys2}}, {"lock", true}});
	REQUIRE(app.waitUntil([&] { return has(rowOf(f.bass), "unavailable") && has(rowOf(f.keys), "unavailable"); }));
	CHECK(!has(rowOf(pads), "unavailable"));
	CHECK(checkOf(f.bass).at("attributes").contains("disabled"));
	CHECK_FALSE(checkOf(f.keys).value("attributes", Json::object()).contains("disabled"));
	click(app, window, row(f.bass) + " .editor-visibility-check");
	click(app, window, row(f.keys) + " .editor-visibility-check", 1, true);
	click(app, window, row(f.bass) + " .editor-visibility-name", 2);
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	CHECK(shows({f.keys1, pads1, strings1}));
	click(app, window, row(f.keys) + " .editor-visibility-check");
	REQUIRE(app.waitUntil([&] { return shows({pads1, strings1}); }));
	app.call("editor.set", {{"editor", "window"}, {"parts", {f.keys1, pads1, strings1}}});
	REQUIRE(app.waitUntil([&] { return shows({f.keys1, pads1, strings1}); }));

	// Nor can a track hidden in the project window, though what it shows still hides.
	app.call("trackVisibility.set", {{"hidden", {pads}}});
	REQUIRE(app.waitUntil([&] { return has(rowOf(pads), "unavailable"); }));
	click(app, window, row(pads) + " .editor-visibility-check");
	REQUIRE(app.waitUntil([&] { return shows({f.keys1, strings1}); }));
	REQUIRE(app.waitUntil([&] { return checkOf(pads).at("attributes").contains("disabled"); }));
	// A folder counts a hidden track's parts only while they show, as showTracks leaves them out: all its other parts
	// show, so it has a check that hides them.
	CHECK(has(checkOf(folder), "checked"));
	click(app, window, row(folder) + " .editor-visibility-check");
	REQUIRE(app.waitUntil([&] { return shows({f.keys1}); }));
	// A folder whose tracks are all hidden has nothing that can show.
	app.call("trackVisibility.set", {{"hidden", {pads, strings}}});
	REQUIRE(app.waitUntil([&] { return elements(app, window, row(folder)).empty(); }));

	// The project window's Visibility tab while the lower zone's editor isn't open: nothing to show or hide.
	app.appUi().docking().open("visibility");
	REQUIRE(app.waitUntil([&] { return !elements(app, std::nullopt, "#left-zone .editor-visibility").empty(); }));
	REQUIRE(app.waitUntil(
		[&] { return has(elements(app, std::nullopt, "#left-zone .editor-visibility").at(0), "closed"); }));
	const Json checks = elements(app, std::nullopt, "#left-zone .editor-visibility-check");
	REQUIRE(!checks.empty());
	for (const Json& check : checks)
		CHECK(check.at("attributes").contains("disabled"));
}

TEST_CASE("a double click on a shown track's name shows it exclusively after the first click's editor.changed", "[ui]")
{
	test::UiApp app({1200, 700});
	Fixture f(app);
	app.call("editor.open", {{"editor", "window"}, {"parts", {f.keys1, f.keys2, f.bass1}}});
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	const Window window = app.appUi().editorWindowId();
	const std::string bassRow = ".editor-visibility-track[track=\"" + std::to_string(f.bass) + "\"]";
	REQUIRE(app.waitUntil([&] { return elements(app, window, ".editor-visibility-track").size() == 2; }));
	REQUIRE(!has(elements(app, window, bassRow).at(0), "active"));
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true); // the two presses are within the double click time however long the wait takes
	const std::shared_ptr<void> thaw(nullptr, [clock](void*) { clock->freeze(false); });
	// The first click activates the track (p. 1149), and its editor.changed reaches the tab before the second click,
	// as it does for a user; the row stays, so the second click makes the double click that shows the track
	// exclusively (p. 1151).
	click(app, window, bassRow + " .editor-visibility-name");
	REQUIRE(app.waitUntil([&] { return has(elements(app, window, bassRow).at(0), "active"); }));
	click(app, window, bassRow + " .editor-visibility-name");
	REQUIRE(app.waitUntil([&] { return parts(view(app, "window")) == std::vector<uint64_t>{f.bass1}; }));
	REQUIRE(app.waitUntil([&]
		{ return !has(elements(app, window, ".editor-visibility-track .editor-visibility-check").at(0), "checked"); }));
	CHECK(has(elements(app, window, bassRow + " .editor-visibility-check").at(0), "checked"));
}

TEST_CASE("an editor the user closed doesn't open again for an editor.changed still on its way", "[ui]")
{
	test::UiApp app({1200, 700});
	Fixture f(app);
	app.call("editor.open", {{"editor", "window"}, {"parts", {f.keys1}}});
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	// The user closes the window while an editor.changed with it open is still on its way to the UI.
	app.shell().close(app.appUi().editorWindowId());
	app.call("editor.set", {{"editor", "window"}, {"activePart", f.keys1}});
	REQUIRE(app.waitUntil([&] { return view(app, "window").at("open") == false; }));
	for (int i = 0; i < 5; ++i)
		app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.appUi().editorWindow() == nullptr);
	CHECK(view(app, "window").at("open") == false);
}

TEST_CASE("the lower zone's Editor tab in front without an open editor opens it with the selected parts (#625)", "[ui]")
{
	test::UiApp app({1200, 700});
	Fixture f(app);
	const auto steps = [&app]
	{
		for (int i = 0; i < 5; ++i)
			app.shell().step(std::chrono::milliseconds(0));
	};
	// Until the UI has the engine's views: the binding hands on the events that came before an answer first.
	const auto caughtUp = [&app]
	{
		const auto answered = std::make_shared<bool>(false);
		app.appUi().binding().send(
			"editor.get", {{"editor", "lower"}}, [answered](const engine::Result&) { *answered = true; });
		REQUIRE(app.waitUntil([&] { return *answered; }));
	};
	// Picked from the tab bar, the Editor tab shows the project window's selected parts (p. 1153).
	f.select({f.keys1});
	app.appUi().docking().open("editor");
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{f.keys1}; }));

	// The tab stays when its editor closes, and shows the selection again (pp. 87-88): showing other parts and closed
	// from another client, the editor opens again with the selected parts.
	app.call("editor.open", {{"editor", "lower"}, {"parts", {f.keys2}}});
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{f.keys2}; }));
	app.call("editor.close", {{"editor", "lower"}});
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{f.keys1}; }));
	CHECK(app.appUi().docking().showing("editor"));
	// Without a selection it stays empty until there is one.
	f.select({});
	app.call("editor.close", {{"editor", "lower"}});
	REQUIRE(app.waitUntil([&] { return view(app, "lower").at("open") == false; }));
	steps();
	CHECK(view(app, "lower").at("open") == false);
	CHECK(app.appUi().docking().showing("editor"));
	f.select({f.bass1});
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{f.bass1}; }));

	// Behind the Mixer it stays closed; Open in Lower Zone from the editor window brings the tab forward with the
	// window's parts, not the project window's selection.
	caughtUp(); // the view opening brings the tab forward
	app.appUi().docking().open("mixer");
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("mixer"); }));
	app.call("editor.close", {{"editor", "lower"}});
	caughtUp();
	f.select({f.keys1});
	steps();
	CHECK(view(app, "lower").at("open") == false);
	app.call("editor.open", {{"editor", "window"}, {"parts", {f.keys2}}});
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	REQUIRE(app.waitUntil([&]
		{ return ids(app, app.appUi().editorWindowId(), ".editor-part", "part") == std::vector<uint64_t>{f.keys2}; }));
	click(app, app.appUi().editorWindowId(), "#editor-move");
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() == nullptr; }));
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("editor"); }));
	REQUIRE(app.waitUntil([&] { return parts(view(app, "lower")) == std::vector<uint64_t>{f.keys2}; }));
	steps();
	CHECK(parts(view(app, "lower")) == std::vector<uint64_t>{f.keys2});

	// A new project closes it and selects nothing: the tab stays in front, empty.
	app.call("project.new");
	REQUIRE(app.waitUntil([&] { return view(app, "lower").at("open") == false; }));
	steps();
	CHECK(view(app, "lower").at("open") == false);
	CHECK(app.appUi().docking().showing("editor"));
}

TEST_CASE("the MIDI editors share one project read for each change (#625)", "[ui]")
{
	test::UiApp app({1200, 700});
	Fixture f(app);
	ui::EngineBinding& binding = app.appUi().binding();
	// How many project.get the UI answers for a note added, until none is out.
	const auto reads = [&](int64_t position)
	{
		REQUIRE(app.waitUntil([&] { return binding.waiting("project.get") == 0; }));
		size_t count = 0;
		const ui::Subscription counted = binding.onResult("project.get", [&count](const engine::Result&) { ++count; });
		app.call("note.add",
			{{"part", f.keys1},
				{"notes", {{{"position", position}, {"length", 120}, {"pitch", 60}, {"velocity", 0.5}}}}});
		for (int i = 0; i < 5; ++i)
			app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(app.waitUntil([&] { return binding.waiting("project.get") == 0; }));
		return count;
	};
	reads(0); // the first after the start may count reads of its own
	// The lower zone's editor and the project window's Visibility tab are there from the start.
	const size_t before = reads(120);

	// Three more: the editor window, with its Visibility tab and inspector. They read no more.
	f.select({f.keys1});
	app.keys().run("window.lowerZoneEditor", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().lowerZoneEditor() != nullptr; }));
	app.call("editor.open", {{"editor", "window"}, {"parts", {f.keys2}}});
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	app.appUi().docking().open("editorVisibility");
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("editorVisibility"); }));
	app.appUi().docking().open("editorInspector");
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("editorInspector"); }));
	CHECK(reads(240) == before);
}

TEST_CASE("a project read asked for after the binding handed on something else is a read of its own (#625)", "[ui]")
{
	test::UiApp app({1200, 700});
	ui::EngineBinding& binding = app.appUi().binding();
	ui::ProjectReads reads(binding);
	// EngineBinding::handed() when each got its answer.
	uint64_t first = 0;
	uint64_t later = 0;
	uint64_t again = 0;
	// An answer the binding hands on while the first read is out: the read asked for then goes out once the first is
	// answered.
	binding.send("project.get", Json::object(),
		[&](const engine::Result&) { reads.read([&](const engine::Result&) { later = binding.handed(); }); });
	reads.read(
		[&](const engine::Result&)
		{
			first = binding.handed();
			// Asked for again from the answer: joins the read that went out for the later one.
			reads.read([&](const engine::Result&) { again = binding.handed(); });
		});
	REQUIRE(app.waitUntil([&] { return again != 0; }));
	CHECK(first != 0);
	CHECK(first < later);
	CHECK(again == later);
}

TEST_CASE("the MIDI menu opens the selected MIDI parts in the editor window's key, drum or list editor (#719)", "[ui]")
{
	test::UiApp app({1200, 700});
	Fixture f(app);
	f.select({f.keys1, f.bass1});
	for (const auto& [command, content] : {std::pair{"editor.openDrum", "drum"}, std::pair{"editor.openList", "list"},
			 std::pair{"editor.openKey", "key"}})
	{
		INFO(command);
		const std::string wanted = content;
		click(app, std::nullopt, "#menu-bar button[menu=midi-menu]");
		click(app, std::nullopt, std::string("#midi-menu menuitem[command=\"") + command + "\"]");
		REQUIRE(app.waitUntil([&] { return view(app, "window").value("content", "") == wanted; }));
		CHECK(parts(view(app, "window")) == std::vector<uint64_t>{f.keys1, f.bass1});
		CHECK(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	}
}
