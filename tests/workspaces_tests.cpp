// SPDX-License-Identifier: GPL-3.0-or-later
// Workspaces in the engine (#163, docs/features/workspaces.md): global ones in the user setting ui.workspaces and
// project ones saved with the project, each a copy of ui.layout, and the active one.
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	struct Fixture
	{
		Json call(const std::string& command, Json parameters = Json::object())
		{
			const engine::Result result = client.call(command, std::move(parameters));
			INFO(command << " failed: " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, Json parameters)
		{
			const engine::Result result = client.call(command, std::move(parameters));
			REQUIRE(!result.ok());
			return result.error;
		}

		void setLayout(const Json& layout) { call("settings.set", {{"key", "ui.layout"}, {"value", layout}}); }
		Json layout() { return call("settings.get", {{"key", "ui.layout"}}).at("value"); }

		// The names of the workspaces of the type, in order.
		std::vector<std::string> names(const std::string& type)
		{
			std::vector<std::string> result;
			const Json workspaces = call("workspace.get");
			for (const Json& workspace : workspaces.at(type))
				result.push_back(workspace.at("name").get<std::string>());
			return result;
		}

		Json active() { return call("workspace.get").at("active"); }

		engine::Engine engine;
		engine::Client client{engine};
	};

	Json activeIs(const std::string& type, const std::string& name)
	{
		return {{"type", type}, {"name", name}};
	}

	Json mixingLayout()
	{
		return {{"windows",
			{{"project", {{"bounds", {{"x", 10}, {"y", 20}, {"width", 800}, {"height", 600}}}, {"monitor", "Left"}}},
				{"mixer1", {{"open", true}, {"bounds", {{"x", 1930}, {"y", 0}, {"width", 900}, {"height", 500}}}}}}}};
	}

	Json editingLayout()
	{
		return {{"windows", {{"project", {{"zones", {{"lower", {{"visible", true}}}}}}}}}};
	}
}

TEST_CASE("workspaces keep window layouts, global ones in ui.workspaces and project ones in the project", "[unit]")
{
	Fixture f;
	std::mutex mutex;
	std::vector<Json> changes;
	const engine::Listener listener(f.engine,
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "workspace.changed")
				changes.push_back(event.data);
		});
	CHECK(f.call("workspace.get") ==
		Json{{"global", Json::array()}, {"project", Json::array()}, {"active", activeIs("", "")}});

	// Add Workspace takes the layout as it is now and makes the workspace the active one.
	f.setLayout(mixingLayout());
	const Json added = f.call("workspace.add", {{"name", "Mixing"}, {"type", "global"}});
	CHECK(added.at("global") == Json{{{"name", "Mixing"}, {"layout", mixingLayout()}}});
	CHECK(added.at("active") == activeIs("global", "Mixing"));
	CHECK(f.call("settings.get", {{"key", "ui.workspaces"}}).at("value") == added.at("global"));
	f.setLayout(editingLayout());
	f.call("workspace.add", {{"name", "Editing"}, {"type", "project"}});
	CHECK(f.names("project") == std::vector<std::string>{"Editing"});
	CHECK(f.active() == activeIs("project", "Editing"));
	{
		const std::scoped_lock lock(mutex);
		REQUIRE(changes.size() == 2);
		CHECK(changes[1].at("project").at(0).at("layout") == editingLayout());
	}
	// Names are unique within a type, not across them.
	CHECK_THAT(f.fails("workspace.add", {{"name", "Mixing"}, {"type", "global"}}), ContainsSubstring("there is"));
	CHECK_THAT(f.fails("workspace.add", {{"name", ""}, {"type", "global"}}), ContainsSubstring("name"));
	f.call("workspace.add", {{"name", "Mixing"}, {"type", "project"}});
	CHECK(f.names("project") == std::vector<std::string>{"Editing", "Mixing"});

	// Activating one sets ui.layout to its layout.
	f.call("workspace.activate", {{"type", "global"}, {"name", "Mixing"}});
	CHECK(f.layout() == mixingLayout());
	CHECK(f.active() == activeIs("global", "Mixing"));
	f.call("workspace.activate", {{"type", "project"}, {"name", "Editing"}});
	CHECK(f.layout() == editingLayout());
	CHECK_THAT(f.fails("workspace.activate", {{"type", "project"}, {"name", "Nope"}}), ContainsSubstring("no project"));
	CHECK(f.active() == activeIs("project", "Editing"));

	// Update Workspace gives the active one the layout as it is now.
	const Json changed = {{"windows", {{"project", {{"zones", {{"left", {{"visible", false}}}}}}}}}};
	f.setLayout(changed);
	f.call("workspace.update");
	CHECK(f.call("workspace.get").at("project").at(0).at("layout") == changed);
	f.call("workspace.activate", {{"type", "global"}, {"name", "Mixing"}});
	f.call("workspace.activate", {{"type", "project"}, {"name", "Editing"}});
	CHECK(f.layout() == changed);

	// No Workspace returns to the layout from before the first workspace became active.
	f.call("workspace.activate", {{"type", ""}});
	CHECK(f.active() == activeIs("", ""));
	CHECK(f.layout() == mixingLayout());
	CHECK_THAT(f.fails("workspace.update", Json::object()), ContainsSubstring("no workspace is active"));

	// The organizer's rename, move and delete, which keep the active one as it is.
	f.call("workspace.activate", {{"type", "project"}, {"name", "Mixing"}});
	f.call("workspace.rename", {{"type", "project"}, {"name", "Mixing"}, {"newName", "Mix"}});
	CHECK(f.active() == activeIs("project", "Mix"));
	CHECK_THAT(f.fails("workspace.rename", {{"type", "project"}, {"name", "Mix"}, {"newName", "Editing"}}),
		ContainsSubstring("there is"));
	f.call("workspace.move", {{"type", "project"}, {"name", "Mix"}, {"index", 0}});
	CHECK(f.names("project") == std::vector<std::string>{"Mix", "Editing"});
	f.call("workspace.move", {{"type", "project"}, {"name", "Mix"}, {"index", 9}});
	CHECK(f.names("project") == std::vector<std::string>{"Editing", "Mix"});
	f.call("workspace.remove", {{"type", "project"}, {"name", "Editing"}});
	CHECK(f.active() == activeIs("project", "Mix"));
	f.call("workspace.remove", {{"type", "project"}, {"name", "Mix"}});
	CHECK(f.active() == activeIs("", ""));
	CHECK(f.names("project").empty());
	CHECK_THAT(f.fails("workspace.remove", {{"type", "global"}, {"name", "Nope"}}), ContainsSubstring("no global"));
}

TEST_CASE("project workspaces are saved with the project, but aren't undo steps", "[unit]")
{
	Fixture f;
	f.call("track.add", {{"type", "audio"}});
	f.setLayout(mixingLayout());
	f.call("workspace.add", {{"name", "Mixing"}, {"type", "project"}});
	f.call("workspace.add", {{"name", "Global"}, {"type", "global"}});

	// Undo takes back the track, not the workspaces.
	f.call("history.undo");
	CHECK(f.call("project.get").at("tracks").empty());
	CHECK(f.names("project") == std::vector<std::string>{"Mixing"});

	f.call("workspace.activate", {{"type", "project"}, {"name", "Mixing"}});
	const test::TempDir dir;
	const std::string file = (dir.path() / "workspaces.dawproject").u8string();
	f.call("project.save", {{"path", file}});
	f.call("project.new");
	CHECK(f.names("project").empty());
	CHECK(f.active() == activeIs("", ""));
	CHECK(f.names("global") == std::vector<std::string>{"Global"}); // for every project
	f.call("project.load", {{"path", file}});
	CHECK(f.call("workspace.get").at("project") == Json{{{"name", "Mixing"}, {"layout", mixingLayout()}}});
	CHECK(f.active() == activeIs("project", "Mixing"));
}

TEST_CASE("only the project workspaces and the active workspace modify the project, and transactions don't run "
		  "workspace commands",
	"[unit]")
{
	Fixture f;
	const test::TempDir dir;
	f.call("project.save", {{"path", (dir.path() / "saved.dawproject").u8string()}});
	const Json saved = f.call("project.revision");
	REQUIRE(saved.at("modified") == false);

	// Global workspaces are user settings: adding one makes it active, which the project saves, but organizing them
	// changes only ui.workspaces.
	f.setLayout(mixingLayout());
	f.call("workspace.add", {{"name", "Mixing"}, {"type", "global"}});
	f.call("workspace.add", {{"name", "Editing"}, {"type", "global"}});
	f.call("project.save", {{"path", (dir.path() / "saved.dawproject").u8string()}});
	const Json before = f.call("project.revision");
	f.call("workspace.update");
	f.call("workspace.rename", {{"type", "global"}, {"name", "Mixing"}, {"newName", "Mix"}});
	f.call("workspace.move", {{"type", "global"}, {"name", "Mix"}, {"index", 1}});
	f.call("workspace.remove", {{"type", "global"}, {"name", "Mix"}});
	f.call("workspace.activate", {{"type", "global"}, {"name", "Editing"}}); // active already
	CHECK(f.call("project.revision") == before);

	// The active workspace and the project workspaces are the project's.
	f.call("workspace.activate", {{"type", ""}});
	CHECK(f.call("project.revision").at("modified") == true);
	f.call("project.save", {{"path", (dir.path() / "saved.dawproject").u8string()}});
	f.call("workspace.add", {{"name", "Project"}, {"type", "project"}});
	CHECK(f.call("project.revision").at("modified") == true);

	const engine::Result transaction = f.client.call("history.transaction",
		{{"description", "Both"},
			{"commands",
				{{{"command", "track.add"}, {"parameters", {{"type", "audio"}}}},
					{{"command", "workspace.add"}, {"parameters", {{"name", "In"}, {"type", "global"}}}}}}});
	CHECK(!transaction.ok());
	CHECK(f.names("global") == std::vector<std::string>{"Editing"});
	CHECK(f.call("project.get").at("tracks").empty());
}

TEST_CASE("No Workspace returns to the layout as it was last without a workspace", "[unit]")
{
	Fixture f;
	const Json own = {{"windows", {{"project", {{"zones", {{"right", {{"visible", true}}}}}}}}}};
	f.setLayout(mixingLayout());
	f.call("workspace.add", {{"name", "Mixing"}, {"type", "global"}});
	f.call("workspace.activate", {{"type", ""}});
	CHECK(f.layout() == mixingLayout()); // none was saved without a workspace yet

	// The layout as it is when a workspace is chosen comes back with No Workspace.
	f.setLayout(own);
	f.call("workspace.activate", {{"type", "global"}, {"name", "Mixing"}});
	CHECK(f.layout() == mixingLayout());
	f.setLayout(editingLayout()); // changes the workspace's view, not the one without
	f.call("workspace.activate", {{"type", ""}});
	CHECK(f.layout() == own);
	CHECK(f.active() == activeIs("", ""));

	// No Workspace while none is active keeps the layout as it is.
	f.setLayout(editingLayout());
	f.call("workspace.activate", {{"type", ""}});
	CHECK(f.layout() == editingLayout());
}

TEST_CASE("an active workspace that is gone, such as a global one another computer has, isn't active", "[unit]")
{
	Fixture f;
	f.setLayout(mixingLayout());
	f.call("workspace.add", {{"name", "Mixing"}, {"type", "global"}});
	const test::TempDir dir;
	const std::string file = (dir.path() / "elsewhere.dawproject").u8string();
	f.call("project.save", {{"path", file}});

	// On a computer without it.
	f.call("settings.set", {{"key", "ui.workspaces"}, {"value", Json::array()}});
	f.call("project.load", {{"path", file}});
	CHECK(f.active() == activeIs("", ""));
	CHECK_THAT(f.fails("workspace.update", Json::object()), ContainsSubstring("no workspace is active"));

	// Once it is there again, it is active as the project says.
	f.call("settings.set", {{"key", "ui.workspaces"}, {"value", {{{"name", "Mixing"}, {"layout", mixingLayout()}}}}});
	CHECK(f.active() == activeIs("global", "Mixing"));
}
