// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/settings/profiles.h"

#include "engine/engine.h"
#include "platform/files.h"
#include "platform/log.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <stdexcept>
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
			// Windows keeps these names for devices, also with an extension.
			std::string base = name.substr(0, name.find('.'));
			while (!base.empty() && base.back() == ' ')
				base.pop_back();
			std::transform(base.begin(), base.end(), base.begin(),
				[](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
			const bool numbered = base.size() == 4 &&
				(base.compare(0, 3, "COM") == 0 || base.compare(0, 3, "LPT") == 0) && base[3] >= '1' && base[3] <= '9';
			require(!numbered && base != "CON" && base != "PRN" && base != "AUX" && base != "NUL",
				"a profile name can't be a device's name on Windows (CON, PRN, AUX, NUL, COM1 to COM9, LPT1 to LPT9)");
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

		// Whether a profile other than except has a name that differs from name at most in case: the profiles'
		// folders aren't case-sensitive on Windows and macOS. ASCII case only, as the folders' names are UTF-8;
		// makeFolder asks the file system about the other letters.
		bool taken(const fs::path& directory, const State& state, const std::string& name,
			const std::string& except = std::string())
		{
			const auto lower = [](std::string text)
			{
				std::transform(text.begin(), text.end(), text.begin(),
					[](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
				return text;
			};
			const std::vector<std::string> names = profiles(directory, state);
			return std::any_of(names.begin(), names.end(),
				[&](const std::string& other) { return other != except && lower(other) == lower(name); });
		}

		// Makes a profile's folder, of a new profile or one renamed from the folder old, and refuses a name that the
		// file system takes for another profile's folder: NTFS and APFS also fold the case of letters such as Ä, and
		// APFS ignores the Unicode normalization, which taken doesn't see. The active profile has no folder, so one
		// that appears with the new one is the same.
		void makeFolder(
			const fs::path& directory, const State& state, const std::string& name, const fs::path& old = fs::path())
		{
			const fs::path folder = folderOf(directory, name);
			const fs::path active = folderOf(directory, state.active);
			require(!fs::exists(folder) || (!old.empty() && fs::equivalent(old, folder)), "there is a profile " + name);
			const bool hadActive = fs::exists(active);
			if (old.empty())
				fs::create_directories(folder);
			else
				fs::rename(old, folder);
			if (hadActive || !fs::exists(active))
				return;
			if (old.empty())
				fs::remove(folder);
			else
				fs::rename(folder, old);
			throw CommandError("there is a profile " + name);
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

		// Copies the profile files from one folder to another as moveFiles moves them.
		void copyFiles(const fs::path& from, const fs::path& to)
		{
			fs::create_directories(to);
			for (const char* name : g_profileFiles)
			{
				fs::remove(to / name);
				if (fs::exists(from / name))
					fs::copy_file(from / name, to / name);
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
		const fs::path saved = folderOf(directory, state.active);
		// Copies of both profiles' files, so that a switch that fails on the way puts the active one's back.
		const fs::path staging = directory / "profile-switch";
		std::error_code error;
		try
		{
			if (!fs::is_directory(next))
			{
				state.next.clear();
				writeState(directory, state);
				return;
			}
			// A folder that the file system takes for the active profile's, such as one whose name differs only in
			// case: the switch would move the active profile's files over the next one's.
			if (fs::exists(saved) && fs::equivalent(saved, next))
				throw std::runtime_error("its folder is the active profile's " + state.active + " on this file system");
			fs::remove_all(staging);
			copyFiles(directory, staging / "active");
			copyFiles(next, staging / "next");
			try
			{
				copyFiles(staging / "active", saved);
				moveFiles(staging / "next", directory);
				writeState(directory, {state.next, std::string()});
			}
			catch (...)
			{
				copyFiles(staging / "active", directory);
				fs::remove_all(saved, error);
				throw;
			}
			logging::info("Switched from the profile {} to {}", state.active, state.next);
			fs::remove_all(next, error);
		}
		catch (const std::exception& failure)
		{
			logging::error("Cannot switch to the profile {}: {}", state.next, failure.what());
		}
		fs::remove_all(staging, error);
	}

	void cancelProfileSwitch(const fs::path& directory)
	{
		State state = readState(directory);
		if (state.next.empty())
			return;
		state.next.clear();
		writeState(directory, state);
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
								 const std::function<void(const Json& parameters, State& state)>& change)
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
				require(!taken(directory, state, added), "there is a profile " + added);
				makeFolder(directory, state, added);
			});
		command("profile.duplicate", "Adds a profile with the settings of another one." + list,
			object({{"name", name}, {"newName", name}}, {"name", "newName"}),
			[directory](const Json& parameters, State& state)
			{
				const auto from = parameters.at("name").get<std::string>();
				const auto added = parameters.at("newName").get<std::string>();
				require(exists(directory, state, from), "no profile " + from);
				checkName(added);
				require(!taken(directory, state, added), "there is a profile " + added);
				makeFolder(directory, state, added);
				const fs::path to = folderOf(directory, added);
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
				require(!taken(directory, state, to, from), "there is a profile " + to);
				if (from == state.active)
				{
					require(!fs::exists(folderOf(directory, to)), "there is a profile " + to);
					state.active = to;
				}
				else
				{
					makeFolder(directory, state, to, folderOf(directory, from));
				}
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
				require(!taken(directory, state, imported), "there is a profile " + imported);
				makeFolder(directory, state, imported);
				const fs::path to = folderOf(directory, imported);
				for (const char* file : g_profileFiles)
					if (const auto found = json.at("files").find(file); found != json.at("files").end())
						files::writeAtomically(to / file, found->dump(1, '\t'));
			});
	}
}
