// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/settings/profiles.h"

#include "engine/engine.h"
#include "platform/files.h"
#include "platform/log.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace gnubase::engine::settings
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr const char* g_defaultProfile = "Default";
		constexpr int g_exportVersion = 1;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		// profiles.json in the settings folder: the active profile's name and the one profile.activate chose.
		struct State
		{
			std::string active = g_defaultProfile;
			std::string next;
		};

		fs::path stateFile(const fs::path& directory)
		{
			return directory / "profiles.json";
		}

		fs::path folderOf(const fs::path& directory, const std::string& name)
		{
			return directory / "profiles" / fs::u8path(name);
		}

		State readState(const fs::path& directory)
		{
			State state;
			const std::optional<std::string> text = files::readFile(stateFile(directory));
			const Json json = text ? Json::parse(*text, nullptr, false) : Json();
			if (json.is_object())
			{
				const Json active = json.value("active", Json());
				const Json next = json.value("next", Json());
				if (active.is_string() && !active.get<std::string>().empty())
					state.active = active.get<std::string>();
				if (next.is_string())
					state.next = next.get<std::string>();
			}
			return state;
		}

		void writeState(const fs::path& directory, const State& state)
		{
			files::writeAtomically(
				stateFile(directory), Json{{"active", state.active}, {"next", state.next}}.dump(1, '\t'));
		}

		// A name that can be a folder's on every OS.
		void checkName(const std::string& name)
		{
			require(!name.empty() && name.size() <= 100, "a profile name has 1 to 100 characters");
			require(name != "." && name != ".." && name.back() != '.' && name.back() != ' ' &&
					std::none_of(name.begin(), name.end(),
						[](char c)
						{
							return static_cast<unsigned char>(c) < 32 ||
								std::string("<>:\"/\\|?*").find(c) != std::string::npos;
						}),
				"a profile name can't have the characters < > : \" / \\ | ? * or end with a dot or a space");
		}

		// The profiles: the active one and those with a folder, sorted by name.
		std::vector<std::string> profiles(const fs::path& directory, const State& state)
		{
			std::vector<std::string> names = {state.active};
			std::error_code error;
			for (const fs::directory_entry& entry : fs::directory_iterator(directory / "profiles", error))
				if (entry.is_directory(error) && entry.path().filename().u8string() != state.active)
					names.push_back(entry.path().filename().u8string());
			std::sort(names.begin(), names.end());
			return names;
		}

		bool exists(const fs::path& directory, const State& state, const std::string& name)
		{
			const std::vector<std::string> names = profiles(directory, state);
			return std::find(names.begin(), names.end(), name) != names.end();
		}

		// The folder with a profile's files: the settings folder for the active one.
		fs::path filesOf(const fs::path& directory, const State& state, const std::string& name)
		{
			return name == state.active ? directory : folderOf(directory, name);
		}

		// Moves the profile files from one folder to another, replacing what is there; a file that from lacks goes
		// from to as well, so that its defaults count.
		void moveFiles(const fs::path& from, const fs::path& to)
		{
			fs::create_directories(to);
			for (const char* name : g_profileFiles)
			{
				fs::remove(to / name);
				if (fs::exists(from / name))
					fs::rename(from / name, to / name);
			}
		}

		Json listJson(const fs::path& directory)
		{
			const State state = readState(directory);
			Json list = Json::array();
			for (const std::string& name : profiles(directory, state))
				list.push_back({{"name", name}, {"active", name == state.active}});
			return {{"profiles", list}, {"active", state.active}, {"next", state.next}};
		}
	}

	void switchProfile(const fs::path& directory)
	{
		if (directory.empty())
			return;
		State state = readState(directory);
		if (state.next.empty() || state.next == state.active)
			return;
		const fs::path next = folderOf(directory, state.next);
		try
		{
			if (fs::is_directory(next))
			{
				moveFiles(directory, folderOf(directory, state.active));
				moveFiles(next, directory);
				fs::remove_all(next);
				logging::info("Switched from the profile {} to {}", state.active, state.next);
				state.active = state.next;
			}
			state.next.clear();
			writeState(directory, state);
		}
		catch (const std::exception& error)
		{
			logging::error("Cannot switch to the profile {}: {}", state.next, error.what());
		}
	}

	void registerProfileCommands(Engine& engine, const fs::path& directory)
	{
		const Json name = {{"type", "string"}};
		const auto object = [](Json properties, Json required)
		{
			return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
				{"additionalProperties", false}};
		};
		const Json listSchema = {{"type", "object"},
			{"properties",
				{{"profiles",
					 {{"type", "array"},
						 {"items",
							 {{"type", "object"},
								 {"properties", {{"name", {{"type", "string"}}}, {"active", {{"type", "boolean"}}}}},
								 {"required", {"name", "active"}}}}}},
					{"active", {{"type", "string"}}}, {"next", {{"type", "string"}}}}},
			{"required", {"profiles", "active", "next"}}};
		const std::string list =
			" Returns what profile.list does: the profiles by name, sorted, the active one, and next, the one that "
			"becomes active when GNUbase starts again (empty if that is the active one). Emits profile.changed with "
			"the same.";
		// Runs a change on the profiles in the folder, then reports them.
		const auto command = [&engine, &listSchema, directory](const char* id, std::string description, Json parameters,
								 std::function<void(const Json& parameters, State& state)> change)
		{
			engine.registerCommand({id, 1, std::move(description), std::move(parameters), listSchema, false},
				[directory, change](CommandContext& context)
				{
					require(!directory.empty(), "the user settings are kept in memory only, so there are no profiles");
					State state = readState(directory);
					try
					{
						change(context.parameters(), state);
					}
					catch (const std::system_error& error) // the file system's, std::filesystem::filesystem_error too
					{
						throw CommandError(std::string("the profiles' files can't be changed: ") + error.what());
					}
					Json result = listJson(directory);
					context.emit("profile.changed", result);
					return result;
				});
		};

		engine.registerCommand(
			{"profile.list", 1,
				"The profiles (reference manual pp. 1579-1583): named sets of the user settings, of the files "
				"settings.json (preferences, key commands, global workspaces and the other user settings) and "
				"quantize-presets.json of the settings folder. The active profile's files are in the settings folder, "
				"the others' in its folder profiles/<name>. Returns the profiles by name, sorted, with active "
				"true for the active one, its name, and next, the one that becomes active when GNUbase starts again "
				"(empty if that is the active one).",
				CommandInfo{}.parameters, listSchema, true},
			[directory](CommandContext&)
			{
				require(!directory.empty(), "the user settings are kept in memory only, so there are no profiles");
				return listJson(directory);
			});

		command("profile.add", "Adds a profile with the factory settings." + list, object({{"name", name}}, {"name"}),
			[directory](const Json& parameters, State& state)
			{
				const auto added = parameters.at("name").get<std::string>();
				checkName(added);
				require(!exists(directory, state, added), "there is a profile " + added);
				fs::create_directories(folderOf(directory, added));
			});
		command("profile.duplicate", "Adds a profile with the settings of another one." + list,
			object({{"name", name}, {"newName", name}}, {"name", "newName"}),
			[directory](const Json& parameters, State& state)
			{
				const auto from = parameters.at("name").get<std::string>();
				const auto added = parameters.at("newName").get<std::string>();
				require(exists(directory, state, from), "no profile " + from);
				checkName(added);
				require(!exists(directory, state, added), "there is a profile " + added);
				const fs::path to = folderOf(directory, added);
				fs::create_directories(to);
				for (const char* file : g_profileFiles)
					if (const fs::path source = filesOf(directory, state, from) / file; fs::exists(source))
						fs::copy_file(source, to / file);
			});
		command("profile.rename", "Renames a profile, also the active one." + list,
			object({{"name", name}, {"newName", name}}, {"name", "newName"}),
			[directory](const Json& parameters, State& state)
			{
				const auto from = parameters.at("name").get<std::string>();
				const auto to = parameters.at("newName").get<std::string>();
				require(exists(directory, state, from), "no profile " + from);
				checkName(to);
				if (to == from)
					return;
				require(!exists(directory, state, to), "there is a profile " + to);
				if (from == state.active)
					state.active = to;
				else
					fs::rename(folderOf(directory, from), folderOf(directory, to));
				if (state.next == from)
					state.next = to;
				writeState(directory, state);
			});
		command("profile.remove", "Deletes a profile and its settings; not the active one." + list,
			object({{"name", name}}, {"name"}),
			[directory](const Json& parameters, State& state)
			{
				const auto removed = parameters.at("name").get<std::string>();
				require(removed != state.active, "the active profile can't be deleted");
				require(exists(directory, state, removed), "no profile " + removed);
				fs::remove_all(folderOf(directory, removed));
				if (state.next == removed)
				{
					state.next.clear();
					writeState(directory, state);
				}
			});
		command("profile.activate",
			"Makes a profile the active one when GNUbase starts again: until then the settings stay as they are." +
				list,
			object({{"name", name}}, {"name"}),
			[directory](const Json& parameters, State& state)
			{
				const auto chosen = parameters.at("name").get<std::string>();
				require(exists(directory, state, chosen), "no profile " + chosen);
				state.next = chosen == state.active ? std::string() : chosen;
				writeState(directory, state);
			});
		command("profile.export",
			"Saves a profile's settings in a file (UTF-8 path), for another computer: JSON with the profile's name and "
			"its files." +
				list,
			object({{"name", name}, {"file", {{"type", "string"}, {"minLength", 1}}}}, {"name", "file"}),
			[directory](const Json& parameters, State& state)
			{
				const auto exported = parameters.at("name").get<std::string>();
				require(exists(directory, state, exported), "no profile " + exported);
				Json contents = Json::object();
				for (const char* file : g_profileFiles)
					if (const std::optional<std::string> text =
							files::readFile(filesOf(directory, state, exported) / file))
					{
						Json json = Json::parse(*text, nullptr, false);
						if (!json.is_discarded())
							contents[file] = std::move(json);
					}
				try
				{
					files::writeAtomically(fs::u8path(parameters.at("file").get<std::string>()),
						Json{{"gnubaseProfile", g_exportVersion}, {"name", exported}, {"files", contents}}.dump(
							1, '\t'));
				}
				catch (const std::system_error& error)
				{
					throw CommandError(std::string("the profile can't be saved: ") + error.what());
				}
			});
		command("profile.import",
			"Adds a profile from a file that profile.export saved (UTF-8 path), under the name it has there or the "
			"one given." +
				list,
			object({{"file", {{"type", "string"}, {"minLength", 1}}}, {"name", name}}, {"file"}),
			[directory](const Json& parameters, State& state)
			{
				const std::optional<std::string> text =
					files::readFile(fs::u8path(parameters.at("file").get<std::string>()));
				require(text.has_value(), "the file can't be read");
				const Json json = Json::parse(*text, nullptr, false);
				require(json.is_object() && json.value("gnubaseProfile", Json()) == g_exportVersion &&
						json.value("files", Json()).is_object(),
					"the file isn't a GNUbase profile");
				const std::string imported = parameters.contains("name") ? parameters.at("name").get<std::string>()
																		 : json.value("name", std::string());
				checkName(imported);
				require(!exists(directory, state, imported), "there is a profile " + imported);
				const fs::path to = folderOf(directory, imported);
				for (const char* file : g_profileFiles)
					if (const auto found = json.at("files").find(file); found != json.at("files").end())
						files::writeAtomically(to / file, found->dump(1, '\t'));
				fs::create_directories(to);
			});
	}
}
