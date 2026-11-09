// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/appui.h"

#include "app/uitools.h"
#include "platform/log.h"
#include "ui/chordpads.h"
#include "ui/fonts.h"
#include "ui/historypanel.h"
#include "ui/inspector.h"
#include "ui/poolpanel.h"
#include "ui/timeformat.h"
#include "ui/widgets.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Lua.h>
#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
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
			{"channel", "Channel", "channel"},
			{"inspector", "Inspector", "left"},
			{"visibility", "Visibility", "left"},
			{"editor", "Editor", "lower"},
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

		constexpr std::array<ProjectCommand, 90> g_projectCommands{{
			{"zoom.in", "Zooms the project window in.", "H"},
			{"zoom.out", "Zooms the project window out.", "G"},
			{"zoom.inVertically", "Makes the project window's tracks taller.", "Shift+H"},
			{"zoom.outVertically", "Makes the project window's tracks lower.", "Shift+G"},
			{"zoom.full", "Shows the whole project in the project window.", "Shift+F"},
			{"zoom.undo", "Undoes the project window's last zoom.", nullptr},
			{"zoom.redo", "Redoes the project window's last undone zoom.", nullptr},
			{"view.autoScroll", "Switches the project window's auto-scroll on or off.", "F"},
			// Nudge Cursor Left and Right (reference manual p. 300), bound everywhere with the transport's keys.
			{"locate.nudgeCursorLeft", "Moves the project cursor left by a grid step.", nullptr},
			{"locate.nudgeCursorRight", "Moves the project cursor right by a grid step.", nullptr},
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
			// Tempo detection and tempo from MIDI (docs/features/tempo-detection.md).
			{"tempo.detection", "Opens the Tempo Detection dialog for the selected audio event or MIDI part.", nullptr},
			{"midi.calculateTempo", "Opens the Calculate Tempo from MIDI Events dialog for the selected MIDI parts.",
				nullptr},
			// Process Tempo, Process Bars and warp markers (docs/features/time-warp.md).
			{"tempo.openProcessTempo", "Opens the Process Tempo dialog for the locator range.", nullptr},
			{"tempo.openProcessBars", "Opens the Process Bars dialog.", nullptr},
			{"audio.setDefinitionFromTempo",
				"Opens the Set Definition from Tempo dialog for the selected audio events.", nullptr},
			{"audio.copyWarpMarkers", "Copies the warp markers of the first selected audio event.", nullptr},
			{"audio.pasteWarpMarkers", "Pastes the copied warp markers into the selected audio events.", nullptr},
			// Fades and crossfades (docs/features/fades.md), with the reference's key for Crossfade (manual p. 388).
			{"audio.fadeInToRange", "Fades the audio events in the selection range in up to the range's start.",
				nullptr},
			{"audio.fadeOutFromRange", "Fades the audio events in the selection range out from the range's end.",
				nullptr},
			{"audio.adjustFadesToRange", "Adjusts the fades of the audio events in the selection range to it.",
				nullptr},
			{"audio.crossfade",
				"Crossfades the selected audio events, or opens the crossfade editor for their crossfade.", "X"},
			{"audio.removeFadeIn", "Removes the fade in of the selected audio events.", nullptr},
			{"audio.removeFadeOut", "Removes the fade out of the selected audio events.", nullptr},
			{"audio.removeFades", "Removes the fades and crossfades of the selected audio events.", nullptr},
			{"audio.fadeEditor", "Opens the fade dialog for the selected audio events.", nullptr},
			// The audio functions (docs/features/audio-functions.md).
			{"audio.detectSilence", "Opens the Detect Silence dialog for the selected audio events.", nullptr},
			{"audio.spectrumAnalyzer", "Opens the Spectrum Analyzer for the selected audio event.", nullptr},
			{"audio.statistics", "Opens the Statistics window for the selected audio events.", nullptr},
			// Warping (docs/features/warp.md).
			{"audio.flatten", "Renders the real-time warping of the selected audio events into new files.", nullptr},
			{"audio.unstretch", "Removes the real-time stretching from the selected audio events' clips.", nullptr},
			// Volume curves (docs/features/volume-curves.md).
			{"audio.removeVolumeCurve",
				"Resets the volume curves of the audio events in the selection range, or of the selected ones.",
				nullptr},
			// Audio parts (docs/features/audio-parts.md).
			{"audio.eventsToPart", "Puts the selected audio events into an audio part.", nullptr},
			{"audio.dissolvePart", "Makes the events of the selected audio parts events of their own.", nullptr},
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
			{"tool.comp", "Chooses the comp tool in the project window.", nullptr},
			{"tool.timeWarp", "Chooses the time warp tool in the project window.", nullptr},
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
		// The time display window at each of its sizes (ui.timeDisplay); the theme sizes its digits to fit.
		constexpr std::array<ui::Size, 5> g_timeDisplaySizes{
			{{320, 80}, {480, 120}, {640, 160}, {960, 240}, {1280, 320}}};

		std::string readText(const std::filesystem::path& file)
		{
			const std::ifstream in(file, std::ios::binary);
			std::ostringstream text;
			text << in.rdbuf();
			return text.str();
		}

		// The track list's commands (ui::TrackList::actions, docs/features/track-list.md) and their keys.
		constexpr std::array<ProjectCommand, 35> g_trackCommands{{
			{"track.selectPrevious",
				"Selects the track above the selected one, and its first event while an event is selected (unless "
				"ui.upDownSelectsTracksOnly).",
				"Up"},
			{"track.selectNext",
				"Selects the track below the selected one, and its first event while an event is selected (unless "
				"ui.upDownSelectsTracksOnly).",
				"Down"},
			{"track.rename", "Renames the first selected track.", "Alt+F2"},
			{"track.removeSelected", "Removes the selected tracks.", nullptr},
			{"track.duplicateSelected", "Duplicates the selected tracks with their contents.", nullptr},
			{"track.disableSelected", "Disables the selected audio, instrument and MIDI tracks.", nullptr},
			{"track.enableSelected", "Enables the selected tracks again.", nullptr},
			// Render in place and freeze (docs/features/render-in-place.md).
			{"track.freezeSelected",
				"Freezes the selected audio and instrument tracks, or unfreezes them if every one is frozen.", nullptr},
			{"track.renderSelected", "Renders the selected audio and instrument tracks in place.", nullptr},
			// The chord track (docs/features/chord-track.md).
			{"track.chordsToMidiSelected",
				"Chords to MIDI: a MIDI part of the chord events on the first selected MIDI or instrument track, which "
				"then follows the chord track.",
				nullptr},
			{"track.createChordEventsSelected",
				"Create Chord Events: chord events from the MIDI notes of the selected MIDI and instrument tracks.",
				nullptr},
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
			// Automation tracks (docs/features/automation-tracks.md).
			{"track.toggleAutomationSelected",
				"Shows the automation tracks of the selected tracks, or hides them if every one shows them.", nullptr},
			{"track.showUsedAutomation", "Shows the automation tracks with automation on every track.", nullptr},
			{"track.hideAllAutomation", "Hides every automation track.", nullptr},
			// Copying automation to other tracks (docs/features/copy-automation.md).
			{"track.copyAutomationAll", "Copies the automation of every track between the locators.", nullptr},
			{"track.copyAutomationSelected", "Copies the automation of the selected tracks between the locators.",
				nullptr},
			{"track.copyAutomationSelectedParameters",
				"Copies the automation of the selected automation tracks between the locators.", nullptr},
			{"track.pasteAutomationAll",
				"Pastes the copied automation onto the tracks it was copied from, between the locators.", nullptr},
			{"track.pasteAutomationSelected",
				"Pastes the copied automation onto the selected tracks, between the locators.", nullptr},
			{"track.pasteAutomationSelectedParameters",
				"Pastes the copied automation onto the selected automation tracks of the same parameters, between the "
				"locators.",
				nullptr},
			// Lanes (docs/features/comping.md).
			{"track.toggleLanesSelected",
				"Shows the lanes of the selected tracks, or hides them if every one shows them.", nullptr},
			// Track versions (docs/features/track-versions.md).
			{"track.newVersion", "Gives the selected tracks a new, empty version.", nullptr},
			{"track.duplicateVersion", "Gives the selected tracks a copy of their active version.", nullptr},
			{"track.deleteVersion", "Deletes the active version of the selected tracks.", nullptr},
			{"track.selectSameVersionId",
				"Selects the tracks with a version of the ID of the first selected track's active version.", nullptr},
			{"track.assignCommonVersionId", "Gives the active versions of the selected tracks a common ID.", nullptr},
		}};
	}

	AppUi::AppUi(engine::Engine& engine, const std::filesystem::path& resources, const ui::WindowSpec& main,
		std::ostream& output, int msaaSamples)
		: m_scriptOutput(output), m_renderer(m_windows, msaaSamples),
		  m_lua(engine, scripting::Trust::Trusted, m_luaOutput), m_rmlui(m_windows),
		  m_themes(fontsAndThemes(resources)), m_shell(m_windows, m_renderer), m_settings(engine, m_shell, m_themes),
		  m_binding(
			  engine, [this] { m_shell.wake(); }, [this] { m_shell.requestRender(); }),
		  m_keys(engine, m_binding), m_main(m_shell.open(main)),
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
		m_rmlui.loadCursors(resources / "cursors");
		lua_State* state = m_lua.state();
		// GNUbase's print, instead of the plugin's, which writes into RmlUi's log. Kept in the registry: the plugin
		// changes the stack.
		lua_getglobal(state, "print");
		const int print = luaL_ref(state, LUA_REGISTRYINDEX);
		Rml::Lua::Initialise(state); // until Rml::Shutdown
		lua_rawgeti(state, LUA_REGISTRYINDEX, print);
		luaL_unref(state, LUA_REGISTRYINDEX, print);
		lua_setglobal(state, "print");
		addUiFunctions(m_lua, m_shell);
		m_shell.eachStep(
			[this]
			{
				bool scripted = false;
				if (const std::optional<std::string> error = m_lua.dispatchEvents(&scripted))
					logging::warning("A Lua event handler of the UI failed: {}", *error);
				if (scripted)
					m_shell.requestRender(); // what documents' gnubase.on handlers changed, wherever it shows
				const bool follows = m_binding.update();
				if (m_menu)
					m_menu->update();
				if (const std::shared_ptr<ui::PoolPanel> pool = m_pool.lock())
					pool->update();
				for (MixerWindow& shown : m_mixers)
					if (shown.mixer && m_shell.find(shown.window) == nullptr)
						shown.mixer.reset(); // the user closed its window
				if (m_channelSettings.mixer && m_shell.find(m_channelSettings.window) == nullptr)
					m_channelSettings.mixer.reset();
				// The channel settings window follows the track selection (reference manual p. 499).
				if (ui::Mixer* settings = channelSettings(); settings != nullptr && settings->follows() && m_trackList)
				{
					const std::vector<uint64_t> tracks = m_trackList->selection();
					if (!tracks.empty() && tracks.front() != m_settingsTrack)
					{
						m_settingsTrack = tracks.front();
						settings->showTrack(m_settingsTrack);
					}
				}
				if (m_keyboard && m_shell.find(m_keyboardWindow) == nullptr)
					m_keyboard.reset(); // so too: its notes end
				// The project window selects events or markers: an event selected deselects the markers.
				if (m_eventDisplay && m_markerLines && m_eventDisplay->selection() != m_eventSelection)
				{
					m_eventSelection = m_eventDisplay->selection();
					if (!m_eventSelection.empty())
						m_markerLines->deselect();
				}
				if (m_infoLine)
					m_infoLine->update();
				followSelection();
				// The inspector shows the first selected track.
				if (const std::shared_ptr<ui::Inspector> inspector = m_inspector.lock(); inspector && m_eventDisplay)
				{
					const std::vector<uint64_t> tracks =
						m_trackList ? m_trackList->selection() : std::vector<uint64_t>();
					inspector->update(tracks.empty() ? 0 : tracks.front(), m_eventDisplay->project(),
						m_eventDisplay->loads(), m_eventDisplay->read());
				}
				return follows;
			});
		m_shell.onUnusedKey(
			[this](ui::Window& window, const ui::KeyPress& keys)
			{
				if (ui::OnScreenKeyboard* keyboard = this->keyboard(); keyboard == nullptr || !keyboard->press(keys))
					m_keys.press(window, keys);
			});
		m_shell.onKeyReleased(
			[this](ui::Window&, const ui::KeyPress& keys)
			{
				if (ui::OnScreenKeyboard* keyboard = this->keyboard(); keyboard != nullptr)
					keyboard->release(keys);
				m_keys.released(keys);
				if (keys.key == Rml::Input::KI_UNKNOWN)
					m_winding.stop(); // the keyboard focus lost: also a button held
			});
		m_keys.add({"window.markers", "Opens or closes the markers window.",
			// Later in the step: the key may be the markers window's own, which closing it would destroy.
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_shell.post([this] { toggleMarkers(); }); }});
		m_keys.bind({"Mod+M", "window.markers"});
		m_keys.add({"window.onScreenKeyboard", "Shows or hides the on-screen keyboard.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_shell.post([this] { toggleKeyboard(); }); }});
		m_keys.bind({"Alt+K", "window.onScreenKeyboard"});
		// The tempo track (reference manual pp. 1447, 1451).
		m_keys.add({"window.tempoTrack", "Opens or closes the tempo track editor.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_shell.post([this] { toggleTempoEditor(); }); }});
		m_keys.bind({"Mod+T", "window.tempoTrack"});
		m_keys.add({"window.arranger", "Opens or closes the arranger editor.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_shell.post([this] { toggleArranger(); }); }});
		m_keys.add({"tempo.toggleTrack", "Switches between fixed tempo and tempo track mode (Activate Tempo Track).",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{
				m_binding.send("project.get", engine::Json::object(),
					[this](const engine::Result& result)
					{
						if (result.ok())
							m_binding.send(
								"project.set", {{"tempoTrackActive", !result.value.value("tempoTrackActive", false)}});
					});
			}});
		// Insert Tempo Event at Cursor (reference manual pp. 1452-1453): a key command without a key, whose dialog
		// starts at the tempo at the cursor and adds the tempo typed there (tempo.add).
		m_keys.add({"tempo.insertAtCursor", "Asks for a tempo and adds a tempo event with it at the project cursor.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{
				m_binding.send("project.get", engine::Json::object(),
					[this, id = window.id()](const engine::Result& project)
					{
						if (!project.ok())
							return;
						m_binding.send("transport.get", engine::Json::object(),
							[this, id, context = ui::projectTime(project.value)](const engine::Result& transport)
							{
								ui::Window* found = m_shell.find(id);
								if (found == nullptr || !transport.ok())
									return;
								const auto cursor = transport.value.at("position").get<int64_t>();
								openDialog(*found, "insert-tempo.rml",
									{{"bpm", ui::tempoText(ui::currentTempo(context, cursor))}});
							});
					});
			}});
		// Select All Events in the tempo track's context menu (p. 1450): every tempo event on its lane.
		m_keys.add({"tempo.selectAll", "Selects every tempo event on the tempo track.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{
				if (m_tempoLane)
					m_tempoLane->curve().selectAll();
			}});
		m_keys.enableWhen("tempo.selectAll",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{
				if (!m_trackList || !m_tempoLane || m_tempoLane->curve().tempo().track == 0)
					return false;
				const std::vector<uint64_t> tracks = m_trackList->selection();
				return std::find(tracks.begin(), tracks.end(), m_tempoLane->curve().tempo().track) != tracks.end();
			});

		// The project window's panels (reference manual pp. 54, 73, 75-98), which their tickets fill: the inspector
		// (docs/features/inspector.md) shows the selected track, the Channel zone's will show its channel strip (#107).
		for (const ProjectPanel& panel : g_projectPanels)
		{
			ui::Panel added{panel.id, panel.title, "project", panel.zone, {}};
			if (added.id == "inspector")
				added.create = [this](Rml::Element& element)
				{
					auto inspector = std::make_shared<ui::Inspector>(m_binding, element,
						[this](uint64_t track) { m_shell.post([this, track] { openChannelSettings(track); }); });
					m_inspector = inspector;
					return std::shared_ptr<void>(inspector);
				};
			// The lower zone's editor (reference manual pp. 1132-1156) and, on the Visibility tab's Editor page, its
			// visibility; the Track page has the track visibility (#166).
			else if (added.id == "editor" || added.id == "visibility")
				added.create = [this, editor = added.id == "editor"](Rml::Element& element)
				{
					Rml::Element& page = editor || !m_trackVisibility ? element : m_trackVisibility->showPanel(element);
					auto shown = std::make_shared<ui::MidiEditor>(
						m_binding, page, "lower", editor ? ui::EditorPart::Editor : ui::EditorPart::Visibility);
					if (editor)
						m_lowerEditor = shown;
					return std::shared_ptr<void>(shown);
				};
			m_docking.add(std::move(added));
		}
		addMixers();
		addEditors();
		// The edit history (reference manual pp. 110-111), a dialog there, which Edit > History opens.
		m_docking.add({"history", "History", "project", "right",
			[this](Rml::Element& element) { return std::make_shared<ui::HistoryPanel>(m_binding, element); }});
		addQuantize();
		addMidiFunctions();
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
		// The chord pads (reference manual pp. 1346-1391, docs/features/chord-pads.md) in the lower zone.
		m_docking.add({"chordPads", "Chord Pads", "project", "lower",
			[this](Rml::Element& element) { return std::make_shared<ui::ChordPadsPanel>(m_binding, element); }});
		m_docking.addKeyCommands(m_keys);
		// What the pool's commands did, sent from its toolbar, the Media menu or Find Missing Files, shows in the
		// pool, which opens for it: its window comes to the front if it is in one of its own.
		for (const char* command :
			{"pool.removeUnused", "media.locate", "media.removeMissing", "media.conform", "media.prepareArchive"})
			m_poolResults.push_back(m_binding.onResult(command,
				[this, command](const engine::Result& result)
				{
					if (!m_docking.showing("pool") || m_docking.windowOf("pool") != nullptr)
						m_docking.open("pool");
					if (const std::shared_ptr<ui::PoolPanel> pool = m_pool.lock())
						pool->report(command, result);
				}));

		m_keys.add({"window.chordPads", "Shows or hides the chord pads in the lower zone.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{
				if (m_docking.showing("chordPads") && m_docking.windowOf("chordPads") == nullptr)
					m_docking.setVisible("project", "lower", false);
				else
					m_docking.open("chordPads");
			}});
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
		m_keys.add({"window.midiDeviceManager",
			"Opens the MIDI device manager, which installs and removes MIDI devices and shows their patch banks.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{ openOnce(window, "midi-devices.rml", m_midiDevices); }});
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
		m_keys.add({"window.preferences",
			"Opens the preferences dialog, which shows and changes the user's preferences, page by page, and their "
			"presets.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openPreferences(window); }});
		// For the preferences dialog: the themes there are, and the tool modifiers that ui.toolModifiers overrides.
		m_lua.addFunction(
			"ui.themes", [this](const engine::Json&) { return engine::Json{{"themes", m_themes.names()}}; });
		m_lua.addFunction("ui.toolModifierDefaults",
			[](const engine::Json&) { return engine::Json{{"modifiers", ui::EventDisplay::defaultToolModifiers()}}; });
		// For the add track dialog's Time Offset: a position typed in a time format, as position displays show it in
		// the project (ui/timeformat.h), in seconds and ticks; nothing for text that isn't one.
		m_lua.addFunction("ui.parseTime",
			[this](const engine::Json& parameters)
			{
				const std::optional<ui::TimeFormat> format =
					ui::timeFormatFromName(parameters.value("format", std::string()));
				const engine::Result project = engine::Client(m_engine).call("project.get");
				ui::TimeContext context;
				if (!format || !project.ok() || !ui::readTimeContext(project.value, context))
					return engine::Json::object();
				const std::optional<double> seconds =
					ui::parseTime(*format, parameters.at("text").get<std::string>(), context);
				return seconds ? engine::Json{{"seconds", *seconds}, {"ticks", context.tempo.toTicks(*seconds)}}
							   : engine::Json::object();
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
		// File > Export (docs/features/mixdown.md): the Export Audio Mixdown dialog, and Quick Audio Export into a WAV
		// file chosen in the OS's file dialog.
		m_keys.add({"file.exportMixdown", "Opens the Export Audio Mixdown dialog.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openDialog(window, "export-mixdown.rml"); }});
		m_keys.add({"file.quickExport", "Exports the main mix of the whole project into a WAV file.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{
				ui::FileDialog dialog;
				dialog.kind = ui::FileDialogKind::Save;
				dialog.title = "Quick Audio Export";
				dialog.filters = {{"Wave files", "wav"}};
				dialog.location = "Mixdown.wav";
				dialog.parent = window.id();
				m_shell.showFileDialog(dialog,
					[this](const ui::FileDialogResult& chosen)
					{
						if (!chosen.error.empty())
							logging::warning("The file dialog failed: {}", chosen.error);
						if (chosen.files.empty())
							return;
						std::filesystem::path file = chosen.files.front();
						if (file.extension().empty())
							file += ".wav";
						m_binding.send("mixdown.quickExport", {{"path", file.u8string()}});
					});
			}});
		// File > Export and File > Import > Tempo Track (docs/features/tempo-track.md) and Markers
		// (docs/features/markers.md): their files, chosen in the OS's file dialog.
		struct TrackFile
		{
			const char* name;    // in the dialog's title and the exported file's name
			const char* command; // the UI commands' suffix
			const char* engine;  // the engine commands' prefix
			const char* filter;
			const char* extension; // an exported file without one gets it
			const char* exportDescription;
			const char* importDescription;
		};
		for (const TrackFile& kind : {TrackFile{"Tempo Track", "Tempo", "tempo", "Tempo track files", "xml",
										  "Exports the tempo track and the time signature events into an XML file.",
										  "Replaces the tempo track and the time signature events with those of an "
										  "exported file."},
				 TrackFile{"Markers", "Markers", "marker", "Marker files", "csv",
					 "Exports the marker tracks into a CSV file.",
					 "Adds the marker tracks of a CSV file to the project."}})
		{
			for (const bool exporting : {true, false})
			{
				m_keys.add({std::string(exporting ? "file.export" : "file.import") + kind.command,
					exporting ? kind.exportDescription : kind.importDescription,
					[this, kind, exporting](ui::Window& window, Rml::Element* /*zone*/)
					{
						ui::FileDialog dialog;
						dialog.kind = exporting ? ui::FileDialogKind::Save : ui::FileDialogKind::Open;
						dialog.title = std::string(exporting ? "Export " : "Import ") + kind.name;
						dialog.filters = {{kind.filter, kind.extension}, {"All files", "*"}};
						if (exporting)
							dialog.location = std::string(kind.name) + "." + kind.extension;
						dialog.parent = window.id();
						m_shell.showFileDialog(dialog,
							[this, kind, exporting](const ui::FileDialogResult& chosen)
							{
								if (!chosen.error.empty())
									logging::warning("The file dialog failed: {}", chosen.error);
								if (chosen.files.empty())
									return;
								std::filesystem::path file = chosen.files.front();
								if (exporting && file.extension().empty())
									file += std::string(".") + kind.extension;
								m_binding.send(std::string(kind.engine) + (exporting ? ".export" : ".import"),
									{{"path", file.u8string()}});
							});
					}});
			}
		}
		// Auto fades (docs/features/fades.md): the project's, or the first selected track's.
		m_keys.add({"project.autoFades", "Opens the auto fades dialog for the whole project.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openAutoFades(window, 0); }});
		m_keys.add({"track.autoFades", "Opens the auto fades dialog for the first selected audio track.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{
				if (m_trackList && !m_trackList->selection().empty())
					openAutoFades(window, m_trackList->selection().front());
			}});
		m_keys.enableWhen("track.autoFades",
			[this](ui::Window&, Rml::Element*) { return m_trackList != nullptr && !m_trackList->selection().empty(); });
		m_keys.add({"project.addTrack", "Opens the add track dialog.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openDialog(window, "add-track.rml"); }});
		// Group Track to Selected Tracks and Effect Track to Selected Tracks (reference manual pp. 546, 554): the add
		// track dialog's page for the new track, which the selected tracks play or send into.
		for (const auto& [id, type, description] :
			{std::tuple{"track.addGroupToSelected", "group",
				 "Opens the add track dialog for a group track that the selected tracks play into."},
				std::tuple{"track.addEffectToSelected", "effect",
					"Opens the add track dialog for an effect track that the selected tracks send to."}})
		{
			m_keys.add({id, description, [this, type = std::string(type)](ui::Window& window, Rml::Element* /*zone*/)
				{
					std::string tracks;
					for (const uint64_t track : m_trackList ? m_trackList->selection() : std::vector<uint64_t>())
						tracks += (tracks.empty() ? "" : ",") + std::to_string(track);
					openDialog(window, "add-track.rml", {{"type", type}, {"tracks", tracks}});
				}});
			m_keys.enableWhen(id, [this](ui::Window&, Rml::Element*)
				{ return m_trackList != nullptr && !m_trackList->selection().empty(); });
		}
		// Project > Project Colors Setup (docs/features/colors.md).
		m_keys.add({"project.colorsSetup", "Opens the project colors setup dialog, which edits the project's colors.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openDialog(window, "project-colors.rml"); }});
		// For documents' buttons and menu items: gnubase.ui.command{command = "project.addTrack"} runs a key command
		// in the main window, as its keys would. A command that doesn't exist or parameters it doesn't take are a Lua
		// error at the call (KeyCommands::check), in users' scripts too, instead of an engine error later or parameters
		// a UI command drops (#669).
		m_lua.addFunction("ui.command",
			[this](const engine::Json& parameters)
			{
				const auto command = parameters.find("command");
				if (!parameters.is_object() || command == parameters.end() || !command->is_string())
					throw std::invalid_argument("ui.command needs a table with a command, the name of a key command");
				for (const auto& item : parameters.items())
					if (item.key() != "command" && item.key() != "parameters")
						throw std::invalid_argument("ui.command takes a command and its parameters, not " + item.key());
				const engine::Json commandParameters = parameters.value("parameters", engine::Json::object());
				m_keys.check(command->get<std::string>(), commandParameters);
				m_keys.run(command->get<std::string>(), commandParameters, m_main, nullptr);
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
			// Those that only edit the selection range wait for one (EventDisplay::canRun).
			m_keys.enableWhen(command.id, [this, id = std::string(command.id)](ui::Window&, Rml::Element*)
				{ return m_eventDisplay != nullptr && m_eventDisplay->canRun(id); });
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
				m_keys.add(
					{command.id, command.description, [this, id = std::string(command.id)](ui::Window&, Rml::Element*)
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

		// Set Root Key for Unassigned Events in the context menu of the transpose track (reference manual p. 416).
		m_keys.enableWhen("transpose.assignRootKey", [this](ui::Window&, Rml::Element*)
			{ return m_trackList != nullptr && m_trackList->selectsType("transpose"); });

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

		// Lock Record (docs/features/recording.md): a stop or punch out that the engine refused because recording is
		// locked asks whether to stop anyway (lock-record.rml).
		for (const char* command : {"transport.stop", "transport.toggle", "transport.record"})
			m_lockRecord.push_back(m_binding.onResult(command,
				[this](const engine::Result& result)
				{
					if (result.ok())
						return;
					m_binding.send("transport.get", engine::Json::object(),
						[this](const engine::Result& status)
						{
							if (status.ok() && status.value.at("lockRecord").get<bool>() &&
								status.value.at("recording").get<bool>())
								openOnce(m_main, "lock-record.rml", m_lockRecordDialog);
						});
				}));
		// The status line's remaining record time changes as the disk fills up, also by other programs.
		m_shell.every(std::chrono::seconds(1),
			[this]
			{
				if (m_statusLine)
					m_statusLine->update();
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
		m_keys.bind({"Mod+Num-", "locate.nudgeCursorLeft"});
		m_keys.bind({"Mod+Num+", "locate.nudgeCursorRight"});
		// Rewind and Forward, and their fast variants, wind while held (reference manual pp. 297, 300, 1571).
		struct Wind
		{
			const char* id;
			const char* description;
			double step;
			const char* keys;
		};
		for (const Wind& wind :
			{Wind{"wind.rewind", "Moves the project cursor back, winding while held.", -1.0, "Num-"},
				Wind{"wind.forward", "Moves the project cursor on, winding while held.", 1.0, "Num+"},
				Wind{
					"wind.fastRewind", "Moves the project cursor back faster, winding while held.", -4.0, "Shift+Num-"},
				Wind{"wind.fastForward", "Moves the project cursor on faster, winding while held.", 4.0, "Shift+Num+"}})
		{
			m_keys.add({wind.id, wind.description, [this, step = wind.step](ui::Window&, Rml::Element*)
				{ m_winding.start(step); }, [this] { m_winding.stop(); }});
			m_keys.bind({wind.keys, wind.id});
		}
		m_shell.eachStep([this] { return m_winding.step(); });
		// The time display window's context menu (reference manual p. 313).
		m_keys.add({"timeDisplay.beatCountOnly",
			"Shows only the beat in its bar in the time display window, or the whole position again.",
			[this](ui::Window&, Rml::Element*)
			{
				if (timeDisplay() != nullptr && m_timeDisplay.controls)
					m_timeDisplay.controls->toggleBeatCountOnly();
			}});
		for (const int steps : {1, -1})
			m_keys.add({steps > 0 ? "timeDisplay.increaseSize" : "timeDisplay.decreaseSize",
				steps > 0 ? "Makes the time display window and its digits larger."
						  : "Makes the time display window and its digits smaller.",
				[this, steps](ui::Window&, Rml::Element*)
				{
					if (timeDisplay() != nullptr && m_timeDisplay.controls)
						m_timeDisplay.controls->resize(steps);
				}});
		for (const char* command :
			{"timeDisplay.beatCountOnly", "timeDisplay.increaseSize", "timeDisplay.decreaseSize"})
			m_keys.enableWhen(command, [this](ui::Window&, Rml::Element*) { return timeDisplay() != nullptr; });
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
		addScripts();
		addKeyCommandsDialog();
	}

	void AppUi::addKeyCommandsDialog()
	{
		// The key commands dialog (#161, docs/features/key-commands.md, reference manual pp. 1554-1559), which
		// Edit > Key Commands opens.
		m_keys.add({"window.keyCommands",
			"Opens the key commands dialog, which assigns keys to commands, sets up macros and keeps key commands "
			"presets.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openOnce(window, "key-commands.rml", m_keyDialog); }});
		// Its Import and Export: the OS's file dialog, then keyCommands.import or keyCommands.export.
		for (const bool exporting : {false, true})
			m_keys.add({exporting ? "keyCommands.exportFile" : "keyCommands.importFile",
				exporting ? "Exports the key commands and macros into a key commands file that the user chooses."
						  : "Imports a key commands file that the user chooses as a key commands preset.",
				[this, exporting](ui::Window& window, Rml::Element* /*zone*/)
				{
					ui::FileDialog dialog;
					dialog.kind = exporting ? ui::FileDialogKind::Save : ui::FileDialogKind::Open;
					dialog.title = exporting ? "Export Key Commands" : "Import Key Commands";
					dialog.filters = {{"Key commands files", "json"}};
					dialog.parent = window.id();
					m_shell.showFileDialog(dialog,
						[this, exporting](const ui::FileDialogResult& result)
						{
							if (!result.error.empty())
								logging::warning("The file dialog failed: {}", result.error);
							if (result.files.empty())
								return;
							const std::filesystem::path& file = result.files.front();
							if (exporting)
								m_binding.send("keyCommands.export", {{"path", file.u8string()}});
							else
								m_binding.send("keyCommands.import",
									{{"path", file.u8string()}, {"name", file.stem().u8string()}});
						});
				}});
		// For the dialog: the commands with their default keys, and keys as the settings and the platform write them.
		m_lua.addFunction(
			"ui.keyCommands", [this](const engine::Json&) { return engine::Json{{"commands", m_keys.commands()}}; });
		m_lua.addFunction("ui.keyName",
			[](const engine::Json& parameters)
			{
				ui::KeyPress keys;
				try
				{
					if (const auto written = parameters.find("keys"); written != parameters.end())
					{
						keys = ui::parseKeys(written->get<std::string>());
					}
					else
					{
						keys.key = static_cast<Rml::Input::KeyIdentifier>(parameters.at("key").get<int>());
						for (const auto& [name, flag] :
							{std::pair<const char*, int>{"ctrl", Rml::Input::KM_CTRL}, {"alt", Rml::Input::KM_ALT},
								{"shift", Rml::Input::KM_SHIFT}, {"meta", Rml::Input::KM_META}})
							if (parameters.value(name, false))
								keys.modifiers |= flag;
					}
					// Keys that can't be written, such as a modifier on its own, are none.
					const std::string setting = ui::keySetting(keys);
					if (!(ui::parseKeys(setting) == keys) || keys.key == Rml::Input::KI_LMENU)
						return engine::Json::object();
					return engine::Json{{"keys", setting}, {"name", ui::keyName(keys)}};
				}
				catch (const std::invalid_argument&)
				{
					return engine::Json::object();
				}
			});
		// The user's key commands and macros, now and when they change.
		const auto setting = [this](const char* key)
		{
			const engine::Result result = engine::Client(m_engine).call("settings.get", {{"key", key}});
			return result.ok() ? result.value.at("value") : engine::Json::array();
		};
		m_keySettings = {{"ui.keyCommands", setting("ui.keyCommands")}, {"ui.macros", setting("ui.macros")}};
		applyKeySettings();
		m_keySettingsChanged.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const engine::Json& values = event.data.at("values");
				bool changed = false;
				for (const char* key : {"ui.keyCommands", "ui.macros"})
					if (const auto value = values.find(key); value != values.end())
					{
						m_keySettings[key] = *value;
						changed = true;
					}
				if (changed)
					applyKeySettings();
			}));
	}

	void AppUi::applyKeySettings()
	{
		m_keys.customize(m_keySettings.at("ui.keyCommands"), m_keySettings.at("ui.macros"));
		// Edit > Macros: the macros, and Key Commands to set them up.
		auto* document = dynamic_cast<Rml::ElementDocument*>(m_menuDocument.get());
		Rml::Element* menu = document != nullptr ? document->GetElementById("macros-menu") : nullptr;
		if (menu == nullptr)
			return;
		std::string items;
		for (const engine::Json& macro : m_keySettings.at("ui.macros"))
		{
			const std::string name = macro.value("name", std::string());
			items += "<menuitem command=\"macro." + Rml::StringUtilities::EncodeRml(name) + "\">" +
				Rml::StringUtilities::EncodeRml(name) + "</menuitem>";
		}
		if (!items.empty())
			items += "<menuseparator/>";
		menu->SetInnerRML(items + "<menuitem command=\"window.keyCommands\">Key Commands...</menuitem>");
		// Edit > Scripts: the user scripts, and the script console (docs/features/scripts.md).
		if (Rml::Element* scripts = document->GetElementById("scripts-menu"))
		{
			items.clear();
			for (const auto& [name, file] : m_scripts)
				items += "<menuitem command=\"script." + Rml::StringUtilities::EncodeRml(name) + "\">" +
					Rml::StringUtilities::EncodeRml(name) + "</menuitem>";
			if (!items.empty())
				items += "<menuseparator/>";
			scripts->SetInnerRML(items + "<menuitem command=\"window.scriptConsole\">Script Console</menuitem>");
		}
		m_menu.reset(); // its items are gone
		m_menu = std::make_unique<ui::MainMenu>(m_keys, m_main, *document);
	}

	std::unique_ptr<ui::TransportControls> AppUi::transportControls(Rml::Element& root, ui::Window& window, bool fill)
	{
		if (fill)
			root.SetInnerRML(readText(m_resources / "ui" / "transport.rml"));
		const ui::WindowId id = window.id();
		auto controls = std::make_unique<ui::TransportControls>(
			m_engine, m_binding, root,
			[this, id](const std::string& command, const engine::Json& parameters)
			{
				if (ui::Window* found = m_shell.find(id))
					m_keys.run(command, parameters, *found, nullptr);
			},
			[this](const std::string& command) { m_keys.release(command); },
			[this](const std::string& command, const engine::Json& parameters) { m_keys.check(command, parameters); });
		controls->followSelection(
			[this] { return m_eventDisplay ? m_eventDisplay->selectionStart() : std::optional<int64_t>(); });
		return controls;
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
		// The transport panel's #transport gets the controls, a docking window for its sections; the time display
		// window has its own, at the size it was given. Controls with a command that doesn't exist or parameters it
		// doesn't take (#446) close the window again, as a document that doesn't load does.
		try
		{
			shown.controls = transportControls(*root, window, root == controls);
		}
		catch (const std::invalid_argument& error)
		{
			m_shell.close(window.id());
			logging::error("Cannot show {}: {}", path.u8string(), error.what());
			return;
		}
		shown.window = window.id();
		if (root == controls)
			m_docking.attach("transportPanel", *document);
		else
			shown.controls->onSize(
				[this, id = window.id()](int size)
				{
					ui::Rect bounds = m_shell.bounds(id);
					const ui::Size sized = g_timeDisplaySizes.at(static_cast<size_t>(size));
					if (bounds.size() == sized)
						return;
					bounds.width = sized.width;
					bounds.height = sized.height;
					m_shell.setBounds(id, bounds);
				});
	}

	void AppUi::toggleTransportPanel()
	{
		toggle(m_transportPanel, "transport-panel.rml", "Transport", g_transportPanelSize);
	}

	void AppUi::toggleTimeDisplay()
	{
		toggle(m_timeDisplay, "time-display.rml", "Time Display", g_timeDisplaySizes.at(1));
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
		m_tempoLane.reset();
		m_signatureLane.reset();
		m_transposeLane.reset();
		m_chordLane.reset();
		m_rulerLanes.reset();
		// The transport bar's sections are there when the docking system reads them.
		m_transportBar.reset();
		if (Rml::Element* bar = document->GetElementById("transport-bar"))
			m_transportBar = transportControls(*bar, m_main, true);
		// Before the docking system fills the Visibility tab with it.
		m_trackVisibility = std::make_unique<ui::TrackVisibility>(m_binding, *document);
		m_docking.attach("project", *document);
		m_colorMenu.reset();
		m_trackList.reset();
		m_infoLine.reset();
		m_toolbarQuantize.reset();
		m_eventDisplay = std::make_unique<ui::EventDisplay>(m_engine, m_binding, *root);
		if (Rml::Element* quantize = document->GetElementById("toolbar-quantize"))
			m_toolbarQuantize = std::make_unique<ui::QuantizePanel>(m_binding, *quantize,
				ui::QuantizePanel::Layout::Toolbar,
				[this](const std::string& command) { m_keys.run(command, engine::Json::object(), m_main, nullptr); });
		m_statusLine = std::make_unique<ui::StatusLine>(m_binding, *document);
		if (Rml::Element* info = document->GetElementById("info-line"))
			m_infoLine = std::make_unique<ui::InfoLine>(*m_eventDisplay, m_binding, *info);
		if (Rml::Element* tracks = document->GetElementById("track-list"))
			m_trackList = std::make_unique<ui::TrackList>(*m_eventDisplay, m_binding, *tracks);
		if (m_trackList)
			m_colorMenu = std::make_unique<ui::ColorMenu>(*m_eventDisplay, *m_trackList, m_binding, *document);
		m_menuDocument = document->GetObserverPtr();
		m_menu = std::make_unique<ui::MainMenu>(m_keys, m_main, *document);
		applyKeySettings(); // Edit > Macros
		m_markerLines = std::make_unique<ui::MarkerLines>(*m_eventDisplay, m_binding);
		m_tempoLane = std::make_unique<ui::TempoLane>(*m_eventDisplay, m_binding);
		m_signatureLane = std::make_unique<ui::SignatureLane>(*m_eventDisplay, m_binding);
		m_transposeLane = std::make_unique<ui::GlobalEventLane>(*m_eventDisplay, m_binding);
		m_chordLane = std::make_unique<ui::GlobalEventLane>(*m_eventDisplay, m_binding, ui::LaneKind::Chord);
		m_rulerLanes = std::make_unique<ui::RulerLanes>(*m_eventDisplay);
		if (m_infoLine)
			m_infoLine->followMarkers(
				[this] { return m_markerLines != nullptr ? m_markerLines->selection() : std::vector<uint64_t>(); });
		m_markerLines->onSelect(
			[this](const std::vector<uint64_t>& selection)
			{
				if (ui::MarkersWindow* window = markers(); window != nullptr && window->syncsSelection())
					window->setSelection(selection);
			});
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
		// Sync Selection: the markers window's selection is the project window's.
		m_markers->onSelect(
			[this](const std::vector<uint64_t>& selection)
			{
				if (m_markerLines != nullptr && m_markers != nullptr && m_markers->syncsSelection())
					m_markerLines->setSelection(selection);
			});
		m_markers->syncFrom(
			[this] { return m_markerLines != nullptr ? m_markerLines->selection() : std::vector<uint64_t>(); });
		return m_markers.get();
	}

	ui::OnScreenKeyboard* AppUi::toggleKeyboard()
	{
		const bool open = m_keyboard != nullptr && m_shell.find(m_keyboardWindow) != nullptr;
		m_keyboard.reset();
		if (open)
		{
			m_shell.close(m_keyboardWindow);
			return nullptr;
		}
		ui::WindowSpec spec;
		spec.title = "On-Screen Keyboard";
		spec.size = {640, 200};
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		m_keyboardWindow = window.id();
		const std::filesystem::path file = m_resources / "ui" / "onscreen-keyboard.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
		{
			m_shell.close(m_keyboardWindow);
			throw std::runtime_error("cannot load the on-screen keyboard from " + file.u8string());
		}
		document->Show();
		m_keyboard = std::make_unique<ui::OnScreenKeyboard>(m_binding, *document, m_keyboardSettings);
		return m_keyboard.get();
	}

	ui::TempoEditor* AppUi::toggleTempoEditor()
	{
		const bool open = m_tempoEditor != nullptr && m_shell.find(m_tempoWindow) != nullptr;
		m_tempoEditor.reset();
		if (open)
		{
			m_shell.close(m_tempoWindow);
			return nullptr;
		}
		ui::WindowSpec spec;
		spec.title = "Tempo Track";
		spec.size = {800, 400};
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		m_tempoWindow = window.id();
		const std::filesystem::path file = m_resources / "ui" / "tempo-editor.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
		{
			m_shell.close(m_tempoWindow);
			throw std::runtime_error("cannot load the tempo track editor from " + file.u8string());
		}
		document->Show();
		m_tempoEditor = std::make_unique<ui::TempoEditor>(m_binding, *document);
		return m_tempoEditor.get();
	}

	void AppUi::toggleArranger()
	{
		if (m_shell.find(m_arrangerWindow) != nullptr)
		{
			m_shell.close(m_arrangerWindow);
			return;
		}
		ui::WindowSpec spec;
		spec.title = "Arranger";
		spec.size = {640, 360};
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		m_arrangerWindow = window.id();
		const std::filesystem::path file = m_resources / "ui" / "arranger-editor.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
		{
			m_shell.close(m_arrangerWindow);
			throw std::runtime_error("cannot load the arranger editor from " + file.u8string());
		}
		document->Show();
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
				// File > Import goes onto the selected track (#480), the pool's Import Medium into the pool only.
				if (clipsOnly)
					m_eventDisplay->importFiles(files, std::nullopt, std::nullopt, true);
				else
					m_eventDisplay->importToSelection(
						files, m_trackList ? m_trackList->selection() : std::vector<uint64_t>());
			});
	}

	void AppUi::addMixers()
	{
		// The mixer in the lower zone (reference manual pp. 434-436), with a view of its own.
		m_docking.add({"mixer", "Mixer", "project", "lower", [this](Rml::Element& element)
			{
				auto mixer = std::make_shared<ui::Mixer>(
					m_binding, m_docking, element, "lower", ui::MixerKind::LowerZone, mixerActions());
				m_lowerMixer = mixer;
				return mixer;
			}});
		m_keys.add({"window.lowerZoneMixer",
			"Shows the mixer in the project window's lower zone, or hides the zone while it shows the mixer.",
			[this](ui::Window&, Rml::Element*)
			{
				if (m_docking.showing("mixer") && m_docking.windowOf("mixer") == nullptr)
					m_docking.setVisible("project", "lower", false);
				else
					m_docking.open("mixer");
			}});
		m_keys.bind({"Alt+F3", "window.lowerZoneMixer"});
		// The mixer windows (pp. 437-462), each with the Visibility tab in its left zone.
		for (int number = 1; number <= static_cast<int>(m_mixers.size()); ++number)
		{
			const std::string name = "mixer" + std::to_string(number);
			m_docking.add({name + "Visibility", "Visibility", name, "left", [this, number](Rml::Element& element)
				{
					if (ui::Mixer* found = mixer(number))
						found->showVisibility(element);
					return std::shared_ptr<void>();
				}});
			m_keys.add({number == 1 ? std::string("window.mixer") : "window.mixer" + std::to_string(number),
				number == 1 ? std::string("Opens or closes the mixer window.")
							: "Opens or closes mixer window " + std::to_string(number) + ".",
				// Later in the step: the key may be the mixer window's own, which closing it would destroy.
				[this, number](ui::Window&, Rml::Element*) { m_shell.post([this, number] { toggleMixer(number); }); }});
		}
		m_keys.bind({"F3", "window.mixer"});
		// Edit Channel Settings (reference manual p. 500).
		m_keys.add({"window.channelSettings",
			"Opens the channel settings window for the first selected track's channel (Edit Channel Settings).",
			[this](ui::Window&, Rml::Element*)
			{
				if (m_trackList && !m_trackList->selection().empty())
					m_shell.post([this, track = m_trackList->selection().front()] { openChannelSettings(track); });
			}});
		m_keys.enableWhen("window.channelSettings",
			[this](ui::Window&, Rml::Element*) { return m_trackList != nullptr && !m_trackList->selection().empty(); });
	}

	ui::MixerActions AppUi::mixerActions()
	{
		ui::MixerActions actions;
		actions.choosePicture = [this](uint64_t track) { choosePicture(track); };
		// Later in the step: the click may be in the channel settings window, which opening it would rebuild.
		actions.editChannel = [this](uint64_t track) { m_shell.post([this, track] { openChannelSettings(track); }); };
		// Add Effect Track to Send (p. 554): the add track dialog's effect page, in the mixer's window, later in the
		// step, outside the drop-down list's event.
		actions.addEffectTrack = [this](Rml::Element& from, uint64_t track, int send)
		{
			for (const ui::Window* window : m_shell.windows())
				if (&window->context() == from.GetContext())
					m_shell.post(
						[this, id = window->id(), track, send]
						{
							if (ui::Window* found = m_shell.find(id))
								openDialog(*found, "add-track.rml",
									{{"type", "effect"}, {"tracks", std::to_string(track)},
										{"send", std::to_string(send)}});
						});
		};
		actions.wentTo = [this](uint64_t track)
		{
			m_settingsTrack = track;
			if (m_trackList)
				m_trackList->select({track});
		};
		return actions;
	}

	ui::Mixer* AppUi::openChannelSettings(uint64_t track)
	{
		if (channelSettings() == nullptr)
		{
			m_channelSettings.mixer.reset();
			ui::WindowSpec spec;
			spec.title = "Channel Settings";
			spec.size = {1000, 560};
			spec.hidden = m_hidden;
			const ui::Window& window = m_shell.open(spec);
			m_channelSettings.window = window.id();
			const std::filesystem::path file = m_resources / "ui" / "channel-settings.rml";
			Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
			Rml::Element* root = document != nullptr ? document->GetElementById("mixer") : nullptr;
			if (root == nullptr)
			{
				m_shell.close(m_channelSettings.window);
				m_channelSettings.window = 0;
				throw std::runtime_error("cannot load the channel settings window from " + file.u8string());
			}
			document->Show();
			m_channelSettings.mixer = std::make_unique<ui::Mixer>(
				m_binding, m_docking, *root, std::string(), ui::MixerKind::ChannelSettings, mixerActions());
			m_docking.attach("channelSettings", *document);
		}
		else if (!m_hidden)
		{
			m_shell.raise(m_channelSettings.window);
		}
		m_settingsTrack = track;
		m_channelSettings.mixer->showTrack(track);
		if (m_trackList)
			m_trackList->select({track});
		return m_channelSettings.mixer.get();
	}

	void AppUi::addEditors()
	{
		// The editor window's left zone: the Visibility tab and the inspector's Global Tracks section (pp. 1144-1149).
		m_docking.add({"editorVisibility", "Visibility", "editorWindow", "left", [this](Rml::Element& element)
			{ return std::make_shared<ui::MidiEditor>(m_binding, element, "window", ui::EditorPart::Visibility); }});
		m_docking.add({"editorInspector", "Inspector", "editorWindow", "left", [this](Rml::Element& element)
			{ return std::make_shared<ui::MidiEditor>(m_binding, element, "window", ui::EditorPart::GlobalTracks); }});
		m_editorSubscriptions.push_back(
			m_binding.on("editor.changed", [this](const engine::Event& event) { followEditor(event.data); }));
		m_editorSubscriptions.push_back(m_binding.on(
			"sampleEditor.changed", [this](const engine::Event& event) { followSampleEditor(event.data); }));
		// A new project closes the editors.
		const auto request = [this]
		{
			for (const char* editor : {"window", "lower"})
				m_binding.send("editor.get", {{"editor", editor}},
					[this](const engine::Result& result)
					{
						if (result.ok())
							followEditor(result.value);
					});
			m_binding.send("sampleEditor.get", engine::Json::object(),
				[this](const engine::Result& result)
				{
					if (result.ok())
						followSampleEditor(result.value);
				});
		};
		m_editorSubscriptions.push_back(m_binding.on("project.reset", [request](const engine::Event&) { request(); }));
		request();
		// Open/Close Editor (Mod+E and Return, p. 1189) opens the selected MIDI parts in the editor window, or else the
		// selected audio parts in the audio part editor (p. 692), or closes it without any; the other one does the
		// same in the lower zone. Without parts, the editor window's command opens the first selected audio event in
		// the sample editor (p. 613), or closes that too.
		for (const bool lower : {false, true})
		{
			const std::string editor = lower ? "lower" : "window";
			m_keys.add({lower ? "window.lowerZoneEditor" : "editor.openClose",
				lower
					? "Opens the selected MIDI parts or audio parts in the editor in the project window's lower zone, "
					  "or closes it."
					: "Opens the selected MIDI parts or audio parts in the editor window, or else the first selected "
					  "audio event in the sample editor, or closes them.",
				[this, editor](ui::Window&, Rml::Element*)
				{
					const std::vector<uint64_t> parts = selectedParts();
					if (!parts.empty())
					{
						m_binding.send("editor.open", {{"editor", editor}, {"parts", parts}});
						// Opening it explicitly brings the Editor tab forward, even from behind the Mixer.
						if (editor == "lower")
							m_docking.open("editor");
					}
					else if (const std::vector<uint64_t> events = selected("audioEvents");
						!events.empty() && editor == "window")
					{
						m_binding.send("sampleEditor.open", {{"event", events.front()}});
					}
					else
					{
						m_binding.send("editor.close", {{"editor", editor}});
						if (editor == "window")
							m_binding.send("sampleEditor.close");
					}
				}});
		}
		m_keys.bind({"Mod+E", "editor.openClose", engine::Json::object(), "project"});
		m_keys.bind({"Return", "editor.openClose", engine::Json::object(), "project"});
		// Open List Editor (Mod+G, reference manual p. 1243): the selected MIDI parts in the editor window's list
		// editor.
		m_keys.add({"editor.openList", "Opens the selected MIDI parts in the list editor window.",
			[this](ui::Window&, Rml::Element*)
			{
				const std::vector<uint64_t> parts = selected("midiParts");
				if (!parts.empty())
					m_binding.send("editor.open", {{"editor", "window"}, {"parts", parts}, {"content", "list"}});
			}});
		// Open Drum Editor (reference manual p. 1221): the selected MIDI parts in the editor window's drum editor.
		m_keys.add({"editor.openDrum", "Opens the selected MIDI parts in the drum editor window.",
			[this](ui::Window&, Rml::Element*)
			{
				const std::vector<uint64_t> parts = selected("midiParts");
				if (!parts.empty())
					m_binding.send("editor.open", {{"editor", "window"}, {"parts", parts}, {"content", "drum"}});
			}});
		// The Drum Map Setup's Load and Save (p. 1236): a drum map file into the project, and the drum map of the
		// editor window's active track into a file.
		m_keys.add({"drumMap.load", "Loads a drum map file into the project.", [this](ui::Window& window, Rml::Element*)
			{
				ui::FileDialog dialog;
				dialog.title = "Load Drum Map";
				dialog.filters = {{"Drum maps", "drm"}, {"All files", "*"}};
				dialog.parent = window.id();
				m_shell.showFileDialog(dialog,
					[this](const ui::FileDialogResult& result)
					{
						if (!result.error.empty())
							logging::warning("The file dialog failed: {}", result.error);
						if (!result.files.empty())
							m_binding.send("drumMap.import", {{"path", result.files.front().u8string()}});
					});
			}});
		m_keys.add({"drumMap.save", "Saves the drum map of the editor's active track as a drum map file.",
			[this](ui::Window& window, Rml::Element*) { saveDrumMap(window.id()); }});
		// Open Audio Part Editor (Audio menu, reference manual p. 692): the selected audio parts in the editor window.
		m_keys.add({"audio.openPartEditor", "Opens the selected audio parts in the audio part editor window.",
			[this](ui::Window&, Rml::Element*)
			{
				const std::vector<uint64_t> parts = selected("audioParts");
				if (!parts.empty())
					m_binding.send("editor.open", {{"editor", "window"}, {"parts", parts}});
			}});
		m_keys.bind({"Mod+G", "editor.openList", engine::Json::object(), "project"});
	}

	std::vector<uint64_t> AppUi::selected(const char* kind) const
	{
		std::vector<uint64_t> found;
		if (!m_eventDisplay || !m_eventDisplay->project().is_object())
			return found;
		const std::vector<uint64_t>& selection = m_eventDisplay->selection();
		for (const engine::Json& item : m_eventDisplay->project().value(kind, engine::Json::array()))
		{
			const auto id = item.at("id").get<uint64_t>();
			if (std::find(selection.begin(), selection.end(), id) != selection.end())
				found.push_back(id);
		}
		return found;
	}

	std::vector<uint64_t> AppUi::selectedParts() const
	{
		const std::vector<uint64_t> parts = selected("midiParts");
		return parts.empty() ? selected("audioParts") : parts;
	}

	void AppUi::followSampleEditor(const engine::Json& view)
	{
		const bool open = view.at("open").get<bool>();
		const bool shown = m_sampleEditor.window != 0 && m_shell.find(m_sampleEditor.window) != nullptr;
		if (open == shown)
			return;
		m_sampleEditor.editor.reset();
		if (shown)
		{
			m_shell.close(m_sampleEditor.window);
			m_sampleEditor.window = 0;
			return;
		}
		ui::WindowSpec spec;
		spec.title = "Sample Editor";
		spec.size = {1000, 600};
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		m_sampleEditor.window = window.id();
		const std::filesystem::path file = m_resources / "ui" / "sample-editor.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		Rml::Element* root = document != nullptr ? document->GetElementById("sample-editor") : nullptr;
		if (root == nullptr)
		{
			m_shell.close(m_sampleEditor.window);
			m_sampleEditor.window = 0;
			logging::error("Cannot load the sample editor window from {}", file.u8string());
			return;
		}
		document->Show();
		m_sampleEditor.editor = std::make_unique<ui::SampleEditor>(m_binding, *root);
	}

	void AppUi::followEditor(const engine::Json& view)
	{
		const std::string editor = view.at("editor").get<std::string>();
		const bool open = view.at("open").get<bool>();
		const bool wasOpen = m_editorViews.value(editor, engine::Json::object()).value("open", false);
		m_editorViews[editor] = view;
		if (editor == "lower")
		{
			// Only opening the view brings the Editor tab to the front: while it's open behind another tab, such as
			// the Mixer, it follows the selection there.
			if (open && !wasOpen && !m_docking.showing("editor"))
				m_docking.open("editor");
			return;
		}
		const bool shown = m_editor.window != 0 && m_shell.find(m_editor.window) != nullptr;
		if (open == shown)
			return;
		m_editor.editor.reset();
		if (shown)
		{
			m_shell.close(m_editor.window);
			m_editor.window = 0;
			return;
		}
		ui::WindowSpec spec;
		spec.title = "Editor";
		spec.size = {1000, 600};
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		m_editor.window = window.id();
		const std::filesystem::path file = m_resources / "ui" / "editor.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		Rml::Element* root = document != nullptr ? document->GetElementById("midi-editor") : nullptr;
		if (root == nullptr)
		{
			m_shell.close(m_editor.window);
			m_editor.window = 0;
			logging::error("Cannot load the editor window from {}", file.u8string());
			return;
		}
		document->Show();
		// Open in Lower Zone opens the Editor tab in the lower zone (reference manual, p. 1198).
		m_editor.editor = std::make_unique<ui::MidiEditor>(
			m_binding, *root, "window", ui::EditorPart::Editor, [this] { m_docking.open("editor"); });
		m_docking.attach("editorWindow", *document);
	}

	void AppUi::followSelection()
	{
		// The user closed the editor window or the lower zone's editor: Keep Editor Contents goes off with it (p.
		// 1153).
		if (m_editor.window != 0 && m_shell.find(m_editor.window) == nullptr)
		{
			m_editor.editor.reset();
			m_editor.window = 0;
			m_binding.send("editor.close", {{"editor", "window"}});
		}
		if (m_sampleEditor.window != 0 && m_shell.find(m_sampleEditor.window) == nullptr)
		{
			m_sampleEditor.editor.reset();
			m_sampleEditor.window = 0;
			m_binding.send("sampleEditor.close");
		}
		const auto open = [this](const char* editor)
		{ return m_editorViews.value(editor, engine::Json::object()).value("open", false); };
		if (open("lower") && m_docking.windowOf("editor") == nullptr && !m_docking.visible("project", "lower"))
		{
			m_editorViews["lower"]["open"] = false; // until editor.changed says so
			m_binding.send("editor.close", {{"editor", "lower"}});
		}
		if (!m_eventDisplay || m_eventDisplay->selection() == m_followed)
			return;
		// Editor Content Follows Event Selection (p. 1153): the MIDI parts, or else the audio parts, selected in the
		// project window.
		m_followed = m_eventDisplay->selection();
		const std::vector<uint64_t> parts = selectedParts();
		if (parts.empty())
			return;
		for (const char* editor : {"window", "lower"})
			if (open(editor) && !m_editorViews.at(editor).at("keepContents").get<bool>())
				m_binding.send("editor.open", {{"editor", editor}, {"parts", parts}});
	}

	ui::Mixer* AppUi::toggleMixer(int number)
	{
		MixerWindow& shown = m_mixers.at(static_cast<size_t>(number - 1));
		const bool open = shown.window != 0 && m_shell.find(shown.window) != nullptr;
		shown.mixer.reset();
		if (open)
		{
			m_shell.close(shown.window);
			shown.window = 0;
			return nullptr;
		}
		ui::WindowSpec spec;
		spec.title = number == 1 ? std::string("Mixer") : "Mixer " + std::to_string(number);
		spec.size = {1000, 560};
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		shown.window = window.id();
		const std::filesystem::path file = m_resources / "ui" / "mixer.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		Rml::Element* root = document != nullptr ? document->GetElementById("mixer") : nullptr;
		if (root == nullptr)
		{
			m_shell.close(shown.window);
			shown.window = 0;
			throw std::runtime_error("cannot load the mixer window from " + file.u8string());
		}
		document->Show();
		shown.mixer = std::make_unique<ui::Mixer>(
			m_binding, m_docking, *root, "window" + std::to_string(number), ui::MixerKind::Window, mixerActions());
		m_docking.attach("mixer" + std::to_string(number), *document);
		return shown.mixer.get();
	}

	ui::Mixer* AppUi::mixer(int number)
	{
		const MixerWindow& shown = m_mixers.at(static_cast<size_t>(number - 1));
		return shown.window != 0 && m_shell.find(shown.window) != nullptr ? shown.mixer.get() : nullptr;
	}

	void AppUi::choosePicture(uint64_t track)
	{
		ui::FileDialog dialog;
		dialog.title = "Choose Track Picture";
		dialog.filters = {{"Images", "png;jpg;jpeg;svg"}, {"All files", "*"}};
		dialog.parent = m_main.id();
		m_shell.showFileDialog(dialog,
			[this, track](const ui::FileDialogResult& result)
			{
				if (!result.error.empty())
					logging::warning("The file dialog failed: {}", result.error);
				if (!result.files.empty())
					m_binding.send("track.set", {{"track", track}, {"picture", result.files.front().u8string()}});
			});
	}

	void AppUi::saveDrumMap(ui::WindowId parent, const std::string& editor)
	{
		// The editor window's active track, else the lower zone's.
		m_binding.send("editor.get", {{"editor", editor}},
			[this, parent, editor](const engine::Result& view)
			{
				if (!view.ok())
					return;
				const engine::Json track = view.value.at("activeTrack");
				if (track == 0)
				{
					if (editor == "window")
						saveDrumMap(parent, "lower");
					return;
				}
				m_binding.send("project.get", engine::Json::object(),
					[this, parent, track](const engine::Result& project)
					{
						if (!project.ok())
							return;
						for (const engine::Json& each : project.value.at("tracks"))
							if (each.at("id") == track && each.value("drumMap", uint64_t{0}) != 0)
								chooseDrumMapFile(parent, each.at("drumMap").get<uint64_t>());
					});
			});
	}

	void AppUi::chooseDrumMapFile(ui::WindowId parent, uint64_t map)
	{
		ui::FileDialog dialog;
		dialog.kind = ui::FileDialogKind::Save;
		dialog.title = "Save Drum Map";
		dialog.filters = {{"Drum maps", "drm"}};
		dialog.parent = parent;
		m_shell.showFileDialog(dialog,
			[this, map](const ui::FileDialogResult& result)
			{
				if (!result.error.empty())
					logging::warning("The file dialog failed: {}", result.error);
				if (result.files.empty())
					return;
				std::filesystem::path file = result.files.front();
				if (file.extension() != ".drm")
					file += ".drm";
				m_binding.send("drumMap.export", {{"map", map}, {"path", file.u8string()}});
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

	Rml::ElementDocument& AppUi::openPreferences(ui::Window& window)
	{
		return openOnce(window, "preferences.rml", m_preferences);
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

	void AppUi::openAutoFades(ui::Window& window, uint64_t track) const
	{
		// Its Lua reads the track when it shows.
		const std::filesystem::path file = m_resources / "ui" / "auto-fades.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
			throw std::runtime_error("cannot load the dialog " + file.u8string());
		if (track != 0)
			document->SetAttribute("track", std::to_string(track));
		document->Show(Rml::ModalFlag::Modal);
	}

	Rml::ElementDocument& AppUi::openDialog(
		ui::Window& window, const char* name, const std::vector<std::pair<std::string, std::string>>& attributes) const
	{
		const std::filesystem::path file = m_resources / "ui" / name;
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
			throw std::runtime_error("cannot load the dialog " + file.u8string());
		for (const auto& [attribute, value] : attributes)
			document->SetAttribute(attribute, value);
		document->Show(Rml::ModalFlag::Modal);
		return *document;
	}
}
