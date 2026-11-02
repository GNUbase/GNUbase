// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/appui.h"

#include "app/uitools.h"
#include "platform/log.h"
#include "ui/fonts.h"
#include "ui/historypanel.h"
#include "ui/poolpanel.h"
#include "ui/timeformat.h"
#include "ui/widgets.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Lua.h>
#include <lua.h>

#include <array>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
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

		constexpr std::array<ProjectCommand, 63> g_projectCommands{{
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
			// Left and Right (reference manual p. 253); Up and Down are the track list's track.selectPrevious and
			// track.selectNext (p. 218).
			{"edit.selectPrevious", "Selects the event before the selected one on its track.", "Left"},
			{"edit.selectNext", "Selects the event after the selected one on its track.", "Right"},
			{"edit.locatorsToSelection", "Sets the locators around the selected events.", "P"},
			// Range editing (docs/features/range-editing.md), with the reference's keys (manual pp. 1562-1563).
			{"edit.cut", "Cuts the selection range to the clipboard.", "Mod+X"},
			{"edit.copy", "Copies the selection range to the clipboard.", "Mod+C"},
			{"edit.paste", "Pastes the clipboard at the selection range's start or the project cursor.", "Mod+V"},
			{"edit.cutTime", "Cuts the selection range and closes the gap.", "Mod+Shift+X"},
			{"edit.deleteTime", "Deletes the selection range and closes the gap.", "Shift+Backspace"},
			{"edit.pasteTime", "Pastes the clipboard and moves what comes after it to make room.", "Mod+Shift+V"},
			{"edit.insertSilence", "Inserts silence as long as the selection range at its start.", "Mod+Shift+E"},
			{"edit.splitRange", "Splits the events at the selection range's edges.", "Shift+X"},
			{"edit.cropRange", "Crops the events that cross the selection range's edges to it.", nullptr},
			{"edit.globalCopy", "Copies everything between the locators to the clipboard.", nullptr},
			// Snap and grid (docs/features/snap.md).
			{"edit.snap", "Switches snap on or off.", "J"},
			{"edit.snapPointToCursor", "Sets the snap point of the selected audio events at the project cursor.",
				nullptr},
			// The tools (docs/features/tools.md), with the reference's keys (manual p. 1569).
			{"tool.objectSelection", "Chooses the object selection tool in the project window.", "1"},
			{"tool.rangeSelection", "Chooses the range selection tool in the project window.", "2"},
			{"tool.split", "Chooses the split tool in the project window.", "3"},
			{"tool.glue", "Chooses the glue tool in the project window.", "4"},
			{"tool.erase", "Chooses the erase tool in the project window.", "5"},
			{"tool.zoom", "Chooses the zoom tool in the project window.", "6"},
			{"tool.mute", "Chooses the mute tool in the project window.", "7"},
			{"tool.draw", "Chooses the draw tool in the project window.", "8"},
			{"tool.play", "Chooses the play tool in the project window.", "9"},
			{"tool.line", "Chooses the line tool in the project window.", nullptr},
			{"tool.previous", "Chooses the project window's previous tool.", "F9"},
			{"tool.next", "Chooses the project window's next tool.", "F10"},
			{"tool.combineSelectionTools", "Switches combining the selection tools on or off.", "Alt+Shift+1"},
		}};

		// The transport controls' displays that the enter.* commands edit, with the reference's keys (manual
		// pp. 311-312, 1570).
		struct EnterCommand
		{
			const char* field;
			const char* id;
			const char* description;
			const char* keys;
		};

		constexpr std::array<EnterCommand, 6> g_enterCommands{{
			{"left", "enter.leftLocator", "Edits the left locator in the transport bar.", "Shift+L"},
			{"right", "enter.rightLocator", "Edits the right locator in the transport bar.", "Shift+R"},
			{"position", "enter.position", "Edits the project cursor position in the transport bar.", "Shift+P"},
			{"length", "enter.length", "Edits the locator range duration in the transport bar.", "Shift+D"},
			{"tempo", "enter.tempo", "Edits the tempo in the transport bar.", "Shift+T"},
			{"signature", "enter.signature", "Edits the time signature in the transport bar.", "Shift+C"},
		}};

		constexpr ui::Size g_transportPanelSize{1100, 48};
		constexpr ui::Size g_timeDisplaySize{480, 120};

		std::string readText(const std::filesystem::path& file)
		{
			const std::ifstream in(file, std::ios::binary);
			std::ostringstream text;
			text << in.rdbuf();
			return text.str();
		}

		// The track list's commands (ui::TrackList::actions, docs/features/track-list.md) and their keys.
		constexpr std::array<ProjectCommand, 17> g_trackCommands{{
			{"track.selectPrevious",
				"Selects the track above the selected one, and its first event while an event is selected.", "Up"},
			{"track.selectNext",
				"Selects the track below the selected one, and its first event while an event is selected.", "Down"},
			{"track.rename", "Renames the first selected track.", "Alt+F2"},
			{"track.removeSelected", "Removes the selected tracks.", nullptr},
			{"track.removeEmpty", "Removes the tracks that hold nothing.", nullptr},
			{"track.duplicateSelected", "Duplicates the selected tracks with their contents.", nullptr},
			{"track.disableSelected", "Disables the selected audio, instrument and MIDI tracks.", nullptr},
			{"track.enableSelected", "Enables the selected tracks again.", nullptr},
			{"track.toggleFolding", "Folds or unfolds the selected folders.", nullptr},
			{"track.foldAll", "Folds every folder.", nullptr},
			{"track.unfoldAll", "Unfolds every folder.", nullptr},
			{"track.flipFolding", "Folds the unfolded folders and unfolds the folded ones.", nullptr},
			{"track.controlsSettings", "Opens the track controls settings dialog.", nullptr},
			// Folder tracks (docs/features/folder-tracks.md).
			{"track.enableSelectedGroupChannels", "Turns on the group channels of the selected folders.", nullptr},
			{"track.disableSelectedGroupChannels", "Turns off the group channels of the selected folders.", nullptr},
			{"track.moveSelectedToNewFolder", "Moves the selected tracks into a new folder.", nullptr},
			{"track.moveSelectedToNewFolderWithGroupChannel",
				"Moves the selected tracks into a new folder with a group channel, which they play through.", nullptr},
		}};
	}

	AppUi::AppUi(engine::Engine& engine, const std::filesystem::path& resources, const ui::WindowSpec& main,
		std::ostream& output, int msaaSamples)
		: m_renderer(m_windows, msaaSamples), m_lua(engine, scripting::Trust::Trusted, output), m_rmlui(m_windows),
		  m_themes(fontsAndThemes(resources)), m_shell(m_windows, m_renderer), m_settings(engine, m_shell, m_themes),
		  m_binding(engine, [this] { m_shell.wake(); }), m_keys(engine, m_binding), m_main(m_shell.open(main)),
		  m_docking(engine, m_binding, m_shell, resources / "ui" / "panel-window.rml", main.hidden), m_engine(engine),
		  m_resources(resources), m_hidden(main.hidden),
		  m_history(m_binding.on("history.changed",
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
				if (const std::shared_ptr<ui::PoolPanel> pool = m_pool.lock())
					pool->update();
				return follows;
			});
		m_shell.onUnusedKey([this](ui::Window& window, const ui::KeyPress& keys) { m_keys.press(window, keys); });
		m_keys.add({"window.markers", "Opens or closes the markers window.",
			// Later in the step: the key may be the markers window's own, which closing it would destroy.
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_shell.post([this] { toggleMarkers(); }); }});
		m_keys.bind({"Mod+M", "window.markers"});

		// The project window's panels (reference manual pp. 54, 75-98), which their tickets fill.
		for (const ProjectPanel& panel : g_projectPanels)
			m_docking.add({panel.id, panel.title, "project", panel.zone, {}});
		// The edit history (reference manual pp. 110-111), a dialog there, which Edit > History opens.
		m_docking.add({"history", "History", "project", "right",
			[this](Rml::Element& element) { return std::make_shared<ui::HistoryPanel>(m_binding, element); }});
		// The pool (reference manual pp. 739-760, docs/features/pool.md), a window of its own there. Its file dialogs
		// belong to the window it shows in, and it reads the pool only while it shows.
		m_docking.add({"pool", "Pool", "project", "lower", [this](Rml::Element& element)
			{
				auto pool = std::make_shared<ui::PoolPanel>(
					m_binding, element,
					[this](const std::string& command)
					{
						ui::Window* window = m_docking.windowOf("pool");
						m_keys.run(command, engine::Json::object(), window != nullptr ? *window : m_main, nullptr);
					},
					[this] { return m_docking.showing("pool"); });
				m_pool = pool;
				return pool;
			}});
		m_docking.addKeyCommands(m_keys);
		// What the pool's commands did, sent from its toolbar, the Media menu or Find Missing Files, shows in the
		// pool, which opens for it.
		for (const char* command : {"pool.removeUnused", "media.locate", "media.removeMissing", "media.conform"})
			m_poolResults.push_back(m_binding.onResult(command,
				[this, command](const engine::Result& result)
				{
					if (!m_docking.showing("pool"))
						m_docking.open("pool");
					if (const std::shared_ptr<ui::PoolPanel> pool = m_pool.lock())
						pool->report(command, result);
				}));

		m_keys.add({"window.history", "Shows the edit history, where steps are undone and redone.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_docking.open("history"); }});
		m_keys.add({"window.pool", "Shows the pool, the project's clips and media files.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_docking.open("pool"); }});
		m_keys.bind({"Mod+P", "window.pool"});
		m_keys.add({"pool.import", "Imports audio files into the pool, without events.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{
				chooseImport(window, "Import Medium",
					{{"Audio files", "wav;w64;rf64;bwf;aif;aiff;aifc;flac;ogg;mp3"}, {"All files", "*"}}, true, true);
			}});
		m_keys.add({"pool.findMissing", "Chooses a folder to look for the pool's missing files in.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { chooseMissingFolder(window); }});
		m_keys.add({"window.studioSetup", "Opens the studio setup dialog for the audio and MIDI devices.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openStudioSetup(window); }});
		m_keys.add({"window.projectSetup",
			"Opens the project setup dialog for the project's sample rate, record format, frame rate, start time, "
			"length, time display, pan law and maximum volume.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openProjectSetup(window); }});
		// For the project setup dialog: a timecode at a project frame rate (see project.set), and the seconds of one
		// typed (none for text that isn't one).
		m_lua.addFunction("ui.formatTimecode",
			[](const engine::Json& parameters)
			{
				const time::FrameRate rate =
					time::frameRateFromName(parameters.value("frameRate", "25")).value_or(time::g_fps25);
				const double seconds = parameters.at("seconds").get<double>();
				return engine::Json{{"text", time::formatTimecode(time::toTimecode(seconds, rate), rate)}};
			});
		m_lua.addFunction("ui.parseTimecode",
			[](const engine::Json& parameters)
			{
				ui::TimeContext context;
				context.frameRate =
					time::frameRateFromName(parameters.value("frameRate", "25")).value_or(time::g_fps25);
				const std::optional<double> seconds =
					ui::parseTime(ui::TimeFormat::Timecode, parameters.at("text").get<std::string>(), context);
				return seconds ? engine::Json{{"seconds", *seconds}} : engine::Json::object();
			});
		// File > Import (docs/features/file-import.md): the OS's file dialog, then the event display imports the files.
		m_keys.add({"file.importAudio", "Imports audio files into the project window at the project cursor.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{
				chooseImport(window, "Import Audio Files",
					{{"Audio files", "wav;w64;rf64;bwf;aif;aiff;aifc;flac;ogg;mp3"}, {"All files", "*"}}, true, false);
			}});
		m_keys.add({"file.importMidi", "Imports a standard MIDI file into new tracks.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{
				chooseImport(
					window, "Import MIDI File", {{"MIDI files", "mid;midi;smf"}, {"All files", "*"}}, false, false);
			}});
		for (const char* command : {"file.importAudio", "file.importMidi", "pool.import"})
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
		for (const ProjectCommand& command : g_trackCommands)
		{
			const std::string id = command.id;
			if (id == "track.controlsSettings")
				m_keys.add({command.id, command.description,
					[this](ui::Window& window, Rml::Element* /*zone*/) { openDialog(window, "track-controls.rml"); }});
			else
				m_keys.add({command.id, command.description, [this, id](ui::Window&, Rml::Element*)
					{
						if (m_trackList)
							m_trackList->actions().at(id)();
					}});
			// The commands for selected tracks wait for a selection, Enable and Disable Group Channel for a selected
			// folder they change.
			const bool selected =
				id.find("Selected") != std::string::npos || id == "track.rename" || id == "track.toggleFolding";
			if (id == "track.enableSelectedGroupChannels" || id == "track.disableSelectedGroupChannels")
				m_keys.enableWhen(command.id,
					[this, off = id == "track.disableSelectedGroupChannels"](ui::Window&, Rml::Element*)
					{ return m_trackList != nullptr && m_trackList->selectsFolder(off); });
			else
				m_keys.enableWhen(command.id, [this, selected](ui::Window&, Rml::Element*)
					{ return m_trackList != nullptr && (!selected || !m_trackList->selection().empty()); });
			if (command.keys != nullptr)
				m_keys.bind({command.keys, command.id, engine::Json::object(), "project"});
		}

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

		// The transport controls' commands (docs/features/transport-bar.md); the engine's transport commands have
		// their keys in the default key map.
		m_keys.add({"window.transportPanel", "Shows or hides the transport panel.",
			[this](ui::Window&, Rml::Element*) { toggleTransportPanel(); }});
		m_keys.bind({"F2", "window.transportPanel"});
		m_keys.add({"window.timeDisplay", "Shows or hides the time display window.",
			[this](ui::Window&, Rml::Element*) { toggleTimeDisplay(); }});
		m_keys.add({"time.exchangeFormats", "Switches the primary and the secondary time format of the transport bar.",
			[this](ui::Window&, Rml::Element*)
			{
				if (m_transportBar)
					m_transportBar->exchangeFormats();
				else if (m_transportPanel.controls)
					m_transportPanel.controls->exchangeFormats();
			}});
		m_keys.bind({".", "time.exchangeFormats"});
		// Go to Previous Marker/Zero and Next Marker/Project End: to the project start or end without a marker.
		for (const bool next : {false, true})
			m_keys.add({next ? "locate.nextMarker" : "locate.previousMarker",
				next ? "Moves the project cursor to the next marker, or to the project end if there is none."
					 : "Moves the project cursor to the previous marker, or to the project start if there is none.",
				[this, next](ui::Window&, Rml::Element*)
				{
					m_binding.send(next ? "marker.locateNext" : "marker.locatePrevious", engine::Json::object(),
						[this, next](const engine::Result& result)
						{
							if (!result.ok() || result.value.at("marker").is_null())
								m_binding.send("transport.locate", {{"target", next ? "projectEnd" : "projectStart"}});
						});
				}});
		for (const EnterCommand& command : g_enterCommands)
		{
			m_keys.add({command.id, command.description,
				[this, field = std::string(command.field)](ui::Window&, Rml::Element*) { enter(field); }});
			m_keys.bind({command.keys, command.id});
		}
	}

	std::unique_ptr<ui::TransportControls> AppUi::transportControls(Rml::Element& root, ui::Window& window, bool fill)
	{
		if (fill)
			root.SetInnerRML(readText(m_resources / "ui" / "transport.rml"));
		const ui::WindowId id = window.id();
		return std::make_unique<ui::TransportControls>(m_engine, m_binding, root,
			[this, id](const std::string& command, const engine::Json& parameters)
			{
				if (ui::Window* found = m_shell.find(id))
					m_keys.run(command, parameters, *found, nullptr);
			});
	}

	void AppUi::toggle(TransportWindow& shown, const char* file, const char* title, ui::Size size)
	{
		if (const ui::Window* window = shown.shown(m_shell))
		{
			shown.controls.reset();
			m_shell.close(window->id());
			shown.window = 0;
			return;
		}
		shown.controls.reset(); // its window was closed
		ui::WindowSpec spec;
		spec.title = title;
		spec.size = size;
		spec.hidden = m_hidden;
		ui::Window& window = m_shell.open(spec);
		const std::filesystem::path path = m_resources / "ui" / file;
		Rml::ElementDocument* document = window.context().LoadDocument(path.generic_u8string());
		Rml::Element* controls = document != nullptr ? document->GetElementById("transport") : nullptr;
		Rml::Element* root = controls != nullptr ? controls : document;
		if (root == nullptr)
		{
			m_shell.close(window.id());
			logging::error("Cannot load {}", path.u8string());
			return;
		}
		document->Show();
		shown.window = window.id();
		// The transport panel's #transport gets the controls; the time display window has its own.
		shown.controls = transportControls(*root, window, root == controls);
	}

	void AppUi::toggleTransportPanel()
	{
		toggle(m_transportPanel, "transport-panel.rml", "Transport", g_transportPanelSize);
	}

	void AppUi::toggleTimeDisplay()
	{
		toggle(m_timeDisplay, "time-display.rml", "Time Display", g_timeDisplaySize);
	}

	void AppUi::enter(const std::string& field)
	{
		if (m_transportBar && m_transportBar->edit(field))
			return;
		if (transportPanel() == nullptr)
			toggleTransportPanel();
		if (m_transportPanel.controls)
			m_transportPanel.controls->edit(field);
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
		m_markerLines.reset();
		m_docking.attach("project", *document);
		m_trackList.reset();
		m_eventDisplay = std::make_unique<ui::EventDisplay>(m_engine, m_binding, *root);
		if (Rml::Element* bar = document->GetElementById("transport-bar"))
			m_transportBar = transportControls(*bar, m_main, true);
		m_statusLine = std::make_unique<ui::StatusLine>(m_binding, *document);
		if (Rml::Element* tracks = document->GetElementById("track-list"))
			m_trackList = std::make_unique<ui::TrackList>(*m_eventDisplay, m_binding, *tracks);
		m_menu = std::make_unique<ui::MainMenu>(m_keys, m_main, *document);
		m_markerLines = std::make_unique<ui::MarkerLines>(*m_eventDisplay, m_binding);
		return *m_eventDisplay;
	}

	ui::MarkersWindow* AppUi::toggleMarkers()
	{
		const bool open = m_markers != nullptr && m_shell.find(m_markersWindow) != nullptr;
		m_markers.reset();
		if (open)
		{
			m_shell.close(m_markersWindow);
			return nullptr;
		}
		ui::WindowSpec spec;
		spec.title = "Markers";
		spec.size = {720, 360};
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		m_markersWindow = window.id();
		const std::filesystem::path file = m_resources / "ui" / "markers.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
		{
			m_shell.close(m_markersWindow);
			throw std::runtime_error("cannot load the markers window from " + file.u8string());
		}
		document->Show();
		m_markers = std::make_unique<ui::MarkersWindow>(m_engine, m_binding, *document);
		return m_markers.get();
	}

	void AppUi::chooseImport(
		ui::Window& window, std::string title, std::vector<ui::FileFilter> filters, bool many, bool clipsOnly)
	{
		ui::FileDialog dialog;
		dialog.title = std::move(title);
		dialog.filters = std::move(filters);
		dialog.many = many;
		dialog.parent = window.id();
		m_shell.showFileDialog(dialog,
			[this, clipsOnly](const ui::FileDialogResult& result)
			{
				if (!result.error.empty())
					logging::warning("The file dialog failed: {}", result.error);
				if (result.files.empty() || !m_eventDisplay)
					return;
				std::vector<std::string> files;
				files.reserve(result.files.size());
				for (const std::filesystem::path& file : result.files)
					files.push_back(file.u8string());
				m_eventDisplay->importFiles(files, std::nullopt, std::nullopt, clipsOnly);
			});
	}

	void AppUi::chooseMissingFolder(ui::Window& window)
	{
		ui::FileDialog dialog;
		dialog.kind = ui::FileDialogKind::Folder;
		dialog.title = "Find Missing Files";
		dialog.parent = window.id();
		m_shell.showFileDialog(dialog,
			[this](const ui::FileDialogResult& result)
			{
				if (!result.error.empty())
					logging::warning("The folder dialog failed: {}", result.error);
				if (!result.files.empty())
					m_binding.send("media.locate", {{"folder", result.files.front().u8string()}});
			});
	}

	Rml::ElementDocument& AppUi::openStudioSetup(ui::Window& window)
	{
		return openOnce(window, "studio-setup.rml", m_studioSetup);
	}

	Rml::ElementDocument& AppUi::openProjectSetup(ui::Window& window)
	{
		return openOnce(window, "project-setup.rml", m_projectSetup);
	}

	Rml::ElementDocument& AppUi::openOnce(
		ui::Window& window, const char* name, Rml::ObserverPtr<Rml::Element>& open) const
	{
		// One dialog: a second document would share the first one's table in the UI's Lua. A closed document has left
		// its context, until the context releases it.
		auto* document = dynamic_cast<Rml::ElementDocument*>(open.get());
		if (document != nullptr && document->GetParentNode() != nullptr)
		{
			document->Show(Rml::ModalFlag::Modal, Rml::FocusFlag::Keep);
			document->PullToFront();
			return *document;
		}
		Rml::ElementDocument& opened = openDialog(window, name);
		open = opened.GetObserverPtr();
		return opened;
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
