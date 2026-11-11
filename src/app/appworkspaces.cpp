// SPDX-License-Identifier: GPL-3.0-or-later
// Workspaces and profiles (#163, docs/features/workspaces.md, docs/features/profiles.md): the app's windows that
// ui.layout keeps, so that workspaces have them, the Workspaces menu's commands and the profile manager.
#include "app/appui.h"
#include "platform/log.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::app
{
	void AppUi::addWorkspaces()
	{
		// A window that opens or closes as a workspace's layout says, where it then is.
		const auto opener = [](std::function<bool()> isOpen, std::function<void()> toggle)
		{
			return [isOpen = std::move(isOpen), toggle = std::move(toggle)](bool open)
			{
				if (isOpen() == open)
					return;
				try
				{
					toggle(); // which saves the windows
				}
				catch (const std::runtime_error& error)
				{
					logging::error("A window of the workspace can't open: {}", error.what());
				}
			};
		};
		m_docking.addWindow("project", [this] { return m_main.id(); });
		for (int number = 1; number <= 4; ++number)
			m_docking.addWindow(
				"mixer" + std::to_string(number), [this, number] { return mixerWindow(number); },
				opener([this, number] { return mixer(number) != nullptr; }, [this, number] { toggleMixer(number); }));
		m_docking.addWindow(
			"markers", [this] { return m_markersWindow; },
			opener([this] { return markers() != nullptr; }, [this] { toggleMarkers(); }));
		m_docking.addWindow(
			"onScreenKeyboard", [this] { return m_keyboardWindow; },
			opener([this] { return keyboard() != nullptr; }, [this] { toggleKeyboard(); }));
		m_docking.addWindow(
			"tempoTrack", [this] { return m_tempoWindow; },
			opener([this] { return tempoEditor() != nullptr; }, [this] { toggleTempoEditor(); }));
		m_docking.addWindow(
			"arranger", [this] { return m_arrangerWindow; },
			opener([this] { return m_shell.find(m_arrangerWindow) != nullptr; }, [this] { toggleArranger(); }));
		m_docking.addWindow(
			"transportPanel", [this] { return m_transportPanel.window; },
			opener([this] { return transportPanel() != nullptr; }, [this] { toggleTransportPanel(); }));
		m_docking.addWindow(
			"timeDisplay", [this] { return m_timeDisplay.window; },
			opener([this] { return timeDisplay() != nullptr; }, [this] { toggleTimeDisplay(); }));
		m_docking.addWindow(
			"scriptConsole", [this] { return m_scriptConsoleWindow; },
			opener(
				[this] { return m_shell.find(m_scriptConsoleWindow) != nullptr; }, [this] { toggleScriptConsole(); }));
		// Windows that follow what they show, which a workspace only places while they are open.
		m_docking.addWindow("channelSettings", [this] { return m_channelSettings.window; });
		m_docking.addWindow("editor", [this] { return m_editor.window; });
		m_docking.addWindow("sampleEditor", [this] { return m_sampleEditor.window; });

		m_keys.add({"window.addWorkspace",
			"Opens the New Workspace dialog, which adds a global or project workspace with the window layout as it "
			"is now.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{
				m_docking.saveWindows(); // the windows as they are now, for the workspace
				openDialog(window, "add-workspace.rml");
			}});
		m_keys.add({"window.updateWorkspace",
			"Gives the active workspace the window layout as it is now (workspace.update), with the windows where "
			"they are at this moment.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{
				m_docking.saveWindows();
				m_binding.send("workspace.update", engine::Json::object());
			}});
		m_keys.add({"window.workspaces",
			"Opens the workspaces organizer, which renames, deletes and reorders the workspaces.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openOnce(window, "workspaces.rml", m_workspaces); }});
		// Window > Windows: the open windows, activated, minimized, restored or closed through the UI tools.
		m_keys.add({"window.windows",
			"Opens the Windows dialog, which activates, minimizes, restores and closes windows.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openOnce(window, "windows.rml", m_windowsDialog); }});
		// The Windows dialog follows the windows that open or close while it is open, also those that opened between
		// its load and its first step. The shell copies the task for each step, so the windows it lists live outside.
		m_shell.eachStep(
			[this, listed = std::make_shared<std::vector<ui::WindowId>>()]
			{
				std::vector<ui::WindowId>& shown = *listed;
				if (m_windowsDialog.get() == nullptr)
				{
					shown.clear();
					return false;
				}
				std::vector<ui::WindowId> ids;
				for (const ui::Window* window : m_shell.windows())
					ids.push_back(window->id());
				if (ids == shown)
					return false;
				shown = std::move(ids);
				if (const std::optional<std::string> error = m_lua.run("WindowsDialog.refresh()", "windows"))
					logging::warning("The Windows dialog can't follow the windows: {}", *error);
				m_shell.requestRender(); // the list, wherever the dialog is
				return false;
			});
		// Edit > Profile Manager (docs/features/profiles.md), whose Import and Export ask the OS for the file.
		m_keys.add({"window.profiles",
			"Opens the profile manager, which adds, duplicates, renames, deletes, imports, exports and activates "
			"profiles.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openOnce(window, "profiles.rml", m_profiles); }});
		m_lua.addFunction("ui.importProfile",
			[this](const engine::Json&)
			{
				ui::FileDialog dialog;
				dialog.title = "Import Profile";
				dialog.filters = {{"GNUbase profiles", "json"}, {"All files", "*"}};
				dialog.parent = m_main.id();
				m_shell.showFileDialog(dialog,
					[this](const ui::FileDialogResult& result)
					{
						if (!result.error.empty())
							logging::warning("The file dialog failed: {}", result.error);
						if (!result.files.empty())
							m_binding.send("profile.import", {{"file", result.files.front().u8string()}});
					});
				return engine::Json::object();
			});
		m_lua.addFunction("ui.exportProfile",
			[this](const engine::Json& parameters)
			{
				const std::string name = parameters.value("name", std::string());
				ui::FileDialog dialog;
				dialog.kind = ui::FileDialogKind::Save;
				dialog.title = "Export Profile";
				dialog.filters = {{"GNUbase profiles", "json"}};
				dialog.location = std::filesystem::u8path(name + ".json");
				dialog.parent = m_main.id();
				m_shell.showFileDialog(dialog,
					[this, name](const ui::FileDialogResult& result)
					{
						if (!result.error.empty())
							logging::warning("The file dialog failed: {}", result.error);
						if (!result.files.empty())
							m_binding.send(
								"profile.export", {{"name", name}, {"file", result.files.front().u8string()}});
					});
				return engine::Json::object();
			});
		for (const char* command : {"workspace.update", "window.updateWorkspace"})
			m_keys.enableWhen(command, [this](ui::Window&, Rml::Element*)
				{ return m_workspacesMenu != nullptr && m_workspacesMenu->hasActive(); });
	}
}
