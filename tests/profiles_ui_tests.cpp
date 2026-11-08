// SPDX-License-Identifier: GPL-3.0-or-later
// The Profile Manager in the app (#163, docs/features/profiles.md), driven with ui::Automation as MCP's UI tools drive
// it, on the user settings in UiApp's temporary folder.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"

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
	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// The profiles the dialog lists, with a * for the active one.
	std::vector<std::string> listed(test::UiApp& app)
	{
		std::vector<std::string> result;
		for (const Json& item : elements(app, "#profiles-list listitem"))
		{
			const auto classes = item.value("classes", Json::array());
			const bool active = std::find(classes.begin(), classes.end(), "active") != classes.end();
			result.push_back(item.at("attributes").at("profile").get<std::string>() + (active ? "*" : ""));
		}
		return result;
	}

	void typeName(test::UiApp& app, const std::string& name)
	{
		click(app, "#profile-name");
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, name);
	}
}

TEST_CASE("the Profile Manager adds, duplicates, renames, deletes and activates profiles", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	// Edit > Profile Manager
	click(app, "#menu-bar button[menu=edit-menu]");
	click(app, "#edit-menu menuitem[command=window.profiles]");
	REQUIRE(app.waitUntil([&app] { return listed(app) == std::vector<std::string>{"Default*"}; }));

	typeName(app, "Studio");
	click(app, "#profile-new");
	REQUIRE(app.waitUntil([&app] { return listed(app) == std::vector<std::string>{"Default*", "Studio"}; }));
	typeName(app, "Laptop");
	click(app, "#profile-duplicate"); // of Studio, which New selected
	REQUIRE(app.waitUntil([&app] { return listed(app) == std::vector<std::string>{"Default*", "Laptop", "Studio"}; }));
	typeName(app, "Travel");
	click(app, "#profile-rename"); // Laptop, which Duplicate selected
	REQUIRE(app.waitUntil([&app] { return listed(app) == std::vector<std::string>{"Default*", "Studio", "Travel"}; }));
	click(app, "#profile-delete");
	REQUIRE(app.waitUntil([&app] { return listed(app) == std::vector<std::string>{"Default*", "Studio"}; }));

	// A name that is taken says why.
	typeName(app, "Studio");
	click(app, "#profile-new");
	REQUIRE(app.waitUntil(
		[&app]
		{
			return elements(app, "#profile-message").at(0).value("text", "").find("there is a profile") !=
				std::string::npos;
		}));

	// Activate Profile waits for the next start, and says so.
	click(app, "#profiles-list listitem[profile=Studio]");
	click(app, "#profile-activate");
	REQUIRE(app.waitUntil([&app] { return app.call("profile.list").at("next") == "Studio"; }));
	REQUIRE(app.waitUntil(
		[&app]
		{
			return elements(app, "#profile-message").at(0).value("text", "").find("Studio becomes active") !=
				std::string::npos;
		}));
	CHECK(listed(app) == std::vector<std::string>{"Default*", "Studio"});
	click(app, "#profile-close");
}
