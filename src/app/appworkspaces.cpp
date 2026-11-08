// SPDX-License-Identifier: GPL-3.0-or-later
// Workspaces and profiles (#163, docs/features/workspaces.md, docs/features/profiles.md): the app's windows that
// ui.layout keeps, so that workspaces have them, the Workspaces menu's commands and the profile manager.
#include "app/appui.h"
#include "platform/log.h"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>

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
		m_keys.add({"window.workspaces",
			"Opens the workspaces organizer, which renames, deletes and reorders the workspaces.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openOnce(window, "workspaces.rml", m_workspaces); }});
		// Window > Windows: the open windows, activated, minimized, restored or closed through the UI tools.
		m_keys.add({"window.windows",
			"Opens the Windows dialog, which activates, minimizes, restores and closes windows.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openOnce(window, "windows.rml", m_windowsDialog); }});
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
		m_keys.enableWhen("workspace.update", [this](ui::Window&, Rml::Element*)
			{ return m_workspacesMenu != nullptr && m_workspacesMenu->hasActive(); });
	}
}
