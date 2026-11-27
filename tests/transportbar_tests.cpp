// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of the transport controls (#90, ui/transport.h): every control of the project window's transport bar, the
// transport panel and the time display window, driven through ui::Automation as MCP's UI tools drive them
// (tests/uiapp.h), and the transport's key commands. Controls with a command that doesn't exist or parameters it
// doesn't take are refused (#446).
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "scripting/runtime.h"
#include "testutils/commandholds.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/docking.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"
#include "ui/timeformat.h"
#include "ui/tracklist.h"
#include "ui/transport.h"
#include "ui/transportpanelfit.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

namespace
{
	constexpr int64_t g_quarter = time::g_ticksPerQuarter;

	ui::Target at(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	Json element(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		const Json found = app.ui().dom(window, selector, 0).at("elements");
		REQUIRE(found.size() == 1);
		return found[0];
	}

	bool hasClass(test::UiApp& app, const std::string& selector, const std::string& name,
		std::optional<ui::WindowId> window = std::nullopt)
	{
		const Json names = element(app, selector, window).value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	bool checked(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		return hasClass(app, selector, "checked", window);
	}

	// Clicks the button beside its icon, which focuses the button itself, then checks that Space starts and stops
	// playback and NumEnter starts it (transport.play, reference manual p. 1572), rather than clicking the button again
	// (#456, #653). on says whether the button is on; focuses whether the click leaves the focus on the button.
	void keysAfterClick(test::UiApp& app, const std::string& button, std::optional<ui::WindowId> window,
		const std::function<bool()>& on, bool focuses = true)
	{
		INFO(button);
		const bool wasOn = on();
		const Json box = element(app, button, window).at("box");
		const ui::Point edge{box.at("x").get<int>() + 2, box.at("y").get<int>() + (box.at("height").get<int>() / 2)};
		app.ui().click(window, {"", 0, edge}, ui::MouseButton::Left, 1, {});
		REQUIRE(app.waitUntil([&on, wasOn] { return on() != wasOn; }));
		const auto playing = [&app](bool playing)
		{
			return app.waitUntil(
				[&app, playing] { return app.call("transport.get").at("playing").get<bool>() == playing; });
		};
		// The focus is the button: buttons without an id are told apart by their classes, attributes and box.
		const Json focus = app.ui().key(window, "Space").at("focus");
		const Json clicked = element(app, button, window);
		REQUIRE(focus.is_object());
		for (const char* key : {"tag", "id", "classes", "attributes", "box"})
		{
			INFO(key);
			CHECK((!focuses || focus.value(key, Json()) == clicked.value(key, Json())));
		}
		REQUIRE(playing(true));
		app.ui().key(window, "Space");
		REQUIRE(playing(false));
		app.ui().key(window, "NumEnter");
		REQUIRE(playing(true));
		app.ui().key(window, "Space");
		REQUIRE(playing(false));
		CHECK(on() != wasOn);
	}

	// What a display shows: a text display's value, another element's text.
	std::string shows(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		const Json found = element(app, selector, window);
		const Json attributes = found.value("attributes", Json::object());
		if (attributes.contains("value"))
			return attributes.at("value").get<std::string>();
		return found.value("text", std::string());
	}

	void click(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		app.ui().click(window, at(selector), ui::MouseButton::Left, 1, {});
	}

	Json transport(test::UiApp& app)
	{
		return app.call("transport.get");
	}

	// The project window with its transport bar shown. The shell fits windows into their monitor, 1024 pixels wide on
	// CI, where a dp ratio of 1 hides the transport bar's last sections: at 0.75, about 1365 dp, they all fit.
	ui::EventDisplay& showBar(test::UiApp& app, float dpRatio = 0.75F)
	{
		app.setDpRatio(dpRatio);
		ui::EventDisplay& display = app.appUi().showProject();
		app.appUi().docking().setVisible("project", "transportBar", true);
		app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(app.appUi().transportBar() != nullptr);
		REQUIRE(element(app, "#transport-bar").at("visible").get<bool>());
		return display;
	}

	// Double-clicks a display, types into it and presses Enter.
	void type(test::UiApp& app, const std::string& selector, const std::string& text,
		std::optional<ui::WindowId> window = std::nullopt)
	{
		app.ui().click(window, at(selector), ui::MouseButton::Left, 2, {});
		app.ui().key(window, "Mod+A");
		app.ui().text(window, text);
		app.ui().key(window, "Enter");
	}

	// Waits until the condition about the engine's transport holds.
	bool waitFor(test::UiApp& app, const std::function<bool(const Json&)>& condition)
	{
		return app.waitUntil([&app, &condition] { return condition(transport(app)); });
	}
}

TEST_CASE("the transport bar's buttons run the transport and show its state", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("transport.set", {{"left", 48000}, {"right", 96000}});

	// Locators: 120 BPM at 48 kHz, so a quarter is 24000 samples.
	REQUIRE(app.waitUntil([&app] { return shows(app, "#left-locator") == "1.3.1.0"; }));
	CHECK(shows(app, "#right-locator") == "2.1.1.0");
	CHECK(shows(app, "#locator-range") == "0.2.0.0");
	click(app, "#go-to-right");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 96000; }));
	click(app, "#go-to-left");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));

	// Rewind and forward move by a second.
	click(app, "#forward");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 96000; }));
	click(app, "#rewind");
	click(app, "#rewind");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));

	// The switches follow the engine's state, whoever changes it.
	const std::array<std::pair<std::string, std::string>, 4> switches{
		{{"#cycle", "cycle"}, {"#punch-in", "punchIn"}, {"#punch-out", "punchOut"}, {"#metronome", "metronome"}}};
	for (const auto& each : switches)
	{
		const std::string& button = each.first;
		const std::string& setting = each.second;
		INFO(button);
		// Not CHECK_FALSE: Catch2's ContinueOnFailure | FalseTest trips clang-analyzer's EnumCastOutOfRange check
		CHECK(!checked(app, button));
		click(app, button);
		REQUIRE(waitFor(app, [&setting](const Json& state) { return state.at(setting) == true; }));
		REQUIRE(app.waitUntil([&app, &button] { return checked(app, button); }));
		app.call("transport.set", {{setting, false}});
		REQUIRE(app.waitUntil([&app, &button] { return !checked(app, button); }));
	}

	// Start, record, stop.
	click(app, "#start");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("playing") == true; }));
	REQUIRE(app.waitUntil([&app] { return checked(app, "#start"); }));
	click(app, "#record");
	REQUIRE(app.waitUntil([&app] { return checked(app, "#record"); }));
	CHECK(transport(app).at("recording") == true);
	click(app, "#record"); // punches out, playback goes on
	REQUIRE(app.waitUntil([&app] { return !checked(app, "#record"); }));
	CHECK(transport(app).at("playing") == true);
	click(app, "#stop");
	REQUIRE(app.waitUntil([&app] { return !checked(app, "#start"); }));
	CHECK(transport(app).at("playing") == false);
}

TEST_CASE("Space starts and stops playback after a click on a toolbar or transport button (#456)", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.ui().key(std::nullopt, "F2");
	ui::Window* panel = app.appUi().transportPanel();
	REQUIRE(panel != nullptr);

	const auto spaceToggles = [&app](const std::string& button, std::optional<ui::WindowId> window)
	{ keysAfterClick(app, button, window, [&app, &button, window] { return checked(app, button, window); }); };
	spaceToggles("#cycle", std::nullopt);
	spaceToggles("#metronome", panel->id());
	spaceToggles("#snap", std::nullopt);
}

TEST_CASE("Space and NumEnter run their key commands after a click on a track's, the inspector's or a mixer channel's "
		  "buttons (#653)",
	"[ui]")
{
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	const Json added = app.call("track.add", {{"type", "audio"}});
	const auto channel = added.at("channel").at("id").get<uint64_t>();
	const auto channelIs = [&app, channel](const char* state)
	{
		const Json project = app.call("project.get");
		for (const Json& each : project.at("channels"))
			if (each.at("id") == channel)
				return each.value(state, false);
		return false;
	};
	const std::string mute = ".track-list button.mute"; // the only track's
	REQUIRE(app.waitUntil([&app, &mute] { return app.ui().dom(std::nullopt, mute, 0).at("elements").size() == 1; }));
	keysAfterClick(app, mute, std::nullopt, [&channelIs] { return channelIs("mute"); });

	// The inspector's buttons are the track list's in another place.
	app.appUi().docking().open("inspector");
	app.appUi().trackList()->select({added.at("track").at("id").get<uint64_t>()});
	const std::string inspectorMute = ".inspector-buttons button.mute";
	REQUIRE(app.waitUntil(
		[&app, &inspectorMute] { return app.ui().dom(std::nullopt, inspectorMute, 0).at("elements").size() == 1; }));
	REQUIRE(app.waitUntil([&app, &inspectorMute] { return element(app, inspectorMute).at("visible").get<bool>(); }));
	keysAfterClick(app, inspectorMute, std::nullopt, [&channelIs] { return channelIs("mute"); });

	app.keys().run("window.mixer", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.appUi().mixer(1) != nullptr; }));
	const ui::WindowId mixer = app.appUi().mixerWindow(1);
	const std::string solo = ".mixer-channel[channel=" + std::to_string(channel) + "] button.solo";
	REQUIRE(app.waitUntil([&app, mixer, &solo] { return app.ui().dom(mixer, solo, 0).at("elements").size() == 1; }));
	keysAfterClick(app, solo, mixer, [&channelIs] { return channelIs("solo"); });
}

TEST_CASE("Space and NumEnter run their key commands after a click on the info line's, the chord pads', a drum sound's "
		  "or a pattern lane's buttons (#2016)",
	"[ui]")
{
	test::UiApp app({1024, 700});
	ui::EventDisplay& display = app.appUi().showProject();
	const auto addPart = [&app](const char* type, const Json& extra)
	{
		const auto track = app.call("track.add", {{"type", type}}).at("track").at("id").get<uint64_t>();
		Json parameters = {{"track", track}, {"position", 0}, {"length", 4 * g_quarter}};
		parameters.update(extra);
		return std::make_pair(track, app.call("midiPart.add", parameters).at("part").at("id").get<uint64_t>());
	};
	const auto shown = [&app](const std::string& selector, std::optional<ui::WindowId> window)
	{
		return app.waitUntil(
			[&app, &selector, window]
			{
				const Json found = app.ui().dom(window, selector, 0).at("elements");
				return found.size() == 1 && found[0].value("visible", false);
			});
	};
	const auto editor = [&app](uint64_t part)
	{
		app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part})}});
		REQUIRE(app.waitUntil([&app] { return app.appUi().editorWindow() != nullptr; }));
		return app.appUi().editorWindowId();
	};

	SECTION("the info line's Mute and Lock")
	{
		app.setDpRatio(0.75F); // the whole info line
		app.appUi().docking().setVisible("project", "infoLine", true);
		display.select({addPart("midi", Json::object()).second});
		for (const std::string button : {"#info-line button[field=mute]", "#info-line button[field=lock]"})
		{
			REQUIRE(shown(button, std::nullopt));
			keysAfterClick(app, button, std::nullopt, [&app, &button] { return checked(app, button); });
		}
	}
	SECTION("the chord pads' Mute Section and Latch Pads")
	{
		app.setDpRatio(0.5F); // the whole toolbar
		app.keys().run("window.chordPads", Json::object(), app.main(), nullptr);
		REQUIRE(shown("#chord-pads-pane-playerRemote-button", std::nullopt));
		click(app, "#chord-pads-pane-playerRemote-button");
		REQUIRE(shown("#chord-pads-mute-2", std::nullopt));
		const auto pads = [&app] { return app.call("chordPads.get"); };
		keysAfterClick(
			app, "#chord-pads-mute-2", std::nullopt, [&pads] { return pads().at("mutedSections")[2].get<bool>(); });
		keysAfterClick(
			app, "#chord-pads-latchPads", std::nullopt, [&pads] { return pads().at("latchPads").get<bool>(); });
	}
	SECTION("a drum sound's Mute")
	{
		const auto [track, part] = addPart("midi", Json::object());
		const Json gm = app.call("project.get").at("drumMaps").at(0).at("id");
		const Json map = app.call("drumMap.add", {{"copy", gm}, {"name", "Kit"}}).at("map").at("id");
		app.call("track.set", {{"track", track}, {"drumMap", map}});
		const ui::WindowId window = editor(part);
		const std::string mute = ".drum-sound[pitch=\"38\"] .drum-sound-mute";
		REQUIRE(shown(mute, window));
		REQUIRE(app.waitUntil([&app] { return app.appUi().editorWindow()->keyEditor()->scrollToPitch(37); }));
		app.shell().step(std::chrono::milliseconds(0));
		// The drum sound list hands the focus on to the keys, as a click on the keys does.
		keysAfterClick(app, mute, window, [&app, &mute, window] { return checked(app, mute, window); }, false);
	}
	SECTION("a pattern lane's Mute and Solo")
	{
		const auto track = app.call("track.add", {{"type", "instrument"}}).at("track").at("id").get<uint64_t>();
		const Json pattern = app.call("pattern.add", {{"track", track}}).at("pattern").at("id");
		const auto part = app.call("midiPart.add",
								 {{"track", track}, {"position", 0}, {"length", 4 * g_quarter}, {"pattern", pattern}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
		const ui::WindowId window = editor(part);
		for (const char* which : {"mute", "solo"})
		{
			const std::string button = std::string(".pattern-lane[pitch=\"38\"] .pattern-lane-") + which;
			REQUIRE(shown(button, window));
			keysAfterClick(app, button, window, [&app, &button, window] { return checked(app, button, window); });
		}
	}
}

TEST_CASE(
	"a narrow window hides the transport bar's last sections, and its dividers keep the sections outside them", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app, 1.0F); // about 1240 dp of sections in 1024 dp
	Rml::Context& context = app.main().context();
	Rml::Element* bar = nullptr;
	for (int i = 0; i < context.GetNumDocuments(); ++i)
		if (Rml::Element* found = context.GetDocument(i)->GetElementById("transport-bar"))
			bar = found;
	REQUIRE(bar != nullptr);
	const float right = bar->GetAbsoluteLeft() + bar->GetOffsetWidth();
	const std::vector<std::string> order = app.appUi().docking().sections("project", "transport");
	REQUIRE(order.size() == 11);
	CHECK(order.front() == "midiAutoQuantize"); // before the left divider, as in the reference manual, p. 306
	CHECK(order.at(1) == "leftDivider");
	CHECK(order.at(order.size() - 2) == "rightDivider");
	const auto shown = [&app] { return app.shownSections("transport-bar"); };
	const auto has = [](const std::vector<std::string>& list, const std::string& id)
	{ return std::find(list.begin(), list.end(), id) != list.end(); };

	// The shown sections keep their controls' width, one after the other, all inside the bar, and those it has no room
	// for go, the last first. What shows is what a click reaches.
	const auto checkShown = [&]
	{
		app.shell().step(std::chrono::milliseconds(0));
		float end = bar->GetAbsoluteLeft();
		for (const std::string& id : shown())
		{
			INFO(id);
			Rml::Element* section = bar->QuerySelector("[section=" + id + "]");
			REQUIRE(section != nullptr);
			CHECK(section->GetAbsoluteLeft() >= end);
			end = section->GetAbsoluteLeft() + section->GetOffsetWidth();
			CHECK(end <= right + 0.5F);
			Rml::ElementList controls;
			section->QuerySelectorAll(controls, "*");
			for (Rml::Element* control : controls)
				CHECK(control->GetAbsoluteLeft() + control->GetOffsetWidth() <= end + 0.5F);
			for (int i = 0; i < section->GetNumChildren(); ++i)
			{
				Rml::Element* control = section->GetChild(i);
				INFO(control->GetId());
				const Rml::Vector2f middle(control->GetAbsoluteLeft() + (control->GetOffsetWidth() / 2.0F),
					control->GetAbsoluteTop() + (control->GetOffsetHeight() / 2.0F));
				const Rml::Element* hit = context.GetElementAtPoint(middle);
				while (hit != nullptr && hit != control)
					hit = hit->GetParentNode();
				CHECK(hit == control);
			}
		}
	};
	checkShown();
	std::vector<std::string> now = shown();
	CHECK(!has(now, "midiAutoQuantize")); // hidden by default
	CHECK(!has(now, "leftDivider"));
	CHECK(!has(now, "click"));
	CHECK(has(now, "transportControls"));
	click(app, "#punch-in");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("punchIn") == true; }));

	// The right divider keeps Click, after it, shown; the sections before it go instead.
	app.appUi().docking().setSectionHidden("project", "transport", "rightDivider", false);
	checkShown();
	now = shown();
	CHECK(has(now, "rightDivider"));
	CHECK(has(now, "click"));
	CHECK(!has(now, "tempoSignature"));

	// The left divider after the transport controls keeps them and the sections before them shown; the time
	// displays and the tempo, between the dividers, go first, also in a window too narrow for those outside.
	app.appUi().docking().moveSection("project", "transport", "leftDivider", "transportControls"); // after them
	app.appUi().docking().setSectionHidden("project", "transport", "leftDivider", false);
	REQUIRE(app.appUi().docking().sections("project", "transport").at(5) == "leftDivider");
	app.setDpRatio(2.0F);
	app.shell().step(std::chrono::milliseconds(0));
	now = app.shownSections("transport-bar");
	CHECK(now ==
		std::vector<std::string>{
			"locators", "locatorRange", "punchPoints", "transportControls", "leftDivider", "rightDivider", "click"});

	// Hidden again, the left divider keeps nothing.
	app.appUi().docking().setSectionHidden("project", "transport", "leftDivider", true);
	app.shell().step(std::chrono::milliseconds(0));
	now = app.shownSections("transport-bar");
	CHECK(!has(now, "transportControls"));
	CHECK(has(now, "click"));

	// The transport panel takes its sections' width and has no dividers, nor buttons for them in its setup pane.
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() != nullptr);
	const ui::WindowId panel = app.appUi().transportPanel()->id();
	CHECK(app.ui().dom(panel, "[divider]", 0).at("elements").empty());
	CHECK(app.ui().dom(panel, "[setup-section=leftDivider]", 0).at("elements").empty());
	CHECK(app.appUi().docking().sections("transportPanel", "transport").size() == 9);
}

TEST_CASE("a transport set saved before the dividers keeps them hidden in their places, and the bar still hides its "
		  "last sections",
	"[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app, 1.0F); // about 1240 dp of sections in 1024 dp
	const Json before = Json::array(
		{"locators", "locatorRange", "punchPoints", "transportControls", "timeDisplays", "tempoSignature", "click"});
	// An older workspace or preset with the order and hidden list, and a layout with only a hidden list.
	const std::array<Json, 2> saved{
		Json{{"order", before}, {"hidden", Json::array()}}, Json{{"hidden", Json::array()}}};
	for (const Json& set : saved)
	{
		INFO(set.dump());
		Json layout = app.call("settings.get", {{"key", "ui.layout"}}).at("value");
		layout["windows"]["project"]["sections"]["transport"] = set;
		app.call("settings.set", {{"key", "ui.layout"}, {"value", layout}});
		REQUIRE(app.waitUntil([&app, &layout] { return app.appUi().docking().layout() == layout; }));
		app.shell().step(std::chrono::milliseconds(0));
		CHECK(app.appUi().docking().sections("project", "transport") ==
			std::vector<std::string>{"midiAutoQuantize", "leftDivider", "locators", "locatorRange", "punchPoints",
				"transportControls", "timeDisplays", "prePostRoll", "tempoSignature", "rightDivider", "click"});
		const std::vector<std::string> shown = app.shownSections("transport-bar");
		const auto has = [&shown](const std::string& id)
		{ return std::find(shown.begin(), shown.end(), id) != shown.end(); };
		CHECK(!has("midiAutoQuantize"));
		CHECK(!has("leftDivider"));
		CHECK(!has("rightDivider"));
		CHECK(!has("prePostRoll"));
		CHECK(!has("click"));
		CHECK(has("transportControls"));
	}
}

TEST_CASE("every control of the default transport bar fits a 1024-pixel window at a dp ratio of 0.8", "[ui]")
{
	// 1280 dp, a 1920-pixel display at 150 % (#1450): the bar holds bars and beats, timecode and a new project's
	// locators.
	test::UiApp app({1024, 700});
	showBar(app, 0.8F);
	Rml::Context& context = app.main().context();
	Rml::Element* bar = nullptr;
	for (int i = 0; i < context.GetNumDocuments(); ++i)
		if (Rml::Element* found = context.GetDocument(i)->GetElementById("transport-bar"))
			bar = found;
	REQUIRE(bar != nullptr);
	const float right = bar->GetAbsoluteLeft() + bar->GetOffsetWidth();
	Rml::ElementList controls;
	bar->QuerySelectorAll(controls, ".transport-section > *");
	REQUIRE(!controls.empty());
	for (Rml::Element* control : controls)
	{
		INFO(control->GetId());
		CHECK(control->GetAbsoluteLeft() + control->GetOffsetWidth() <= right);
	}
}

namespace
{
	// Opens a new transport panel with F2, at a dp ratio, and checks that it has the width of its sections once the
	// controls show the project: every control inside the window and the window ending a section gap after the last
	// one, or, where the sections are wider than the primary monitor's usable area, as wide as that area (#1424,
	// #1474). shown says when the panel shows what the test set up; reopen closes the panel right after the fit and
	// opens it again, with the bounds the fit kept, which it follows from (#1527).
	void checkPanelFits(
		test::UiApp& app, float dpRatio, const std::function<bool()>& shown = [] { return true; }, bool reopen = false)
	{
		INFO("dp ratio " << dpRatio);
		app.setDpRatio(dpRatio);
		app.ui().key(std::nullopt, "F2");
		// The panel follows its sections whenever they change, so it fits once it shows what the test set up.
		if (reopen)
		{
			REQUIRE(app.waitUntil(shown, std::chrono::seconds(2)));
			app.ui().key(std::nullopt, "F2");
			REQUIRE(app.appUi().transportPanel() == nullptr);
			app.ui().key(std::nullopt, "F2");
		}
		ui::Window* panel = app.appUi().transportPanel();
		REQUIRE(panel != nullptr);
		Rml::Context& context = panel->context();
		Rml::Element* transport = nullptr;
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (Rml::Element* found = context.GetDocument(i)->GetElementById("transport"))
				transport = found;
		REQUIRE(transport != nullptr);
		const int usable = app.shell().displays().front().usable.width;
		// Where the last control ends and whether every one is inside the window, in pixels.
		const auto measure = [&]
		{
			context.Update();
			const auto width = static_cast<float>(panel->pixelSize().width);
			float last = 0;
			bool inside = true;
			Rml::ElementList controls;
			transport->QuerySelectorAll(controls, ".transport-section > *");
			for (Rml::Element* control : controls)
			{
				if (!control->IsVisible(true)) // in a hidden section, such as Pre-roll & Post-roll, not laid out
					continue;
				const float end = control->GetAbsoluteLeft() + control->GetOffsetWidth();
				inside = inside && end <= width;
				last = std::max(last, end);
			}
			return std::make_pair(last, inside);
		};
		// After the controls' first queries: the next step hands their answers on, and the panel follows from then.
		app.call("engine.version");
		app.shell().step(std::chrono::milliseconds(0));
		const auto fitted = [&]
		{
			const auto [last, inside] = measure();
			const float gap = static_cast<float>(panel->pixelSize().width) - last;
			return inside ? gap <= (16.0F * dpRatio) + 1.0F : app.shell().bounds(panel->id()).width == usable;
		};
		app.waitUntil([&] { return shown() && fitted(); }, std::chrono::seconds(2));
		CHECK(shown());
		const auto [last, inside] = measure();
		const auto width = static_cast<float>(panel->pixelSize().width);
		INFO("last control ends at " << last << " of " << width << " pixels; usable width " << usable);
		CHECK(last > 0);
		CHECK(app.shell().bounds(panel->id()).width <= usable); // never wider than the monitor has room for
		if (inside)
			CHECK(width - last <= (16.0F * dpRatio) + 1.0F);
		else // the sections are wider than the monitor has room for
			CHECK(app.shell().bounds(panel->id()).width == usable);
	}
}

TEST_CASE("a new transport panel takes the width of its sections", "[ui]")
{
	// At a dp ratio of 1 the sections are wider than CI's 1024-pixel monitor, and the panel takes its usable width.
	for (const float dpRatio : {0.5F, 0.75F, 1.0F})
	{
		test::UiApp app({1024, 700});
		app.appUi().showProject();
		checkPanelFits(app, dpRatio);
	}
}

TEST_CASE("a new transport panel takes the width of the project it shows", "[ui]")
{
	// The locator and range displays grow with their text: in timecode, with the locators an hour out, the sections are
	// wider than in bars and beats from the project start, the defaults the controls show before the engine answers.
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	app.call("project.set", {{"timeFormat", "timecode"}});
	app.call("transport.set", {{"left", int64_t{48000} * 3600}, {"right", int64_t{48000} * 7200}});
	checkPanelFits(
		app, 0.5F, [&app] { return shows(app, "#left-locator", app.appUi().transportPanel()->id()) == "01:00:00:00"; });
}

TEST_CASE("a transport panel closed right after its fit comes back as wide", "[ui]")
{
	// The fit keeps the bounds at once, not on the docking system's next save: closed and opened again, the panel has
	// the width of the project it shows, wider than the controls' defaults.
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	app.call("project.set", {{"timeFormat", "timecode"}});
	app.call("transport.set", {{"left", int64_t{48000} * 3600}, {"right", int64_t{48000} * 7200}});
	checkPanelFits(
		app, 0.5F,
		[&app]
		{
			const ui::Window* panel = app.appUi().transportPanel();
			return panel != nullptr && shows(app, "#left-locator", panel->id()) == "01:00:00:00";
		},
		true);
}

namespace
{
	// The transport panel's width in its bounds, once it is open.
	int panelWidth(test::UiApp& app)
	{
		const ui::Window* panel = app.appUi().transportPanel();
		return panel != nullptr ? app.shell().bounds(panel->id()).width : 0;
	}
}

TEST_CASE("an open transport panel follows the width of its sections until the user sizes it", "[ui]")
{
	// In bars and beats from the project start the sections are narrower than in timecode with the locators an hour
	// out; at a dp ratio of 0.5 both fit CI's 1024-pixel monitor.
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	checkPanelFits(app, 0.5F);
	const int narrow = panelWidth(app);
	const int left = app.shell().bounds(app.appUi().transportPanel()->id()).x;

	// A time format picked later and the locators moved far out: the panel grows where it is.
	app.call("project.set", {{"timeFormat", "timecode"}});
	app.call("transport.set", {{"left", int64_t{48000} * 3600}, {"right", int64_t{48000} * 7200}});
	REQUIRE(app.waitUntil([&] { return panelWidth(app) > narrow; }, std::chrono::seconds(2)));
	const int wide = panelWidth(app);
	CHECK(app.shell().bounds(app.appUi().transportPanel()->id()).x == left);
	// Closed and opened again, it has the width it had and still follows its sections.
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() == nullptr);
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.waitUntil([&] { return panelWidth(app) == wide; }, std::chrono::seconds(2)));

	// A section hidden in the setup pane makes it narrower, and shown again wider.
	const auto hidden = [&app](const Json& sections)
	{
		Json layout = app.call("settings.get", {{"key", "ui.layout"}}).at("value");
		layout["windows"]["transportPanel"]["sections"]["transport"]["hidden"] = sections;
		app.call("settings.set", {{"key", "ui.layout"}, {"value", layout}});
	};
	hidden(Json::array({"click"}));
	REQUIRE(app.waitUntil([&] { return panelWidth(app) < wide; }, std::chrono::seconds(2)));
	hidden(Json::array());
	REQUIRE(app.waitUntil([&] { return panelWidth(app) == wide; }, std::chrono::seconds(2)));

	// Sized by the user, it keeps its width: back in bars and beats it doesn't get narrower, also when it opens again.
	ui::Rect sized = app.shell().bounds(app.appUi().transportPanel()->id());
	sized.width = wide + 40;
	app.shell().setBounds(app.appUi().transportPanel()->id(), sized);
	app.call("project.set", {{"timeFormat", "barsBeats"}});
	app.call("transport.set", {{"left", 0}, {"right", int64_t{48000} * 8}});
	REQUIRE(
		app.waitUntil([&app] { return shows(app, "#left-locator", app.appUi().transportPanel()->id()) == "1.1.1.0"; },
			std::chrono::seconds(2)));
	const auto settle = [&app] { app.waitUntil([] { return false; }, std::chrono::milliseconds(300)); };
	settle();
	CHECK(panelWidth(app) == wide + 40);
	app.ui().key(std::nullopt, "F2");
	app.ui().key(std::nullopt, "F2");
	settle();
	CHECK(panelWidth(app) == wide + 40);
}

namespace
{
	// Keeps the engine's command thread busy until it goes: what the UI sends meanwhile is answered later, as by a
	// command thread busy with a long command.
	class BusyEngine
	{
	public:
		explicit BusyEngine(engine::Engine& engine)
		{
			engine.post("engine.version", Json::object(),
				[released = m_release.get_future().share()](const engine::Result&) { released.wait(); });
		}
		~BusyEngine() { m_release.set_value(); }
		BusyEngine(const BusyEngine&) = delete;
		BusyEngine& operator=(const BusyEngine&) = delete;
		BusyEngine(BusyEngine&&) = delete;
		BusyEngine& operator=(BusyEngine&&) = delete;

	private:
		std::promise<void> m_release;
	};

	// A project whose transport controls are wider than their defaults: timecode, with the locators an hour out.
	void wideProject(test::UiApp& app)
	{
		app.call("project.set", {{"timeFormat", "timecode"}});
		app.call("transport.set", {{"left", int64_t{48000} * 3600}, {"right", int64_t{48000} * 7200}});
	}

	bool showsWideProject(test::UiApp& app)
	{
		const ui::Window* panel = app.appUi().transportPanel();
		return panel != nullptr && shows(app, "#left-locator", panel->id()) == "01:00:00:00";
	}

	// Steps the shell for a while, adding the transport panel's width after every step.
	void recordWidths(test::UiApp& app, std::vector<int>& widths, std::chrono::milliseconds time)
	{
		app.waitUntil(
			[&]
			{
				widths.push_back(panelWidth(app));
				return false;
			},
			time);
	}
}

TEST_CASE("a fitted transport panel opens as wide as it was, also while the engine is busy", "[ui]")
{
	// Until the engine answers, the controls show their defaults, narrower than the project's: the panel follows its
	// sections only once they show the project, so it doesn't get narrower and then as wide as it was (#1612).
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	wideProject(app);
	checkPanelFits(app, 0.5F, [&app] { return showsWideProject(app); });
	const int wide = panelWidth(app);
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() == nullptr);
	std::vector<int> widths;
	{
		const BusyEngine busy(app.engine());
		app.ui().key(std::nullopt, "F2");
		recordWidths(app, widths, std::chrono::milliseconds(200));
		CHECK_FALSE(showsWideProject(app));
	}
	REQUIRE(app.waitUntil(
		[&]
		{
			widths.push_back(panelWidth(app));
			return showsWideProject(app);
		}));
	recordWidths(app, widths, std::chrono::milliseconds(300));
	for (const int width : widths)
		CHECK(width == wide);
}

TEST_CASE("a new transport panel is centred for the project it shows, also when the engine answers late", "[ui]")
{
	// Fitted to the controls' defaults at once, centred, and once they show the project centred again for its width.
	test::UiApp app({1024, 700});
	app.setDpRatio(0.5F);
	app.appUi().showProject();
	wideProject(app);
	{
		const BusyEngine busy(app.engine());
		app.ui().key(std::nullopt, "F2");
		REQUIRE(app.appUi().transportPanel() != nullptr);
		app.shell().step(std::chrono::milliseconds(0));
	}
	const int defaults = panelWidth(app);
	REQUIRE(app.waitUntil([&] { return showsWideProject(app) && panelWidth(app) > defaults; }));
	const ui::Rect bounds = app.shell().bounds(app.appUi().transportPanel()->id());
	CHECK(bounds == ui::centred(bounds.size(), app.shell().displays().front()));
}

TEST_CASE("each fit of the transport panel saves the window layout once", "[ui]")
{
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	checkPanelFits(app, 0.5F);
	REQUIRE(app.waitUntil([&app]
		{ return app.call("settings.get", {{"key", "ui.layout"}}).at("value") == app.appUi().docking().layout(); }));
	int saves = 0;
	const ui::Subscription counting = app.appUi().binding().on("settings.changed",
		[&saves](const engine::Event& event)
		{
			if (event.data.at("values").contains("ui.layout"))
				++saves;
		});
	int fits = 0;
	int last = panelWidth(app);
	wideProject(app);
	app.waitUntil(
		[&]
		{
			if (const int width = panelWidth(app); width != last)
			{
				++fits;
				last = width;
			}
			return false;
		},
		std::chrono::seconds(1));
	REQUIRE(fits > 0);
	CHECK(saves == fits);
}

TEST_CASE("a transport panel keeps a width set right before its window's close button", "[ui]")
{
	// The close request saves the bounds before the window goes, not only once the shell closed it (#1612).
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	checkPanelFits(app, 0.5F);
	const ui::WindowId id = app.appUi().transportPanel()->id();
	ui::Rect sized = app.shell().bounds(id);
	sized.width += 40;
	app.shell().setBounds(id, sized);
	app.shell().requestClose(id);
	REQUIRE(app.appUi().transportPanel() == nullptr);
	app.ui().key(std::nullopt, "F2");
	std::vector<int> widths;
	recordWidths(app, widths, std::chrono::milliseconds(300));
	for (const int width : widths)
		CHECK(width == sized.width);
}

TEST_CASE("Reset All in its setup pane makes a transport panel the user sized follow its sections again", "[ui]")
{
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	checkPanelFits(app, 0.5F);
	const int fitted = panelWidth(app);
	const ui::WindowId id = app.appUi().transportPanel()->id();
	ui::Rect sized = app.shell().bounds(id);
	sized.width += 40;
	app.shell().setBounds(id, sized);
	std::vector<int> widths;
	recordWidths(app, widths, std::chrono::milliseconds(300));
	CHECK(panelWidth(app) == sized.width); // sized by the user

	// As the user does: Reset All in the setup pane that a right click on a section of the panel opens.
	app.ui().click(id, at("[section=click]"), ui::MouseButton::Right, 1, {});
	click(app, "#transport-setup [setup-reset]", id);
	REQUIRE(app.waitUntil([&] { return panelWidth(app) == fitted; }, std::chrono::seconds(2)));
	// And it follows from there: wider for a wider project.
	wideProject(app);
	REQUIRE(app.waitUntil([&] { return panelWidth(app) > fitted; }, std::chrono::seconds(2)));
}

namespace
{
	// The transport panel's fit (ui/transportpanelfit.h) on the fake window system, with a monitor 1920 points wide
	// at a scale of 1, and a panel of two sections of 300 dp, until the second one's class makes it wider: wide
	// 600 dp, wider 900 dp, and huge 2500 dp, wider than the monitor.
	struct FakePanel
	{
		engine::Engine engine;
		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1040}, 1.0F}}};
		ui::WindowId main = ui.shell.open({}).id();
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		ui::Docking docking{
			engine, binding, ui.shell, std::filesystem::path(GNUBASE_RESOURCES_DIR) / "ui" / "panel-window.rml"};
		ui::TransportPanelFit fit{ui.shell, docking};
		ui::Window* panel = nullptr;
		Rml::Element* second = nullptr;

		// Opens a new panel as the app does, 1100 points wide in the middle of the monitor, and fits it at once; with
		// its sections hidden, which the class shown on the body shows, if not shown.
		void open(bool shown = true)
		{
			ui::WindowSpec spec;
			spec.size = {1100, 48};
			panel = &ui.shell.open(spec);
			Rml::ElementDocument* document = panel->context().LoadDocumentFromMemory(R"(<rml><head><style>
				body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; }
				.transport-section { position: absolute; top: 0; width: 300dp; height: 20dp; display: none; }
				body.shown .transport-section { display: block; }
				#second { left: 300dp; }
				#second.wide { width: 600dp; }
				#second.wider { width: 900dp; }
				#second.huge { width: 2500dp; }
				</style></head><body><div class="transport-section"></div><div class="transport-section" id="second">
				</div></body></rml>)");
			REQUIRE(document != nullptr);
			document->Show();
			document->SetClass("shown", shown);
			second = document->GetElementById("second");
			fit.open(*panel, *document, true);
		}
		// The second section takes another width, as a display of the controls does with its text, and the shell
		// steps.
		void resize(const std::string& width)
		{
			second->SetClassNames("transport-section " + width);
			panel->context().RequestNextUpdate(0);
			step();
		}
		void step() { ui.shell.step(std::chrono::milliseconds(0)); }
		ui::Rect bounds() const { return ui.shell.bounds(panel->id()); }
		// A panel of that width in the middle of the monitor.
		ui::Rect centred(int width) const { return ui::centred({width, 48}, ui.shell.displays().front()); }
	};
}

TEST_CASE(
	"a new transport panel is centred for what it shows also where the window system moves windows later", "[unit]")
{
	// As SDL's on X11, the window system moves the panel only after the first fit looked at its bounds again (#1701).
	FakePanel fake;
	fake.ui.windows.setBoundsLater(true);
	fake.open();
	CHECK(fake.bounds() == fake.centred(1100));
	fake.ui.windows.applyBounds();
	fake.step();
	REQUIRE(fake.bounds() == fake.centred(600));

	// The controls show the project, which is wider: centred again, where the first fit put it.
	fake.second->SetClass("wide", true);
	fake.fit.follow();
	fake.step();
	fake.ui.windows.applyBounds();
	fake.step();
	CHECK(fake.bounds() == fake.centred(900));
}

TEST_CASE("a new transport panel is centred for what it shows also before the window system moved it", "[unit]")
{
	// The controls show the project before the window system gave the panel the first fit's bounds (#1765).
	FakePanel fake;
	fake.ui.windows.setBoundsLater(true);
	fake.open();
	REQUIRE(fake.bounds() == fake.centred(1100));
	fake.second->SetClass("wide", true);
	fake.fit.follow();
	fake.step();
	CHECK(fake.bounds() == fake.centred(1100));
	fake.ui.windows.applyBounds();
	fake.step();
	CHECK(fake.bounds() == fake.centred(900));
}

TEST_CASE("a new transport panel whose first fit found no sections is centred where it opened, not where the last one "
		  "was",
	"[unit]")
{
	// The panel before was fitted in the middle of the monitor, which then got smaller.
	FakePanel fake;
	fake.open();
	REQUIRE(fake.bounds() == fake.centred(600));
	fake.ui.windows.setDisplays({{1, "Monitor", {0, 0, 1600, 900}, {0, 0, 1600, 860}, 1.0F}});
	fake.step();

	// A new one shows no sections at first: nothing to fit (#1765). It follows them from the width it opened with,
	// not the fit before's (#1848).
	fake.open(false);
	REQUIRE(fake.bounds() == fake.centred(1100));
	fake.panel->context().GetDocument(0)->SetClass("shown", true);
	fake.second->SetClass("wide", true);
	fake.fit.follow();
	fake.step();
	CHECK(fake.bounds() == fake.centred(900));
}

TEST_CASE("a new transport panel whose first fit found no sections follows them, also with a fresh layout", "[unit]")
{
	// No fit before it, so ui.layout has no fittedWidth (#1848).
	FakePanel fake;
	fake.open(false);
	REQUIRE(fake.bounds() == fake.centred(1100));
	CHECK(fake.docking.fittedWidth("transportPanel") == 1100);
	fake.panel->context().GetDocument(0)->SetClass("shown", true);
	fake.fit.follow();
	fake.step();
	CHECK(fake.bounds() == fake.centred(600));
	fake.resize("wide");
	CHECK(fake.bounds().width == 900);
}

TEST_CASE("a new transport panel whose first fit asked for nothing and that the user moved to the monitor's corner "
		  "stays there",
	"[unit]")
{
	// No fit asked for bounds, so none match the corner the user moved it to before the controls showed the project
	// (#1848).
	FakePanel fake;
	fake.open(false);
	fake.ui.shell.setBounds(fake.panel->id(), {0, 0, 1100, 48});
	fake.step();
	fake.panel->context().GetDocument(0)->SetClass("shown", true);
	fake.fit.follow();
	fake.step();
	CHECK(fake.bounds() == ui::Rect{0, 0, 600, 48});
}

TEST_CASE("the shell leaves a transport panel's fit alone once it is gone", "[unit]")
{
	static_assert(
		!std::is_copy_constructible_v<ui::TransportPanelFit> && !std::is_move_constructible_v<ui::TransportPanelFit>);
	FakePanel fake;
	fake.open();
	const ui::Rect fitted = fake.bounds();
	{
		ui::TransportPanelFit gone{fake.ui.shell, fake.docking};
		gone.open(*fake.panel, *fake.panel->context().GetDocument(0), false);
		gone.follow();
	}
	// Its tasks stay with the shell, which updates the panel and finds other monitors: they do nothing now (#1765).
	fake.second->SetClass("wide", true);
	fake.panel->context().RequestNextUpdate(0);
	fake.step();
	fake.ui.windows.setDisplays({{1, "Monitor", {0, 0, 1600, 900}, {0, 0, 1600, 860}, 1.0F}});
	fake.step();
	CHECK(fake.bounds() == fitted);
}

TEST_CASE(
	"a transport panel follows its sections while the window system hasn't given it the last fit's width", "[unit]")
{
	FakePanel fake;
	fake.open();
	fake.fit.follow();
	fake.step();
	REQUIRE(fake.bounds().width == 600);

	// The window keeps its width for a while after each fit, and the sections get wider twice meanwhile.
	fake.ui.windows.setBoundsLater(true);
	fake.resize("wide");
	CHECK(fake.bounds().width == 600);
	fake.resize("wider");
	CHECK(fake.bounds().width == 600);
	fake.ui.windows.applyBounds();
	fake.step();
	CHECK(fake.bounds().width == 1200);
	CHECK(fake.docking.fittedWidth("transportPanel") == 1200);
}

TEST_CASE("an open transport panel asks for the monitors only when its sections or its bounds change", "[unit]")
{
	FakePanel fake;
	fake.open();
	fake.fit.follow();
	fake.step();
	REQUIRE(fake.bounds().width == 600);

	// In every frame while the transport runs, the panel's context updates with the same sections.
	const int asked = fake.ui.windows.displaysAsked();
	constexpr int frames = 40;
	for (int frame = 0; frame < frames; ++frame)
	{
		fake.panel->context().RequestNextUpdate(0);
		fake.step();
	}
	// The docking system looks for moved windows twice a second, which asks for them too.
	CHECK(fake.ui.windows.displaysAsked() - asked < frames / 4);

	const int unchanged = fake.ui.windows.displaysAsked();
	fake.resize("wide");
	CHECK(fake.ui.windows.displaysAsked() > unchanged);
	CHECK(fake.bounds().width == 900);
}

TEST_CASE("a transport panel as wide as its monitor gets narrower with the monitor's usable area", "[unit]")
{
	FakePanel fake;
	fake.open();
	fake.second->SetClass("huge", true);
	fake.fit.follow();
	fake.step();
	REQUIRE(fake.bounds().width == 1920);

	// Another resolution: the panel is still within reach, so the shell leaves it where it is, and it fits again.
	fake.ui.windows.setDisplays({{1, "Monitor", {0, 0, 1600, 900}, {0, 0, 1600, 860}, 1.0F}});
	fake.step();
	CHECK(fake.bounds().width == 1600);
	CHECK(fake.docking.fittedWidth("transportPanel") == 1600);
}

TEST_CASE("a transport panel that the shell moved onto the primary monitor follows its sections from there", "[unit]")
{
	// The panel is on a second monitor, wider than the primary one, as wide as that monitor's usable area.
	FakePanel fake;
	const ui::Display primary{1, "Primary", {0, 0, 1600, 900}, {0, 0, 1600, 860}, 1.0F};
	fake.ui.windows.setDisplays({primary, {2, "Wide", {1600, 0, 2560, 1440}, {1600, 0, 2560, 1400}, 1.0F}});
	fake.step();
	fake.open();
	fake.second->SetClass("huge", true);
	fake.fit.follow();
	fake.step();
	REQUIRE(fake.bounds().width == 1600);
	fake.ui.shell.setBounds(fake.panel->id(), {1700, 100, 1600, 48});
	fake.panel->context().RequestNextUpdate(0);
	fake.step();
	REQUIRE(fake.bounds() == ui::Rect{1600, 100, 2560, 48});

	// That monitor goes: the shell centres the panel on the primary one, as wide as its usable area, and the panel
	// follows its sections on from that width (#1798), also where the window system sizes windows later.
	const bool later = GENERATE(false, true);
	INFO((later ? "sized later" : "sized at once"));
	fake.ui.windows.setBoundsLater(later);
	fake.ui.windows.setDisplays({primary});
	fake.step();
	fake.ui.windows.applyBounds();
	fake.panel->context().RequestNextUpdate(0);
	fake.step();
	REQUIRE(fake.bounds() == ui::Rect{0, 406, 1600, 48});
	CHECK(fake.docking.fittedWidth("transportPanel") == 1600);
	fake.resize("wide");
	fake.ui.windows.applyBounds();
	fake.step();
	CHECK(fake.bounds() == ui::Rect{0, 406, 900, 48});
}

TEST_CASE("the transport bar goes to markers, the project's ends and the selection", "[ui]")
{
	test::UiApp app({1024, 700});
	ui::EventDisplay& display = showBar(app);
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	const Json part =
		app.call("midiPart.add", {{"track", track.at("id")}, {"position", 4 * g_quarter}, {"length", 4 * g_quarter}})
			.at("part");
	app.call("note.add",
		{{"part", part.at("id")},
			{"notes", {{{"position", 3 * g_quarter}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 1.0}}}}});

	// Without markers: to zero and to the project end, the end of the note at 4 seconds.
	click(app, "#next-marker");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 192000; }));
	click(app, "#previous-marker");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));

	app.call("marker.add", {{"position", 2 * g_quarter}});
	click(app, "#next-marker");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
	click(app, "#next-marker");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 192000; }));
	click(app, "#previous-marker");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));

	// Locators to Selection: around the selected part, 2 to 4 seconds, with the button and with P in the project.
	const auto partShown = [&app]
	{
		Rml::Context& context = app.main().context();
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (auto* lanes =
					dynamic_cast<ui::EventLanes*>(context.GetDocument(i)->QuerySelector("event-lanes.events")))
				return std::any_of(lanes->lanes().begin(), lanes->lanes().end(),
					[](const ui::EventLanes::Lane& lane) { return !lane.events.empty(); });
		return false;
	};
	REQUIRE(app.waitUntil(partShown));
	display.select({part.at("id").get<uint64_t>()});
	click(app, "#locators-to-selection");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 96000 && state.at("right") == 192000; }));
	app.call("transport.set", {{"left", 0}, {"right", 0}});
	app.ui().click(std::nullopt, at("#event-display"), ui::MouseButton::Left, 1, {}); // the focus zone "project"
	display.select({part.at("id").get<uint64_t>()});
	app.ui().key(std::nullopt, "P");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 96000 && state.at("right") == 192000; }));
}

TEST_CASE("the time displays show the cursor in their formats and take positions typed into them", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("transport.locate", {{"position", 72000}}); // 1.5 seconds
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.4.1.0"; }));
	CHECK(shows(app, "#secondary-time") == "00:00:01:12");

	// Leaving a display without typing changes nothing, also at a position off its rounded value.
	app.call("transport.locate", {{"position", 72001}});
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 72001; }));
	app.ui().click(std::nullopt, at("#primary-time"), ui::MouseButton::Left, 2, {});
	CHECK(element(app, "#primary-time").value("classes", Json::array()).dump().find("editing") != std::string::npos);
	click(app, "#signature");
	app.ui().click(std::nullopt, at("#left-locator"), ui::MouseButton::Left, 2, {});
	app.ui().key(std::nullopt, "Enter");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(transport(app).at("position") == 72001);
	CHECK(transport(app).at("left") == 0);

	// Typing the position it shows moves the cursor onto it.
	type(app, "#primary-time", "1.4.1.0");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 72000; }));

	// The format menus: the primary format is the project's time format (reference manual p. 317), the secondary one
	// is saved in ui.timeFormats.
	click(app, "#primary-format");
	click(app, "menuitem[slot=primary][format=seconds]");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));
	CHECK(element(app, "menuitem[slot=primary][format=seconds]").at("attributes").contains("checked"));
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("timeFormat") == "seconds"; }));
	CHECK(shows(app, "#left-locator") == "0:00:00.000"); // the locators show the primary format

	// Exchange Time Formats, with its button and its key.
	click(app, "#exchange-formats");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "00:00:01:12"; }));
	CHECK(shows(app, "#secondary-time") == "0:00:01.500");
	app.ui().key(std::nullopt, ".");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));

	// The secondary format menu.
	click(app, "#secondary-format");
	click(app, "menuitem[slot=secondary][format=barsBeats]");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "1.4.1.0"; }));
	CHECK(element(app, "menuitem[slot=secondary][format=barsBeats]").at("attributes").contains("checked"));
	CHECK_FALSE(element(app, "menuitem[slot=secondary][format=timecode]").at("attributes").contains("checked"));
	REQUIRE(app.waitUntil([&app]
		{ return app.call("settings.get", {{"key", "ui.timeFormats"}}).at("value").at("secondary") == "barsBeats"; }));
	CHECK(shows(app, "#primary-time") == "0:00:01.500");

	// Another client's change of the setting and of the project's time format; a primary format in the setting, as
	// before #537, is the project's.
	app.call("settings.set",
		{{"key", "ui.timeFormats"},
			{"value", {{"primary", "timecode"}, {"secondary", "samples"}, {"timeDisplay", "barsBeats"}}}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "72000"; }));
	CHECK(shows(app, "#primary-time") == "0:00:01.500");
	app.call("project.set", {{"timeFormat", "barsBeats"}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.4.1.0"; }));
	app.call("history.undo"); // the project's time format is a project setting, undone as one
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));
	app.call("history.redo");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.4.1.0"; }));

	// Positions typed in the display's format.
	type(app, "#primary-time", "3");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 192000; }));
	type(app, "#secondary-time", "24000");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 24000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.2.1.0"; }));
	type(app, "#left-locator", "2");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 96000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#left-locator") == "2.1.1.0"; }));
	type(app, "#right-locator", "4.3");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == 336000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#right-locator") == "4.3.1.0"; }));
	type(app, "#locator-range", "1");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == 192000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#locator-range") == "1.0.0.0"; }));

	// Text that isn't a position changes nothing and goes.
	type(app, "#left-locator", "soon");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(transport(app).at("left") == 96000);
	CHECK(shows(app, "#left-locator") == "2.1.1.0");
}

TEST_CASE("the time displays keep their width while the cursor moves, and their format's width after a change, in "
		  "every theme",
	"[ui][slow]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	Rml::Context& context = app.main().context();
	const auto rml = [&context](const char* id)
	{
		Rml::Element* found = nullptr;
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (Rml::Element* in = context.GetDocument(i)->GetElementById(id))
				found = in;
		REQUIRE(found != nullptr);
		return found;
	};
	const auto width = [&app](const char* selector)
	{ return element(app, selector).at("box").at("width").get<double>(); };
	const auto x = [&app](const char* selector) { return element(app, selector).at("box").at("x").get<double>(); };
	// The widest texts the widths hold (base.rcss, #1379): a signature with two-digit beats and sixteenths, and bars
	// from -999 on.
	app.call("signature.add", {{"bar", 0}, {"numerator", 99}, {"denominator", 1}});
	app.call("project.set", {{"barOffset", -1000}});
	const ui::TimeContext time = ui::projectTime(app.call("project.get")); // 120 BPM at 48 kHz and 25 fps
	REQUIRE(time.barOffset == -1000);
	const int64_t bar = 9504000; // 99 whole notes at 120 BPM
	// Positions whose texts have more and fewer digits (#1335), and each format's widest (#1379): bar -999 and 9999,
	// just under 100 hours and 11 digits of samples.
	const std::vector<int64_t> common = {0, 1234, 72001, 431999, 863999, 864000, 2000000, 95999999};
	const std::vector<std::pair<const char*, std::pair<std::vector<int64_t>, size_t>>> formats = {
		{"barsBeats", {{bar - 100, (10999 * bar) - 100}, 14}},                  // -999.99.16.118, 9999.99.16.118
		{"seconds", {{17279999952}, 12}},                                       // 99:59:59.999
		{"timecode", {{17279999000}, 11}},                                      // 99:59:59:24
		{"samples", {{99999999999}, 11}}, {"userFrames", {{17279999000}, 11}}}; // 99:59:59:29
	// RmlUi cuts the font size to whole pixels before it asks for the face, and FreeType rounds the hinted advances: at
	// a dp ratio of 1 a 13 dp digit is 8 px, at 0.85 an 11 px face's is 7 px of 11.05, the most a character takes of
	// its em (#1379).
	for (const float dpRatio : {1.0F, 0.85F})
		for (const std::string& theme : test::themes(GNUBASE_RESOURCES_DIR))
		{
			INFO("dp ratio " << dpRatio << ", theme " << theme);
			app.setDpRatio(dpRatio);
			app.call("settings.set", {{"key", "ui.theme"}, {"value", theme}});
			// The shell applies the setting in a task, and a name it has no theme for would keep the default (#1424).
			REQUIRE(app.waitUntil([&app, theme] { return app.shell().theme() == theme; }));
			app.shell().step(std::chrono::milliseconds(0));
			// The default theme's primary display is larger than the secondary, the others' as large (#1342).
			const float primarySize = rml("primary-time")->GetComputedValues().font_size();
			const float secondarySize = rml("secondary-time")->GetComputedValues().font_size();
			if (std::string_view(theme) == "gnubase")
				CHECK(primarySize > secondarySize);
			else
				CHECK(primarySize == secondarySize);
			// The cursor's displays are as high as the transport's other displays, the larger primary display higher.
			const float height = rml("signature")->GetOffsetHeight();
			CHECK(rml("left-locator")->GetOffsetHeight() == height);
			CHECK(rml("secondary-time")->GetOffsetHeight() == height);
			CHECK(rml("primary-time")->GetOffsetHeight() >= height);
			for (const auto& [name, widest] : formats)
			{
				INFO("format " << name);
				const std::optional<ui::TimeFormat> format = ui::timeFormatFromName(name);
				REQUIRE(format.has_value());
				const ui::TimeFormat displayed = format.value_or(ui::TimeFormat::BarsBeats);
				// A format change may change the width.
				app.call("project.set", {{"timeFormat", name}});
				app.call("settings.set",
					{{"key", "ui.timeFormats"},
						{"value", {{"primary", name}, {"secondary", name}, {"timeDisplay", "barsBeats"}}}});
				REQUIRE(app.waitUntil(
					[&, name = name]
					{
						return element(app, "#secondary-time")
								   .value("classes", Json::array())
								   .dump()
								   .find(std::string("format-") + name) != std::string::npos;
					}));
				std::vector<int64_t> positions = common;
				positions.insert(positions.end(), widest.first.begin(), widest.first.end());
				std::array<double, 4> first{};
				bool measured = false;
				size_t longest = 0;
				for (const int64_t position : positions)
				{
					INFO("position " << position);
					app.call("transport.locate", {{"position", position}});
					const std::string text =
						ui::formatTime(displayed, static_cast<double>(position) / time.sampleRate, time);
					longest = std::max(longest, text.size());
					REQUIRE(app.waitUntil(
						[&] { return shows(app, "#primary-time") == text && shows(app, "#secondary-time") == text; }));
					app.shell().step(std::chrono::milliseconds(0));
					// The displays and the controls to their right stay where they are, and the text fits.
					const std::array<double, 4> now = {
						width("#primary-time"), width("#secondary-time"), x("#exchange-formats"), x("#tempo")};
					if (!measured)
						first = now;
					measured = true;
					CHECK(now == first);
					// The text, measured in the display's font, fits its content box, its line too: the box's height
					// follows the font size (#1342).
					for (const char* id : {"primary-time", "secondary-time"})
					{
						Rml::Element* display = rml(id);
						const Rml::Vector2f content = display->GetBox().GetSize(Rml::BoxArea::Content);
						CHECK(static_cast<float>(Rml::ElementUtilities::GetStringWidth(display, text)) <=
							content.x + 0.5F);
						CHECK(display->GetLineHeight() <= content.y + 0.5F);
					}
				}
				CHECK(longest == widest.second); // the widest text the width holds was shown
			}
		}
}

TEST_CASE("the transport bar and the transport panel show the same time formats", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() != nullptr);
	const ui::WindowId panel = app.appUi().transportPanel()->id();
	app.call("transport.locate", {{"position", 72000}}); // 1.5 seconds
	REQUIRE(app.waitUntil([&app, panel] { return shows(app, "#secondary-time", panel) == "00:00:01:12"; }));

	// A format picked in the panel, then exchanged twice with the key, which the bar's controls run.
	click(app, "#primary-format", panel);
	click(app, "menuitem[slot=primary][format=seconds]", panel);
	REQUIRE(app.waitUntil([&app, panel] { return shows(app, "#primary-time", panel) == "0:00:01.500"; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));
	app.ui().key(std::nullopt, ".");
	REQUIRE(app.waitUntil(
		[&app, panel]
		{
			return shows(app, "#primary-time", panel) == "00:00:01:12" &&
				shows(app, "#primary-time") == "00:00:01:12" && shows(app, "#secondary-time", panel) == "0:00:01.500" &&
				shows(app, "#secondary-time") == "0:00:01.500";
		}));
	app.ui().key(std::nullopt, ".");
	// Primary and secondary changes have separate event echoes; wait for both in both windows.
	REQUIRE(app.waitUntil(
		[&app, panel]
		{
			return shows(app, "#primary-time", panel) == "0:00:01.500" &&
				shows(app, "#primary-time") == "0:00:01.500" && shows(app, "#secondary-time", panel) == "00:00:01:12" &&
				shows(app, "#secondary-time") == "00:00:01:12";
		}));
	CHECK(shows(app, "#secondary-time", panel) == "00:00:01:12");
	CHECK(element(app, "menuitem[slot=primary][format=seconds]", panel).at("attributes").contains("checked"));
	CHECK(element(app, "menuitem[slot=secondary][format=timecode]", panel).at("attributes").contains("checked"));
	CHECK_FALSE(element(app, "menuitem[slot=primary][format=timecode]", panel).at("attributes").contains("checked"));
	CHECK(shows(app, "#primary-time") == "0:00:01.500");
	CHECK(shows(app, "#secondary-time") == "00:00:01:12");
	CHECK(app.call("project.get").at("timeFormat") == "seconds");
	const Json formats = app.call("settings.get", {{"key", "ui.timeFormats"}}).at("value");
	CHECK(!formats.contains("primary"));
	CHECK(formats.at("secondary") == "timecode");
}

TEST_CASE("transport time format choices and exchange do not add undo steps", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("transport.locate", {{"position", 72000}});
	app.call("project.set", {{"tempo", 130.0}});
	const size_t before = app.call("history.get").at("steps").size();

	click(app, "#primary-format");
	click(app, "menuitem[slot=primary][format=seconds]");
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.500"; }));
	CHECK(app.call("history.get").at("steps").size() == before);

	click(app, "#exchange-formats");
	REQUIRE(app.waitUntil([&app]
		{ return shows(app, "#primary-time") == "00:00:01:12" && shows(app, "#secondary-time") == "0:00:01.500"; }));
	CHECK(shows(app, "#secondary-time") == "0:00:01.500");
	CHECK(app.call("history.get").at("steps").size() == before);

	// Undo reaches the earlier tempo edit, leaving both exchanged display formats in place.
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo")[0].at("bpm") == 120.0; }));
	CHECK(shows(app, "#primary-time") == "00:00:01:12");
	CHECK(shows(app, "#secondary-time") == "0:00:01.500");
	CHECK(app.call("project.get").at("timeFormat") == "timecode");
	app.call("history.redo");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo")[0].at("bpm") == 130.0; }));
	CHECK(shows(app, "#primary-time") == "00:00:01:12");
	CHECK(shows(app, "#secondary-time") == "0:00:01.500");
}

TEST_CASE("the tempo and signature fields set the project's first tempo and signature", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	REQUIRE(app.waitUntil([&app] { return shows(app, "#signature") == "4/4"; }));
	CHECK(element(app, "#tempo").at("attributes").at("value") == "120");

	type(app, "#tempo", "90");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tempo").at(0).at("bpm") == 90.0; }));
	type(app, "#signature", "3/4");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("signatures").at(0).at("numerator") == 3; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#signature") == "3/4"; }));
	type(app, "#signature", "3/5"); // refused by the engine
	type(app, "#signature", "three");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.call("project.get").at("signatures").at(0).at("denominator") == 4);

	// The displays follow the project's tempo: 1.5 seconds at 90 BPM are 2.25 quarters.
	app.call("transport.locate", {{"position", 72000}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.3.2.0"; }));

	// Undo puts the tempo back, and the field follows.
	app.call("history.undo");
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return element(app, "#tempo").at("attributes").at("value") == "120"; }));
	CHECK(shows(app, "#signature") == "4/4");
}

TEST_CASE("the transport's key commands", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);

	app.ui().key(std::nullopt, "Num/");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("cycle") == true; }));
	app.ui().key(std::nullopt, "C");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("metronome") == true; }));
	app.ui().key(std::nullopt, "I");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("punchIn") == true; }));
	app.ui().key(std::nullopt, "O");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("punchOut") == true; }));
	app.ui().key(std::nullopt, "Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
	app.ui().key(std::nullopt, "Shift+Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 240000; }));
	app.ui().key(std::nullopt, "Shift+Num-");
	app.ui().key(std::nullopt, "Num-");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));
	app.ui().key(std::nullopt, "Num*");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("recording") == true; }));
	app.ui().key(std::nullopt, "Num0");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("playing") == false; }));

	// The enter commands edit the displays of the transport bar.
	app.ui().key(std::nullopt, "Shift+L");
	CHECK(element(app, "#left-locator").value("classes", Json::array()).dump().find("editing") != std::string::npos);
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "3");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 192000; }));
	for (const char* keys : {"Shift+R", "Shift+P", "Shift+D", "Shift+T", "Shift+C"})
	{
		INFO(keys);
		const Json focus = app.ui().key(std::nullopt, keys).at("focus");
		CHECK(focus.at("tag") == "input");
		app.ui().key(std::nullopt, "Escape");
	}
}

TEST_CASE("the transport panel and the time display window", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject(); // the transport bar is hidden by default

	// F2 shows the transport panel, a window of its own with the same controls.
	app.ui().key(std::nullopt, "F2");
	ui::Window* panel = app.appUi().transportPanel();
	REQUIRE(panel != nullptr);
	const ui::WindowId id = panel->id();
	click(app, "#cycle", id);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("cycle") == true; }));
	REQUIRE(app.waitUntil([&app, id] { return checked(app, "#cycle", id); }));
	app.call("transport.locate", {{"position", 96000}});
	REQUIRE(app.waitUntil([&app, id] { return shows(app, "#primary-time", id) == "2.1.1.0"; }));
	app.ui().key(std::nullopt, "F2");
	CHECK(app.appUi().transportPanel() == nullptr);

	// With the transport bar and the panel hidden, an enter command opens the panel to edit there.
	app.ui().key(std::nullopt, "Shift+P");
	panel = app.appUi().transportPanel();
	REQUIRE(panel != nullptr);
	app.ui().key(panel->id(), "Mod+A");
	app.ui().text(panel->id(), "1");
	app.ui().key(panel->id(), "Enter");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));

	// Closing its window closes the panel; F2 opens it again.
	SDL_Event close{};
	close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
	close.window.windowID = panel->id();
	REQUIRE(SDL_PushEvent(&close));
	REQUIRE(app.waitUntil([&app] { return app.appUi().transportPanel() == nullptr; }));
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() != nullptr);

	// The time display window shows the cursor in a format of its own, picked from its context menu.
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	ui::Window* display = app.appUi().timeDisplay();
	REQUIRE(display != nullptr);
	const ui::WindowId window = display->id();
	app.call("transport.locate", {{"position", 72000}});
	INFO(element(app, "#time-display", window).dump());
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "1.4.1.0"; }));
	app.ui().click(window, at("#time-display"), ui::MouseButton::Right, 1, {});
	click(app, "menuitem[format=samples]", window);
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "72000"; }));
	CHECK(app.call("settings.get", {{"key", "ui.timeFormats"}}).at("value").at("timeDisplay") == "samples");
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	CHECK(app.appUi().timeDisplay() == nullptr);
}

namespace
{
	// Steps the shell for a while, e.g. for what the UI sends to arrive.
	void settle(test::UiApp& app, std::chrono::milliseconds time = std::chrono::milliseconds(300))
	{
		app.waitUntil([] { return false; }, time);
	}

	int64_t position(test::UiApp& app)
	{
		return transport(app).at("position").get<int64_t>();
	}

	// Waits until the position stays where it is, and returns it.
	int64_t stopped(test::UiApp& app)
	{
		settle(app);
		const int64_t at = position(app);
		settle(app);
		CHECK(position(app) == at);
		return at;
	}

	size_t undoSteps(test::UiApp& app)
	{
		return app.call("history.get").at("steps").size();
	}

	double tempo(test::UiApp& app)
	{
		return app.call("project.get").at("tempo").at(0).at("bpm").get<double>();
	}

	bool visible(test::UiApp& app, const std::string& selector, std::optional<ui::WindowId> window = std::nullopt)
	{
		return element(app, selector, window).at("visible").get<bool>();
	}

	// Presses or releases the left mouse button where the mouse is in the main window.
	void mouseButton(test::UiApp& app, bool down)
	{
		Rml::Context& context = app.main().context();
		if (down)
			context.ProcessMouseButtonDown(0, 0);
		else
			context.ProcessMouseButtonUp(0, 0);
		app.shell().step(std::chrono::milliseconds(0));
	}
}

TEST_CASE("Rewind and Forward wind while their key or button is held", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);

	// Held, Forward steps a second and then winds on until it is released.
	app.ui().key(std::nullopt, "Num+", ui::KeyAction::Down);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 2 * 48000; }));
	app.ui().key(std::nullopt, "Num+", ui::KeyAction::Up);
	const int64_t wound = stopped(app);

	// Rewind winds back.
	app.ui().key(std::nullopt, "Num-", ui::KeyAction::Down);
	REQUIRE(waitFor(app, [wound](const Json& state) { return state.at("position") < wound - 48000; }));
	app.ui().key(std::nullopt, "Num-", ui::KeyAction::Up);
	stopped(app);

	// Fast Forward winds faster; it stops when its key goes up, also after Shift.
	app.call("transport.locate", {{"position", 0}});
	app.ui().key(std::nullopt, "Shift+Num+", ui::KeyAction::Down);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 8 * 48000; }));
	app.ui().key(std::nullopt, "Num+", ui::KeyAction::Up);
	stopped(app);

	// The buttons wind while the mouse button is held on them.
	app.call("transport.locate", {{"position", 0}});
	app.ui().move(std::nullopt, at("#forward"));
	mouseButton(app, true);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 2 * 48000; }));
	mouseButton(app, false);
	const int64_t forward = stopped(app);
	app.ui().move(std::nullopt, at("#rewind"));
	mouseButton(app, true);
	REQUIRE(waitFor(app, [forward](const Json& state) { return state.at("position") < forward - 48000; }));
	mouseButton(app, false);
	stopped(app);

	// The mouse leaving the button held ends winding, before the mouse button goes up; so does the window losing the
	// keyboard focus.
	app.call("transport.locate", {{"position", 0}});
	app.ui().move(std::nullopt, at("#forward"));
	mouseButton(app, true);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 2 * 48000; }));
	app.ui().move(std::nullopt, at("#primary-time"));
	stopped(app);
	mouseButton(app, false);
	app.ui().move(std::nullopt, at("#forward"));
	mouseButton(app, true);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 4 * 48000; }));
	ui::KeyPress lost;
	lost.focusLost = true;
	app.shell().keyReleased(app.main(), lost);
	stopped(app);
	mouseButton(app, false);

	// A key pressed and released at once steps a second.
	app.call("transport.locate", {{"position", 0}});
	app.ui().key(std::nullopt, "Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
	CHECK(stopped(app) == 48000);

	// Run without a key or button that ends it, as a menu item or a script runs it, it steps once (#566, #569).
	app.call("transport.locate", {{"position", 0}});
	app.keys().run("wind.forward", Json::object(), app.main(), nullptr);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
	CHECK(stopped(app) == 48000);
	app.keys().run("wind.fastRewind", Json::object(), app.main(), nullptr);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));
	CHECK(stopped(app) == 0);
}

TEST_CASE("each held Rewind or Forward key and button winds as its own, and a key RmlUi can't map ends none", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);

	// A key that RmlUi can't map, such as a letter of another layout or a media key, goes up while Num+ is held (#569).
	app.ui().key(std::nullopt, "Num+", ui::KeyAction::Down);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 2 * 48000; }));
	app.shell().keyReleased(app.main(), ui::KeyPress{Rml::Input::KI_UNKNOWN, 0});
	const int64_t unmapped = position(app);
	REQUIRE(waitFor(app, [unmapped](const Json& state) { return state.at("position") > unmapped + 96000; }));

	// Rewind held with the mouse winds back; its button going up leaves Num+ winding on (#566).
	app.ui().move(std::nullopt, at("#rewind"));
	mouseButton(app, true);
	const int64_t forward = position(app);
	REQUIRE(waitFor(app, [forward](const Json& state) { return state.at("position") < forward - 48000; }));
	mouseButton(app, false);
	const int64_t lowest = position(app);
	REQUIRE(waitFor(app, [lowest](const Json& state) { return state.at("position") > lowest + 96000; }));

	// The Forward button, which runs the command that Num+ holds, held and released too: Num+ winds on (#566).
	app.ui().move(std::nullopt, at("#forward"));
	mouseButton(app, true);
	mouseButton(app, false);
	const int64_t both = position(app);
	REQUIRE(waitFor(app, [both](const Json& state) { return state.at("position") > both + 96000; }));
	app.ui().key(std::nullopt, "Num+", ui::KeyAction::Up);
	stopped(app);

	// The window loses the focus while a button is held: the mouse button going up later doesn't end the winding that
	// a key started meanwhile (#566).
	app.call("transport.locate", {{"position", 0}});
	app.ui().move(std::nullopt, at("#forward"));
	mouseButton(app, true);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 2 * 48000; }));
	ui::KeyPress lost;
	lost.focusLost = true;
	app.shell().keyReleased(app.main(), lost);
	stopped(app);
	app.ui().key(std::nullopt, "Num+", ui::KeyAction::Down);
	mouseButton(app, false);
	const int64_t started = position(app);
	REQUIRE(waitFor(app, [started](const Json& state) { return state.at("position") > started + 96000; }));
	app.ui().key(std::nullopt, "Num+", ui::KeyAction::Up);
	stopped(app);
}

TEST_CASE("Nudge Cursor Left and Right move the project cursor by a grid step", "[ui]")
{
	test::UiApp app({1024, 700});
	ui::EventDisplay& display = showBar(app);

	// The grid is on bars by default: 4 quarters at 120 BPM, 96000 samples.
	app.ui().key(std::nullopt, "Mod+Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 96000; }));
	app.ui().key(std::nullopt, "Mod+Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 192000; }));
	app.ui().key(std::nullopt, "Mod+Num-");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 96000; }));

	// On beats, by a beat; never before the project start.
	app.call("snap.set", {{"grid", "beat"}});
	settle(app);
	app.ui().key(std::nullopt, "Mod+Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 120000; }));
	app.call("transport.locate", {{"position", 1000}});
	settle(app);
	app.ui().key(std::nullopt, "Mod+Num-");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));
	app.ui().key(std::nullopt, "Mod+Num-");
	CHECK(stopped(app) == 0);

	// A linear grid: seconds.
	app.call("snap.set", {{"grid", "second"}});
	settle(app);
	app.ui().key(std::nullopt, "Mod+Num+");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));

	// The grid of the ruler's format (#559, #604): samples, 100 by default; seconds, 100 ms; bars and beats again,
	// the beat chosen there before; user frames, a frame at the user's 30 fps, not the project's 25.
	for (const auto& [format, step] :
		{std::pair{ui::TimeFormat::Samples, 100}, std::pair{ui::TimeFormat::Seconds, 4800},
			std::pair{ui::TimeFormat::BarsBeats, 24000}, std::pair{ui::TimeFormat::UserFrames, 1600}})
	{
		INFO(ui::timeFormatName(format));
		display.setRulerFormat(format);
		app.call("transport.locate", {{"position", 0}});
		settle(app);
		app.ui().key(std::nullopt, "Mod+Num+");
		REQUIRE(waitFor(app, [step = step](const Json& state) { return state.at("position") == step; }));
	}
}

TEST_CASE("the mouse wheel changes a display's value", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("transport.set", {{"left", 48000}, {"right", 96000}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#left-locator") == "1.3.1.0"; }));

	// A scroll sideways, as a touchpad or a tilt wheel sends, moves nothing.
	app.ui().wheel(std::nullopt, at("#primary-time"), 1, 0);
	app.ui().wheel(std::nullopt, at("#left-locator"), -1, 0);
	settle(app);
	CHECK(position(app) == 0);
	CHECK(transport(app).at("left") == 48000);

	// Up for later, by a beat in bars and beats.
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 24000; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -2);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 72000; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, 1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));

	// By a frame in timecode (25 fps), a second in seconds; never before the project start.
	app.ui().wheel(std::nullopt, at("#secondary-time"), 0, -1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 49920; }));
	app.call("project.set", {{"timeFormat", "seconds"}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#left-locator") == "0:00:01.000"; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, 3);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));

	// The locators, and the range's length, which moves the right locator.
	app.ui().wheel(std::nullopt, at("#left-locator"), 0, 1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 0; }));
	app.ui().wheel(std::nullopt, at("#right-locator"), 0, -1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == 144000; }));
	app.ui().wheel(std::nullopt, at("#locator-range"), 0, 1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == 96000; }));
}

TEST_CASE("small wheel turns on a display add up to whole steps (#566, #569)", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);

	// A touchpad's smooth scrolling sends small deltas: four quarters of a notch make one beat, not four.
	for (int turn = 0; turn < 3; ++turn)
		app.ui().wheel(std::nullopt, at("#primary-time"), 0, -0.25F);
	settle(app, std::chrono::milliseconds(100));
	CHECK(position(app) == 0);
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -0.25F);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 24000; }));

	// Turns the other way start afresh rather than undo a fraction; the next one makes the step.
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -0.5F);
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, 0.5F);
	settle(app, std::chrono::milliseconds(100));
	CHECK(position(app) == 24000);
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, 0.5F);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));

	// A turn of one and a half steps keeps its half for the next.
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -1.5F);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 24000; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -0.5F);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));

	// The fractions don't carry over to another display.
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -0.5F);
	app.ui().wheel(std::nullopt, at("#left-locator"), 0, -0.5F);
	settle(app, std::chrono::milliseconds(100));
	CHECK(position(app) == 48000);
	CHECK(transport(app).at("left") == 0);

	// A pause forgets the fractions: two halves a second apart make no step.
	settle(app, std::chrono::milliseconds(1100));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -0.5F);
	settle(app, std::chrono::milliseconds(1100));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -0.5F);
	settle(app, std::chrono::milliseconds(100));
	CHECK(position(app) == 48000);
}

namespace
{
	// Three turns of a control that the engine's answers reach in between, as on a busy machine: the second right
	// after the first, and the third once the engine's echo of the first has reached the UI, but before the engine did
	// what the second sent. A control that took the first one's echo as where it is would go on from there, and lose
	// the second step. The engine's command thread holds, so that it goes so on any machine; runs counts the holds it
	// has had (#569).
	void turnsBetweenEchoes(test::UiApp& app, test::CommandHolds& holds, engine::Client& holder, size_t& runs,
		const int& echoes, const std::function<void()>& turn)
	{
		const int before = echoes;
		holder.send("test.hold");
		REQUIRE(holds.waitEntered(runs + 1));
		turn();
		holder.send("test.hold");
		turn();
		holds.release(runs + 1);
		REQUIRE(app.waitUntil([&echoes, before] { return echoes > before; }));
		turn();
		holds.release(runs + 2);
		runs += 2;
	}
}

TEST_CASE(
	"the wheel, Nudge Cursor and the window size go on from what they sent, not from an older echo (#569)", "[ui]")
{
	test::CommandHolds holds; // declared before the app: its engine's command thread waits in hold()
	test::UiApp app({1024, 700});
	const test::ReleaseHolds releaseHolds{holds};
	app.engine().registerCommand(
		{"test.hold", 1, "Waits for the test to release it.", engine::CommandInfo{}.parameters, Json::object(), false},
		[&holds](engine::CommandContext&) { return holds.hold(); });
	engine::Client holder(app.engine(), "holder");
	showBar(app);
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	REQUIRE(app.appUi().timeDisplay() != nullptr);
	const ui::WindowId window = app.appUi().timeDisplay()->id();
	REQUIRE(app.waitUntil([&app, window] { return hasClass(app, "#time-display", "size-1", window); }));
	int transports = 0;
	int displays = 0;
	const ui::Subscription counting =
		app.appUi().binding().on("transport.changed", [&transports](const engine::Event&) { ++transports; });
	const ui::Subscription countingSettings = app.appUi().binding().on("settings.changed",
		[&displays](const engine::Event& event)
		{
			if (event.data.at("values").contains("ui.timeDisplay"))
				++displays;
		});
	size_t runs = 0;

	// The wheel on a time display: three beats.
	turnsBetweenEchoes(
		app, holds, holder, runs, transports, [&app] { app.ui().wheel(std::nullopt, at("#primary-time"), 0, -1); });
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 72000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.4.1.0"; }));

	// Nudge Cursor: three bars.
	app.call("transport.locate", {{"position", 0}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.1.1.0"; }));
	turnsBetweenEchoes(app, holds, holder, runs, transports, [&app] { app.ui().key(std::nullopt, "Mod+Num+"); });
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 288000; }));

	// Increase Window Size: from size 1 to 4.
	turnsBetweenEchoes(app, holds, holder, runs, displays,
		[&app] { app.keys().run("timeDisplay.increaseSize", Json::object(), app.main(), nullptr); });
	CHECK(app.call("settings.get", {{"key", "ui.timeDisplay"}}).at("value").at("size") == 4);
	REQUIRE(app.waitUntil([&app, window] { return hasClass(app, "#time-display", "size-4", window); }));
}

TEST_CASE(
	"the mouse wheel steps a timecode display by a frame at the project's frame rate, also a pull-down one (#576)",
	"[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);

	// From one second on, where each frame rate shows another frame, so that the test knows the display has read the
	// project's. 29.97: a frame is 1.001/30 s, 1601.6 samples at 48 kHz, rounded to a whole sample.
	app.call("transport.locate", {{"position", 48000}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "00:00:01:00"; }));
	app.call("project.set", {{"frameRate", "29.97"}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "00:00:00:29"; }));
	app.ui().wheel(std::nullopt, at("#secondary-time"), 0, -1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000 + 1602; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "00:00:01:00"; }));
	app.ui().wheel(std::nullopt, at("#secondary-time"), 0, 1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "00:00:00:29"; }));

	// The other pull-down rates too, and a rate without it.
	for (const auto& [rate, frames, pullDown] : {std::tuple{"23.976", 24, true}, std::tuple{"24.975", 25, true},
			 std::tuple{"29.97dfps", 30, true}, std::tuple{"59.94", 60, true}, std::tuple{"30", 30, false}})
	{
		INFO(rate);
		const std::string before = shows(app, "#secondary-time");
		app.call("project.set", {{"frameRate", rate}});
		REQUIRE(app.waitUntil([&app, &before] { return shows(app, "#secondary-time") != before; }));
		const std::string home = shows(app, "#secondary-time");
		const int64_t frame = std::llround(48000.0 + (48000.0 * (pullDown ? 1.001 : 1.0) / frames));
		app.ui().wheel(std::nullopt, at("#secondary-time"), 0, -1);
		const bool reached = waitFor(app, [frame](const Json& state) { return state.at("position") == frame; });
		INFO("expected " << frame << ", at " << position(app) << ", display " << shows(app, "#secondary-time"));
		REQUIRE(reached);
		app.call("transport.locate", {{"position", 48000}});
		REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
		REQUIRE(app.waitUntil([&app, &home] { return shows(app, "#secondary-time") == home; }));
	}
}

TEST_CASE("positions typed or wheeled past the largest project position stop there (#457)", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("project.set", {{"timeFormat", "seconds"}});
	app.call("settings.set",
		{{"key", "ui.timeFormats"}, {"value", {{"secondary", "samples"}, {"timeDisplay", "barsBeats"}}}});
	app.call("transport.locate", {{"position", 48000}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#secondary-time") == "48000"; }));
	// The project's time format comes with its own reply, maybe after the setting's.
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "0:00:01.000"; }));
	const std::string maximum = std::to_string(time::g_maxPosition);

	// Fifteen digits of seconds are more samples than 64 bits hold: the cursor goes to the largest position.
	type(app, "#primary-time", "999999999999999");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == time::g_maxPosition; }));
	REQUIRE(app.waitUntil([&app, &maximum] { return shows(app, "#secondary-time") == maximum; }));

	// Past the largest position in samples, which the engine would refuse.
	app.call("transport.locate", {{"position", 48000}});
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 48000; }));
	type(app, "#secondary-time", "999999999999999");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == time::g_maxPosition; }));
	REQUIRE(app.waitUntil([&app, &maximum] { return shows(app, "#secondary-time") == maximum; }));

	// The wheel stops there too, and the next turn goes back from there.
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -1);
	settle(app);
	CHECK(position(app) == time::g_maxPosition);
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, 1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == time::g_maxPosition - 48000; }));

	// Turns far larger than a wheel's, as injected input may send, are a billion steps: a billion seconds here.
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -1e13F);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == time::g_maxPosition; }));
	REQUIRE(app.waitUntil([&app, &maximum] { return shows(app, "#secondary-time") == maximum; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, 1e13F);
	REQUIRE(waitFor(app,
		[](const Json& state) { return state.at("position") == time::g_maxPosition - (int64_t{48000} * 1000000000); }));

	// The locators and the range's length.
	type(app, "#left-locator", "999999999999999");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == time::g_maxPosition; }));
	app.call("transport.set", {{"left", 48000}, {"right", 96000}});
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("left") == 48000; }));
	type(app, "#right-locator", "999999999999999");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == time::g_maxPosition; }));
	app.call("transport.set", {{"right", 96000}});
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == 96000; }));
	type(app, "#locator-range", "999999999999999");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("right") == time::g_maxPosition; }));

	// A billion beats in bars and beats: 500 million seconds at 120 BPM.
	app.call("project.set", {{"timeFormat", "barsBeats"}});
	app.call("transport.locate", {{"position", 0}});
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == 0; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#primary-time") == "1.1.1.0"; }));
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, -1e13F);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") == int64_t{24000} * 1000000000; }));
	const int64_t beforeNaN = position(app);
	app.ui().wheel(std::nullopt, at("#primary-time"), 0, std::numeric_limits<float>::quiet_NaN());
	settle(app);
	CHECK(position(app) == beforeNaN);
}

TEST_CASE("a drag or wheel gesture on the tempo field is one undo step", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	REQUIRE(app.waitUntil([&app] { return element(app, "#tempo").at("attributes").at("value") == "120"; }));
	const size_t before = undoSteps(app);

	// A drag up raises the tempo in many steps of the field, and sets it once, at its end.
	const Json box = element(app, "#tempo").at("box");
	const int x = static_cast<int>(box.at("x").get<double>() + (box.at("width").get<double>() / 2));
	const int y = static_cast<int>(box.at("y").get<double>() + (box.at("height").get<double>() / 2));
	app.ui().drag(std::nullopt, at("#tempo"), {std::string(), 0, ui::Point{x, y - 40}});
	// The field keeps where the drag took it, also before the project's tempo is read again: wheel turns go on from
	// there.
	CHECK(element(app, "#tempo").at("attributes").at("value") != "120");
	REQUIRE(app.waitUntil([&app] { return tempo(app) > 121.0; }));
	settle(app);
	CHECK(undoSteps(app) == before + 1);
	const double dragged = tempo(app);

	// Turns of the wheel in a row are one step too.
	for (int turn = 0; turn < 3; ++turn)
		app.ui().wheel(std::nullopt, at("#tempo"), 0, -1);
	REQUIRE(app.waitUntil([&app, dragged] { return std::abs(tempo(app) - (dragged + 3.0)) < 0.001; }));
	settle(app);
	CHECK(undoSteps(app) == before + 2);

	app.call("history.undo");
	REQUIRE(app.waitUntil([&app, dragged] { return tempo(app) == dragged; }));
	app.call("history.undo");
	REQUIRE(app.waitUntil([&app] { return element(app, "#tempo").at("attributes").at("value") == "120"; }));
}

TEST_CASE(
	"a wheel gesture on the tempo field is set when the transport panel closes within its pause (#566, #569)", "[ui]")
{
	test::UiApp app({1024, 700});
	app.setDpRatio(0.75F); // at 1, a 1024-pixel panel hides the tempo section, as the transport bar does (see showBar)
	app.appUi().showProject();
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() != nullptr);
	const ui::WindowId panel = app.appUi().transportPanel()->id();
	REQUIRE(
		app.waitUntil([&app, panel] { return element(app, "#tempo", panel).at("attributes").at("value") == "120"; }));
	const size_t before = undoSteps(app);

	// The panel goes with the gesture running: it sets the tempo the field showed, as one undo step.
	app.ui().wheel(panel, at("#tempo"), 0, -1);
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() == nullptr);
	REQUIRE(app.waitUntil([&app] { return tempo(app) > 120.5; }));
	CHECK(tempo(app) == 121.0);
	CHECK(undoSteps(app) == before + 1);
}

TEST_CASE("a drag or wheel gesture on the tempo field in tempo track mode changes the event at the cursor", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("project.set", {{"tempoTrackActive", true}});
	app.call("tempo.add", {{"position", 4 * g_quarter}, {"bpm", 100.0}, {"ramp", false}});
	// The cursor after the event, three seconds in: the field shows its tempo, and a gesture changes that event,
	// not the first one (reference manual p. 1446).
	app.call("transport.locate", {{"position", 3 * 48000}});
	REQUIRE(app.waitUntil([&app] { return element(app, "#tempo").at("attributes").at("value") == "100"; }));
	const size_t before = undoSteps(app);
	const auto events = [&app] { return app.call("project.get").at("tempo"); };

	// A drag up raises the tempo of the event at the cursor in many steps of the field, and sets it once, at its end.
	const Json box = element(app, "#tempo").at("box");
	const int x = static_cast<int>(box.at("x").get<double>() + (box.at("width").get<double>() / 2));
	const int y = static_cast<int>(box.at("y").get<double>() + (box.at("height").get<double>() / 2));
	app.ui().drag(std::nullopt, at("#tempo"), {std::string(), 0, ui::Point{x, y - 40}});
	REQUIRE(app.waitUntil([&events] { return events()[1].at("bpm").get<double>() > 100.0; }));
	CHECK(events()[0].at("bpm").get<double>() == 120.0); // the first event stays
	settle(app);
	CHECK(undoSteps(app) == before + 1);
	const double dragged = events()[1].at("bpm").get<double>();

	// Turns of the wheel in a row are one step on the same event too.
	for (int turn = 0; turn < 3; ++turn)
		app.ui().wheel(std::nullopt, at("#tempo"), 0, -1);
	REQUIRE(app.waitUntil(
		[&events, dragged] { return std::abs(events()[1].at("bpm").get<double>() - (dragged + 3.0)) < 0.001; }));
	CHECK(events()[0].at("bpm").get<double>() == 120.0);
	settle(app);
	CHECK(undoSteps(app) == before + 2);

	app.call("history.undo");
	REQUIRE(app.waitUntil([&events, dragged] { return events()[1].at("bpm").get<double>() == dragged; }));
	app.call("history.undo");
	REQUIRE(app.waitUntil([&events] { return events()[1].at("bpm").get<double>() == 100.0; }));
	CHECK(events()[0].at("bpm").get<double>() == 120.0);
	REQUIRE(app.waitUntil([&app] { return element(app, "#tempo").at("attributes").at("value") == "100"; }));
}

TEST_CASE("the tempo field changes the tempo only when a number is typed, also the one it shows", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	// A tempo with more decimals than the field shows, e.g. from a MIDI file.
	app.call("project.set", {{"tempo", 130.0001}});
	REQUIRE(app.waitUntil([&app] { return element(app, "#tempo").at("attributes").at("value") == "130.0001"; }));
	const size_t before = undoSteps(app);

	// Enter or leaving the field without typing keeps the tempo and adds no undo step.
	app.ui().click(std::nullopt, at("#tempo"), ui::MouseButton::Left, 2, {});
	CHECK(element(app, "#tempo input").at("value") == "130.000");
	app.ui().key(std::nullopt, "Enter");
	app.ui().click(std::nullopt, at("#tempo"), ui::MouseButton::Left, 2, {});
	click(app, "#signature");
	CHECK(!hasClass(app, "#tempo", "editing"));
	settle(app);
	CHECK(tempo(app) == 130.0001);
	CHECK(undoSteps(app) == before);

	// Typing the number it shows sets that tempo.
	type(app, "#tempo", "130.000");
	REQUIRE(app.waitUntil([&app] { return tempo(app) == 130.0; }));
	settle(app);
	CHECK(undoSteps(app) == before + 1);
}

TEST_CASE("typing the position the primary time display showed during playback jumps back to it", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");
	app.call("transport.play");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("position") > 0; }));

	app.ui().click(std::nullopt, at("#primary-time"), ui::MouseButton::Left, 2, {});
	const std::string shown = element(app, "#primary-time input").at("value").get<std::string>();
	const int64_t from = position(app);
	REQUIRE(waitFor(app, [from](const Json& state) { return state.at("position") > from + 24000; })); // a beat on
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, shown);
	const int64_t typed = position(app);
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(waitFor(app, [typed](const Json& state) { return state.at("position") < typed; }));
	CHECK(transport(app).at("playing") == true);
	app.call("transport.stop");
	app.call("audio.stop");
}

TEST_CASE("the time display window shows beat counts only and changes its size", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	ui::Window* display = app.appUi().timeDisplay();
	REQUIRE(display != nullptr);
	const ui::WindowId window = display->id();
	app.call("transport.locate", {{"position", 72000}}); // 1.4.1.0
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "1.4.1.0"; }));
	CHECK(hasClass(app, "#time-display", "size-1", window));
	const ui::Size normal = app.shell().bounds(window).size();

	// Show Beat Count Only: the beat in its bar.
	app.ui().click(window, at("#time-display"), ui::MouseButton::Right, 1, {});
	click(app, "#beat-count-only", window);
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "4"; }));
	CHECK(element(app, "#beat-count-only", window).at("attributes").contains("checked"));
	CHECK(app.call("settings.get", {{"key", "ui.timeDisplay"}}).at("value").at("beatCountOnly") == true);
	app.call("transport.locate", {{"position", 96000}});
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "1"; }));

	// Increase and Decrease Window Size: the window and its digits, whose sizes the theme has.
	app.ui().click(window, at("#time-display"), ui::MouseButton::Right, 1, {});
	click(app, "#increase-size", window);
	REQUIRE(app.waitUntil([&app, window] { return hasClass(app, "#time-display", "size-2", window); }));
	const ui::Size larger = app.shell().bounds(window).size();
	CHECK(larger.width > normal.width);
	CHECK(larger.height > normal.height);
	CHECK(app.call("settings.get", {{"key", "ui.timeDisplay"}}).at("value").at("size") == 2);
	for (int smaller = 0; smaller < 4; ++smaller)
		app.keys().run("timeDisplay.decreaseSize", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app, window] { return hasClass(app, "#time-display", "size-0", window); }));
	CHECK(app.shell().bounds(window).size().width < normal.width);
	CHECK(app.call("settings.get", {{"key", "ui.timeDisplay"}}).at("value").at("size") == 0);

	// Both stay for the window opened again.
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	display = app.appUi().timeDisplay();
	REQUIRE(display != nullptr);
	const ui::WindowId again = display->id();
	REQUIRE(app.waitUntil([&app, again] { return shows(app, "#time-display", again) == "1"; }));
	REQUIRE(app.waitUntil([&app, again] { return hasClass(app, "#time-display", "size-0", again); }));
	CHECK(app.shell().bounds(again).size().width < normal.width);
}

namespace
{
	// Whether the digits of the time display window fit its height: a line of the display's text is not taller than
	// the window.
	bool digitsFit(test::UiApp& app, ui::Window& window)
	{
		Rml::Context& context = window.context();
		Rml::Element* display =
			context.GetNumDocuments() > 0 ? context.GetDocument(0)->GetElementById("time-display") : nullptr;
		REQUIRE(display != nullptr);
		const float line = display->GetLineHeight();
		const ui::Size pixels = window.pixelSize();
		INFO("a line " << line << " pixels high in a window " << pixels.height << " pixels high");
		return line > 0.0F && line <= static_cast<float>(pixels.height) && app.shell().find(window.id()) != nullptr;
	}

	ui::Window& openTimeDisplay(test::UiApp& app)
	{
		app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
		ui::Window* display = app.appUi().timeDisplay();
		REQUIRE(display != nullptr);
		return *display;
	}

	void setTimeDisplaySize(test::UiApp& app, int size)
	{
		app.call("settings.set", {{"key", "ui.timeDisplay"}, {"value", {{"beatCountOnly", false}, {"size", size}}}});
	}
}

TEST_CASE(
	"the time display window's sizes scale with the UI scale, and its digits fit at every size (#566, #569)", "[ui]")
{
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	ui::Window& display = openTimeDisplay(app);
	const ui::WindowId window = display.id();
	REQUIRE(app.waitUntil([&app, window] { return hasClass(app, "#time-display", "size-1", window); }));
	const ui::Size normal = app.shell().bounds(window).size();
	CHECK(normal == ui::Size{480, 120});
	CHECK(digitsFit(app, display));

	// A larger UI scale grows the window with its digits, the one at a preset size to the preset's size at the new
	// scale; the digits' line, 144 pixels at size 1 and a scale of 1.5, fits the window's 180.
	app.shell().setUiScale(1.5F);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.shell().bounds(window).size() == ui::Size{720, 180});
	CHECK(digitsFit(app, display));
	app.shell().setUiScale(2.0F);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.shell().bounds(window).size() == ui::Size{960, 240});
	CHECK(digitsFit(app, display));
	setTimeDisplaySize(app, 0);
	REQUIRE(app.waitUntil([&app, window] { return hasClass(app, "#time-display", "size-0", window); }));
	CHECK(app.shell().bounds(window).size() == ui::Size{640, 160});
	CHECK(digitsFit(app, display));

	// A window the user sized keeps its proportions.
	ui::Rect bounds = app.shell().bounds(window);
	bounds.width = 500;
	bounds.height = 130;
	app.shell().setBounds(window, bounds);
	app.shell().setUiScale(1.0F);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.shell().bounds(window).size() == ui::Size{250, 65});
	app.shell().setUiScale(2.0F);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.shell().bounds(window).size() == ui::Size{500, 130});
}

TEST_CASE("the time display window opens at the saved size, and a window the user sized keeps its bounds (#566, #569)",
	"[ui]")
{
	test::UiApp app({1024, 700});
	app.appUi().showProject();
	app.shell().setUiScale(1.5F);
	setTimeDisplaySize(app, 2);
	app.shell().step(std::chrono::milliseconds(0));

	// At the saved size and the UI scale from the start, not at the default and then jumping when the setting is read.
	ui::Window& display = openTimeDisplay(app);
	const ui::WindowId window = display.id();
	CHECK(app.shell().bounds(window).size() == ui::Size{960, 240});
	REQUIRE(app.waitUntil([&app, window] { return hasClass(app, "#time-display", "size-2", window); }));
	CHECK(app.shell().bounds(window).size() == ui::Size{960, 240});
	CHECK(digitsFit(app, display));

	// The user sizes it: other options of the setting leave it as it is.
	ui::Rect bounds = app.shell().bounds(window);
	bounds.width = 400;
	bounds.height = 100;
	app.shell().setBounds(window, bounds);
	app.keys().run("timeDisplay.beatCountOnly", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app, window] { return shows(app, "#time-display", window) == "1"; }));
	settle(app);
	CHECK(app.shell().bounds(window).size() == ui::Size{400, 100});

	// A size from the setting does, and a window opened again takes the bounds the user left (#569).
	setTimeDisplaySize(app, 1);
	REQUIRE(app.waitUntil([&app, window] { return hasClass(app, "#time-display", "size-1", window); }));
	CHECK(app.shell().bounds(window).size() == ui::Size{720, 180});
	app.shell().setBounds(window, bounds);
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	CHECK(app.appUi().timeDisplay() == nullptr);
	const ui::Window& reopened = openTimeDisplay(app);
	REQUIRE(app.waitUntil([&app, &reopened] { return hasClass(app, "#time-display", "size-1", reopened.id()); }));
	CHECK(app.shell().bounds(reopened.id()).size() == ui::Size{400, 100});

	// The UI scale changes while the window is closed: it opens with the bounds it had at the new scale, at a preset's
	// size if it had one.
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	CHECK(app.appUi().timeDisplay() == nullptr);
	app.shell().setUiScale(0.75F);
	const ui::Window& scaled = openTimeDisplay(app);
	CHECK(app.shell().bounds(scaled.id()).size() == ui::Size{200, 50});
	setTimeDisplaySize(app, 1);
	ui::Rect atPreset = app.shell().bounds(scaled.id());
	atPreset.width = 360; // the preset of size 1 at that scale
	atPreset.height = 90;
	app.shell().setBounds(scaled.id(), atPreset);
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	app.shell().setUiScale(1.5F);
	ui::Window& preset = openTimeDisplay(app);
	CHECK(app.shell().bounds(preset.id()).size() == ui::Size{720, 180});
	CHECK(digitsFit(app, preset));
}

TEST_CASE("a setup pane's buttons keep Space, also in the transport bar (#653)", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.ui().click(std::nullopt, at("#transport-bar [section=click]"), ui::MouseButton::Right, 1, {});
	REQUIRE(visible(app, "#transport-setup"));
	click(app, "#transport-setup [setup-section=locators]");
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(!visible(app, "#transport-bar [section=locators]"));
	// Space clicks the focused section button again rather than starting playback.
	const Json focus = app.ui().key(std::nullopt, "Space").at("focus");
	REQUIRE(focus.is_object());
	CHECK(focus.value("attributes", Json::object()).value("setup-section", std::string()) == "locators");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(visible(app, "#transport-bar [section=locators]"));
	app.shell().step(std::chrono::milliseconds(50));
	CHECK(transport(app).at("playing") == false);
}

TEST_CASE("setup panes show and hide the transport bar's and the transport panel's sections", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	CHECK(visible(app, "#transport-bar [section=locators]"));
	const std::vector<std::string> before = app.shownSections("transport-bar");

	// A right click on the bar opens its setup pane, which stays open while its buttons hide sections. The pane, in
	// the bar and wider than the room it has left, takes no room from its sections.
	app.ui().click(std::nullopt, at("#transport-bar [section=click]"), ui::MouseButton::Right, 1, {});
	REQUIRE(visible(app, "#transport-setup"));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.shownSections("transport-bar") == before);
	CHECK(visible(app, "#transport-bar [section=click]"));
	click(app, "#transport-setup [setup-section=locators]");
	click(app, "#transport-setup [setup-section=punchPoints]");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(!visible(app, "#transport-bar [section=locators]"));
	CHECK(!visible(app, "#transport-bar [section=punchPoints]"));
	CHECK(visible(app, "#transport-bar [section=transportControls]"));
	const Json layout = app.call("settings.get", {{"key", "ui.layout"}}).at("value");
	CHECK(layout.at("windows").at("project").at("sections").at("transport").at("hidden") ==
		Json::array({"midiAutoQuantize", "leftDivider", "locators", "punchPoints", "prePostRoll", "rightDivider"}));

	// The transport panel has sections of its own.
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() != nullptr);
	const ui::WindowId panel = app.appUi().transportPanel()->id();
	CHECK(visible(app, "[section=locators]", panel));
	app.ui().click(panel, at("[section=click]"), ui::MouseButton::Right, 1, {});
	click(app, "#transport-setup [setup-section=click]", panel);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(!visible(app, "[section=click]", panel));
	CHECK(visible(app, "#transport-bar [section=click]"));

	// Show All shows every section of the bar again, and saves the layout once, not once for each section (#566).
	app.ui().click(std::nullopt, at("#transport-bar [section=click]"), ui::MouseButton::Right, 1, {});
	REQUIRE(app.waitUntil([&app]
		{ return app.call("settings.get", {{"key", "ui.layout"}}).at("value") == app.appUi().docking().layout(); }));
	// The saves that change the hidden sections: others, such as the panel fitting its window, aren't Show All's.
	int saves = 0;
	Json hidden = app.call("settings.get", {{"key", "ui.layout"}})
					  .at("value")
					  .at("windows")
					  .at("project")
					  .at("sections")
					  .at("transport")
					  .at("hidden");
	const ui::Subscription counting = app.appUi().binding().on("settings.changed",
		[&saves, &hidden](const engine::Event& event)
		{
			if (const auto layout = event.data.at("values").find("ui.layout"); layout != event.data.at("values").end())
				if (const Json now = layout->at("windows").at("project").at("sections").at("transport").at("hidden");
					now != hidden)
				{
					++saves;
					hidden = now;
				}
		});
	click(app, "#transport-setup [setup-all]");
	app.shell().step(std::chrono::milliseconds(0));
	settle(app);
	CHECK(saves == 1);
	CHECK(visible(app, "#transport-bar [section=locators]"));
	CHECK(visible(app, "#transport-bar [section=punchPoints]"));
	CHECK(!visible(app, "[section=click]", panel));

	// The panel opened again keeps its own.
	app.ui().key(std::nullopt, "F2");
	app.ui().key(std::nullopt, "F2");
	REQUIRE(app.appUi().transportPanel() != nullptr);
	CHECK(!visible(app, "[section=click]", app.appUi().transportPanel()->id()));
}

TEST_CASE("setup panes save and recall presets and reset their sections to the defaults", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	const auto openPane = [&app]
	{
		if (!visible(app, "#transport-setup"))
			app.ui().click(std::nullopt, at("#transport-bar [section=click]"), ui::MouseButton::Right, 1, {});
		REQUIRE(visible(app, "#transport-setup"));
	};
	const auto exists = [&app](const std::string& selector)
	{ return !app.ui().dom(std::nullopt, selector, 0).at("elements").empty(); };
	const auto presets = [&app] { return app.call("settings.get", {{"key", "ui.setupPresets"}}).at("value"); };
	const std::vector<std::string> defaults = app.appUi().docking().sections("project", "transport");
	REQUIRE(defaults.size() > 1);
	openPane();
	click(app, "#transport-setup [setup-section=locators]");
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(!visible(app, "#transport-bar [section=locators]"));

	// Save Preset names the sections as they are, in the user setting ui.setupPresets, which profiles carry.
	openPane();
	click(app, "#transport-setup [setup-preset-save]");
	REQUIRE(app.waitUntil([&exists] { return exists("#setup-preset-name"); }));
	click(app, "#setup-preset-name");
	app.ui().text(std::nullopt, "Compact");
	click(app, "#setup-preset-ok");
	REQUIRE(app.waitUntil([&presets] { return presets().contains("transport"); }));
	const Json saved = presets().at("transport");
	REQUIRE(saved.size() == 1);
	CHECK(saved.at(0).at("name") == "Compact");
	CHECK(saved.at(0).at("hidden") ==
		Json::array({"midiAutoQuantize", "leftDivider", "locators", "prePostRoll", "rightDivider"}));
	CHECK(saved.at(0).at("order").at(2) == "locators");
	REQUIRE(app.waitUntil([&exists] { return exists("#transport-setup [setup-preset=Compact]"); }));

	// Reset All: the default order and sections, also after the sections were reordered.
	app.appUi().docking().moveSection("project", "transport", defaults.back(), defaults.front());
	REQUIRE(app.appUi().docking().sections("project", "transport").front() == defaults.back());
	openPane();
	click(app, "#transport-setup [setup-reset]");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(visible(app, "#transport-bar [section=locators]"));
	CHECK(app.appUi().docking().sections("project", "transport") == defaults);
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json layout = app.call("settings.get", {{"key", "ui.layout"}}).at("value");
			return !layout.at("windows").at("project").value("sections", Json::object()).contains("transport");
		}));

	// The preset brings its sections back.
	openPane();
	click(app, "#transport-setup [setup-preset=Compact]");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(!visible(app, "#transport-bar [section=locators]"));
	CHECK(visible(app, "#transport-bar [section=punchPoints]"));

	// Another client's presets fill the pane too.
	app.call("settings.set", {{"key", "ui.setupPresets"}, {"value", {{"transport", Json::array()}}}});
	REQUIRE(app.waitUntil([&exists] { return !exists("#transport-setup [setup-preset=Compact]"); }));
}

TEST_CASE("the transport controls refuse buttons and menu items with a command that doesn't exist or parameters it "
		  "doesn't take (#446)",
	"[unit]")
{
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	ui::Window& window = fake.shell.open(ui::WindowSpec{});
	ui::EngineBinding binding(engine);
	ui::KeyCommands keys(engine, binding);
	keys.add({"timeDisplay.increaseSize", "Makes the time display bigger.", {}});
	keys.add({"wind.forward", "Winds forward while held.", {}, [] {}});
	const auto run = [](const std::string& /*command*/, const Json& /*parameters*/) {};
	const auto check = [&keys](const std::string& command, const Json& parameters) { keys.check(command, parameters); };
	const auto load = [&window](const std::string& controls)
	{ return window.context().LoadDocumentFromMemory("<rml><body>" + controls + "</body></rml>"); };
	const std::string good =
		R"(<button command="transport.toggle" parameters='{"setting": "cycle"}'>Cycle</button>)"
		R"(<button command="wind.forward" hold="">Forward</button>)"
		R"(<menu><menuitem command="timeDisplay.increaseSize">Increase Window Size</menuitem></menu>)";

	Rml::ElementDocument* document = load(good);
	REQUIRE(document != nullptr);
	CHECK_NOTHROW(std::make_unique<ui::TransportControls>(engine, binding, *document, run, nullptr, check));
	document->Close();

	struct Bad
	{
		const char* control;
		const char* named; // in the message
	};
	const Bad bad[] = {
		{R"(<button command="transport.toggel">Cycle</button>)", "transport.toggel"},
		{R"(<button command="transport.toggle" parameters='{"setting": 1}'>Cycle</button>)", "setting"},
		{R"(<button command="wind.backward" hold="">Rewind</button>)", "wind.backward"},
		{R"(<button command="wind.forward" hold="" parameters='{"fast": true}'>Forward</button>)", "wind.forward"},
		{R"(<menu><menuitem command="timeDisplay.increaseSize" parameters='{"steps": 2}'>Bigger</menuitem></menu>)",
			"timeDisplay.increaseSize"},
		{R"(<button command="transport.play" parameters='{"setting"'>Start</button>)", "JSON object"},
	};
	for (const Bad& control : bad)
	{
		INFO(control.control);
		document = load(good + control.control);
		REQUIRE(document != nullptr);
		CHECK_THROWS_AS(std::make_unique<ui::TransportControls>(engine, binding, *document, run, nullptr, check),
			std::invalid_argument);
		CHECK_THROWS_WITH(std::make_unique<ui::TransportControls>(engine, binding, *document, run, nullptr, check),
			ContainsSubstring(control.named));
		document->Close();
	}
	// Without a check, only parameters that aren't a JSON object.
	document = load(good + R"(<button command="transport.toggel">Cycle</button>)");
	REQUIRE(document != nullptr);
	CHECK_NOTHROW(std::make_unique<ui::TransportControls>(engine, binding, *document, run));
	document->Close();
	document = load(good + R"(<button command="transport.play" parameters='[]'>Start</button>)");
	REQUIRE(document != nullptr);
	CHECK_THROWS_AS(std::make_unique<ui::TransportControls>(engine, binding, *document, run), std::invalid_argument);
	document->Close();
}

namespace
{
	// The first element in the window's documents that matches the selector. Only drawing updates RmlUi here, so its
	// box is where the window last drew it.
	Rml::Element& drawn(ui::Window& window, const std::string& selector)
	{
		Rml::Context& context = window.context();
		for (int i = 0; i < context.GetNumDocuments(); ++i)
			if (Rml::Element* found = context.GetDocument(i)->QuerySelector(selector))
				return *found;
		throw std::invalid_argument("no " + selector);
	}

	// What a time display shows: a text display's value, another element's text.
	std::string value(Rml::Element& display)
	{
		return display.GetTagName() == "textdisplay" ? display.GetAttribute<Rml::String>("value", "")
													 : display.GetInnerRML();
	}

	// Where the project window draws the project cursor for the position its event display has: pixels right of the
	// lanes' left edge.
	double cursorPixels(test::UiApp& app, const ui::EventDisplay& display)
	{
		return (display.cursor() - display.view().start) / display.view().scale * app.main().dpRatio();
	}
}

TEST_CASE(
	"the project cursor and the time displays follow playback and changes made through Lua, without input", "[ui]")
{
	test::UiApp app({1024, 600});
	const ui::EventDisplay& display = showBar(app);
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	ui::Window* timeWindow = app.appUi().timeDisplay();
	REQUIRE(timeWindow != nullptr);
	app.ui().key(std::nullopt, "F2");
	ui::Window* panelWindow = app.appUi().transportPanel();
	REQUIRE(panelWindow != nullptr);
	Rml::Element& cursor = drawn(app.main(), ".project-cursor");
	Rml::Element& bar = drawn(app.main(), "#primary-time");
	Rml::Element& time = drawn(*timeWindow, "#time-display");
	Rml::Element& panel = drawn(*panelWindow, "#primary-time");
	REQUIRE(panel.GetTagName() == "textdisplay");
	REQUIRE(
		app.waitUntil([&] { return value(bar) == "1.1.1.0" && value(panel) == value(bar) && !value(time).empty(); }));
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");
	app.call("transport.play");

	// No input from here on. The views follow the playback position once a frame, and whenever what one shows changes
	// in a step, its window draws it in that step.
	int moved = 0;
	int counted = 0;
	int panelCounted = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while ((moved < 10 || counted < 10 || panelCounted < 10) && std::chrono::steady_clock::now() < deadline)
	{
		const uint64_t mainFrames = app.main().frames();
		const uint64_t timeFrames = timeWindow->frames();
		const uint64_t panelFrames = panelWindow->frames();
		const double before = display.cursor();
		const std::string barBefore = value(bar);
		const std::string timeBefore = value(time);
		const std::string panelBefore = value(panel);
		app.shell().step(std::chrono::milliseconds(20));
		if (display.cursor() != before || value(bar) != barBefore)
		{
			++moved;
			CHECK(app.main().frames() == mainFrames + 1);
			CHECK(std::abs(cursor.GetOffsetLeft() - cursorPixels(app, display)) < 1.0);
		}
		if (value(time) != timeBefore)
		{
			++counted;
			CHECK(timeWindow->frames() == timeFrames + 1);
		}
		if (value(panel) != panelBefore)
		{
			++panelCounted;
			CHECK(panelWindow->frames() == panelFrames + 1);
		}
	}
	CHECK(moved >= 10);
	CHECK(counted >= 10);
	CHECK(panelCounted >= 10);
	app.call("transport.stop");
	app.call("audio.stop");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("playing") == false; }));
	// The cursor is in seconds: sample / 48000 * 48000 doesn't give every sample back exactly.
	REQUIRE(app.waitUntil([&display, &app]
		{ return std::llround(display.cursor() * 48000.0) == static_cast<long long>(position(app)); }));

	// A change made through Lua, as through any other engine client: the windows draw it in the step it arrives in.
	app.shell().step(std::chrono::milliseconds(0));
	const std::string stopped = value(time);
	CHECK(!app.lua().run("gnubase.transport.locate{position = 96000}", "locate").has_value());
	bool shown = false;
	const auto locateDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!shown && std::chrono::steady_clock::now() < locateDeadline)
	{
		const uint64_t mainFrames = app.main().frames();
		const uint64_t timeFrames = timeWindow->frames();
		const uint64_t panelFrames = panelWindow->frames();
		const double before = display.cursor();
		const std::string barBefore = value(bar);
		const std::string timeBefore = value(time);
		const std::string panelBefore = value(panel);
		app.shell().step(std::chrono::milliseconds(20));
		if (display.cursor() != before || value(bar) != barBefore)
		{
			CHECK(app.main().frames() == mainFrames + 1);
			CHECK(std::abs(cursor.GetOffsetLeft() - cursorPixels(app, display)) < 1.0);
		}
		if (value(time) != timeBefore)
			CHECK(timeWindow->frames() == timeFrames + 1);
		if (value(panel) != panelBefore)
			CHECK(panelWindow->frames() == panelFrames + 1);
		shown =
			display.cursor() == 2.0 && value(bar) == "2.1.1.0" && value(panel) == value(bar) && value(time) != stopped;
	}
	CHECK(shown);
}

TEST_CASE("windows whose documents nothing changed in don't draw, also during playback", "[ui][slow]")
{
	test::UiApp app({1024, 600});
	showBar(app);
	app.keys().run("window.mixer", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.appUi().mixer(1) != nullptr; }));
	const ui::Window* mixer = app.shell().find(app.appUi().mixerWindow(1));
	REQUIRE(mixer != nullptr);
	// The time display window shows only the beat, which changes twice a second at 120 BPM, while the position it
	// follows changes once a frame (#745).
	app.keys().run("window.timeDisplay", Json::object(), app.main(), nullptr);
	ui::Window* timeWindow = app.appUi().timeDisplay();
	REQUIRE(timeWindow != nullptr);
	Rml::Element& beat = drawn(*timeWindow, "#time-display");
	app.call("settings.set", {{"key", "ui.timeDisplay"}, {"value", {{"beatCountOnly", true}, {"size", 1}}}});
	REQUIRE(app.waitUntil([&beat] { return value(beat) == "1"; }));
	app.call("audio.setDevice", {{"blockSize", 256}}); // the dummy device
	app.call("audio.start");

	// Longer than the status line takes to ask the engine for the record time again (once a second): its answer
	// draws only what it changed.
	const auto idle = [&app, mixer, timeWindow, &beat](bool playing)
	{
		const uint64_t mainFrames = app.main().frames();
		const uint64_t mixerFrames = mixer->frames();
		int beats = 0;
		int wrong = 0; // steps in which the time display window drew without its beat changing, or the other way round
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
		while (std::chrono::steady_clock::now() < until)
		{
			const uint64_t timeFrames = timeWindow->frames();
			const std::string before = value(beat);
			app.shell().step(std::chrono::milliseconds(20));
			const bool changed = value(beat) != before;
			beats += changed ? 1 : 0;
			wrong += timeWindow->frames() == timeFrames + (changed ? 1 : 0) ? 0 : 1;
		}
		CHECK(wrong == 0); // it draws in the steps its beat changes in, and in no other
		CHECK(mixer->frames() == mixerFrames);
		if (playing)
		{
			CHECK(app.main().frames() > mainFrames + 10); // the project cursor and the transport bar's time displays
			CHECK(beats >= 3);
		}
	};
	const auto settle = [&app]
	{
		for (int i = 0; i < 10; ++i)
			app.shell().step(std::chrono::milliseconds(20));
	};
	settle(); // what the audio's events changed
	idle(false);
	app.call("transport.play");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("playing") == true; }));
	settle(); // what the transport's events changed
	idle(true);
	app.call("transport.stop");
	app.call("audio.stop");
}

TEST_CASE(
	"the pre-roll and post-roll section and the Transport menu switch the rolls and set their amounts (#583)", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.call("transport.set", {{"left", 48000}, {"right", 96000}, {"preRollAmount", 48000}, {"postRollAmount", 24000}});

	// Hidden until the setup pane shows it (reference manual p. 318).
	CHECK(!visible(app, "#transport-bar [section=prePostRoll]"));
	app.appUi().docking().setSectionHidden("project", "transport", "prePostRoll", false);
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(visible(app, "#transport-bar [section=prePostRoll]"));

	// The amounts as lengths in the primary time format: 120 BPM at 48 kHz, so a quarter is 24000 samples.
	REQUIRE(app.waitUntil([&app] { return shows(app, "#pre-roll-amount") == "0.2.0.0"; }));
	CHECK(shows(app, "#post-roll-amount") == "0.1.0.0");
	type(app, "#pre-roll-amount", "0.1.0.0");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("preRollAmount") == 24000; }));
	app.ui().wheel(std::nullopt, at("#post-roll-amount"), 0, -1); // up: a beat longer
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("postRollAmount") == 48000; }));
	app.ui().wheel(std::nullopt, at("#post-roll-amount"), 0, 5); // not below 0
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("postRollAmount") == 0; }));
	CHECK(transport(app).at("right") == 96000); // the locators stay
	CHECK(transport(app).at("left") == 48000);

	// The buttons switch them and follow them.
	for (const std::string setting : {"preRoll", "postRoll"})
	{
		const std::string button = setting == "preRoll" ? "#pre-roll" : "#post-roll";
		INFO(button);
		CHECK(!checked(app, button));
		click(app, button);
		REQUIRE(waitFor(app, [&setting](const Json& state) { return state.at(setting) == true; }));
		REQUIRE(app.waitUntil([&app, &button] { return checked(app, button); }));
		app.call("transport.set", {{setting, false}});
		REQUIRE(app.waitUntil([&app, &button] { return !checked(app, button); }));
	}

	// Transport > Pre-roll & Post-roll > Use Pre-roll and Use Post-roll, checked while on.
	const auto menu = [&app](const std::string& item)
	{
		click(app, "#menu-bar button[menu=transport-menu]");
		app.ui().move(std::nullopt, {"#pre-post-roll-submenu", 0, std::nullopt});
		app.shell().step(std::chrono::milliseconds(0));
		REQUIRE(app.waitUntil([&app, &item] { return visible(app, item); }));
	};
	menu("#use-post-roll");
	CHECK(!checked(app, "#use-post-roll"));
	click(app, "#use-post-roll");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("postRoll") == true; }));
	menu("#use-pre-roll");
	CHECK(checked(app, "#use-post-roll"));
	CHECK(!checked(app, "#use-pre-roll"));
	click(app, "#use-pre-roll");
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("preRoll") == true; }));
}

TEST_CASE("a wheel turn that the engine refuses leaves the display at the engine's amount, and the next turn goes on "
		  "from there (#569)",
	"[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	app.appUi().docking().setSectionHidden("project", "transport", "prePostRoll", false);
	app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(visible(app, "#transport-bar [section=prePostRoll]"));
	app.call("transport.set", {{"left", 48000}, {"right", 96000}, {"preRollAmount", 48000}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#pre-roll-amount") == "0.2.0.0"; }));

	// A billion beats, which injected input may turn, are far more pre-roll than transport.set takes (an hour at the
	// highest sample rate): the engine refuses it, with no transport.changed. The UI asks for the transport instead.
	const auto refused = [&app]
	{
		const Json log = app.call("engine.commandLog").at("commands");
		return std::count_if(log.begin(), log.end(),
			[](const Json& entry) { return entry.at("command") == "transport.set" && entry.contains("error"); });
	};
	app.ui().wheel(std::nullopt, at("#pre-roll-amount"), 0, -1e13F);
	REQUIRE(app.waitUntil([&refused] { return refused() == 1; }));
	REQUIRE(app.waitUntil(
		[&app]
		{
			const ui::EngineBinding& binding = app.appUi().binding();
			return binding.waiting("transport.set") == 0 && binding.waiting("transport.get") == 0;
		}));
	CHECK(transport(app).at("preRollAmount") == 48000);
	CHECK(shows(app, "#pre-roll-amount") == "0.2.0.0");
	// Shown afresh in seconds, the amount the controls hold is the engine's: two beats, a second, not a billion.
	app.call("project.set", {{"timeFormat", "seconds"}});
	REQUIRE(app.waitUntil([&app] { return shows(app, "#left-locator") == "0:00:01.000"; }));
	CHECK(shows(app, "#pre-roll-amount") == "0:00:01.000");

	// The next turn goes on from the engine's amount: a second longer.
	app.ui().wheel(std::nullopt, at("#pre-roll-amount"), 0, -1);
	REQUIRE(waitFor(app, [](const Json& state) { return state.at("preRollAmount") == 96000; }));
	REQUIRE(app.waitUntil([&app] { return shows(app, "#pre-roll-amount") == "0:00:02.000"; }));
	CHECK(refused() == 1);
}

TEST_CASE(
	"the Record button shows a recording that waits for the pre-roll, and a second press cancels it (#2039)", "[ui]")
{
	test::UiApp app({1024, 700});
	showBar(app);
	// A minute of pre-roll: the wait outlasts the test.
	app.call("transport.set", {{"preRoll", true}, {"preRollAmount", 48000 * 60}});
	app.call("transport.locate", {{"position", 48000 * 60}});
	click(app, "#record");
	REQUIRE(app.waitUntil([&app] { return hasClass(app, "#record", "waiting"); }));
	CHECK(!checked(app, "#record"));
	CHECK(transport(app).at("recordWaiting") == true);
	click(app, "#record");
	REQUIRE(app.waitUntil([&app] { return !hasClass(app, "#record", "waiting"); }));
	const Json state = transport(app);
	CHECK(state.at("recordWaiting") == false);
	CHECK(state.at("recording") == false);
	CHECK(!checked(app, "#record"));
	click(app, "#stop");
	REQUIRE(waitFor(app, [](const Json& stopped) { return stopped.at("playing") == false; }));
}
