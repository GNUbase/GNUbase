// SPDX-License-Identifier: GPL-3.0-or-later
// Project handling (#77, docs/features/project-files.md): what happens around saving and opening projects, besides
// project.save, project.load and project.new in projectmodel.cpp: save new version, revert, auto save, the recent
// projects and the project templates. None of these commands is an undo step.
#include "engine/dawproject/dawproject.h"
#include "engine/document/projectmodel.h"
#include "engine/media/backup.h"
#include "engine/settings/store.h"
#include "platform/log.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr const char* g_extension = ".dawproject";
		constexpr const char* g_autoSaves = "Auto Saves";        // in the project folder, or the settings folder
		constexpr const char* g_templates = "Project Templates"; // in the settings folder
		constexpr const char* g_recentKey = "project.recent";
		constexpr size_t g_recentCount = 10;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		const settings::Format& recentFormat()
		{
			static const settings::Format format{"recent-projects", 1,
				{{g_recentKey, "The project files saved or opened last, the newest first.",
					Json{{"type", "array"}, {"items", {{"type", "string"}}}}, Json::array()}},
				{}};
			return format;
		}

		// A number with at least two digits, as versions and auto saves are numbered: 01, 02, ... 100.
		std::string numbered(const std::string& name, int number)
		{
			const std::string digits = std::to_string(number);
			return name + (digits.size() < 2 ? "-0" : "-") + digits;
		}

		// The next version of a project file: the same name with the next number after the highest one of that name in
		// its folder and its own, "Song-01.dawproject" for "Song.dawproject".
		fs::path nextVersion(const fs::path& file)
		{
			static const std::regex version(R"((.*)-(\d+))");
			std::string name = file.stem().u8string();
			int highest = 0;
			std::smatch match;
			if (std::regex_match(name, match, version))
			{
				highest = std::stoi(match[2].str());
				name = match[1].str();
			}
			std::error_code error;
			for (const fs::directory_entry& entry : fs::directory_iterator(file.parent_path(), error))
			{
				const std::string other = entry.path().stem().u8string();
				if (entry.path().extension() == g_extension && std::regex_match(other, match, version) &&
					match[1].str() == name)
					highest = std::max(highest, std::stoi(match[2].str()));
			}
			return file.parent_path() / fs::u8path(numbered(name, highest + 1) + g_extension);
		}

		// Where the next auto save of a project named name goes: the first of name-01.bak to name-<count>.bak that
		// doesn't exist, or else the oldest of them.
		fs::path nextAutoSave(const fs::path& folder, const std::string& name, int count)
		{
			fs::path oldest;
			std::optional<fs::file_time_type> oldestTime;
			for (int number = 1; number <= count; ++number)
			{
				const fs::path file = folder / fs::u8path(numbered(name, number) + ".bak");
				std::error_code error;
				const fs::file_time_type time = fs::last_write_time(file, error);
				if (error)
					return file; // there is none
				if (!oldestTime || time < *oldestTime)
				{
					oldest = file;
					oldestTime = time;
				}
			}
			return oldest;
		}

		Json userSetting(CommandContext& context, const char* key)
		{
			return context.call("settings.get", {{"key", key}}).at("value");
		}
	}

	void ProjectModel::opened(Project project, const std::string& file, CommandContext& context)
	{
		m_project = std::move(project);
		m_file = file;
		m_history.clear();
		context.emit("project.reset");
		announceHistory(context);
		publish();
		m_savedRevision = m_revision;
		m_autoSavedRevision = m_revision;
		m_lastSave = std::chrono::steady_clock::now();
		if (!file.empty())
			remember(file);
	}

	void ProjectModel::saved(const std::string& file, CommandContext& context)
	{
		m_file = file;
		m_savedRevision = m_revision;
		m_autoSavedRevision = m_revision;
		m_lastSave = std::chrono::steady_clock::now();
		remember(file);
		context.emit("project.saved", {{"file", m_file}});
	}

	// Bookkeeping only: the project was saved or opened already, so a recent list that can't be written doesn't make
	// the command fail.
	void ProjectModel::remember(const std::string& file)
	{
		try
		{
			m_recent->modify(g_recentKey,
				[&file](const Json& value)
				{
					Json files = Json::array({file});
					for (const Json& other : value)
						if (other != file && files.size() < g_recentCount)
							files.push_back(other);
					return files;
				});
		}
		catch (const settings::SettingsError& error)
		{
			logging::warning("Cannot add {} to the recent projects: {}", file, error.what());
		}
	}

	Project ProjectModel::loadTemplate(const std::string& name) const
	{
		const fs::path file = m_settingsDirectory / g_templates / fs::u8path(name + g_extension);
		std::error_code error;
		require(!m_settingsDirectory.empty() && fs::is_regular_file(file, error), "there is no template " + name);
		try
		{
			// Templates have no media of their own (project.saveTemplate), so nothing is extracted.
			return loadDawProject(file.u8string(), (file.parent_path() / fs::u8path(name + " media")).u8string());
		}
		catch (const std::exception& problem)
		{
			throw CommandError(problem.what());
		}
	}

	void ProjectModel::registerProjectFileCommands(Engine& engine)
	{
		m_settingsDirectory = engine.settingsDirectory();
		m_recent = std::make_shared<settings::Store>(recentFormat(), m_settingsDirectory);
		const Json stringSchema = {{"type", "string"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json fileResult = objectSchema({{"file", stringSchema}}, {"file"});
		const Json embedMedia = {{"type", "boolean"},
			{"description", "Copy the media files into the file (the default), as project.save does."}};

		addControl(engine,
			{"project.saveNewVersion", 1,
				"Saves the project as a new version of its file and goes on with that: in the same folder, with the "
				"same name and the next number after the highest one there, \"Song-01.dawproject\" after "
				"\"Song.dawproject\", \"Song-03.dawproject\" after \"Song-02.dawproject\". The earlier versions stay "
				"as they are. Returns the new file. Fails for a project without a file: save it first.",
				objectSchema({{"embedMedia", embedMedia}}), fileResult, false},
			[this](CommandContext& context)
			{
				require(!m_file.empty(), "the project has no file yet: save it with a path first");
				const fs::path file = nextVersion(fs::u8path(m_file));
				DawProjectSaveOptions options;
				options.embedMedia = context.parameters().value("embedMedia", true);
				try
				{
					saveDawProject(m_project, file.u8string(), options);
				}
				catch (const std::exception& error)
				{
					throw CommandError(error.what());
				}
				saved(file.u8string(), context);
				return Json{{"file", m_file}};
			});

		addControl(engine,
			{"project.revert", 1,
				"Discards every change since the project was last saved: opens its file again and clears the undo "
				"history, as project.load does. Fails for a project without a file.",
				objectSchema({}), Json::object(), false},
			[this](CommandContext& context)
			{
				require(!m_file.empty(), "the project has no file to revert to");
				context.call("project.load", {{"path", m_file}});
				return Json::object();
			});

		addControl(engine,
			{"project.autoSave", 1,
				"Saves a copy of the project if it changed since it was last saved or auto saved, as an auto save: "
				"into the folder Auto Saves of its project folder as <name>-01.bak, <name>-02.bak and so on up to "
				"the user setting project.autoSaveFiles, after which the oldest is overwritten. A project without a "
				"file goes to Auto Saves in the user settings' folder, named after the project, and isn't auto saved "
				"without one. Media files aren't copied, and the project itself stays unsaved. An auto save opens "
				"with project.load. With due, as the engine asks every 30 seconds, only if the user setting "
				"project.autoSave is on and project.autoSaveInterval minutes have passed since the project was "
				"saved, opened or auto saved. Returns the file, or null if nothing was saved; emits "
				"project.autoSaved with the file.",
				objectSchema({{"due", booleanSchema}}),
				objectSchema({{"file", {{"type", {"string", "null"}}}}}, {"file"}), false},
			[this](CommandContext& context)
			{
				const auto now = std::chrono::steady_clock::now();
				if (context.parameters().value("due", false) &&
					(!userSetting(context, "project.autoSave").get<bool>() ||
						now - m_lastSave <
							std::chrono::minutes(userSetting(context, "project.autoSaveInterval").get<int>())))
				{
					context.leaveOutOfLog();
					return Json{{"file", nullptr}};
				}
				fs::path folder = m_file.empty() ? m_settingsDirectory : fs::u8path(m_file).parent_path();
				if (m_revision == m_autoSavedRevision || folder.empty())
				{
					context.leaveOutOfLog();
					return Json{{"file", nullptr}};
				}
				folder /= g_autoSaves;
				const std::string name =
					m_file.empty() ? safeFileName(m_project.name) : fs::u8path(m_file).stem().u8string();
				const fs::path file =
					nextAutoSave(folder, name, userSetting(context, "project.autoSaveFiles").get<int>());
				try
				{
					fs::create_directories(folder);
					DawProjectSaveOptions options;
					options.embedMedia = false;
					saveDawProject(m_project, file.u8string(), options);
				}
				catch (const std::exception& error)
				{
					throw CommandError(error.what());
				}
				m_autoSavedRevision = m_revision;
				m_lastSave = now;
				context.emit("project.autoSaved", {{"file", file.u8string()}});
				return Json{{"file", file.u8string()}};
			});

		const Json recentResult = objectSchema(
			{{"projects",
				{{"type", "array"},
					{"items", objectSchema({{"file", stringSchema}, {"exists", booleanSchema}}, {"file", "exists"})}}}},
			{"projects"});
		const auto listRecent = [this]
		{
			Json projects = Json::array();
			for (const Json& file : m_recent->value(g_recentKey))
			{
				std::error_code error;
				projects.push_back(
					{{"file", file}, {"exists", fs::is_regular_file(fs::u8path(file.get<std::string>()), error)}});
			}
			return Json{{"projects", std::move(projects)}};
		};
		addControl(engine,
			{"project.recent", 1,
				"The recent projects: the project files saved (not auto saved) or opened last, the newest first, at "
				"most 10, kept in the user settings' folder. exists says whether the file is still there.",
				objectSchema({}), recentResult, true},
			[listRecent](CommandContext&) { return listRecent(); });
		addControl(engine,
			{"project.forgetRecent", 1,
				"Removes file from the recent projects (see project.recent), or every project without file. The "
				"files stay. Returns the recent projects left.",
				objectSchema({{"file", stringSchema}}), recentResult, false},
			[this, listRecent](CommandContext& context)
			{
				const std::optional<std::string> forgotten = context.parameters().contains("file")
					? std::optional<std::string>(context.parameters().at("file").get<std::string>())
					: std::nullopt;
				m_recent->modify(g_recentKey,
					[&forgotten](const Json& value)
					{
						Json files = Json::array();
						if (forgotten)
							for (const Json& file : value)
								if (file != *forgotten)
									files.push_back(file);
						return files;
					});
				return listRecent();
			});

		addControl(engine,
			{"project.templates", 1,
				"The project templates, which project.new starts new projects from, sorted by name: each a name and "
				"its file in the folder Project Templates in the user settings' folder. None without that folder.",
				objectSchema({}),
				objectSchema(
					{{"templates",
						{{"type", "array"},
							{"items",
								objectSchema({{"name", stringSchema}, {"file", stringSchema}}, {"name", "file"})}}}},
					{"templates"}),
				true},
			[this](CommandContext&)
			{
				std::vector<fs::path> files;
				std::error_code error;
				if (!m_settingsDirectory.empty())
					for (const fs::directory_entry& entry :
						fs::directory_iterator(m_settingsDirectory / g_templates, error))
						if (entry.path().extension() == g_extension && entry.is_regular_file(error))
							files.push_back(entry.path());
				std::sort(files.begin(), files.end());
				Json templates = Json::array();
				for (const fs::path& file : files)
					templates.push_back({{"name", file.stem().u8string()}, {"file", file.u8string()}});
				return Json{{"templates", std::move(templates)}};
			});
		addControl(engine,
			{"project.saveTemplate", 1,
				"Saves the project as the project template name (see project.templates), replacing a template of "
				"that name. Templates refer to media files where they are instead of copying them. The project "
				"itself, its file and whether it is saved stay as they are. Returns the template's file.",
				objectSchema({{"name", stringSchema}}, {"name"}), fileResult, false},
			[this](CommandContext& context)
			{
				const std::string name = context.parameters().at("name").get<std::string>();
				const std::string problem = fileNameProblem(name);
				require(problem.empty(), problem);
				require(!m_settingsDirectory.empty(), "there is no settings folder to keep templates in");
				const fs::path file = m_settingsDirectory / g_templates / fs::u8path(name + g_extension);
				try
				{
					fs::create_directories(file.parent_path());
					DawProjectSaveOptions options;
					options.embedMedia = false;
					saveDawProject(m_project, file.u8string(), options);
				}
				catch (const std::exception& error)
				{
					throw CommandError(error.what());
				}
				return Json{{"file", file.u8string()}};
			});
	}
}
