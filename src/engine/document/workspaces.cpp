// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/workspaces.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		// The index of the workspace of that name in a list of them, [{name, layout}], or the list's size.
		size_t indexOf(const Json& list, const std::string& name)
		{
			size_t index = 0;
			while (index < list.size() && list.at(index).value("name", std::string()) != name)
				++index;
			return index;
		}

		Json userSetting(CommandContext& context, const char* key)
		{
			return context.call("settings.get", {{"key", key}}).at("value");
		}

		// The workspaces of the type, global or project, as [{name, layout}].
		Json listOf(const Project& project, CommandContext& context, const std::string& type)
		{
			return type == "global" ? userSetting(context, "ui.workspaces")
									: toJson(project.workspaces).at("workspaces");
		}

		Json workspacesJson(const Project& project, CommandContext& context)
		{
			return {{"global", listOf(project, context, "global")}, {"project", listOf(project, context, "project")},
				{"active", {{"type", project.workspaces.activeType}, {"name", project.workspaces.active}}}};
		}

		// Changes the list of the type: change gets it as [{name, layout}] and changes it. Global workspaces change in
		// the settings file as it is now, so that what another GNUbase process saved meanwhile stays.
		void changeList(Project& project, CommandContext& context, Engine& engine, const std::string& type,
			const std::function<void(Json& list)>& change)
		{
			if (type == "global")
			{
				engine.modifySetting(context, "ui.workspaces",
					[&change](const Json& value)
					{
						Json list = value;
						change(list);
						return list;
					});
				return;
			}
			Json list = toJson(project.workspaces).at("workspaces");
			change(list);
			project.workspaces.workspaces.clear();
			for (const Json& item : list)
				project.workspaces.workspaces.push_back({item.at("name").get<std::string>(), item.at("layout").dump()});
		}

		bool isActive(const Project& project, const std::string& type, const std::string& name)
		{
			return project.workspaces.activeType == type && project.workspaces.active == name;
		}

		Json changed(const Project& project, CommandContext& context)
		{
			Json result = workspacesJson(project, context);
			context.emit("workspace.changed", result);
			return result;
		}
	}

	void registerWorkspaceCommands(ProjectModel& model, Engine& engine)
	{
		const Json typeSchema = {{"type", "string"}, {"enum", {"global", "project"}}};
		const Json nameSchema = {{"type", "string"}, {"minLength", 1}};
		const Json listSchema = {{"type", "array"},
			{"items",
				{{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}, {"layout", {{"type", "object"}}}}},
					{"required", {"name", "layout"}}}}};
		const Json resultSchema = {{"type", "object"},
			{"properties",
				{{"global", listSchema}, {"project", listSchema},
					{"active",
						{{"type", "object"},
							{"properties",
								{{"type", {{"type", "string"}, {"enum", {"", "global", "project"}}}},
									{"name", {{"type", "string"}}}}},
							{"required", {"type", "name"}}}}}},
			{"required", {"global", "project", "active"}}};
		const auto object = [](Json properties, Json required)
		{
			return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
				{"additionalProperties", false}};
		};
		const std::string text =
			"global: the global workspaces, for every project (the user setting ui.workspaces); project: the "
			"project workspaces, saved with the project; each a name and its layout, a copy of the user setting "
			"ui.layout with the windows that are open, their bounds and monitors. active: the workspace the project "
			"was last shown in, its type (global or project, empty for none) and name.";
		const std::string emits = " Emits workspace.changed with what workspace.get returns.";

		engine.registerCommand({"workspace.get", 1, "The workspaces (reference manual pp. 1576-1578). " + text,
								   CommandInfo{}.parameters, resultSchema, true},
			[&model](CommandContext& context) { return workspacesJson(model.project(), context); });

		model.addEdit(engine,
			{"workspace.add", 1,
				"Adds a workspace with the window layout as it is now (the user setting ui.layout), a global one for "
				"every project or a project one saved with the project, after the others of its type, and makes it "
				"the active one. Names are unique within a type." +
					emits,
				object({{"name", nameSchema}, {"type", typeSchema}}, {"name", "type"}), resultSchema, false},
			"Add workspace",
			[&engine](Project& project, CommandContext& context)
			{
				const auto name = context.parameters().at("name").get<std::string>();
				const auto type = context.parameters().at("type").get<std::string>();
				require(!name.empty(), "name must not be empty");
				const Json layout = userSetting(context, "ui.layout");
				changeList(project, context, engine, type,
					[&](Json& list)
					{
						require(indexOf(list, name) == list.size(), "there is a " + type + " workspace " + name);
						list.push_back({{"name", name}, {"layout", layout}});
					});
				project.workspaces.activeType = type;
				project.workspaces.active = name;
				return changed(project, context);
			});

		model.addEdit(engine,
			{"workspace.update", 1,
				"Gives the active workspace the window layout as it is now (the user setting ui.layout)." + emits,
				CommandInfo{}.parameters, resultSchema, false},
			"Update workspace",
			[&engine](Project& project, CommandContext& context)
			{
				const std::string type = project.workspaces.activeType;
				const std::string name = project.workspaces.active;
				require(!type.empty(), "no workspace is active");
				const Json layout = userSetting(context, "ui.layout");
				changeList(project, context, engine, type,
					[&](Json& list)
					{
						const size_t index = indexOf(list, name);
						require(index < list.size(), "the active workspace " + name + " is gone");
						list.at(index)["layout"] = layout;
					});
				return changed(project, context);
			});

		model.addEdit(engine,
			{"workspace.activate", 1,
				"Shows a workspace: its layout becomes the user setting ui.layout, which opens, closes and places the "
				"windows as it says, and it becomes the active workspace. A window whose monitor isn't connected goes "
				"onto the primary one, fully visible. Without a type (No Workspace) the layout stays and no workspace "
				"is active." +
					emits,
				object({{"type", {{"type", "string"}, {"enum", {"", "global", "project"}}}},
						   {"name", {{"type", "string"}}}},
					{"type"}),
				resultSchema, false},
			"Activate workspace",
			[&engine](Project& project, CommandContext& context)
			{
				const auto type = context.parameters().at("type").get<std::string>();
				const auto name = context.parameters().value("name", std::string());
				if (!type.empty())
				{
					const Json list = listOf(project, context, type);
					const size_t index = indexOf(list, name);
					require(index < list.size(), "no " + type + " workspace " + name);
					engine.setSettings(context, {{"ui.layout", list.at(index).at("layout")}});
				}
				project.workspaces.activeType = type;
				project.workspaces.active = type.empty() ? std::string() : name;
				return changed(project, context);
			});

		const Json renameParameters = object({{"type", typeSchema}, {"name", nameSchema}}, {"type", "name"});
		model.addEdit(engine,
			{"workspace.remove", 1, "Deletes a workspace; if it was the active one, no workspace is active." + emits,
				renameParameters, resultSchema, false},
			"Remove workspace",
			[&engine](Project& project, CommandContext& context)
			{
				const auto type = context.parameters().at("type").get<std::string>();
				const auto name = context.parameters().at("name").get<std::string>();
				changeList(project, context, engine, type,
					[&](Json& list)
					{
						const size_t index = indexOf(list, name);
						require(index < list.size(), "no " + type + " workspace " + name);
						list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
					});
				if (isActive(project, type, name))
				{
					project.workspaces.activeType.clear();
					project.workspaces.active.clear();
				}
				return changed(project, context);
			});

		model.addEdit(engine,
			{"workspace.rename", 1, "Renames a workspace. Names are unique within a type." + emits,
				object(
					{{"type", typeSchema}, {"name", nameSchema}, {"newName", nameSchema}}, {"type", "name", "newName"}),
				resultSchema, false},
			"Rename workspace",
			[&engine](Project& project, CommandContext& context)
			{
				const auto type = context.parameters().at("type").get<std::string>();
				const auto name = context.parameters().at("name").get<std::string>();
				const auto newName = context.parameters().at("newName").get<std::string>();
				require(!newName.empty(), "newName must not be empty");
				changeList(project, context, engine, type,
					[&](Json& list)
					{
						const size_t index = indexOf(list, name);
						require(index < list.size(), "no " + type + " workspace " + name);
						require(newName == name || indexOf(list, newName) == list.size(),
							"there is a " + type + " workspace " + newName);
						list.at(index)["name"] = newName;
					});
				if (isActive(project, type, name))
					project.workspaces.active = newName;
				return changed(project, context);
			});

		model.addEdit(engine,
			{"workspace.move", 1,
				"Moves a workspace to a position among those of its type, 0 the first; positions past the end move it "
				"to the end." +
					emits,
				object({{"type", typeSchema}, {"name", nameSchema}, {"index", {{"type", "integer"}, {"minimum", 0}}}},
					{"type", "name", "index"}),
				resultSchema, false},
			"Move workspace",
			[&engine](Project& project, CommandContext& context)
			{
				const auto type = context.parameters().at("type").get<std::string>();
				const auto name = context.parameters().at("name").get<std::string>();
				const auto to = context.parameters().at("index").get<size_t>();
				changeList(project, context, engine, type,
					[&](Json& list)
					{
						const size_t index = indexOf(list, name);
						require(index < list.size(), "no " + type + " workspace " + name);
						Json moved = list.at(index);
						list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
						list.insert(list.begin() + static_cast<std::ptrdiff_t>(std::min(to, list.size())), moved);
					});
				return changed(project, context);
			});
	}
}
