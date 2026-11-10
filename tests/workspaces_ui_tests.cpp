// SPDX-License-Identifier: GPL-3.0-or-later
// Workspaces in the UI (#163, docs/features/workspaces.md): the windows that ui.layout keeps with their bounds and
// monitors, recalled on a machine that lacks a monitor (on the fake window system), and the Workspaces menu, the New
// Workspace dialog and the organizer in the app, driven with ui::Automation as MCP's UI tools drive them.
#include "uiapp.h"

#include "engine/engine.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/docking.h"
#include "ui/enginebinding.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	ui::Display leftMonitor()
	{
		return {1, "Left", {0, 0, 1920, 1080}, {0, 0, 1920, 1040}, 1.0F};
	}

	ui::Display rightMonitor()
	{
		return {2, "Right", {1920, 0, 2560, 1440}, {1920, 0, 2560, 1400}, 1.0F};
	}

	bool inside(const ui::Rect& window, const ui::Rect& area)
	{
		return window.x >= area.x && window.y >= area.y && window.x + window.width <= area.x + area.width &&
			window.y + window.height <= area.y + area.height;
	}

	// The docking system on the fake window system with two monitors, the main window as "project" and a tool window
	// that opens and closes as ui.layout says, at the bounds the docking system has for it. layout is ui.layout from
	// the last session, which the docking system starts on.
	struct Fixture
	{
		explicit Fixture(const Json& layout = Json::object())
			: laidOut(layout.empty() || client.call("settings.set", {{"key", "ui.layout"}, {"value", layout}}).ok())
		{
			docking.addWindow("project", [this] { return main; });
			docking.addWindow(
				"tool", [this] { return tool; },
				[this](bool open)
				{
					if (!open)
					{
						ui.shell.close(tool);
						tool = 0;
						return;
					}
					ui::WindowSpec spec;
					spec.size = {600, 400};
					spec.bounds = docking.bounds("tool");
					tool = ui.shell.open(spec).id();
				});
		}

		// Steps until done() is true, as the app's event loop does.
		bool waitUntil(const std::function<bool()>& done)
		{
			const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!done())
			{
				if (std::chrono::steady_clock::now() > end)
					return false;
				binding.update();
				ui.shell.step(std::chrono::milliseconds(1));
			}
			return true;
		}

		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			const engine::Result result = client.call(command, parameters);
			INFO(command << " failed: " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		Json saved() { return call("settings.get", {{"key", "ui.layout"}}).at("value"); }

		engine::Engine engine;
		engine::Client client{engine};
		bool laidOut;
		test::FakeUi ui{{leftMonitor(), rightMonitor()}};
		ui::WindowId main = ui.shell.open({}).id();
		ui::WindowId tool = 0;
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		ui::Docking docking{
			engine, binding, ui.shell, std::filesystem::path(GNUBASE_RESOURCES_DIR) / "ui" / "panel-window.rml"};
	};
}

TEST_CASE("a workspace keeps the open windows with their bounds and monitors, and puts them onto a connected monitor",
	"[unit]")
{
	Fixture f;
	// Nothing goes into ui.layout for windows that haven't opened, moved or resized.
	f.docking.saveWindows();
	CHECK(f.docking.layout() == Json::object());

	// The project window on the left monitor, the tool window open on the right one.
	f.ui.windows.drag(f.main, {100, 50, 1200, 800});
	f.call("settings.set",
		{{"key", "ui.layout"}, {"value", {{"windows", {{"tool", {{"open", true}}}}}}}}); // as a workspace opens it
	REQUIRE(f.waitUntil([&f] { return f.tool != 0; }));
	f.ui.windows.drag(f.tool, {2200, 300, 900, 700});
	f.docking.saveWindows();
	REQUIRE(f.waitUntil([&f] { return f.saved() == f.docking.layout(); }));
	const Json windows = f.saved().at("windows");
	CHECK(windows.at("project") ==
		Json{{"bounds", {{"x", 100}, {"y", 50}, {"width", 1200}, {"height", 800}}}, {"monitor", "Left"}});
	CHECK(windows.at("tool") ==
		Json{{"open", true}, {"bounds", {{"x", 2200}, {"y", 300}, {"width", 900}, {"height", 700}}},
			{"monitor", "Right"}});
	f.call("workspace.add", {{"name", "Two Monitors"}, {"type", "global"}});

	// The tool window closes, and the project window moves onto the right monitor.
	f.ui.shell.close(f.tool);
	f.ui.windows.drag(f.main, {2000, 100, 1200, 800});
	f.docking.saveWindows();
	REQUIRE(f.waitUntil([&f] { return f.saved().at("windows").at("tool").at("open") == false; }));
	CHECK(f.saved().at("windows").at("project").at("monitor") == "Right");

	SECTION("with both monitors, the windows go back where they were")
	{
		f.call("workspace.activate", {{"type", "global"}, {"name", "Two Monitors"}});
		REQUIRE(f.waitUntil([&f] { return f.tool != 0 && f.ui.shell.find(f.tool) != nullptr; }));
		CHECK(f.ui.shell.bounds(f.tool) == ui::Rect{2200, 300, 900, 700});
		CHECK(f.ui.shell.bounds(f.main) == ui::Rect{100, 50, 1200, 800});
	}
	SECTION("without the right monitor, the tool window opens on the left one, fully visible")
	{
		f.ui.windows.setDisplays({leftMonitor()});
		f.call("workspace.activate", {{"type", "global"}, {"name", "Two Monitors"}});
		REQUIRE(f.waitUntil([&f] { return f.tool != 0 && f.ui.shell.find(f.tool) != nullptr; }));
		CHECK(inside(f.ui.shell.bounds(f.tool), leftMonitor().usable));
		CHECK(f.ui.shell.bounds(f.tool).size() == ui::Size{900, 700});
		CHECK(f.ui.shell.bounds(f.main) == ui::Rect{100, 50, 1200, 800});
	}
	SECTION("a window that is open closes when the workspace doesn't have it open")
	{
		f.ui.windows.setDisplays({leftMonitor()});
		f.call("workspace.add", {{"name", "Closed"}, {"type", "project"}});
		f.call("workspace.activate", {{"type", "global"}, {"name", "Two Monitors"}});
		REQUIRE(f.waitUntil([&f] { return f.tool != 0; }));
		f.call("workspace.activate", {{"type", "project"}, {"name", "Closed"}});
		REQUIRE(f.waitUntil([&f] { return f.tool == 0; }));
		// The project window was on the right monitor, which is gone: it comes onto the left one, fully visible.
		CHECK(inside(f.ui.shell.bounds(f.main), leftMonitor().usable));
	}
}

TEST_CASE(
	"a window that ui.layout has open from the last session but is closed now isn't open in a new workspace", "[unit]")
{
	Fixture f({{"windows", {{"tool", {{"open", true}}}}}});
	REQUIRE(f.laidOut);
	CHECK(f.tool == 0);
	REQUIRE(f.waitUntil([&f] { return f.saved().at("windows").at("tool").at("open") == false; }));
	f.call("workspace.add", {{"name", "Now"}, {"type", "global"}});
	CHECK(f.call("workspace.get").at("global").at(0).at("layout").at("windows").at("tool").at("open") == false);
}

TEST_CASE("maximized windows come back maximized, and ui.layout keeps their normal bounds", "[unit]")
{
	const Json right = {{"x", 2200}, {"y", 300}, {"width", 900}, {"height", 700}};
	Fixture f({{"windows",
		{{"project",
			{{"maximized", true}, {"bounds", {{"x", 100}, {"y", 50}, {"width", 800}, {"height", 600}}},
				{"monitor", "Left"}}}}}});
	REQUIRE(f.laidOut);
	// The main window as the last session left it.
	CHECK(f.ui.windows.maximized(f.main));
	Json layout = f.saved();
	layout["windows"]["tool"] = {{"open", true}, {"bounds", right}, {"monitor", "Right"}}; // as a workspace opens it
	f.call("settings.set", {{"key", "ui.layout"}, {"value", layout}});
	REQUIRE(f.waitUntil([&f] { return f.tool != 0; }));
	f.docking.saveWindows();

	// The tool window maximized as the user would: its normal bounds stay in ui.layout.
	f.ui.windows.setMaximized(f.tool, true);
	f.docking.saveWindows();
	REQUIRE(f.waitUntil([&f] { return f.saved() == f.docking.layout(); }));
	CHECK(f.saved().at("windows").at("tool").value("maximized", false));
	CHECK(f.saved().at("windows").at("tool").at("bounds") == right);
	f.call("workspace.add", {{"name", "Big"}, {"type", "global"}});

	// Its normal size again, and closed: the workspace opens it maximized again.
	f.ui.windows.setMaximized(f.tool, false);
	f.docking.saveWindows();
	REQUIRE(f.waitUntil([&f] { return !f.saved().at("windows").at("tool").contains("maximized"); }));
	f.ui.shell.close(f.tool);
	f.docking.saveWindows();
	f.call("workspace.activate", {{"type", "global"}, {"name", "Big"}});
	REQUIRE(f.waitUntil([&f] { return f.tool != 0 && f.ui.shell.find(f.tool) != nullptr; }));
	f.docking.saveWindows(); // as the app's windows do when they open
	CHECK(f.ui.windows.maximized(f.tool));
	f.ui.windows.setMaximized(f.tool, false);
	CHECK(f.ui.shell.bounds(f.tool) == ui::Rect{2200, 300, 900, 700});

	// A workspace applied to the open window maximizes it too.
	f.docking.saveWindows();
	REQUIRE(f.waitUntil([&f] { return !f.saved().at("windows").at("tool").contains("maximized"); }));
	f.call("workspace.activate", {{"type", "global"}, {"name", "Big"}});
	REQUIRE(f.waitUntil([&f] { return f.ui.windows.maximized(f.tool); }));
	f.ui.windows.setMaximized(f.main, false);
	CHECK(f.ui.shell.bounds(f.main) == ui::Rect{100, 50, 800, 600});
}

namespace
{
	using Window = std::optional<ui::WindowId>;

	Json elements(test::UiApp& app, const std::string& selector, Window window = std::nullopt)
	{
		return app.ui().dom(window, selector, 0).at("elements");
	}

	bool exists(test::UiApp& app, const std::string& selector)
	{
		return !elements(app, selector).empty();
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	void openMenu(test::UiApp& app)
	{
		click(app, "#menu-bar button[menu=workspaces-menu]");
		REQUIRE(
			app.waitUntil([&app] { return exists(app, "#workspaces-menu menuitem[command=window.addWorkspace]"); }));
	}

	// The workspaces in the Workspaces menu, "G name" or "P name", with a * for the checked one.
	std::vector<std::string> menuItems(test::UiApp& app)
	{
		std::vector<std::string> result;
		for (const Json& item : elements(app, "#workspaces-menu menuitem[workspace-type]"))
		{
			const Json& attributes = item.at("attributes");
			const std::string type = attributes.at("workspace-type").get<std::string>();
			std::string text = "-";
			if (type == "global")
				text = "G ";
			else if (type == "project")
				text = "P ";
			result.push_back(
				text + attributes.at("workspace").get<std::string>() + (attributes.contains("checked") ? "*" : ""));
		}
		return result;
	}

	// Adds a workspace through Workspaces > Add Workspace and the New Workspace dialog.
	void addWorkspace(test::UiApp& app, const std::string& name, bool project)
	{
		openMenu(app);
		click(app, "#workspaces-menu menuitem[command=window.addWorkspace]");
		REQUIRE(app.waitUntil([&app] { return exists(app, "#workspace-name"); }));
		click(app, "#workspace-name");
		app.ui().text(std::nullopt, name);
		if (project)
			click(app, "#workspace-project");
		click(app, "#workspace-ok");
		REQUIRE(app.waitUntil([&app] { return !exists(app, "#workspace-name"); }));
	}
}

TEST_CASE("the Workspaces menu adds, updates and recalls workspaces, and the organizer organizes them", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	const auto names = [&app](const std::string& type)
	{
		std::vector<std::string> result;
		const Json workspaces = app.call("workspace.get");
		for (const Json& workspace : workspaces.at(type))
			result.push_back(workspace.at("name").get<std::string>());
		return result;
	};

	// Without workspaces the menu has No Workspace, checked, and Update Workspace is disabled.
	openMenu(app);
	REQUIRE(app.waitUntil([&app] { return menuItems(app) == std::vector<std::string>{"-*"}; }));
	CHECK(elements(app, "menuitem[command=window.updateWorkspace]").at(0).at("attributes").contains("disabled"));
	app.ui().key(std::nullopt, "Escape");

	// A global workspace with the lower zone shown, then a project workspace with the mixer window open.
	app.appUi().docking().setVisible("project", "lower", true);
	addWorkspace(app, "Editing", false);
	CHECK(names("global") == std::vector<std::string>{"Editing"});
	app.appUi().docking().setVisible("project", "lower", false);
	app.appUi().toggleMixer(1); // F3
	REQUIRE(app.appUi().mixer(1) != nullptr);
	addWorkspace(app, "Mixing", true);
	CHECK(names("project") == std::vector<std::string>{"Mixing"});
	const Json mixing = app.call("workspace.get").at("project").at(0).at("layout");
	CHECK(mixing.at("windows").at("mixer1").at("open") == true);
	CHECK(mixing.at("windows").at("mixer1").contains("bounds"));
	openMenu(app);
	REQUIRE(
		app.waitUntil([&app] { return menuItems(app) == std::vector<std::string>{"-", "G Editing", "P Mixing*"}; }));
	CHECK(!elements(app, "menuitem[command=window.updateWorkspace]").at(0).at("attributes").contains("disabled"));

	// Choosing a workspace in the menu shows its windows: the mixer window closes, the lower zone shows.
	click(app, "#workspaces-menu menuitem[workspace=Editing]");
	REQUIRE(app.waitUntil([&app] { return app.appUi().mixer(1) == nullptr; }));
	REQUIRE(app.waitUntil([&app] { return app.appUi().docking().visible("project", "lower"); }));
	openMenu(app);
	REQUIRE(
		app.waitUntil([&app] { return menuItems(app) == std::vector<std::string>{"-", "G Editing*", "P Mixing"}; }));
	click(app, "#workspaces-menu menuitem[workspace=Mixing]");
	REQUIRE(app.waitUntil([&app] { return app.appUi().mixer(1) != nullptr; }));
	CHECK(!app.appUi().docking().visible("project", "lower"));

	// Update Workspace takes the windows as they are now.
	app.appUi().toggleMixer(1);
	REQUIRE(app.appUi().mixer(1) == nullptr);
	openMenu(app);
	click(app, "#workspaces-menu menuitem[command=window.updateWorkspace]");
	REQUIRE(app.waitUntil(
		[&app]
		{
			return app.call("workspace.get").at("project").at(0).at("layout").at("windows").at("mixer1").at("open") ==
				false;
		}));

	// The organizer renames with Return, moves and deletes.
	addWorkspace(app, "Recording", false);
	app.ui().key(std::nullopt, "Escape");
	app.keys().run("window.workspaces", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return elements(app, "#workspaces-global listitem").size() == 2; }));
	click(app, "#workspaces-global listitem[value=\"2\"]");
	click(app, "#workspaces-up");
	REQUIRE(app.waitUntil([&] { return names("global") == std::vector<std::string>{"Recording", "Editing"}; }));
	click(app, "#workspace-name");
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "Tracking");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(app.waitUntil([&] { return names("global") == std::vector<std::string>{"Tracking", "Editing"}; }));
	click(app, "#workspaces-delete");
	REQUIRE(app.waitUntil([&] { return names("global") == std::vector<std::string>{"Editing"}; }));
	REQUIRE(app.waitUntil([&app] { return elements(app, "#workspaces-global listitem").size() == 1; }));
	click(app, "#workspaces-project listitem");
	click(app, "#workspaces-delete");
	REQUIRE(app.waitUntil([&] { return names("project").empty(); }));
	click(app, "#workspaces-close");
}

TEST_CASE("the Windows dialog lists the open windows, activates and closes them", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	REQUIRE(app.appUi().toggleMarkers() != nullptr);
	REQUIRE(app.appUi().toggleTempoEditor() != nullptr);
	const ui::WindowId markers = app.appUi().markersWindow();
	// In the project window, also after another window came to the front.
	const auto press = [&app](const std::string& selector)
	{
		app.ui().click(app.main().id(), {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	};

	// Window > Windows
	click(app, "#menu-bar button[menu=window-menu]");
	click(app, "#window-menu menuitem[command=window.windows]");
	REQUIRE(app.waitUntil([&app] { return elements(app, "#windows-list listitem", app.main().id()).size() == 3; }));
	std::vector<std::string> titles;
	for (const Json& item : elements(app, "#windows-list listitem", app.main().id()))
		titles.push_back(item.at("text").get<std::string>());
	CHECK(titles == std::vector<std::string>{"Project", "Markers", "Tempo Track"});

	// The list follows a window that opens while the dialog is open.
	REQUIRE(app.appUi().toggleKeyboard() != nullptr);
	REQUIRE(app.waitUntil([&app] { return elements(app, "#windows-list listitem", app.main().id()).size() == 4; }));

	// Close Windows has nothing to close with the project window selected.
	const auto closeDisabled = [&app]
	{ return elements(app, "#windows-close", app.main().id()).at(0).at("attributes").contains("disabled"); };
	press("#windows-list listitem[window=\"" + std::to_string(app.main().id()) + "\"]");
	REQUIRE(app.waitUntil(closeDisabled));

	// The selected window: activated, then closed.
	press("#windows-list listitem[window=\"" + std::to_string(markers) + "\"]");
	REQUIRE(app.waitUntil([&] { return !closeDisabled(); }));
	press("#windows-activate");
	press("#windows-close");
	REQUIRE(app.waitUntil([&app] { return app.appUi().markers() == nullptr; }));
	CHECK(app.appUi().tempoEditor() != nullptr);
	REQUIRE(app.waitUntil([&app] { return elements(app, "#windows-list listitem", app.main().id()).size() == 3; }));

	// All closes every window but the project window.
	press("#windows-mode"); // its options aren't in the DOM that the UI tools see
	app.ui().key(app.main().id(), "Down");
	press("#windows-mode");
	REQUIRE(app.waitUntil(
		[&app]
		{
			return elements(app, "#windows-mode", app.main().id()).at(0).at("attributes").value("value", "") == "all";
		}));
	press("#windows-close");
	REQUIRE(app.waitUntil([&app] { return app.appUi().tempoEditor() == nullptr; }));
	CHECK(app.appUi().keyboard() == nullptr);
	CHECK(app.shell().windows().size() == 1);
	REQUIRE(app.waitUntil(closeDisabled));
	press("#windows-ok");
}

TEST_CASE("the script console is a workspace window", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	app.call("workspace.add", {{"name", "Plain"}, {"type", "global"}});
	const auto open = [&app] { return app.shell().find(app.appUi().scriptConsoleWindow()) != nullptr; };
	app.keys().run("window.scriptConsole", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil(open));
	REQUIRE(app.waitUntil(
		[&app]
		{
			const Json layout = app.call("settings.get", {{"key", "ui.layout"}}).at("value");
			const Json console = layout.value("windows", Json::object()).value("scriptConsole", Json::object());
			return console.value("open", false) && console.contains("bounds");
		}));
	app.call("workspace.add", {{"name", "Scripting"}, {"type", "global"}});

	app.call("workspace.activate", {{"type", "global"}, {"name", "Plain"}});
	REQUIRE(app.waitUntil([&open] { return !open(); }));
	app.call("workspace.activate", {{"type", "global"}, {"name", "Scripting"}});
	REQUIRE(app.waitUntil(open));
}
