// SPDX-License-Identifier: GPL-3.0-or-later
// The user settings that belong to the UI (#53, docs/features/themes.md): ui.theme and ui.scale reach the shell when
// the UI starts and whenever they change, e.g. through settings.set from MCP or Lua.
#include "engine/engine.h"
#include "testutils/fakeui.h"
#include "ui/shell.h"
#include "ui/themes.h"
#include "ui/uisettings.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

using namespace gnubase;
using std::chrono::milliseconds;

namespace
{
	ui::WindowSpec spec()
	{
		ui::WindowSpec spec;
		spec.bounds = ui::Rect{0, 0, 800, 600};
		return spec;
	}

	void set(engine::Client& client, const char* key, const engine::Json& value)
	{
		const engine::Result result = client.call("settings.set", {{"key", key}, {"value", value}});
		INFO(result.error);
		REQUIRE(result.ok());
	}
}

TEST_CASE("the UI takes its theme and scale from the user settings, and follows them as they change", "[unit]")
{
	engine::Engine engine; // settings in memory
	engine::Client client(engine);
	set(client, "ui.theme", "dark");
	test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	const ui::Themes themes(std::filesystem::path(GNUBASE_RESOURCES_DIR));
	const ui::Window& window = fake.shell.open(spec());
	const ui::UiSettings settings(engine, fake.shell, themes);
	CHECK(fake.shell.theme() == "dark");
	CHECK(window.dpRatio() == 1.0F);

	// Changes arrive on the engine's thread and apply on the UI thread, in the shell's next step.
	set(client, "ui.theme", "light");
	set(client, "ui.scale", 1.5);
	CHECK(fake.shell.theme() == "dark");
	fake.shell.step(milliseconds(0));
	CHECK(fake.shell.theme() == "light");
	CHECK(window.dpRatio() == 1.5F);

	// A theme that doesn't exist, say one a newer GNUbase saved, falls back to the default.
	set(client, "ui.theme", "neon");
	fake.shell.step(milliseconds(0));
	CHECK(fake.shell.theme() == ui::g_defaultTheme);

	set(client, "ui.theme", "light");
	REQUIRE(client.call("settings.reset").ok());
	fake.shell.step(milliseconds(0));
	CHECK(fake.shell.theme() == ui::g_defaultTheme);
	CHECK(window.dpRatio() == 1.0F);
}

TEST_CASE("an unknown theme in the user settings starts the UI in the default theme", "[unit]")
{
	engine::Engine engine;
	engine::Client client(engine);
	set(client, "ui.theme", "neon");
	test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	const ui::Themes themes(std::filesystem::path(GNUBASE_RESOURCES_DIR));
	const ui::UiSettings settings(engine, fake.shell, themes);
	CHECK(fake.shell.theme() == ui::g_defaultTheme);
}
