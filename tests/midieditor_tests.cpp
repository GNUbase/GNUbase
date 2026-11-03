// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI editors' common framework (#126, docs/features/midi-editors.md) in the app, driven with ui::Automation as
// MCP's UI tools drive it: the editor window and the lower zone's editor opened for several parts on several tracks,
// the active part, the part editing mode, the Visibility tab, the global tracks and Keep Editor Contents.
// tests/editors_tests.cpp checks the engine's editor views.
#include "ui/automation.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
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
			REQUIRE(app.waitUntil(
				[&] { return display.project().is_object() && display.project().at("midiParts").size() == 3; }));
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

	// Open/Close Editor (Mod+E, Return, p. 1189) with parts of two tracks selected.
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
	f.select({f.keys1, f.bass1});
	app.keys().run("window.lowerZoneEditor", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("editor"); }));
	CHECK(app.appUi().lowerZoneEditor() != nullptr);
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
	REQUIRE(app.waitUntil([&] { return view(app, "lower").at("open") == false; }));
	CHECK(view(app, "lower").at("keepContents") == false);
	// And back: Open in Lower Zone.
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
	app.keys().run("editor.openClose", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() != nullptr; }));
	CHECK_FALSE(app.appUi().docking().showing("editor"));
	click(app, app.appUi().editorWindowId(), "#editor-move");
	REQUIRE(app.waitUntil([&] { return app.appUi().editorWindow() == nullptr; }));
	REQUIRE(app.waitUntil([&] { return app.appUi().docking().showing("editor"); }));
	app.appUi().docking().setVisible("project", "lower", false);
	REQUIRE(app.waitUntil([&] { return view(app, "lower").at("open") == false; }));
}
