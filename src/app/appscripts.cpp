// SPDX-License-Identifier: GPL-3.0-or-later
// User scripts and the script console (#209, docs/features/scripts.md).
#include "app/appui.h"
#include "platform/log.h"
#include "scripting/reference.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_process.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
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

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// shellapi.h needs windows.h first.
#include <shellapi.h>
#endif

namespace gnubase::app
{
	namespace
	{
		constexpr size_t g_maxOutput = size_t{64} * 1024;
		constexpr std::chrono::seconds g_settleTimeout{10};

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

		// Opens the file in the user's editor: opening a .lua file may run it, where a Lua interpreter is the
		// application for .lua files. Returns false if this OS has no editor for it, e.g. Linux, where the desktop's
		// application for Lua files is an editor.
		bool editExternally([[maybe_unused]] const std::filesystem::path& file)
		{
#ifdef _WIN32
			// The file's edit action, else Notepad.
			const std::wstring path = file.wstring();
			const std::wstring quoted = L"\"" + path + L"\"";
			const auto opened = [](HINSTANCE instance) { return reinterpret_cast<INT_PTR>(instance) > 32; };
			if (!opened(ShellExecuteW(nullptr, L"edit", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) &&
				!opened(ShellExecuteW(nullptr, nullptr, L"notepad.exe", quoted.c_str(), nullptr, SW_SHOWNORMAL)))
				logging::warning("Can't edit {}", file.u8string());
			return true;
#elif defined(__APPLE__)
			// The default text editor.
			const std::string path = file.u8string();
			const char* arguments[] = {"open", "-t", path.c_str(), nullptr};
			if (SDL_Process* process = SDL_CreateProcess(arguments, false))
				SDL_DestroyProcess(process); // it runs on
			else
				logging::warning("Can't edit {}: {}", path, SDL_GetError());
			return true;
#else
			return false;
#endif
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
		m_start += m_text.size();
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
			const size_t dropped = line == std::string::npos ? m_text.size() - g_maxOutput : line + 1;
			m_text.erase(0, dropped);
			m_start += dropped;
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
		m_openExternally = [](const std::filesystem::path& path, bool edit)
		{
			if (edit && editExternally(path))
				return;
			if (!SDL_OpenURL(fileUrl(path).c_str()))
				logging::warning("Can't open {}: {}", path.u8string(), SDL_GetError());
		};
		// A runaway script asks whether it goes on (#930), but can't with the windows hidden, e.g. in tests.
		m_keepScriptRunning = [this](const std::string& script)
		{
			if (m_hidden)
				return false;
			const std::string message = "The script " + script + " has run for " +
				std::to_string(m_scriptTimeLimit.count() / 1000) + " seconds or more. Stop it?";
			std::array<SDL_MessageBoxButtonData, 2> buttons = {};
			buttons[0].flags = SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
			buttons[0].buttonID = 0;
			buttons[0].text = "Stop";
			buttons[1].buttonID = 1;
			buttons[1].text = "Keep Running";
			SDL_MessageBoxData box = {};
			box.flags = SDL_MESSAGEBOX_WARNING;
			box.title = "Script Running";
			box.message = message.c_str();
			box.numbuttons = static_cast<int>(buttons.size());
			box.buttons = buttons.data();
			int pressed = 0;
			return SDL_ShowMessageBox(&box, &pressed) && pressed == 1;
		};
		// The scripts' and the console's globals, apart from the documents' (#930).
		m_scriptEnvironment = m_lua.newEnvironment();
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
				runAsOneStep("Script Console",
					[this, &code] { return m_lua.run(code->get<std::string>(), "console", m_scriptEnvironment); });
				return engine::Json::object();
			});
		m_lua.addFunction("ui.openExternally",
			[this](const engine::Json& parameters)
			{
				const auto edit = parameters.find("edit");
				m_openExternally(std::filesystem::u8path(pathParameter(parameters)),
					edit != parameters.end() && edit->is_boolean() && edit->get<bool>());
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
				// What is new goes at the end as a text node of its own, so that a script printing in a loop doesn't
				// lay out all of the output again in every step; a node goes once the output dropped all of its text
				// (Clear, or older than the last 64 KiB).
				const std::string& text = m_scriptOutput.text();
				const uint64_t start = m_scriptOutput.start();
				const uint64_t end = start + text.size();
				while (!m_shownChunks.empty() && m_shownChunks.front() <= start && output->GetFirstChild() != nullptr)
				{
					output->RemoveChild(output->GetFirstChild());
					m_shownChunks.pop_front();
				}
				if (const uint64_t from = std::max(m_shownEnd, start); from < end)
				{
					output->AppendChild(document->CreateTextNode(text.substr(static_cast<size_t>(from - start))));
					m_shownChunks.push_back(end);
				}
				m_shownEnd = end;
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
			// Incremented with an error code: a range-for's increment throws, e.g. when the folder goes away meanwhile.
			using Entries = std::filesystem::directory_iterator;
			std::error_code error;
			// A new folder starts with the examples.
			if (!std::filesystem::exists(folder, error) && std::filesystem::create_directories(folder, error))
				for (Entries example(m_resources / "scripts", error); !error && example != Entries();
					example.increment(error))
					std::filesystem::copy_file(example->path(), folder / example->path().filename(), error);
			if (error)
				logging::warning("The script folder {}: {}", folder.u8string(), error.message());
			error.clear();
			for (Entries entry(folder, error); !error && entry != Entries(); entry.increment(error))
			{
				std::error_code notFile;
				if (entry->path().extension() == ".lua" && entry->is_regular_file(notFile))
					scripts.emplace(entry->path().stem().u8string(), entry->path());
			}
		}
		if (scripts == m_scripts)
			return false;
		// Only the scripts that are gone lose their command and keys, and only new ones get one (#934).
		for (const auto& [name, file] : m_scripts)
			if (scripts.count(name) == 0)
				m_keys.remove("script." + name);
		for (const auto& [name, file] : scripts)
			if (m_scripts.count(name) == 0)
				m_keys.add({"script." + name, "Runs the user script " + file.filename().u8string() + ".",
					[this, name = name](ui::Window& /*window*/, Rml::Element* /*zone*/)
					{
						const auto script = m_scripts.find(name);
						if (script == m_scripts.end())
							return;
						const std::filesystem::path file = script->second; // the script may find the scripts again
						runAsOneStep(name, [this, &file] { return m_lua.runFile(file, m_scriptEnvironment); });
					}});
		m_scripts = std::move(scripts);
		return true;
	}

	void AppUi::runAsOneStep(const std::string& action, const std::function<std::optional<std::string>()>& run)
	{
		// The steps after the last done one join (history.join), however the script ends. What the UI sent before goes
		// before them, and what the script sent through the UI, e.g. engine key commands, into them.
		if (!m_binding.settle(g_settleTimeout))
			logging::warning("The UI's commands before {} are still running", action);
		engine::Client client(m_engine);
		int64_t after = 0;
		if (const engine::Result history = client.call("history.get"); history.ok())
		{
			const auto position = history.value.at("position").get<size_t>();
			if (position > 0)
				after = history.value.at("steps").at(position - 1).at("id").get<int64_t>();
		}
		// A script that runs too long asks whether it goes on; scripts that it runs share its time.
		const bool outermost = !m_lua.running();
		if (outermost)
			m_lua.setTimeLimit(m_scriptTimeLimit, [this, &action] { return m_keepScriptRunning(action); });
		const std::optional<std::string> error = run();
		if (outermost)
			m_lua.setTimeLimit(std::chrono::milliseconds(0));
		if (error)
			m_luaOutput << *error << '\n' << std::flush;
		if (!m_binding.settle(g_settleTimeout))
			logging::warning("The UI's commands of {} are still running", action);
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
		m_shownEnd = 0;
		m_shownChunks.clear();
		document->Show();
		m_docking.saveWindows();
	}
}
