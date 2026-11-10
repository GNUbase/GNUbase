// SPDX-License-Identifier: GPL-3.0-or-later
// User scripts and the script console (#209, docs/features/scripts.md).
#include "app/appui.h"
#include "platform/log.h"
#include "scripting/reference.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/StringUtilities.h>
#include <SDL3/SDL_misc.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace gnubase::app
{
	namespace
	{
		constexpr size_t g_maxOutput = size_t{64} * 1024;

		// A file URL for SDL_OpenURL, e.g. file:///C:/Users/Me/My%20Scripts on Windows.
		std::string fileUrl(const std::filesystem::path& path)
		{
			std::error_code error;
			const std::filesystem::path absolute = std::filesystem::absolute(path, error);
			const std::string generic = (error ? path : absolute).generic_u8string();
			std::string url = generic.rfind('/', 0) == 0 ? "file://" : "file:///";
			for (const char character : generic)
			{
				const auto byte = static_cast<unsigned char>(character);
				if (std::isalnum(byte) != 0 || std::strchr("/-._~:", character) != nullptr)
				{
					url += character;
				}
				else
				{
					constexpr std::string_view digits = "0123456789ABCDEF";
					url += '%';
					url += digits[byte >> 4U];
					url += digits[byte & 15U];
				}
			}
			return url;
		}

		std::string pathParameter(const engine::Json& parameters)
		{
			const auto path = parameters.find("path");
			if (!parameters.is_object() || path == parameters.end() || !path->is_string())
				throw std::invalid_argument("ui.openExternally needs a table with a path");
			return path->get<std::string>();
		}
	}

	void ScriptOutput::clear()
	{
		m_text.clear();
		++m_version;
	}

	ScriptOutput::int_type ScriptOutput::overflow(int_type character)
	{
		if (traits_type::eq_int_type(character, traits_type::eof()))
			return traits_type::not_eof(character);
		const char text = traits_type::to_char_type(character);
		xsputn(&text, 1);
		return character;
	}

	std::streamsize ScriptOutput::xsputn(const char* text, std::streamsize count)
	{
		m_output.write(text, count);
		m_text.append(text, static_cast<size_t>(count));
		if (m_text.size() > g_maxOutput)
		{
			// The oldest lines go.
			const size_t line = m_text.find('\n', m_text.size() - g_maxOutput);
			m_text.erase(0, line == std::string::npos ? m_text.size() - g_maxOutput : line + 1);
		}
		++m_version;
		return count;
	}

	int ScriptOutput::sync()
	{
		m_output.flush();
		return 0;
	}

	void AppUi::addScripts()
	{
		m_openExternally = [](const std::filesystem::path& path)
		{
			if (!SDL_OpenURL(fileUrl(path).c_str()))
				logging::warning("Can't open {}: {}", path.u8string(), SDL_GetError());
		};
		scanScripts();
		m_keys.add({"window.scriptConsole",
			"Opens or closes the script console, which runs the user scripts and the Lua typed into it.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{ m_shell.post([this] { toggleScriptConsole(); }); }});

		// For the console: the scripts, found again (Reload), running Lua typed into it, opening files and the Lua API
		// reference with the OS, and clearing its output.
		m_lua.addFunction("ui.scripts",
			[this](const engine::Json&)
			{
				// Keys and macros for scripts that are new, and Edit > Scripts: in the next step, since a script or the
				// menu item that runs it may be what calls this.
				if (scanScripts())
					m_shell.post([this] { applyKeySettings(); });
				engine::Json scripts = engine::Json::array();
				for (const auto& [name, file] : m_scripts)
					scripts.push_back({{"name", name}, {"command", "script." + name}, {"file", file.u8string()}});
				return engine::Json{{"folder", scriptFolder().u8string()}, {"scripts", scripts}};
			});
		m_lua.addFunction("ui.runLua",
			[this](const engine::Json& parameters)
			{
				const auto code = parameters.find("code");
				if (!parameters.is_object() || code == parameters.end() || !code->is_string())
					throw std::invalid_argument("ui.runLua needs a table with code");
				m_luaOutput << "> " << code->get<std::string>() << '\n';
				runAsOneStep(
					"Script Console", [this, &code] { return m_lua.run(code->get<std::string>(), "console"); });
				return engine::Json::object();
			});
		m_lua.addFunction("ui.openExternally",
			[this](const engine::Json& parameters)
			{
				m_openExternally(std::filesystem::u8path(pathParameter(parameters)));
				return engine::Json::object();
			});
		m_lua.addFunction("ui.luaReference",
			[this](const engine::Json&)
			{
				const std::filesystem::path folder = scriptFolder();
				if (folder.empty())
					throw std::runtime_error("there is no script folder");
				const std::filesystem::path file = folder / "lua-api.md";
				std::ofstream out(file, std::ios::binary);
				out << scripting::reference(m_engine);
				if (!out.flush())
					throw std::runtime_error("can't write " + file.u8string());
				return engine::Json{{"file", file.u8string()}};
			});
		m_lua.addFunction("ui.clearScriptOutput",
			[this](const engine::Json&)
			{
				m_scriptOutput.clear();
				return engine::Json::object();
			});

		// The console shows the output as it grows.
		m_shell.eachStep(
			[this]
			{
				if (m_shownOutput == m_scriptOutput.version() || m_shell.find(m_scriptConsoleWindow) == nullptr)
					return false;
				auto* document = dynamic_cast<Rml::ElementDocument*>(m_scriptConsole.get());
				Rml::Element* output = document != nullptr ? document->GetElementById("script-output") : nullptr;
				if (output == nullptr)
					return false;
				m_shownOutput = m_scriptOutput.version();
				output->SetInnerRML(Rml::StringUtilities::EncodeRml(m_scriptOutput.text()));
				document->UpdateDocument();
				output->SetScrollTop(output->GetScrollHeight()); // the newest lines
				m_shell.requestRender();
				return false;
			});
	}

	std::filesystem::path AppUi::scriptFolder() const
	{
		const std::filesystem::path& settings = m_engine.settingsDirectory();
		return settings.empty() ? std::filesystem::path() : settings / "scripts";
	}

	bool AppUi::scanScripts()
	{
		std::map<std::string, std::filesystem::path> scripts;
		if (const std::filesystem::path folder = scriptFolder(); !folder.empty())
		{
			std::error_code error;
			// A new folder starts with the examples.
			if (!std::filesystem::exists(folder, error) && std::filesystem::create_directories(folder, error))
				for (const auto& example : std::filesystem::directory_iterator(m_resources / "scripts", error))
					std::filesystem::copy_file(example.path(), folder / example.path().filename(), error);
			if (error)
				logging::warning("The script folder {}: {}", folder.u8string(), error.message());
			for (const auto& entry : std::filesystem::directory_iterator(folder, error))
				if (entry.path().extension() == ".lua" && entry.is_regular_file(error))
					scripts.emplace(entry.path().stem().u8string(), entry.path());
		}
		if (scripts == m_scripts)
			return false;
		for (const auto& [name, file] : m_scripts)
			m_keys.remove("script." + name);
		m_scripts = std::move(scripts);
		for (const auto& [name, file] : m_scripts)
			m_keys.add({"script." + name, "Runs the user script " + file.filename().u8string() + ".",
				[this, name = name](ui::Window& /*window*/, Rml::Element* /*zone*/)
				{
					const auto script = m_scripts.find(name);
					if (script == m_scripts.end())
						return;
					const std::filesystem::path file = script->second; // the script may find the scripts again
					runAsOneStep(name, [this, &file] { return m_lua.runFile(file); });
				}});
		return true;
	}

	void AppUi::runAsOneStep(const std::string& action, const std::function<std::optional<std::string>()>& run)
	{
		// The steps after the last done one join (history.join), however the script ends.
		engine::Client client(m_engine);
		int64_t after = 0;
		if (const engine::Result history = client.call("history.get"); history.ok())
		{
			const auto position = history.value.at("position").get<size_t>();
			if (position > 0)
				after = history.value.at("steps").at(position - 1).at("id").get<int64_t>();
		}
		if (const std::optional<std::string> error = run())
			m_luaOutput << *error << '\n' << std::flush;
		if (const engine::Result joined = client.call("history.join", {{"after", after}, {"action", action}});
			!joined.ok())
			logging::warning("The undo steps of {} stay apart: {}", action, joined.error);
	}

	void AppUi::toggleScriptConsole()
	{
		if (m_shell.find(m_scriptConsoleWindow) != nullptr)
		{
			m_shell.close(m_scriptConsoleWindow);
			m_docking.saveWindows();
			return;
		}
		ui::WindowSpec spec;
		spec.title = "Script Console";
		spec.size = {720, 400};
		spec.bounds = m_docking.bounds("scriptConsole");
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		m_scriptConsoleWindow = window.id();
		const std::filesystem::path file = m_resources / "ui" / "script-console.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
		{
			m_shell.close(m_scriptConsoleWindow);
			throw std::runtime_error("cannot load the script console from " + file.u8string());
		}
		m_scriptConsole = document->GetObserverPtr();
		m_shownOutput = m_scriptOutput.version() - 1; // shows it in the next step
		document->Show();
		m_docking.saveWindows();
	}
}
