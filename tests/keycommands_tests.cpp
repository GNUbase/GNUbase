// SPDX-License-Identifier: GPL-3.0-or-later
// Key commands (#57): keys bound to the engine's commands and to the UI's own, written the same way on every platform,
// and run for the keys that no element used, by the focus zone they were pressed in. The UI tests press keys through
// ui::Automation, as MCP's ui_key does, and through SDL's own events.
#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "ui/keycommands.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;
using ui::Platform;
using Strings = std::vector<std::string>;

namespace
{
	ui::Target at(const char* selector)
	{
		return {selector, 0, std::nullopt};
	}

	void click(test::UiApp& app, const char* selector)
	{
		app.ui().click(std::nullopt, at(selector), ui::MouseButton::Left, 1, {});
	}

	void press(test::UiApp& app, const char* keys)
	{
		app.ui().key(std::nullopt, keys);
	}

	bool playing(test::UiApp& app)
	{
		return app.call("transport.get").at("playing").get<bool>();
	}

	// The last command that changed something, as the engine's command log has it.
	Json lastCommand(test::UiApp& app)
	{
		return app.call("engine.commandLog").at("commands").back();
	}

	// UI commands that record where they ran: their id, and the id of the zone they got.
	struct Recorder
	{
		explicit Recorder(ui::KeyCommands& keys)
		{
			for (const char* id : {"test.project", "test.editor", "test.lower", "test.anywhere"})
				keys.add({id, "Records that it ran.", [this, id](ui::Window& /*window*/, Rml::Element* zone)
					{
						std::string what = id;
						if (zone != nullptr)
							what += " in " + zone->GetId();
						ran.push_back(std::move(what));
					}});
		}

		// What ran since the last call.
		Strings take() { return std::exchange(ran, {}); }

		Strings ran;
	};
}

TEST_CASE("keys are written the same way on every platform, Mod being Cmd on macOS and Ctrl elsewhere", "[unit]")
{
	using namespace Rml::Input;
	CHECK(ui::parseKeys("Mod+S", Platform::Windows) == ui::KeyPress{KI_S, KM_CTRL});
	CHECK(ui::parseKeys("Mod+S", Platform::Linux) == ui::KeyPress{KI_S, KM_CTRL});
	CHECK(ui::parseKeys("Mod+S", Platform::Mac) == ui::KeyPress{KI_S, KM_META});
	// Ctrl is the Control key everywhere, as in the reference's few bindings with a real Ctrl on macOS.
	CHECK(ui::parseKeys("Ctrl+Alt+Num6", Platform::Mac) == ui::KeyPress{KI_NUMPAD6, KM_CTRL | KM_ALT});
	CHECK(ui::parseKeys("Opt+Cmd+Q", Platform::Windows) == ui::KeyPress{KI_Q, KM_ALT | KM_META});
	CHECK(ui::parseKeys("shift+num+", Platform::Linux) == ui::KeyPress{KI_ADD, KM_SHIFT});
	CHECK(ui::parseKeys("Num+") == ui::KeyPress{KI_ADD, 0});
	CHECK(ui::parseKeys("Alt+.") == ui::KeyPress{KI_OEM_PERIOD, KM_ALT});
	CHECK(ui::parseKeys("Space") == ui::KeyPress{KI_SPACE, 0});
	CHECK_THROWS_WITH(ui::parseKeys("Hyper+Q"), ContainsSubstring("Hyper"));
	CHECK_THROWS_WITH(ui::parseKeys("Ctrl+Flux"), ContainsSubstring("Flux"));
	CHECK_THROWS(ui::parseKeys(""));

	// Names as each platform writes them: modifiers in the order Ctrl, Alt, Shift, Cmd.
	CHECK(ui::keyName(ui::parseKeys("Shift+Mod+Z", Platform::Windows), Platform::Windows) == "Ctrl+Shift+Z");
	CHECK(ui::keyName(ui::parseKeys("Shift+Mod+Z", Platform::Linux), Platform::Linux) == "Ctrl+Shift+Z");
	CHECK(ui::keyName(ui::parseKeys("Shift+Mod+Z", Platform::Mac), Platform::Mac) == "Shift+Cmd+Z");
	CHECK(ui::keyName(ui::parseKeys("Alt+Ctrl+Num+", Platform::Mac), Platform::Mac) == "Ctrl+Opt+Num+");
	for (const char* keys : {"A", "Z", "0", "9", "F1", "F24", "Num0", "Num9", "NumEnter", "Num*", "Num/", "Num-",
			 "Num.", ".", ",", "-", "+", "Enter", "Escape", "Tab", "Backspace", "Delete", "Insert", "Home", "End",
			 "PageUp", "PageDown", "Up", "Down", "Left", "Right", "Space", "Shift++"})
	{
		INFO(keys);
		CHECK(ui::keyName(ui::parseKeys(keys, Platform::Windows), Platform::Windows) == keys);
	}
}

TEST_CASE("the default key map binds the engine's commands as the reference does, per platform", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	for (const Platform platform : {Platform::Windows, Platform::Mac, Platform::Linux})
	{
		// Binding checks every command and its parameters against the engine's schemas.
		const ui::KeyCommands keys(engine, binding, platform);
		CHECK(keys.keysFor("transport.toggle") == Strings{"Space"});
		CHECK(keys.keysFor("transport.stop") == Strings{"Num0"});
		CHECK(keys.keysFor("transport.locate", {{"target", "leftLocator"}}) == Strings{"Num1"});
		CHECK(keys.keysFor("marker.locate", {{"number", 3}}) == Strings{"Shift+3", "Num3"});
		CHECK(keys.keysFor("transport.locate").empty()); // not without parameters
		const Strings redo = keys.keysFor("history.redo");
		CHECK(redo == (platform == Platform::Mac ? Strings{"Shift+Cmd+Z"} : Strings{"Ctrl+Shift+Z"}));
	}

	// No two bindings share keys in a zone.
	std::set<std::pair<std::string, ui::KeyPress>> bound;
	for (const ui::KeyBinding& keys : ui::defaultKeyMap())
	{
		INFO(keys.keys);
		CHECK(bound.emplace(keys.zone, ui::parseKeys(keys.keys)).second);
	}
}

TEST_CASE("key commands take the UI's own commands, and bindings replace what the keys did", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	ui::KeyCommands keys(engine, binding, Platform::Windows);
	int zoomed = 0;
	keys.add({"view.zoomIn", "Zooms in.", [&zoomed](ui::Window& /*window*/, Rml::Element* /*zone*/) { ++zoomed; }});
	CHECK_THROWS_WITH(keys.add({"view.zoomIn", "Again.", {}}), ContainsSubstring("view.zoomIn"));
	CHECK_THROWS_WITH(keys.add({"transport.play", "An engine command's id.", {}}), ContainsSubstring("transport.play"));

	keys.bind({"H", "view.zoomIn"});
	keys.bind({"Mod+Num+", "view.zoomIn", Json::object(), "editor"});
	CHECK(keys.keysFor("view.zoomIn") == Strings{"H", "Ctrl+Num+"});
	keys.bind({"H", "transport.play"});
	CHECK(keys.keysFor("view.zoomIn") == Strings{"Ctrl+Num+"});
	CHECK(keys.keysFor("transport.play") == Strings{"NumEnter", "H"});

	CHECK_THROWS_WITH(keys.bind({"F5", "transport.fly"}), ContainsSubstring("transport.fly"));
	CHECK_THROWS_WITH(keys.bind({"F5", "transport.locate", {{"target", 5}}}), ContainsSubstring("target"));
	CHECK_THROWS_WITH(keys.bind({"Flux", "transport.play"}), ContainsSubstring("Flux"));
	CHECK(keys.keysFor("transport.locate", {{"target", 5}}).empty());
}

TEST_CASE("keys pressed in GNUbase's UI run the engine's commands from the default key map", "[ui]")
{
	test::UiApp app;
	app.show("keycommands.rml");
	app.call("transport.set", {{"left", 48000}, {"right", 96000}});

	press(app, "Space");
	REQUIRE(app.waitUntil([&app] { return playing(app); }));
	press(app, "Num0");
	REQUIRE(app.waitUntil([&app] { return !playing(app); }));
	press(app, "Num2");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("position") == 96000; }));

	// Mod is the platform's command key, so the same keys undo everywhere. The UI sent the command through its
	// engine client, as for a click on a button.
	app.call("track.add", {{"type", "audio"}});
	press(app, "Mod+Z");
	REQUIRE(app.waitUntil([&app] { return app.call("history.get").at("position") == 0; }));
	CHECK(lastCommand(app).at("command") == "history.undo");
	CHECK(lastCommand(app).at("client") == "ui");
}

TEST_CASE("the same keys run the command of the focus zone they're pressed in", "[ui]")
{
	test::UiApp app;
	app.show("keycommands.rml");
	ui::KeyCommands& keys = app.keys();
	Recorder recorder(keys);
	keys.bind({"Up", "test.project", Json::object(), "project"});
	keys.bind({"Up", "test.editor", Json::object(), "editor"});
	keys.bind({"Down", "test.lower", Json::object(), "lower"});
	keys.bind({"F5", "test.anywhere"});

	click(app, "#project");
	press(app, "Up");
	press(app, "Down");
	press(app, "F5");
	CHECK(recorder.take() == Strings{"test.project in project", "test.anywhere in project"});

	// A zone inside another: its own bindings first, then the outer zone's, then those that apply everywhere, which
	// get the innermost zone.
	click(app, "#editor");
	press(app, "Up");
	press(app, "Down");
	press(app, "F5");
	CHECK(recorder.take() == Strings{"test.editor in editor", "test.lower in lower", "test.anywhere in editor"});

	click(app, "#meters"); // in the lower zone, no zone of its own
	press(app, "Up");
	press(app, "Down");
	CHECK(recorder.take() == Strings{"test.lower in lower"});

	click(app, "#outside");
	press(app, "Up");
	press(app, "F5");
	CHECK(recorder.take() == Strings{"test.anywhere"});

	// Lock keys don't change what a key does.
	using namespace Rml::Input;
	CHECK(keys.press(app.main(), {KI_F5, KM_NUMLOCK | KM_CAPSLOCK}));
	CHECK_FALSE(keys.press(app.main(), {KI_F5, KM_SHIFT}));
	CHECK(recorder.take() == Strings{"test.anywhere"});

	// Keys from the OS take the same way.
	SDL_Event event{};
	event.type = SDL_EVENT_KEY_DOWN;
	event.key.windowID = app.main().id();
	event.key.key = SDLK_F5;
	event.key.scancode = SDL_SCANCODE_F5;
	event.key.down = true;
	REQUIRE(SDL_PushEvent(&event));
	REQUIRE(app.waitUntil([&recorder] { return !recorder.ran.empty(); }));
	CHECK(recorder.take() == Strings{"test.anywhere"});
}

TEST_CASE("elements keep the keys they use, and a modal dialog keeps every key", "[ui]")
{
	test::UiApp app;
	app.show("keycommands.rml");
	Recorder recorder(app.keys());
	app.keys().bind({"F5", "test.anywhere"});

	// A text field keeps what's typed into it, and a focused button is clicked by Space: neither starts playback.
	click(app, "#name");
	press(app, "Space");
	press(app, "F5");
	click(app, "#apply");
	press(app, "Space");
	press(app, "Num1"); // a later command, to wait for
	REQUIRE(app.waitUntil([&app] { return lastCommand(app).at("command") == "transport.locate"; }));
	CHECK_FALSE(playing(app));
	CHECK(recorder.take().empty());

	// The button doesn't use F5.
	press(app, "F5");
	CHECK(recorder.take() == Strings{"test.anywhere"});

	// A focused knob turns with the arrow keys, which then don't run their command.
	app.keys().bind({"Up", "test.anywhere"});
	click(app, "#pan");
	press(app, "Up");
	CHECK(app.ui().dom(std::nullopt, "#pan", 0).at("elements")[0].at("attributes").at("value") == "1");
	CHECK(recorder.take().empty());

	// No key runs a command while a modal dialog has the focus, so typing into it never edits the project.
	Rml::ElementDocument* dialog =
		app.main().context().LoadDocument(std::string(GNUBASE_TEST_DATA_DIR) + "/ui/automation-dialog.rml");
	REQUIRE(dialog != nullptr);
	dialog->Show(Rml::ModalFlag::Modal);
	press(app, "F5");
	click(app, "#ok");
	press(app, "F5");
	CHECK(recorder.take().empty());
	dialog->Close();
	app.shell().step(std::chrono::milliseconds(0));
	click(app, "#outside");
	press(app, "F5");
	CHECK(recorder.take() == Strings{"test.anywhere"});
}
