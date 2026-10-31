// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/appui.h"

#include "app/uitools.h"
#include "platform/log.h"
#include "ui/fonts.h"
#include "ui/historypanel.h"
#include "ui/widgets.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Lua.h>
#include <lua.h>

#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::app
{
	namespace
	{
		// GNUbase's fonts, which documents need, then the themes.
		ui::Themes fontsAndThemes(const std::filesystem::path& resources)
		{
			ui::Fonts(resources / "fonts").load();
			return ui::Themes(resources);
		}

		struct ProjectPanel
		{
			const char* id;
			const char* title;
			const char* zone;
		};

		constexpr std::array<ProjectPanel, 6> g_projectPanels{{
			{"inspector", "Inspector", "left"},
			{"visibility", "Visibility", "left"},
			{"editor", "Editor", "lower"},
			{"mixer", "Mixer", "lower"},
			{"media", "Media", "right"},
			{"meter", "Meter", "right"},
		}};

		// The event display's commands (ui::EventDisplay::actions) and the reference's keys for them in the project
		// window (manual p. 1573), if it has any.
		struct ProjectCommand
		{
			const char* id;
			const char* description;
			const char* keys;
		};

		constexpr std::array<ProjectCommand, 37> g_projectCommands{{
			{"zoom.in", "Zooms the project window in.", "H"},
			{"zoom.out", "Zooms the project window out.", "G"},
			{"zoom.inVertically", "Makes the project window's tracks taller.", "Shift+H"},
			{"zoom.outVertically", "Makes the project window's tracks lower.", "Shift+G"},
			{"zoom.full", "Shows the whole project in the project window.", "Shift+F"},
			{"zoom.undo", "Undoes the project window's last zoom.", nullptr},
			{"zoom.redo", "Redoes the project window's last undone zoom.", nullptr},
			{"view.autoScroll", "Switches the project window's auto-scroll on or off.", "F"},
			// Editing parts and events (docs/features/parts-and-events.md); Backspace deletes too.
			{"edit.delete", "Deletes the selected events.", "Delete"},
			{"edit.mute", "Mutes the selected events.", "Shift+M"},
			{"edit.unmute", "Unmutes the selected events.", "Shift+U"},
			{"edit.lock", "Locks the selected events.", "Mod+Shift+L"},
			{"edit.unlock", "Unlocks the selected events.", "Mod+Shift+U"},
			{"edit.splitAtCursor", "Splits the selected events, or all there, at the project cursor.", "Alt+X"},
			{"edit.splitLoop", "Splits the selected events, or all there, at the locators.", nullptr},
			{"edit.glue", "Glues the selected events together.", nullptr},
			{"edit.duplicate", "Duplicates the selected events.", "Mod+D"},
			{"edit.repeat", "Opens the Repeat Events dialog for the selected events.", "Mod+K"},
			{"edit.fillLoop", "Fills the locator range with copies of the selected events.", nullptr},
			{"edit.toRealCopy", "Converts the selected shared copies to real copies.", nullptr},
			{"edit.nudgeLeft", "Moves the selected events left.", "Mod+Left"},
			{"edit.nudgeRight", "Moves the selected events right.", "Mod+Right"},
			{"edit.nudgeStartLeft", "Moves the start of the selected events left.", "Alt+Left"},
			{"edit.nudgeStartRight", "Moves the start of the selected events right.", "Alt+Shift+Right"},
			{"edit.nudgeEndLeft", "Moves the end of the selected events left.", "Alt+Shift+Left"},
			{"edit.nudgeEndRight", "Moves the end of the selected events right.", "Alt+Right"},
			{"edit.moveStartToCursor", "Moves the selected events so that they start at the project cursor.", "Mod+L"},
			{"edit.moveEndToCursor", "Moves the selected events so that they end at the project cursor.", nullptr},
			{"edit.selectAll", "Selects every event in the project window.", "Mod+A"},
			{"edit.selectNone", "Selects no events.", "Mod+Shift+A"},
			{"edit.invertSelection", "Selects the events that aren't selected, and no others.", "Mod+Alt+I"},
			{"edit.selectInLoop", "Selects the events between the locators.", nullptr},
			{"edit.selectFromStartToCursor", "Selects the events that end before the project cursor.", nullptr},
			{"edit.selectFromCursorToEnd", "Selects the events that start after the project cursor.", nullptr},
			{"edit.selectUnderCursor", "Selects the events at the project cursor.", nullptr},
			// Snap and grid (docs/features/snap.md).
			{"edit.snap", "Switches snap on or off.", "J"},
			{"edit.snapPointToCursor", "Sets the snap point of the selected audio events at the project cursor.",
				nullptr},
		}};
	}

	AppUi::AppUi(engine::Engine& engine, const std::filesystem::path& resources, const ui::WindowSpec& main,
		std::ostream& output, int msaaSamples)
		: m_renderer(m_windows, msaaSamples), m_lua(engine, scripting::Trust::Trusted, output), m_rmlui(m_windows),
		  m_themes(fontsAndThemes(resources)), m_shell(m_windows, m_renderer), m_settings(engine, m_shell, m_themes),
		  m_binding(engine, [this] { m_shell.wake(); }), m_keys(engine, m_binding), m_main(m_shell.open(main)),
		  m_docking(engine, m_binding, m_shell, resources / "ui" / "panel-window.rml", main.hidden), m_engine(engine),
		  m_resources(resources), m_history(m_binding.on("history.changed",
									  [this](const engine::Event& event)
									  {
										  const auto position = event.data.at("position").get<size_t>();
										  m_canUndo = position > 0;
										  m_canRedo = position < event.data.at("steps").get<size_t>();
									  })),
		  m_files(m_keys, m_binding, m_shell, resources / "ui")
	{
		ui::registerWidgets(m_rmlui);
		lua_State* state = m_lua.state();
		lua_getglobal(state, "print");
		Rml::Lua::Initialise(state);   // until Rml::Shutdown
		lua_setglobal(state, "print"); // GNUbase's, instead of the plugin's, which writes into RmlUi's log
		addUiFunctions(m_lua, m_shell);
		m_shell.eachStep(
			[this]
			{
				if (const std::optional<std::string> error = m_lua.dispatchEvents())
					logging::warning("A Lua event handler of the UI failed: {}", *error);
				const bool follows = m_binding.update();
				if (m_menu)
					m_menu->update();
				return follows;
			});
		m_shell.onUnusedKey([this](ui::Window& window, const ui::KeyPress& keys) { m_keys.press(window, keys); });

		// The project window's panels (reference manual pp. 54, 75-98), which their tickets fill.
		for (const ProjectPanel& panel : g_projectPanels)
			m_docking.add({panel.id, panel.title, "project", panel.zone, {}});
		// The edit history (reference manual pp. 110-111), a dialog there, which Edit > History opens.
		m_docking.add({"history", "History", "project", "right",
			[this](Rml::Element& element) { return std::make_shared<ui::HistoryPanel>(m_binding, element); }});
		m_docking.addKeyCommands(m_keys);

		m_keys.add({"window.history", "Shows the edit history, where steps are undone and redone.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_docking.open("history"); }});
		m_keys.add({"window.studioSetup", "Opens the studio setup dialog for the audio and MIDI devices.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openStudioSetup(window); }});
		// File > Import (docs/features/file-import.md): the OS's file dialog, then the event display imports the files.
		m_keys.add({"file.importAudio", "Imports audio files into the project window at the project cursor.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{
				chooseImport(window, "Import Audio Files",
					{{"Audio files", "wav;w64;rf64;bwf;aif;aiff;aifc;flac;ogg;mp3"}, {"All files", "*"}}, true);
			}});
		m_keys.add({"file.importMidi", "Imports a standard MIDI file into new tracks.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{
				chooseImport(window, "Import MIDI File", {{"MIDI files", "mid;midi;smf"}, {"All files", "*"}}, false);
			}});
		for (const char* command : {"file.importAudio", "file.importMidi"})
			m_keys.enableWhen(command, [this](ui::Window&, Rml::Element*) { return m_eventDisplay != nullptr; });
		m_keys.add({"project.addTrack", "Opens the add track dialog.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openDialog(window, "add-track.rml"); }});
		// For documents' buttons and menu items: gnubase.ui.command{command = "project.addTrack"} runs a key command
		// in the main window, as its keys would.
		m_lua.addFunction("ui.command",
			[this](const engine::Json& parameters)
			{
				m_keys.run(parameters.at("command").get<std::string>(),
					parameters.value("parameters", engine::Json::object()), m_main, nullptr);
				return engine::Json::object();
			});
		for (const ProjectCommand& command : g_projectCommands)
		{
			m_keys.add(
				{command.id, command.description, [this, id = std::string(command.id)](ui::Window&, Rml::Element*)
					{
						if (m_eventDisplay)
							m_eventDisplay->actions().at(id)();
					}});
			m_keys.enableWhen(command.id, [this](ui::Window&, Rml::Element*) { return m_eventDisplay != nullptr; });
			if (command.keys != nullptr)
				m_keys.bind({command.keys, command.id, engine::Json::object(), "project"});
		}
		m_keys.bind({"Backspace", "edit.delete", engine::Json::object(), "project"});

		// Undo and Redo in the menus while the history has a step to undo or redo.
		m_keys.enableWhen("history.undo", [this](ui::Window&, Rml::Element*) { return m_canUndo; });
		m_keys.enableWhen("history.redo", [this](ui::Window&, Rml::Element*) { return m_canRedo; });
		m_binding.send("history.get", engine::Json::object(),
			[this](const engine::Result& result)
			{
				if (!result.ok())
					return;
				const auto position = result.value.at("position").get<size_t>();
				m_canUndo = position > 0;
				m_canRedo = position < result.value.at("steps").size();
			});
	}

	ui::EventDisplay& AppUi::showProject()
	{
		const std::filesystem::path file = m_resources / "ui" / "project.rml";
		Rml::ElementDocument* document = m_main.context().LoadDocument(file.generic_u8string());
		Rml::Element* root = document == nullptr ? nullptr : document->GetElementById("event-display");
		if (root == nullptr)
		{
			if (document != nullptr)
				document->Close();
			throw std::runtime_error("cannot load the project window from " + file.u8string());
		}
		document->Show();
		m_docking.attach("project", *document);
		m_eventDisplay = std::make_unique<ui::EventDisplay>(m_engine, m_binding, *root);
		m_menu = std::make_unique<ui::MainMenu>(m_keys, m_main, *document);
		return *m_eventDisplay;
	}

	void AppUi::chooseImport(ui::Window& window, std::string title, std::vector<ui::FileFilter> filters, bool many)
	{
		ui::FileDialog dialog;
		dialog.title = std::move(title);
		dialog.filters = std::move(filters);
		dialog.many = many;
		dialog.parent = window.id();
		m_shell.showFileDialog(dialog,
			[this](const ui::FileDialogResult& result)
			{
				if (!result.error.empty())
					logging::warning("The file dialog failed: {}", result.error);
				if (result.files.empty() || !m_eventDisplay)
					return;
				std::vector<std::string> files;
				files.reserve(result.files.size());
				for (const std::filesystem::path& file : result.files)
					files.push_back(file.u8string());
				m_eventDisplay->importFiles(files);
			});
	}

	Rml::ElementDocument& AppUi::openStudioSetup(ui::Window& window)
	{
		// One dialog: a second document would share the first one's StudioSetup table in the UI's Lua. A closed
		// document has left its context, until the context releases it.
		auto* open = dynamic_cast<Rml::ElementDocument*>(m_studioSetup.get());
		if (open != nullptr && open->GetParentNode() != nullptr)
		{
			open->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Keep);
			open->PullToFront();
			return *open;
		}
		Rml::ElementDocument& document = openDialog(window, "studio-setup.rml");
		m_studioSetup = document.GetObserverPtr();
		return document;
	}

	Rml::ElementDocument& AppUi::openDialog(ui::Window& window, const char* name) const
	{
		const std::filesystem::path file = m_resources / "ui" / name;
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
			throw std::runtime_error("cannot load the dialog " + file.u8string());
		document->Show(Rml::ModalFlag::Modal);
		return *document;
	}
}
