// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of GNUbase's UI as the app shows it (#59), through the harness in uiapp.h: the app's own UI setup on the
// dummy device, driven with ui::Automation as MCP's UI tools drive it, with screenshots compared with golden images.
#include "uiapp.h"

#include "scripting/runtime.h"
#include "ui/automation.h"
#include "ui/themes.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

using namespace gnubase;
using engine::Json;

namespace
{
	// Opens the rename dialog when the button is clicked, as an app's button would.
	struct DialogOpener final : Rml::EventListener
	{
		void ProcessEvent(Rml::Event& event) override
		{
			Rml::ElementDocument* dialog = event.GetCurrentElement()->GetContext()->LoadDocument(
				std::string(GNUBASE_TEST_DATA_DIR) + "/ui/automation-dialog.rml");
			REQUIRE(dialog != nullptr);
			dialog->Show(Rml::ModalFlag::Modal);
		}
	};

	ui::Target selector(const char* text)
	{
		return {text, 0, std::nullopt};
	}
}

TEST_CASE("UI tests run GNUbase's UI on the dummy device, with its fonts and theme, at a fixed dp ratio", "[ui]")
{
	test::UiApp app;
	CHECK(app.call("audio.get").at("device").at("backend") == "dummy");
	CHECK(app.shell().theme() == ui::g_defaultTheme);
	REQUIRE(app.shell().windows().size() == 1);
	CHECK(app.main().main());

	// The same dp ratio gives the same pixels on every monitor. The theme styles the document's first box.
	app.show("sample.rml");
	for (const int ratio : {1, 2})
	{
		app.setDpRatio(static_cast<float>(ratio));
		CHECK(app.main().dpRatio() == static_cast<float>(ratio));
		const std::string name = "app-sample-" + std::to_string(ratio) + "x";
		test::UiApp::checkGolden(name, app.screenshot(name, {320, 240}));
	}
}

TEST_CASE("UI tests click through GNUbase's UI and read it back, as MCP's UI tools do", "[ui]")
{
	test::UiApp app;
	DialogOpener opener;
	app.show("automation.rml").GetElementById("open")->AddEventListener(Rml::EventId::Click, &opener);

	CHECK(
		app.ui().click(std::nullopt, selector("#open"), ui::MouseButton::Left, 1, {}).at("element").at("id") == "open");
	const auto dialog = [&app] { return app.ui().dom(std::nullopt, "#message", 0).at("elements"); };
	REQUIRE(app.waitUntil([&dialog] { return !dialog().empty(); }));
	CHECK(dialog()[0].at("text") == "Enter a new name for Audio 01.");

	app.ui().click(std::nullopt, selector("#name"), ui::MouseButton::Left, 1, {});
	app.ui().key(std::nullopt, "Ctrl+A");
	app.ui().text(std::nullopt, "Bass");
	CHECK(app.ui().dom(std::nullopt, "#name", 0).at("elements")[0].at("value") == "Bass");
	const ui::Image field = app.ui().screenshot(std::nullopt, selector("#name"));
	CHECK(field.width == static_cast<int>(200.0F * app.main().dpRatio()));
}

TEST_CASE("settings changed through the engine restyle GNUbase's UI", "[ui]")
{
	test::UiApp app;
	app.show("widgets.rml");
	app.setDpRatio(1.0F);
	const ui::Image before = app.screenshot("app-theme-before", {560, 360});

	app.call("settings.set", {{"key", "ui.theme"}, {"value", "dark"}});
	REQUIRE(app.waitUntil([&app] { return app.shell().theme() == "dark"; }));
	const ui::Image dark = app.screenshot("app-theme-dark", {560, 360});
	CHECK(!test::compareImages(dark, before, {8}).equal);

	// ui.scale applies on top of the monitor's scale.
	const float monitorScale = app.main().dpRatio() / app.shell().uiScale();
	app.call("settings.set", {{"key", "ui.scale"}, {"value", 2.0}});
	REQUIRE(
		app.waitUntil([&app, monitorScale] { return std::abs(app.main().dpRatio() - (2.0F * monitorScale)) < 0.01F; }));
}

// UI tests in Lua (#208): ctest runs each tests/lua/ui/*.lua through this test case, which GNUBASE_LUA_UI_TEST tells
// which file, in the source folder. The file runs in the app's UI Lua on the UI thread, and prints its report.
TEST_CASE("a UI test written in Lua passes", "[.][ui][lua-ui]")
{
	const char* variable = std::getenv("GNUBASE_LUA_UI_TEST");
	const std::string file = variable != nullptr ? variable : "";
	REQUIRE(!file.empty());
	test::UiApp app;
	const std::optional<std::string> error = app.lua().runFile(std::filesystem::u8path(file));
	INFO(error.value_or(""));
	// Not CHECK_FALSE: Catch2's ContinueOnFailure | FalseTest trips clang-analyzer's EnumCastOutOfRange check
	CHECK(!error.has_value());
}
