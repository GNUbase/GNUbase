// SPDX-License-Identifier: GPL-3.0-or-later
// The main menu bar (#392, ui/mainmenu.h): the project window's menus run engine and UI commands as keys do, show
// their keys and are disabled while their command can't run. The tests click through them with ui::Automation, as
// MCP's ui_click does. Items with a command that doesn't exist or parameters it doesn't take are refused (#446), and
// the transport windows whose controls are refused close again (#669). The mixer and editor windows have reduced menus,
// and Show Menu Bar is in a window's title bar context menu on Windows (#444).
#include "engine/engine.h"
#include "testutils/fakeui.h"
#include "testutils/heldjob.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/keycommands.h"
#include "ui/mainmenu.h"
#include "ui/rmlui.h"
#include "ui/sdlwindowsystem.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/SystemInterface.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#endif

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

TEST_CASE("Audio Hitpoints divides the selected folder with Group Editing enabled", "[ui]")
{
	const bool hitpoints = GENERATE(false, true);
	test::UiApp app({1200, 800});
	auto& display = app.appUi().showProject();
	const auto folder = app.call("track.add", {{"type", "folder"}}).at("track").at("id").get<uint64_t>();
	const auto track = app.call("track.add", {{"type", "audio"}, {"parent", folder}}).at("track").at("id");
	const auto media =
		app.call("media.add", {{"path", "missing-loop.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 48000}})
			.at("media")
			.at("id");
	const auto clip = app.call("clip.add", {{"media", media}}).at("clip").at("id");
	app.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}});
	if (hitpoints)
		app.call("clip.setHitpoints",
			{{"clip", clip}, {"hitpoints", Json::array({{{"position", 12000}, {"peak", -3}, {"intensity", 1}}})}});
	REQUIRE(app.waitUntil([&] { return display.project().at("tracks").size() == 2; }));
	app.appUi().trackList()->select({folder});
	CHECK(!app.keys().canRun("audio.divideAtHitpoints", app.main(), nullptr));
	app.call("track.set", {{"track", folder}, {"groupEditing", true}});
	REQUIRE(app.waitUntil([&] { return app.keys().canRun("audio.divideAtHitpoints", app.main(), nullptr); }));
	const Json before = app.call("project.get").at("audioEvents");
	app.ui().click(std::nullopt, {"#menu-bar button[menu=audio-menu]", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	app.ui().move(std::nullopt, {"#audio-hitpoints-submenu", 0, std::nullopt});
	app.ui().click(
		std::nullopt, {"menuitem[command='audio.divideAtHitpoints']", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	if (!hitpoints)
	{
		REQUIRE(app.waitUntil(
			[&]
			{
				const Json found = app.ui().dom(std::nullopt, "#edit-refused", 0).at("elements");
				return !found.empty() && found[0].value("text", "").find("detect them first") != std::string::npos;
			}));
		CHECK(app.call("project.get").at("audioEvents") == before);
		return;
	}
	REQUIRE(app.waitUntil([&] { return display.project().at("audioEvents").size() == 2; }));
	app.call("history.undo");
	CHECK(app.call("project.get").at("audioEvents").size() == 1);
}

namespace
{
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		INFO(selector);
		REQUIRE(found.size() == 1);
		return found[0];
	}

	bool shown(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		return found.size() == 1 && found[0].at("visible").get<bool>();
	}

	bool disabled(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).value("attributes", Json::object()).contains("disabled");
	}

	bool checked(test::UiApp& app, const std::string& selector)
	{
		app.shell().step(std::chrono::milliseconds(0));
		return element(app, selector).value("attributes", Json::object()).contains("checked");
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// The keys a menu item shows.
	std::string keysOf(test::UiApp& app, const std::string& item)
	{
		const Json found = elements(app, item + " .menu-keys");
		return found.empty() ? std::string() : found[0].value("text", std::string());
	}

	std::string keyName(const char* keys)
	{
		return ui::keyName(ui::parseKeys(keys));
	}
}

TEST_CASE("Studio > Studio Setup in the main menu opens the studio setup dialog", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	CHECK(shown(app, "#menu-bar"));
	CHECK(!shown(app, "#studio-menu"));

	click(app, "#menu-bar button[menu=studio-menu]");
	REQUIRE(shown(app, "#studio-menu"));
	click(app, "menuitem[command=\"window.studioSetup\"]");
	REQUIRE(app.waitUntil([&app] { return !elements(app, "#studio-setup").empty(); }));
	CHECK(!shown(app, "#studio-menu")); // the click closed the menu
}

TEST_CASE("main menu items show their keys and run engine commands with their parameters", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.call("transport.set", {{"left", 48000}, {"right", 96000}});
	app.shell().step(std::chrono::milliseconds(0));

	CHECK(keysOf(app, "menuitem[command=\"history.undo\"]") == keyName("Mod+Z"));
	CHECK(keysOf(app, "#transport-menu > menuitem[command=\"transport.toggle\"]") == "Space"); // Start/Stop
	CHECK(keysOf(app, "menuitem[command=\"edit.delete\"]") == "Delete");                       // the first of its keys
	CHECK(keysOf(app, "menuitem[command=\"window.studioSetup\"]").empty());

	// Transport > Locators > Go to Right Locator Position: transport.locate with the item's parameters.
	CHECK(keysOf(app, "#locate-right-locator") == "Num2");
	click(app, "#menu-bar button[menu=transport-menu]");
	app.ui().move(std::nullopt, {"#locators-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#locate-right-locator");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("position") == 96000; }));
	Json located;
	const Json log = app.call("engine.commandLog");
	for (const Json& entry : log.at("commands"))
		if (entry.at("command") == "transport.locate")
			located = entry;
	REQUIRE(!located.is_null());
	CHECK(located.at("client") == "ui");

	// UI commands run in the window the menu is in: Edit > Select > All selects through the event display, in a
	// submenu that shows while the mouse is over its item.
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	app.call("midiPart.add", {{"track", track.at("id")}, {"position", 0}, {"length", 1920}});
	REQUIRE(app.waitUntil(
		[&app]
		{
			auto* lanes =
				dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
			return lanes != nullptr && !lanes->lanes().empty() && !lanes->lanes()[0].events.empty();
		}));
	click(app, "#menu-bar button[menu=edit-menu]");
	app.ui().move(std::nullopt, {"#select-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "menuitem[command=\"edit.selectAll\"]");
	click(app, "#menu-bar button[menu=edit-menu]");
	click(app, "menuitem[command=\"edit.delete\"]");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("midiParts").empty(); }));
}

TEST_CASE("Transport > Audio Record Mode sets the transport's audio record mode", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#menu-bar button[menu=transport-menu]");
	app.ui().move(std::nullopt, {"#audio-record-mode-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(!checked(app, "#audio-record-mode-replace"));
	CHECK(checked(app, "#audio-record-mode-keep-history"));
	click(app, "#audio-record-mode-replace");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("audioRecordMode") == "replace"; }));

	// The item of the mode that is set is checked (#531), also when another client set it.
	REQUIRE(app.waitUntil([&app] { return checked(app, "#audio-record-mode-replace"); }));
	CHECK(!checked(app, "#audio-record-mode-keep-history"));
	app.call("transport.set", {{"audioRecordMode", "keepHistory"}});
	REQUIRE(app.waitUntil([&app] { return !checked(app, "#audio-record-mode-replace"); }));
	CHECK(checked(app, "#audio-record-mode-keep-history"));
	CHECK(!checked(app, "#lock-record")); // Lock and Unlock Record are commands, not a state
	CHECK(!checked(app, "#unlock-record"));
}

TEST_CASE("Transport > MIDI Record Mode sets the transport's MIDI record mode", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#menu-bar button[menu=transport-menu]");
	app.ui().move(std::nullopt, {"#midi-record-mode-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#midi-record-mode-merge");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("midiRecordMode") == "merge"; }));
	REQUIRE(app.waitUntil([&app] { return checked(app, "#midi-record-mode-merge"); }));
}

TEST_CASE("Transport > MIDI Retrospective Recording > Empty All Buffers empties every track's buffer", "[ui]")
{
	// Reference manual p. 356.
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.call("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	const Json added = app.call("track.add", {{"type", "instrument"}});
	const Json channel = added.at("channel").at("id");
	app.call("channel.set", {{"channel", channel}, {"recordEnable", false}});
	app.call("midi.inject",
		{{"channel", channel},
			{"messages",
				{{{"position", 100}, {"bytes", {0x90, 60, 100}}}, {{"position", 200}, {"bytes", {0x80, 60, 0}}}}}});
	app.call("audio.render", {{"frames", 640}});
	app.call("midi.insertRetrospective", {{"track", added.at("track").at("id")}}); // the buffer holds the note
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#menu-bar button[menu=transport-menu]");
	app.ui().move(std::nullopt, {"#midi-retrospective-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#midi-retrospective-empty");
	REQUIRE(app.waitUntil(
		[&]
		{
			const engine::Result inserted =
				engine::Client(app.engine()).call("midi.insertRetrospective", {{"track", added.at("track").at("id")}});
			return !inserted.ok() && inserted.error.find("nothing") != std::string::npos;
		}));
}

TEST_CASE(
	"Transport > MIDI Retrospective Recording > Insert from All MIDI Inputs inserts into the selected track", "[ui]")
{
	// Reference manual p. 354 (#641).
	test::UiApp app({900, 640});
	const auto& display = app.appUi().showProject();
	app.call("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	const Json added = app.call("track.add", {{"type", "instrument"}});
	const auto track = added.at("track").at("id").get<uint64_t>();
	app.call("channel.set", {{"channel", added.at("channel").at("id")}, {"recordEnable", false}});
	for (const int status : {0x90, 0x80})
	{
		app.call("midi.keyboard", {{"messages", Json::array({{{"bytes", {status, 60, 100}}}})}});
		app.call("audio.render", {{"frames", 640}});
	}
	REQUIRE(app.waitUntil([&] { return display.project().at("tracks").size() == 1; }));
	app.appUi().trackList()->select({track});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#menu-bar button[menu=transport-menu]");
	app.ui().move(std::nullopt, {"#midi-retrospective-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#midi-retrospective-all-inputs");
	REQUIRE(app.waitUntil([&] { return app.call("project.get").at("midiParts").size() == 1; }));
	CHECK(app.call("project.get").at("midiParts")[0].at("track") == track);
}

TEST_CASE("Transport > MIDI Record Mode sets the transport's MIDI cycle record mode", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#menu-bar button[menu=transport-menu]");
	app.ui().move(std::nullopt, {"#midi-record-mode-submenu", 0, std::nullopt});
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#midi-cycle-record-mode-stacked");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("midiCycleRecordMode") == "stacked"; }));
	REQUIRE(app.waitUntil([&app] { return checked(app, "#midi-cycle-record-mode-stacked"); }));
	CHECK(checked(app, "#midi-record-mode-new-parts"));
}

TEST_CASE("main menu items are disabled while their command can't run", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0));

	// Nothing to undo or redo in a new project.
	CHECK(disabled(app, "menuitem[command=\"history.undo\"]"));
	CHECK(disabled(app, "menuitem[command=\"history.redo\"]"));
	CHECK(!disabled(app, "#transport-menu > menuitem[command=\"transport.toggle\"]"));

	app.call("track.add", {{"type", "audio"}});
	REQUIRE(app.waitUntil([&app] { return !disabled(app, "menuitem[command=\"history.undo\"]"); }));
	CHECK(disabled(app, "menuitem[command=\"history.redo\"]"));

	click(app, "#menu-bar button[menu=edit-menu]");
	click(app, "menuitem[command=\"history.undo\"]");
	REQUIRE(app.waitUntil([&app] { return !disabled(app, "menuitem[command=\"history.redo\"]"); }));
	CHECK(disabled(app, "menuitem[command=\"history.undo\"]"));
	CHECK(app.call("project.get").at("tracks").empty());

	// A click on a disabled item does nothing, and the menu stays open.
	click(app, "#menu-bar button[menu=edit-menu]");
	click(app, "menuitem[command=\"history.undo\"]");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(app.call("project.get").at("tracks").empty());
	CHECK(!disabled(app, "menuitem[command=\"history.redo\"]"));
}

// What a release and an item's own command change shows in the DOM that MCP's UI tools read right after the input,
// without a step of the shell (#1173): the event display selects when the mouse button is released, and Edit >
// Select changes what the items for selected events can do.
TEST_CASE(
	"the main menu's items follow a release's selection and an item's own command without a step of the shell", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.setDpRatio(1.0F);
	const Json track = app.call("track.add", {{"type", "midi"}}).at("track");
	app.call("midiPart.add", {{"track", track.at("id")}, {"position", 0}, {"length", 1920}});
	ui::EventLanes* lanes = nullptr;
	REQUIRE(app.waitUntil(
		[&app, &lanes]
		{
			lanes =
				dynamic_cast<ui::EventLanes*>(app.main().context().GetDocument(0)->QuerySelector("event-lanes.events"));
			return lanes != nullptr && !lanes->lanes().empty() && !lanes->lanes()[0].events.empty();
		}));
	const std::string quantize = "menuitem[command=\"edit.quantize\"]";
	REQUIRE(disabled(app, quantize));
	const auto select = [&app](const char* command)
	{
		click(app, "#menu-bar button[menu=edit-menu]");
		app.ui().move(std::nullopt, {"#select-submenu", 0, std::nullopt});
		app.shell().step(std::chrono::milliseconds(0));
		app.ui().click(std::nullopt, {std::string("menuitem[command=\"") + command + "\"]", 0, std::nullopt},
			ui::MouseButton::Left, 1, {});
	};

	// Edit > Select > All and None: Quantize follows right after the click on the item.
	select("edit.selectAll");
	CHECK(!disabled(app, quantize));
	select("edit.selectNone");
	CHECK(disabled(app, quantize));

	// A Ctrl-click on the part selects it, and another one takes it out of the selection when the button is released.
	const ui::TimeSpan& part = lanes->lanes()[0].events[0];
	const ui::Target middle{"", 0,
		ui::Point{static_cast<int>(lanes->GetAbsoluteLeft() + ((part.start + part.end()) / 2.0)),
			static_cast<int>(lanes->GetAbsoluteTop() + (lanes->lanes()[0].height / 2.0F))}};
	app.ui().click(std::nullopt, middle, ui::MouseButton::Left, 1, {"ctrl"});
	CHECK(!disabled(app, quantize));
	if (auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface()))
		clock->advance(1.0); // no double click
	app.ui().click(std::nullopt, middle, ui::MouseButton::Left, 1, {"ctrl"});
	CHECK(disabled(app, quantize));
}

// A click's own listeners may change what the clicked item can do: the menu follows them before it looks for the
// item (#1120, #1176), as far as they ran before the menu's listener on the document (#1233).
TEST_CASE("the main menu follows what a click's listeners changed before it runs the clicked item", "[unit]")
{
	// Lets the command run; it outlives the documents it listens to.
	bool enabled = false;
	class Enabler final : public Rml::EventListener
	{
	public:
		explicit Enabler(bool& enabled) : m_enabled(enabled) {}
		void ProcessEvent(Rml::Event& /*event*/) override { m_enabled = true; }

	private:
		bool& m_enabled;
	} enabler(enabled);
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	ui::Window& window = fake.shell.open(ui::WindowSpec{});
	ui::EngineBinding binding(engine);
	ui::KeyCommands keys(engine, binding);
	int runs = 0;
	keys.add({"test.command", "A test command.", [&runs](ui::Window& /*window*/, Rml::Element* /*zone*/) { ++runs; }});
	keys.enableWhen("test.command", [&enabled](ui::Window& /*window*/, Rml::Element* /*zone*/) { return enabled; });
	Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(
		R"(<rml><head><style>body { width: 100%; height: 100%; } menuitem { display: block; height: 20dp; }</style>)"
		R"(</head><body><menuitem id="item" command="test.command"/></body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	Rml::Element* item = document->GetElementById("item");
	REQUIRE(item != nullptr);
	const auto click = [&fake]
	{ ui::Automation(fake.shell).click(std::nullopt, {"#item", 0, std::nullopt}, ui::MouseButton::Left, 1, {}); };

	// The item's own listener lets its command run, before the menu's listener on the document.
	item->AddEventListener(Rml::EventId::Click, &enabler);
	{
		const ui::MainMenu menu(keys, window, *document);
		REQUIRE(item->HasAttribute("disabled"));
		click();
		CHECK(runs == 1);
		CHECK(!item->HasAttribute("disabled"));
	}
	item->RemoveEventListener(Rml::EventId::Click, &enabler);

	// So does a listener on the document added before the menu.
	enabled = false;
	document->AddEventListener(Rml::EventId::Click, &enabler);
	{
		const ui::MainMenu menu(keys, window, *document);
		REQUIRE(item->HasAttribute("disabled"));
		click();
		CHECK(runs == 2);
	}
	document->RemoveEventListener(Rml::EventId::Click, &enabler);

	// One added to the document after the menu runs once the menu has looked for the item: that click doesn't run the
	// command. The update after the input, as the app does it, enables the item, and the next click runs it.
	enabled = false;
	{
		ui::MainMenu menu(keys, window, *document);
		fake.shell.onInputHandled([&menu] { menu.update(); });
		document->AddEventListener(Rml::EventId::Click, &enabler);
		REQUIRE(item->HasAttribute("disabled"));
		click();
		CHECK(runs == 2);
		CHECK(!item->HasAttribute("disabled"));
		click();
		CHECK(runs == 3);
		document->RemoveEventListener(Rml::EventId::Click, &enabler);
		fake.shell.onInputHandled({});
	}

	// A capture phase listener of the document added after the menu runs before the menu's listener, which looks for
	// the item in the target phase or later: that click runs the command.
	enabled = false;
	{
		const ui::MainMenu menu(keys, window, *document);
		document->AddEventListener(Rml::EventId::Click, &enabler, true);
		REQUIRE(item->HasAttribute("disabled"));
		click();
		CHECK(runs == 4);
		CHECK(!item->HasAttribute("disabled"));
		document->RemoveEventListener(Rml::EventId::Click, &enabler, true);
	}
	document->Close();
}

// The menu's update runs in every frame (#1901): it asks each command once per zone whether it can run, a click asks
// only the clicked item's, and the keys are worked out and written again only when the bindings change.
TEST_CASE("the main menu asks each command once per update and writes keys only when the bindings change", "[unit]")
{
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	ui::Window& window = fake.shell.open(ui::WindowSpec{});
	ui::EngineBinding binding(engine);
	ui::KeyCommands keys(engine, binding);
	int asked = 0;
	int runs = 0;
	for (const char* id : {"test.first", "test.second"})
	{
		keys.add({id, "A test command.", [&runs](ui::Window& /*window*/, Rml::Element* /*zone*/) { ++runs; }});
		keys.enableWhen(id,
			[&asked](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{
				++asked;
				return true;
			});
	}
	keys.bind({"Mod+1", "test.first"});
	keys.bind({"Mod+2", "test.second"});
	Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(
		R"(<rml><head><style>body { width: 100%; height: 100%; } menuitem { display: block; height: 20dp; }</style>)"
		R"(</head><body><menuitem id="first" command="test.first"/><menuitem id="second" command="test.second"/>)"
		R"(<menuitem id="again" command="test.first"/></body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	{
		ui::MainMenu menu(keys, window, *document);
		Rml::Element* first = document->QuerySelector("#first .menu-keys");
		Rml::Element* second = document->QuerySelector("#second .menu-keys");
		REQUIRE(first != nullptr);
		REQUIRE(second != nullptr);
		CHECK(first->GetInnerRML() == keyName("Mod+1"));
		CHECK(second->GetInnerRML() == keyName("Mod+2"));

		// The third item, apart from the first, has the first's command in the same zone: it adds no ask.
		asked = 0;
		menu.update();
		CHECK(asked == 2);
		asked = 0;
		ui::Automation(fake.shell).click(std::nullopt, {"#first", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		CHECK(asked == 1);
		CHECK(runs == 1);

		// The menu compares the keys with what it wrote, not with the DOM: while the bindings stay, it writes nothing.
		first->SetInnerRML("first");
		second->SetInnerRML("second");
		menu.update();
		CHECK(first->GetInnerRML() == "first");
		CHECK(second->GetInnerRML() == "second");
		// Mod+2 runs test.first now: only the second item's keys change.
		keys.bind({"Mod+2", "test.first"});
		menu.update();
		CHECK(first->GetInnerRML() == "first");
		CHECK(second->GetInnerRML().empty());
	}
	document->Close();
}

TEST_CASE("the main menu's update reports its time per frame", "[ui][slow]")
{
	// A benchmark (#1901): the time of the main menu's update in an idle frame of the project window, with the default
	// key commands and with a key command of the user's for every command, so that a regression shows in the output.
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0));
	ui::MainMenu* menu = app.appUi().mainMenu();
	REQUIRE(menu != nullptr);
	const auto time = [menu]
	{
		constexpr int frames = 200;
		const auto start = std::chrono::steady_clock::now();
		for (int frame = 0; frame < frames; ++frame)
			menu->update();
		const std::chrono::duration<double, std::milli> taken = std::chrono::steady_clock::now() - start;
		return taken.count() / frames;
	};
	const double defaults = time();

	std::vector<std::string> free;
	for (const char* modifiers : {"Mod+Alt+", "Mod+Alt+Shift+", "Ctrl+Alt+Shift+Meta+"})
	{
		for (char letter = 'A'; letter <= 'Z'; ++letter)
			free.push_back(modifiers + std::string(1, letter));
		for (int number = 1; number <= 24; ++number)
			free.push_back(modifiers + std::string("F") + std::to_string(number));
	}
	Json custom = Json::array();
	for (const Json& command : app.keys().commands())
	{
		if (free.empty())
			break;
		custom.push_back({{"command", command.at("command")}, {"parameters", command.at("parameters")},
			{"keys", Json::array({free.back()})}});
		free.pop_back();
	}
	app.keys().customize(custom, Json::array());
	menu->update();
	const double customized = time();
	WARN("The main menu's update in an idle frame: " << defaults << " ms with the default key commands, " << customized
													 << " ms with " << custom.size() << " key commands of the user's");
}

// ui::Automation runs the shell's input handler once after each input, once the input's listeners have run, so that
// the main menu's items show what any input changed before a script reads the DOM (#1173, #1233).
TEST_CASE("each input of the UI automation runs the shell's input handler once, after the input's listeners", "[unit]")
{
	std::vector<std::string> happened; // outlives the shell and the documents that write to it
	class Recorder final : public Rml::EventListener
	{
	public:
		explicit Recorder(std::vector<std::string>& happened) : m_happened(happened) {}
		void ProcessEvent(Rml::Event& event) override { m_happened.push_back(event.GetType()); }

	private:
		std::vector<std::string>& m_happened;
	} recorder(happened);
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	const ui::Window& window = fake.shell.open(ui::WindowSpec{});
	Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(
		R"(<rml><head><style>body { width: 100%; height: 100%; }</style></head><body/></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	document->Focus();
	for (const char* type : {"mousemove", "mousedown", "mouseup", "click", "mousescroll", "keydown", "keyup",
			 "textinput", ui::g_fileDropEvent})
		document->AddEventListener(type, &recorder);
	fake.shell.onUnusedKey(
		[&happened](ui::Window& /*window*/, const ui::KeyPress& /*keys*/) { happened.emplace_back("unused key"); });
	fake.shell.onKeyReleased(
		[&happened](ui::Window& /*window*/, const ui::KeyPress& /*keys*/) { happened.emplace_back("key released"); });
	fake.shell.onInputHandled([&happened] { happened.emplace_back("input handled"); });
	ui::Automation automation(fake.shell);

	// The last input ran the expected listeners and shell handlers in this order, and the input handler once, after
	// everything else.
	const auto handledAfter = [&happened](const std::vector<std::string>& expected)
	{
		INFO("happened: " << engine::Json(happened).dump());
		CHECK(std::count(happened.begin(), happened.end(), "input handled") == 1);
		CHECK((!happened.empty() && happened.back() == "input handled"));
		auto at = happened.begin();
		for (const std::string& one : expected)
		{
			INFO(one);
			at = std::find(at, happened.end(), one);
			REQUIRE(at != happened.end());
			++at;
		}
		happened.clear();
	};
	const auto point = [](int x, int y) { return ui::Target{"", 0, ui::Point{x, y}}; };

	automation.move(std::nullopt, point(100, 100));
	handledAfter({"mousemove"});
	automation.click(std::nullopt, point(110, 100), ui::MouseButton::Left, 1, {});
	handledAfter({"mousedown", "mouseup", "click"});
	automation.click(std::nullopt, point(110, 100), ui::MouseButton::Left, 2, {}); // a double click is one input
	handledAfter({"mousedown", "mouseup", "click", "mousedown", "mouseup", "click"});
	automation.drag(std::nullopt, point(120, 100), point(200, 150), {});
	handledAfter({"mousedown", "mousemove", "mouseup"});
	automation.press(std::nullopt, point(130, 100), ui::MouseButton::Left, {});
	handledAfter({"mousedown"});
	automation.release(std::nullopt, point(130, 100), ui::MouseButton::Left, {});
	handledAfter({"mouseup"});
	automation.wheel(std::nullopt, point(140, 100), 0.0F, 1.0F, {});
	handledAfter({"mousescroll"});
	automation.key(std::nullopt, "F2", ui::KeyAction::Press);
	handledAfter({"keydown", "unused key", "keyup", "key released"});
	automation.key(std::nullopt, "F2", ui::KeyAction::Down);
	handledAfter({"keydown", "unused key"});
	automation.key(std::nullopt, "F2", ui::KeyAction::Up);
	handledAfter({"keyup", "key released"});
	automation.text(std::nullopt, "a");
	handledAfter({"textinput"});
	automation.drop(std::nullopt, point(150, 100), {"/music/kick.wav"});
	handledAfter({ui::g_fileDropEvent});

	fake.shell.onInputHandled({});
	fake.shell.onKeyReleased({});
	fake.shell.onUnusedKey({});
	document->Close();
}

TEST_CASE("key commands say whether a command can run, for menu items", "[ui]")
{
	test::UiApp app;
	ui::KeyCommands& keys = app.keys();
	bool enabled = false;
	keys.add({"test.command", "A test command.", {}});
	CHECK(keys.canRun("test.command", app.main(), nullptr));
	CHECK(keys.canRun("transport.play", app.main(), nullptr));
	CHECK(!keys.canRun("test.missing", app.main(), nullptr));

	keys.enableWhen("test.command", [&enabled](ui::Window& /*window*/, Rml::Element* /*zone*/) { return enabled; });
	CHECK(!keys.canRun("test.command", app.main(), nullptr));
	enabled = true;
	CHECK(keys.canRun("test.command", app.main(), nullptr));
	CHECK_THROWS(keys.enableWhen("test.missing", {}));
}

TEST_CASE("the menu bar's menus follow the mouse and Left and Right, Alt opens them, and submenus have arrows", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();

	click(app, "#menu-bar button[menu=file-menu]");
	REQUIRE(shown(app, "#file-menu"));
	CHECK(shown(app, "#import-submenu > .menu-arrow"));
	CHECK(elements(app, "menuitem[command=\"file.save\"] .menu-arrow").empty());
	app.ui().move(std::nullopt, {"#menu-bar button[menu=edit-menu]", 0, std::nullopt});
	CHECK(!shown(app, "#file-menu"));
	REQUIRE(shown(app, "#edit-menu"));
	app.ui().move(std::nullopt, {"#menu-bar", 0, ui::Point{0, 0}});
	app.ui().key(std::nullopt, "Right");
	CHECK(!shown(app, "#edit-menu"));
	CHECK(shown(app, "#media-menu"));
	app.ui().key(std::nullopt, "Left");
	CHECK(shown(app, "#edit-menu"));
	app.ui().key(std::nullopt, "Escape");
	CHECK(!shown(app, "#edit-menu"));

#ifndef __APPLE__
	app.ui().key(std::nullopt, "Alt");
	CHECK(shown(app, "#file-menu"));
	app.ui().key(std::nullopt, "Alt");
	CHECK(!shown(app, "#file-menu"));
#endif
}

TEST_CASE("the menu bar shows and hides like the project window's strips, and File > Quit quits", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.shell().step(std::chrono::milliseconds(0)); // fit the toolbar before clicking Window Layout

	// Show Menu Bar, in the Set up Window Layout menu.
	click(app, "#window-layout");
	click(app, "menuitem[toggle=menuBar]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#menu-bar"); }));
	app.keys().run("window.menuBar", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#menu-bar"); }));

	CHECK(keysOf(app, "menuitem[command=\"file.quit\"]") == keyName("Mod+Q"));
	click(app, "#menu-bar button[menu=file-menu]");
	click(app, "menuitem[command=\"file.quit\"]");
	REQUIRE(app.waitUntil([&app] { return !app.shell().running(); }));
}

#ifdef _WIN32
namespace
{
	HWND hwndOf(ui::WindowId window)
	{
		return static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(ui::SdlWindowSystem::sdlWindow(window)),
			SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	}

	// The id of Show Menu Bar in the window's system menu, the context menu of its title bar; 0 without it.
	UINT showMenuBar(ui::WindowId window)
	{
		HMENU menu = GetSystemMenu(hwndOf(window), FALSE);
		for (int i = 0; i < GetMenuItemCount(menu); ++i)
		{
			std::array<wchar_t, 64> text{};
			GetMenuStringW(menu, static_cast<UINT>(i), text.data(), static_cast<int>(text.size()), MF_BYPOSITION);
			if (std::wstring(text.data()) == L"Show Menu Bar")
				return GetMenuItemID(menu, i);
		}
		return 0;
	}

	bool menuBarChecked(ui::WindowId window)
	{
		return (GetMenuState(GetSystemMenu(hwndOf(window), FALSE), showMenuBar(window), MF_BYCOMMAND) & MF_CHECKED) !=
			0;
	}
}
#endif

TEST_CASE("the mixer and editor windows have reduced menus whose menu bars show and hide (#444)", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const uint64_t track = app.call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
	const uint64_t part = app.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", 192000}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
	const auto shownIn = [&app](ui::WindowId window, const std::string& selector)
	{
		const Json found = app.ui().dom(window, selector, 0).at("elements");
		return found.size() == 1 && found[0].at("visible").get<bool>();
	};
	const auto clickIn = [&app](ui::WindowId window, const std::string& selector)
	{
		app.ui().click(window, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	};
	// Show Menu Bar in the window's title bar context menu, picked as the user would; elsewhere window.menuBar, which
	// it runs.
	const auto toggle = [&app](ui::WindowId window)
	{
#ifdef _WIN32
		REQUIRE(showMenuBar(window) != 0);
		SendMessageW(hwndOf(window), WM_SYSCOMMAND, showMenuBar(window), 0);
		app.shell().step(std::chrono::milliseconds(0));
#else
		app.keys().run("window.menuBar", Json::object(), *app.shell().find(window), nullptr);
#endif
	};
	// The menu bar, hidden at first, shows from the title bar's context menu, hides and shows again; Edit > Undo takes
	// back the track added last.
	const auto check = [&](ui::WindowId window)
	{
		REQUIRE(app.waitUntil([&] { return !app.ui().dom(window, "#menu-bar", 0).at("elements").empty(); }));
		CHECK(!shownIn(window, "#menu-bar"));
		toggle(window);
		REQUIRE(app.waitUntil([&] { return shownIn(window, "#menu-bar"); }));
#ifdef _WIN32
		CHECK(menuBarChecked(window));
#endif
		toggle(window);
		REQUIRE(app.waitUntil([&] { return !shownIn(window, "#menu-bar"); }));
#ifdef _WIN32
		CHECK(!menuBarChecked(window));
#endif
		toggle(window);
		REQUIRE(app.waitUntil([&] { return shownIn(window, "#menu-bar"); }));
		app.call("track.add", {{"type", "audio"}});
		REQUIRE(app.waitUntil([&] { return app.call("project.get").at("tracks").size() == 2; }));
		clickIn(window, "#menu-bar button[menu=edit-menu]");
		REQUIRE(shownIn(window, "#edit-menu"));
		REQUIRE(app.waitUntil(
			[&]
			{
				return app.ui().dom(window, "menuitem[command=\"history.undo\"][disabled]", 0).at("elements").empty();
			}));
		clickIn(window, "menuitem[command=\"history.undo\"]");
		REQUIRE(app.waitUntil([&] { return app.call("project.get").at("tracks").size() == 1; }));
	};

	app.keys().run("window.mixer", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return app.appUi().mixer(1) != nullptr; }));
	const ui::WindowId mixer = app.appUi().mixerWindow(1);
	check(mixer);
	// The mixer window's Set up Window Layout pane has Menu Bar too: GNUbase's own addition for platforms whose
	// title bar has no Show Menu Bar item (reference manual p. 55).
	clickIn(mixer, "#mixer-layout");
	clickIn(mixer, "button[toggle=menuBar]");
	REQUIRE(app.waitUntil([&] { return !shownIn(mixer, "#menu-bar"); }));
	CHECK(shownIn(app.main().id(), "#menu-bar")); // each window has its own

	app.call("editor.open", {{"editor", "window"}, {"parts", Json::array({part})}});
	REQUIRE(app.waitUntil([&app] { return app.appUi().editorWindow() != nullptr; }));
	check(app.appUi().editorWindowId());
}

TEST_CASE("the main menu refuses items with a command it doesn't know or parameters the command doesn't take (#446)",
	"[unit]")
{
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	ui::Window& window = fake.shell.open(ui::WindowSpec{});
	ui::EngineBinding binding(engine);
	ui::KeyCommands keys(engine, binding);
	keys.add({"view.zoomIn", "Zooms in.", {}});
	const auto load = [&window](const std::string& items)
	{ return window.context().LoadDocumentFromMemory("<rml><body><menu>" + items + "</menu></body></rml>"); };
	const std::string good =
		R"(<menuitem command="transport.play">Start</menuitem>)"
		R"(<menuitem command="transport.locate" parameters='{"target": "leftLocator"}'>L</menuitem>)"
		R"(<menuitem command="view.zoomIn">Zoom In</menuitem>)";

	Rml::ElementDocument* document = load(good);
	REQUIRE(document != nullptr);
	CHECK_NOTHROW(std::make_unique<ui::MainMenu>(keys, window, *document));
	document->Close();

	struct Bad
	{
		const char* item;
		const char* named; // in the message
	};
	const Bad bad[] = {
		{R"(<menuitem command="transport.ply">Start</menuitem>)", "transport.ply"},
		{R"(<menuitem command="">Nothing</menuitem>)", "no command"},
		{R"(<menuitem command="transport.locate" parameters='{"target": 5}'>L</menuitem>)", "target"},
		{R"(<menuitem command="transport.locate" parameters='{"traget": "leftLocator"}'>L</menuitem>)", "traget"},
		{R"(<menuitem command="view.zoomIn" parameters='{"factor": 2}'>Zoom In</menuitem>)", "view.zoomIn"},
		{R"(<menuitem command="transport.play" parameters='{"target"'>Start</menuitem>)", "JSON object"},
		{R"(<menuitem command="transport.play" parameters='[]'>Start</menuitem>)", "JSON object"},
	};
	for (const Bad& item : bad)
	{
		INFO(item.item);
		document = load(good + item.item);
		REQUIRE(document != nullptr);
		CHECK_THROWS_AS(std::make_unique<ui::MainMenu>(keys, window, *document), std::invalid_argument);
		CHECK_THROWS_WITH(std::make_unique<ui::MainMenu>(keys, window, *document), ContainsSubstring(item.named));
		// The document is as it was: no item got its keys.
		CHECK(document->QuerySelector(".menu-keys") == nullptr);
		document->Close();
	}
}

TEST_CASE(
	"the command items of every shipped document name commands that exist, with parameters they take (#446)", "[ui]")
{
	test::UiApp app({900, 640});
	// The documents whose menus and buttons run commands attach without throwing: the project window's menu bar and
	// transport bar, the transport panel and the time display window.
	app.appUi().showProject();
	app.appUi().toggleTransportPanel();
	CHECK(app.appUi().transportPanel() != nullptr);
	app.appUi().toggleTimeDisplay();
	CHECK(app.appUi().timeDisplay() != nullptr);

	// And so does every item in the documents and fragments in resources/ui, whatever attaches them.
	const ui::KeyCommands& keys = app.keys();
	int items = 0;
	int calls = 0;
	const std::regex uiCommand(R"(ui\.command\s*\{\s*command\s*=\s*['"]([^'"]*)['"]\s*\})");
	for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "ui"))
	{
		if (entry.path().extension() != ".rml")
			continue;
		INFO(entry.path().filename().u8string());
		const std::string text = test::readText(entry.path());
		const bool fragment = text.find("<rml") == std::string::npos; // such as transport.rml
		Rml::ElementDocument* document = app.main().context().LoadDocumentFromMemory(
			fragment ? std::string("<rml><body/></rml>") : text, entry.path().generic_u8string());
		REQUIRE(document != nullptr);
		if (fragment)
			document->SetInnerRML(text);
		Rml::ElementList found;
		document->QuerySelectorAll(found, "[command]");
		for (const Rml::Element* element : found)
		{
			const auto command = element->GetAttribute<Rml::String>("command", "");
			const auto parameters = element->GetAttribute<Rml::String>("parameters", "{}");
			INFO(command << " " << parameters);
			CHECK_NOTHROW(keys.check(command, Json::parse(parameters, nullptr, false)));
			++items;
		}
		document->Close();

		// Buttons that run a key command from Lua: gnubase.ui.command{command = 'project.addTrack'}.
		for (auto call = std::sregex_iterator(text.begin(), text.end(), uiCommand); call != std::sregex_iterator();
			++call)
		{
			const std::string command = (*call)[1].str();
			INFO(command);
			CHECK_NOTHROW(keys.check(command, Json::object()));
			++calls;
		}
	}
	CHECK(items > 100);
	CHECK(calls >= 3);
}

TEST_CASE("a transport window whose controls are refused closes again (#669)", "[ui]")
{
	struct Broken
	{
		const char* file; // in resources/ui
		const char* from;
		const char* to;
		bool panel; // the transport panel, otherwise the time display window
	};
	const Broken broken[] = {
		// A button with a command that doesn't exist, in the transport controls that fill the transport panel.
		{"transport.rml", R"(command="locate.nextMarker")", R"(command="locate.nextMarkr")", true},
		// A menu item that gives a UI command parameters, in the time display window's context menu.
		{"time-display.rml", R"(command="timeDisplay.increaseSize")",
			R"(command="timeDisplay.increaseSize" parameters='{"steps": 2}')", false},
	};
	for (const Broken& each : broken)
	{
		INFO(each.file);
		const test::TempDir folder;
		const std::filesystem::path resources = test::patchedResources(folder.path(), each.file, each.from, each.to);

		// The project window isn't shown: the broken transport.rml would refuse its transport bar too.
		test::UiApp app({900, 640}, resources);
		app::AppUi& appUi = app.appUi();
		const auto toggleBroken = [&appUi, &each]
		{
			if (each.panel)
				appUi.toggleTransportPanel();
			else
				appUi.toggleTimeDisplay();
		};
		const auto brokenShown = [&appUi, &each]
		{ return each.panel ? appUi.transportPanel() != nullptr : appUi.timeDisplay() != nullptr; };

		// Twice, as F2 pressed again would: the window opens, its controls are refused and it closes again.
		for (int attempt = 0; attempt < 2; ++attempt)
		{
			toggleBroken();
			app.shell().step(std::chrono::milliseconds(0));
			CHECK(!brokenShown());
			CHECK(app.shell().windows().size() == 1);
		}

		// The other window, whose controls are fine, opens from the same copy.
		if (each.panel)
			appUi.toggleTimeDisplay();
		else
			appUi.toggleTransportPanel();
		app.shell().step(std::chrono::milliseconds(0));
		CHECK((each.panel ? appUi.timeDisplay() != nullptr : appUi.transportPanel() != nullptr));
		CHECK(app.shell().windows().size() == 2);
	}
}

TEST_CASE("Audio Advanced slice menu entries follow the audio selection and edit with undo", "[ui]")
{
	struct Case
	{
		const char* command;
		int64_t length;
		int64_t expected;
	};
	for (const Case& each : {Case{"audio.closeGapsCrossfade", 48000, 96480},
			 Case{"audio.closeGapsTimeStretch", 48000, 96000}, Case{"audio.deleteOverlaps", 120000, 96000}})
	{
		CAPTURE(each.command);
		const test::TempDir dir;
		test::Audio audio;
		audio.samples.assign(144000, 0.25F);
		const auto path = dir.path() / "slices.wav";
		test::writeWav(path, audio);
		test::UiApp app({1000, 760});
		auto& display = app.appUi().showProject();
		app.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
		const Json track = app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
		const Json media = app.call("media.add", {{"path", path.u8string()}}).at("media").at("id");
		const Json clip = app.call("clip.add", {{"media", media}}).at("clip").at("id");
		const auto add = [&](int64_t position, int64_t offset, int64_t length)
		{
			return app
				.call("audioEvent.add",
					{{"track", track}, {"clip", clip}, {"position", position}, {"offset", offset}, {"length", length}})
				.at("event")
				.at("id")
				.get<uint64_t>();
		};
		const uint64_t first = add(0, 0, each.length);
		const uint64_t second = add(96000, 48000, 24000);
		add(240000, 0, 24000); // An unselected event of the same clip must stay unchanged.
		const Json midiTrack = app.call("track.add", {{"type", "midi"}}).at("track").at("id");
		const uint64_t midi = app.call("midiPart.add", {{"track", midiTrack}, {"position", 0}, {"length", 960}})
								  .at("part")
								  .at("id")
								  .get<uint64_t>();
		REQUIRE(app.waitUntil(
			[&]
			{
				return display.project().at("audioEvents").size() == 3 && display.project().at("midiParts").size() == 1;
			}));
		const std::string item = "menuitem[command=\"" + std::string(each.command) + "\"]";
		const auto openMenu = [&]
		{
			click(app, "#menu-bar button[menu=audio-menu]");
			app.ui().move(std::nullopt, {"#audio-advanced-submenu", 0, std::nullopt});
			REQUIRE(app.waitUntil([&] { return shown(app, item); }));
		};
		display.select({});
		openMenu();
		CHECK(disabled(app, item));
		click(app, "#menu-bar button[menu=audio-menu]");
		display.select({midi});
		openMenu();
		CHECK(disabled(app, item));
		click(app, "#menu-bar button[menu=audio-menu]");
		display.select({first, second, midi});
		openMenu();
		CHECK(!disabled(app, item));
		const Json before = app.call("project.get");
		const size_t steps = app.call("history.get").at("steps").size();
		click(app, item);
		REQUIRE(app.waitUntil([&] { return display.project().at("audioEvents")[0].at("length") == each.expected; }));
		const Json after = app.call("project.get");
		CHECK(after.at("audioEvents")[0].at("position") == 0);
		CHECK(after.at("audioEvents")[1].at("position") == 96000);
		CHECK(after.at("audioEvents")[1].at("offset") == 48000);
		CHECK(after.at("audioEvents")[1].at("length") == 24000);
		if (std::string(each.command) == "audio.closeGapsCrossfade")
		{
			CHECK(after.at("audioEvents")[1].at("crossfade").at("from") == first);
		}
		else
		{
			CHECK(after.at("audioEvents")[1] == before.at("audioEvents")[1]);
		}
		CHECK(after.at("audioEvents")[2] == before.at("audioEvents")[2]);
		CHECK(after.at("midiParts") == before.at("midiParts"));
		CHECK(app.call("history.get").at("steps").size() == steps + 1);
		app.call("history.undo");
		CHECK(app.call("project.get") == before);
		app.call("history.redo");
		CHECK(app.call("project.get") == after);
	}
}

TEST_CASE("Audio Advanced edits hitpoint slices in the selected audio part", "[ui]")
{
	struct Case
	{
		const char* command;
		int64_t length;
		int64_t expected;
	};
	for (const Case& each : {Case{"audio.closeGapsCrossfade", 12000, 24480},
			 Case{"audio.closeGapsTimeStretch", 12000, 24000}, Case{"audio.deleteOverlaps", 30000, 24000}})
	{
		CAPTURE(each.command);
		const test::TempDir dir;
		test::Audio audio;
		audio.samples.assign(96000, 0.25F);
		const auto file = dir.path() / "slices.wav";
		test::writeWav(file, audio);
		test::UiApp app({1000, 760});
		auto& display = app.appUi().showProject();
		app.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
		const Json track = app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
		const Json media = app.call("media.add", {{"path", file.u8string()}}).at("media").at("id");
		const Json clip = app.call("clip.add", {{"media", media}}).at("clip").at("id");
		const Json event =
			app.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}}).at("event").at("id");
		Json points = Json::array();
		for (const int64_t frame : {24000, 48000, 72000})
			points.push_back({{"position", frame}, {"peak", 0.0}, {"intensity", 1.0}});
		app.call("clip.setHitpoints", {{"clip", clip}, {"hitpoints", points}});
		const uint64_t part =
			app.call("event.fromHitpoints", {{"event", event}, {"create", "slices"}}).at("events")[0].get<uint64_t>();
		const Json slices = app.call("project.get").at("audioEvents");
		REQUIRE(slices.size() == 4);
		for (size_t i = 0; i < 3; ++i)
			app.call("audioEvent.set", {{"event", slices[i].at("id")}, {"length", each.length}});
		app.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 200000}, {"length", 12000}});
		REQUIRE(app.waitUntil([&] { return display.project().at("audioEvents").size() == 5; }));
		display.select({part});
		app.ui().click(
			std::nullopt, {"#menu-bar button[menu=audio-menu]", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.ui().move(std::nullopt, {"#audio-advanced-submenu", 0, std::nullopt});
		const std::string item = "menuitem[command=\"" + std::string(each.command) + "\"]";
		REQUIRE(app.waitUntil(
			[&] { return app.ui().dom(std::nullopt, item, 0).at("elements")[0].at("visible").get<bool>(); }));
		const Json shown = app.ui().dom(std::nullopt, item, 0).at("elements")[0];
		REQUIRE(!shown.at("attributes").contains("disabled"));
		const Json before = app.call("project.get");
		const size_t steps = app.call("history.get").at("steps").size();
		const Json clicked = app.ui().click(std::nullopt, {item, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		const bool edited =
			app.waitUntil([&] { return display.project().at("audioEvents")[0].at("length") == each.expected; });
		INFO(clicked.dump());
		INFO(app.call("engine.commandLog").dump());
		REQUIRE(edited);
		const Json after = app.call("project.get");
		for (size_t i = 0; i < 3; ++i)
		{
			CHECK(after.at("audioEvents")[i].at("part") == part);
			CHECK(after.at("audioEvents")[i].at("length") == each.expected);
		}
		CHECK(after.at("audioEvents")[3].at("length") == 24000);
		CHECK(after.at("audioEvents")[4] == before.at("audioEvents")[4]);
		CHECK(after.at("audioParts") == before.at("audioParts"));
		CHECK(app.call("history.get").at("steps").size() == steps + 1);
		app.call("history.undo");
		CHECK(app.call("project.get") == before);
		app.call("history.redo");
		CHECK(app.call("project.get") == after);
	}
}

namespace
{
	// Holds the job after its first rendered slice, so progress and cleanup are checked with a file already written.
	class RenderedSliceHold
	{
	public:
		explicit RenderedSliceHold(engine::Engine& engine) : m_engine(engine)
		{
			m_engine.setJobProgressHook(
				[state = m_state](uint64_t job, const std::string& kind)
				{
					if (kind == "event.closeGaps" && ++state->calls == 2)
					{
						std::unique_lock lock(state->mutex);
						state->job = job;
						state->changed.wait(lock, [&] { return state->released; });
					}
				});
		}
		~RenderedSliceHold()
		{
			release();
			m_engine.setJobProgressHook({});
		}
		RenderedSliceHold(const RenderedSliceHold&) = delete;
		RenderedSliceHold& operator=(const RenderedSliceHold&) = delete;
		RenderedSliceHold(RenderedSliceHold&&) = delete;
		RenderedSliceHold& operator=(RenderedSliceHold&&) = delete;
		uint64_t job() const { return m_state->job.load(); }
		void release()
		{
			{
				const std::scoped_lock lock(m_state->mutex);
				m_state->released = true;
			}
			m_state->changed.notify_all();
		}

	private:
		struct State
		{
			int calls = 0; // job thread only
			std::atomic<uint64_t> job{0};
			std::mutex mutex;
			std::condition_variable changed;
			bool released = false; // guarded by mutex
		};
		engine::Engine& m_engine;
		std::shared_ptr<State> m_state = std::make_shared<State>();
	};
}

TEST_CASE("Audio Advanced shows Close Gaps progress and Cancel until the job ends", "[ui]")
{
	const bool cancel = GENERATE(false, true);
	const test::TempDir dir;
	test::Audio audio;
	audio.samples.assign(48000, 0.25F);
	const auto file = dir.path() / "slices.wav";
	test::writeWav(file, audio);
	test::UiApp app({1000, 760});
	auto& display = app.appUi().showProject();
	app.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	const Json track = app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const Json media = app.call("media.add", {{"path", file.u8string()}}).at("media").at("id");
	const Json clip = app.call("clip.add", {{"media", media}}).at("clip").at("id");
	std::vector<uint64_t> events;
	for (const int64_t position : {0, 24000, 48000})
		events.push_back(
			app.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", position}, {"length", 12000}})
				.at("event")
				.at("id")
				.get<uint64_t>());
	REQUIRE(app.waitUntil([&] { return display.project().at("audioEvents").size() == 3; }));
	display.select(events);
	const Json before = app.call("project.get");
	const Json history = app.call("history.get");
	RenderedSliceHold held(app.engine());
	click(app, "#menu-bar button[menu=audio-menu]");
	app.ui().move(std::nullopt, {"#audio-advanced-submenu", 0, std::nullopt});
	click(app, "menuitem[command=audio.closeGapsTimeStretch]");
	REQUIRE(app.waitUntil([&] { return held.job() != 0; }, std::chrono::seconds(30)));
	REQUIRE(app.waitUntil([&] { return shown(app, "#render-progress-dialog"); }));
	// Job events can still be waiting after every command result was handled.
	REQUIRE(app.waitUntil([&]
		{ return std::stod(element(app, "#render-progress").at("attributes").at("value").get<std::string>()) > 0.0; }));
	CHECK(std::stod(element(app, "#render-progress").at("attributes").at("value").get<std::string>()) > 0.0);
	CHECK_THAT(element(app, "#render-progress-file").at("text").get<std::string>(), ContainsSubstring("Closing gaps"));
	CHECK_THAT(element(app, "#render-progress-message").at("text").get<std::string>(), ContainsSubstring("wait"));
	const auto waveFiles = [&]
	{
		size_t files = 0;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(dir.path()))
			if (entry.is_regular_file() && entry.path().extension() == ".wav")
				++files;
		return files;
	};
	CHECK(waveFiles() == 2); // The source and the first rendered slice.
	if (cancel)
	{
		click(app, "#render-progress-dialog button[result=cancel]");
		REQUIRE(app.waitUntil(
			[&]
			{
				const Json commands = app.call("engine.commandLog").at("commands");
				return std::any_of(commands.begin(), commands.end(),
					[&](const Json& command)
					{
						return command.at("command") == "job.cancel" &&
							command.at("parameters").at("job") == held.job();
					});
			}));
	}
	held.release();
	engine::Client observer(app.engine());
	const Json finished = test::waitForJob(observer, held.job());
	CHECK(finished.at("state") == (cancel ? "cancelled" : "done"));
	REQUIRE(app.waitUntil([&] { return !shown(app, "#render-progress-dialog"); }));
	if (cancel)
	{
		CHECK(app.call("project.get") == before);
		CHECK(app.call("history.get") == history);
		CHECK(waveFiles() == 1);
	}
	else
	{
		CHECK(app.call("project.get") != before);
		app.call("history.undo");
		CHECK(app.call("project.get") == before);
	}
}

TEST_CASE("Audio Advanced reports a failed Close Gaps time stretch job", "[ui]")
{
	for (const bool finishBeforeResult : {false, true})
	{
		CAPTURE(finishBeforeResult);
		const test::TempDir dir;
		test::Audio audio;
		audio.samples.assign(48000, 0.25F);
		const auto file = dir.path() / "slices.wav";
		test::writeWav(file, audio);
		test::UiApp app({1000, 760});
		auto& display = app.appUi().showProject();
		app.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
		const Json track = app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
		const Json media = app.call("media.add", {{"path", file.u8string()}}).at("media").at("id");
		const Json clip = app.call("clip.add", {{"media", media}}).at("clip").at("id");
		std::vector<uint64_t> events;
		for (const int64_t position : {0, 48000})
			events.push_back(
				app.call("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", position}, {"length", 1000}})
					.at("event")
					.at("id")
					.get<uint64_t>());
		REQUIRE(app.waitUntil([&] { return display.project().at("audioEvents").size() == 2; }));
		display.select(events);
		click(app, "#menu-bar button[menu=audio-menu]");
		app.ui().move(std::nullopt, {"#audio-advanced-submenu", 0, std::nullopt});
		const std::string item = "menuitem[command=audio.closeGapsTimeStretch]";
		REQUIRE(app.waitUntil([&] { return shown(app, item); }));
		const Json before = app.call("project.get");
		const Json history = app.call("history.get");
		test::HeldJob held(app.engine(), "event.closeGaps");
		app.ui().click(std::nullopt, {item, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		REQUIRE(held.waitUntilHeld());
		if (finishBeforeResult)
		{
			// No UI update until the job has failed: its command result is still waiting.
			CHECK(app.appUi().binding().waiting("event.closeGaps") == 1);
		}
		else
		{
			REQUIRE(app.waitUntil([&] { return app.appUi().binding().waiting() == 0; }));
			CHECK(!shown(app, "#edit-refused"));
		}
		CHECK(held.release());
		engine::Client observer(app.engine());
		const Json finished = test::waitForJob(observer, held.job());
		REQUIRE(finished.at("state") == "failed");
		CHECK_THAT(finished.at("error").get<std::string>(), ContainsSubstring("ratio"));
		REQUIRE(app.waitUntil(
			[&]
			{
				return shown(app, "#edit-refused") &&
					element(app, "#edit-refused").value("text", std::string()).find("ratio") != std::string::npos;
			}));
		CHECK_THAT(element(app, "#edit-refused").value("text", std::string()), ContainsSubstring("ratio"));
		REQUIRE(app.waitUntil([&] { return !shown(app, "#render-progress-dialog"); }));
		CHECK(!shown(app, "#project-error-dialog"));
		CHECK(app.call("project.get") == before);
		CHECK(app.call("history.get") == history);
	}
}
