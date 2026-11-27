// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/appui.h"

#include "app/uitools.h"
#include "platform/files.h"
#include "platform/log.h"
#include "ui/chordpads.h"
#include "ui/displays.h"
#include "ui/fonts.h"
#include "ui/historypanel.h"
#include "ui/inspector.h"
#include "ui/keyeditor.h"
#include "ui/meterpanel.h"
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
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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

		// The tracks that track.import or track.export left out (skipped) for the message that tells, or empty.
		std::string leftOut(const engine::Json& result)
		{
			const engine::Json& skipped = result.at("skipped");
			if (skipped.empty())
				return {};
			std::string names;
			for (const engine::Json& track : skipped)
				names += (names.empty() ? "" : ", ") + track.at("name").get<std::string>();
			return std::string(skipped.size() == 1 ? "the track " : "the tracks ") + names +
				", which can't be imported or exported yet";
		}

		// The key command assignments as an HTML table (reference manual p. 1560): every command with the keys that run
		// it, by category. File > Export > Key Command Assignments writes it.
		std::string keyAssignmentsHtml(const engine::Json& assignments)
		{
			const auto escape = [](const std::string& text)
			{
				std::string escaped;
				for (const char character : text)
				{
					if (character == '&')
						escaped += "&amp;";
					else if (character == '<')
						escaped += "&lt;";
					else if (character == '>')
						escaped += "&gt;";
					else if (character == '"')
						escaped += "&quot;";
					else
						escaped += character;
				}
				return escaped;
			};
			const auto parameters = [](const engine::Json& values)
			{
				if (!values.is_object() || values.empty())
					return std::string();
				std::vector<std::string> names;
				for (const auto& item : values.items())
					names.push_back(item.key());
				std::sort(names.begin(), names.end());
				std::string text = " (";
				bool first = true;
				for (const std::string& name : names)
				{
					if (!first)
						text += ", ";
					first = false;
					const engine::Json& value = values.at(name);
					text += name + ": " + (value.is_string() ? value.get<std::string>() : value.dump());
				}
				return text + ")";
			};
			std::ostringstream html;
			html << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
					"<title>Key Command Assignments</title>\n<style>\n"
					"body { font-family: sans-serif; margin: 2em; }\n"
					"table { border-collapse: collapse; }\n"
					"th, td { border: 1px solid #808080; padding: 0.3em 0.8em; text-align: left; vertical-align: top; "
					"}\n"
					"th { background: #eeeeee; }\n</style>\n</head>\n<body>\n<h1>Key Command Assignments</h1>\n"
					"<table>\n<thead><tr><th>Category</th><th>Command</th><th>Key</th></tr></thead>\n<tbody>\n";
			for (const engine::Json& entry : assignments)
			{
				const std::string command = entry.at("command").get<std::string>();
				std::string keys;
				for (const engine::Json& key : entry.at("keys"))
					keys += (keys.empty() ? "" : ", ") + escape(key.get<std::string>());
				html << "<tr><td>" << escape(command.substr(0, command.find('.'))) << "</td><td>"
					 << escape(command + parameters(entry.at("parameters"))) << "</td><td>" << keys << "</td></tr>\n";
			}
			html << "</tbody>\n</table>\n</body>\n</html>\n";
			return html.str();
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

		constexpr std::array<ProjectCommand, 104> g_projectCommands{{
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
			{"edit.delete", "Deletes the selection range, or without one the selected events.", "Delete"},
			{"edit.mute", "Mutes the selected events.", "Shift+M"},
			{"edit.unmute", "Unmutes the selected events.", "Shift+U"},
			{"edit.lock", "Locks the selected events.", "Mod+Shift+L"},
			{"edit.unlock", "Unlocks the selected events.", "Mod+Shift+U"},
			{"edit.splitAtCursor", "Splits the selected events, or all there, at the project cursor.", "Alt+X"},
			{"edit.splitLoop", "Splits the selected events, or all there, at the locators.", nullptr},
			{"edit.glue", "Glues the selected events together.", nullptr},
			{"edit.duplicate", "Duplicates the selected events, or copies the selection range's contents to its end.",
				"Mod+D"},
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
			{"audio.crossfadeEditor", "Opens the crossfade editor for the crossfade of the selected audio events.",
				nullptr},
			{"audio.removeFadeIn", "Removes the fade in of the selected audio events.", nullptr},
			{"audio.removeFadeOut", "Removes the fade out of the selected audio events.", nullptr},
			{"audio.removeFades", "Removes the fades and crossfades of the selected audio events.", nullptr},
			{"audio.fadeEditor", "Opens the fade dialog for the selected audio events.", nullptr},
			{"audio.applyStandardFadeIn", "Gives the selected audio events the default fade in.", nullptr},
			{"audio.applyStandardFadeOut", "Gives the selected audio events the default fade out.", nullptr},
			// The audio functions (docs/features/audio-functions.md).
			{"audio.closeGapsCrossfade", "Closes gaps between selected audio events with crossfades.", nullptr},
			{"audio.closeGapsTimeStretch", "Closes gaps between selected audio events with time stretch.", nullptr},
			{"audio.deleteOverlaps", "Deletes overlaps between selected audio events.", nullptr},
			{"audio.detectSilence", "Opens the Detect Silence dialog for the selected audio events.", nullptr},
			{"audio.spectrumAnalyzer", "Opens the Spectrum Analyzer for selected audio events or pool clips.", nullptr},
			{"audio.statistics", "Opens Statistics for selected audio events or pool clips.", nullptr},
			// Sampler tracks (docs/features/sampler-track.md).
			{"audio.createSamplerTrack", "Creates a sampler track that plays the first selected audio event.", nullptr},
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
			{"edit.nudgeLeft", "Moves the selected events, or the selection range without its contents, left.",
				"Mod+Left"},
			{"edit.nudgeRight", "Moves the selected events, or the selection range without its contents, right.",
				"Mod+Right"},
			{"edit.nudgeStartLeft", "Moves the start of the selected events or the selection range left.", "Alt+Left"},
			{"edit.nudgeStartRight", "Moves the start of the selected events or the selection range right.",
				"Alt+Shift+Right"},
			{"edit.nudgeEndLeft", "Moves the end of the selected events or the selection range left.",
				"Alt+Shift+Left"},
			{"edit.nudgeEndRight", "Moves the end of the selected events or the selection range right.", "Alt+Right"},
			// The selection range's top and bottom, with the reference's keys (manual p. 289).
			{"edit.nudgeTopUp", "Moves the top of the selection range up a track.", "Alt+Up"},
			{"edit.nudgeTopDown", "Moves the top of the selection range down a track.", "Alt+Shift+Down"},
			{"edit.nudgeBottomUp", "Moves the bottom of the selection range up a track.", "Alt+Shift+Up"},
			{"edit.nudgeBottomDown", "Moves the bottom of the selection range down a track.", "Alt+Down"},
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
			{"edit.cut", "Cuts the selection range, or else the selected events, to the clipboard.", "Mod+X"},
			{"edit.copy", "Copies the selection range, or else the selected events, to the clipboard.", "Mod+C"},
			{"edit.paste", "Pastes the clipboard at the selection range's start or the project cursor.", "Mod+V"},
			{"edit.cutTime", "Cuts the selection range and closes the gap.", "Mod+Shift+X"},
			{"edit.deleteTime", "Deletes the selection range and closes the gap.", "Shift+Backspace"},
			{"edit.pasteTime", "Pastes the clipboard and moves what comes after it to make room.", "Mod+Shift+V"},
			{"edit.pasteAtOrigin", "Pastes the clipboard where it was copied from.", "Alt+V"},
			{"edit.pasteRelative", "Pastes the clipboard as far from the project cursor as it was when copied.",
				"Shift+V"},
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
			{"tool.color", "Chooses the color tool in the project window.", nullptr},
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

		// A new transport panel's size until it takes the width of its shown sections (ui::TransportPanelFit), and the
		// one it keeps without a display or without a shown section.
		constexpr ui::Size g_transportPanelSize{1100, 48};
		// The time display window at each of its sizes (ui.timeDisplay), in dp: a line twice the height of the font
		// size of the theme's timeDisplaySizes for the digits, with room around it (#566).
		constexpr std::array<ui::Size, 5> g_timeDisplaySizes{
			{{320, 80}, {480, 120}, {640, 160}, {960, 240}, {1280, 320}}};

		// A time display window's size at one of its sizes and a dp ratio, in the window system's coordinates. They
		// aren't pixels on every system, so the pixels of a window's size give how much a coordinate is.
		ui::Size timeDisplaySize(const ui::Shell& shell, const ui::Window& window, int size, float ratio)
		{
			const ui::Size dp = g_timeDisplaySizes.at(static_cast<size_t>(std::clamp(size, 0, 4)));
			const ui::Rect bounds = shell.bounds(window.id());
			const ui::Size pixels = window.pixelSize();
			const float pixelsPerCoordinate = bounds.width > 0 && pixels.width > 0
				? static_cast<float>(pixels.width) / static_cast<float>(bounds.width)
				: 1.0F;
			const auto coordinates = [&](int length)
			{ return static_cast<int>(std::lround(static_cast<float>(length) * ratio / pixelsPerCoordinate)); };
			return {coordinates(dp.width), coordinates(dp.height)};
		}

		std::string readText(const std::filesystem::path& file)
		{
			const std::ifstream in(file, std::ios::binary);
			std::ostringstream text;
			text << in.rdbuf();
			return text.str();
		}

		// The track list's commands (ui::TrackList::actions, docs/features/track-list.md) and their keys.
		constexpr std::array<ProjectCommand, 34> g_trackCommands{{
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
				"Freezes the selected audio and instrument tracks with the Tail Size of render.freezeTail and the "
				"Include Inserts for Instrument Tracks of render.freezeInserts, or unfreezes them if every one is "
				"frozen.",
				nullptr},
			{"track.freezeSelectedSettings",
				"Opens Freeze Channel Options for the selected audio and instrument tracks that aren't frozen, and "
				"freezes them with its Tail Size and Include Inserts for Instrument Tracks (render.freezeTail, "
				"render.freezeInserts); unfreezes them if every one is frozen.",
				nullptr},
			{"track.renderSelected",
				"Renders in place with the options of the user setting render.options: the selected audio events, "
				"audio parts and MIDI parts, else the selection range, else the selected tracks, on audio and "
				"instrument tracks.",
				nullptr},
			{"track.renderSettings",
				"Opens Render Selection for the selected events and parts or the selection range, else Render Tracks "
				"for the selected tracks, and renders them in place with its options, which it saves in "
				"render.options.",
				nullptr},
			// The chord track (docs/features/chord-track.md).
			{"track.chordsToMidiSelected",
				"Chords to MIDI: a MIDI part of the chord events on the first selected MIDI or instrument track, which "
				"then follows the chord track.",
				nullptr},
			{"track.createChordEventsSelected",
				"Create Chord Events: chord events from the selected audio events and from the MIDI notes of the "
				"selected MIDI parts, with the Create Chord Events dialog's options; with none selected, from those "
				"of the selected tracks. Audio events in audio parts don't count.",
				nullptr},
			{"track.mapToChordTrackSelected",
				"Map to Chord Track: moves the notes of the selected MIDI parts, or with none selected those of the "
				"selected tracks, to the chord track's chords, by the Map to Chord Track dialog's mapping mode.",
				nullptr},
			{"track.assignVoicesSelected",
				"Assign Voices to Notes: moves the notes of the selected MIDI parts, or with none selected those of "
				"the selected tracks, to the voices of the chord track's voicings.",
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
		}};

		// The track version commands (docs/features/track-versions.md), their own key command category as in the
		// reference (manual p. 231).
		constexpr std::array<ProjectCommand, 6> g_trackVersionCommands{{
			{"trackVersions.new", "Gives the selected tracks a new, empty version.", nullptr},
			{"trackVersions.duplicate", "Gives the selected tracks a copy of their active version.", nullptr},
			{"trackVersions.rename", "Names the active version of the selected tracks.", nullptr},
			{"trackVersions.remove", "Deletes the active version of the selected tracks.", nullptr},
			{"trackVersions.selectSameId",
				"Selects the tracks with a version of the ID of the first selected track's active version.", nullptr},
			{"trackVersions.assignCommonId", "Gives the active versions of the selected tracks a common ID.", nullptr},
		}};
	}

	AppUi::AppUi(engine::Engine& engine, const std::filesystem::path& resources, const ui::WindowSpec& main,
		std::ostream& output, int msaaSamples)
		: m_scriptOutput(output), m_renderer(m_windows, msaaSamples),
		  m_lua(engine, scripting::Trust::Trusted, m_luaOutput), m_rmlui(m_windows),
		  m_themes(fontsAndThemes(resources)), m_shell(m_windows, m_renderer), m_settings(engine, m_shell, m_themes),
		  m_binding(
			  engine, [this] { m_shell.wake(); }, [this] { m_shell.requestRender(); }),
		  m_keys(engine, m_binding), m_meters(engine, m_binding), m_main(m_shell.open(main)),
		  m_docking(engine, m_binding, m_shell, resources / "ui" / "panel-window.rml", main.hidden), m_engine(engine),
		  m_resources(resources), m_hidden(main.hidden),
		  m_history(m_binding.on("history.changed",
			  [this](const engine::Event& event)
			  {
				  const auto position = event.data.at("position").get<size_t>();
				  m_canUndo = position > 0;
				  m_canRedo = position < event.data.at("steps").get<size_t>();
			  })),
		  m_projectReset(m_binding.on("project.reset",
			  [this](const engine::Event&)
			  {
				  for (const Rml::ObserverPtr<Rml::Element>& open : m_projectDialogs)
					  if (auto* document = dynamic_cast<Rml::ElementDocument*>(open.get());
						  document != nullptr && document->GetParentNode() != nullptr)
						  document->Close();
				  m_projectDialogs.clear();
				  ++m_resets;
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
				for (auto it = m_audioFunctions.begin(); it != m_audioFunctions.end();)
				{
					if (!it->second.document || it->second.document->GetParentNode() == nullptr ||
						m_shell.find(it->second.window) == nullptr)
					{
						m_shell.close(it->second.window);
						it = m_audioFunctions.erase(it);
					}
					else
					{
						++it;
					}
				}
				updateMenus();
				if (const std::shared_ptr<ui::PoolPanel> pool = m_pool.lock())
					pool->update();
				// The cutting positions show while the quantize panel is open.
				if (m_eventDisplay && m_quantizePanel.expired())
					m_eventDisplay->setCuttingPositions({});
				updateMixerWindows();
				if (m_keyboard && m_shell.find(m_keyboardWindow) == nullptr)
					m_keyboard.reset(); // so too: its notes end
				if (m_markers && m_shell.find(m_markersWindow) == nullptr)
					m_markers.reset(); // its project subscriptions and playback watch end with its window
				// The project window selects events, markers, arranger or time signature events: an event selected
				// deselects the others.
				if (m_eventDisplay && m_markerLines && m_eventDisplay->selection() != m_eventSelection)
				{
					m_eventSelection = m_eventDisplay->selection();
					if (!m_eventSelection.empty())
					{
						m_markerLines->deselect();
						if (m_arrangerLane)
							m_arrangerLane->deselect();
						if (m_signatureLane)
							m_signatureLane->row().setSelection({});
					}
				}
				// So do automation points (#2011). Whatever selects markers or time signature events deselects the
				// points (onSelect, Sync Selection), so points selected beside them were selected later.
				if (m_eventDisplay && m_markerLines && !m_eventDisplay->selectedPoints().empty())
				{
					m_markerLines->deselect();
					if (m_arrangerLane)
						m_arrangerLane->deselect();
					if (m_signatureLane && !m_signatureLane->row().selection().empty())
						m_signatureLane->row().setSelection({});
				}
				// So does a range selected, e.g. between markers with the range tool: Delete deletes what was selected
				// last.
				if (m_eventDisplay && m_markerLines)
				{
					const std::optional<ui::RangeSelection>& range = m_eventDisplay->range();
					const bool same = range.has_value() == m_eventRange.has_value() &&
						(!range || !m_eventRange ||
							(range->start == m_eventRange->start && range->end == m_eventRange->end &&
								range->tracks == m_eventRange->tracks));
					if (!same)
					{
						m_eventRange = range;
						if (range)
						{
							m_markerLines->deselect();
							if (m_arrangerLane)
								m_arrangerLane->deselect();
							if (m_signatureLane)
								m_signatureLane->row().setSelection({});
						}
					}
				}
				if (m_infoLine)
					m_infoLine->update();
				if (m_inPlace)
					m_inPlace->update();
				followSelection();
				// The inspector and the sampler panel show the first selected track.
				if (m_eventDisplay)
				{
					const std::vector<uint64_t> tracks =
						m_trackList ? m_trackList->selection() : std::vector<uint64_t>();
					const uint64_t first = tracks.empty() ? 0 : tracks.front();
					if (const std::shared_ptr<ui::Inspector> inspector = m_inspector.lock())
						inspector->update(
							tracks, m_eventDisplay->project(), m_eventDisplay->loads(), m_eventDisplay->read());
					if (const std::shared_ptr<ui::SamplerPanel> sampler = m_sampler.lock())
						sampler->update(
							first, m_eventDisplay->project(), m_eventDisplay->loads(), m_eventDisplay->read());
					if (const std::shared_ptr<ui::DrumMachinePanel> drums = m_drums.lock())
						drums->update(
							first, m_eventDisplay->project(), m_eventDisplay->loads(), m_eventDisplay->read());
				}
				return follows;
			});
		m_shell.onUnusedKey(
			[this](ui::Window& window, const ui::KeyPress& keys)
			{
				if (ui::OnScreenKeyboard* keyboard = this->keyboard();
					keyboard == nullptr || !keyboard->press(keys, m_keys.bound(window, keys)))
					m_keys.press(window, keys);
			});
		m_shell.onKeyReleased(
			[this](ui::Window&, const ui::KeyPress& keys)
			{
				if (ui::OnScreenKeyboard* keyboard = this->keyboard(); keyboard != nullptr)
					keyboard->release(keys);
				m_keys.released(keys);
				if (keys.focusLost)
				{
					// Also a button held, which the mouse button going up elsewhere doesn't end (#566).
					m_winding.stopAll();
					for (ui::TransportControls* controls :
						{m_transportBar.get(), m_transportPanel.controls.get(), m_timeDisplay.controls.get()})
						if (controls != nullptr)
							controls->releaseHeld();
				}
			});
		// What a script's click or key changed shows in the menu before the script reads the DOM, as after a step.
		m_shell.onInputHandled([this] { updateMenus(); });
		// Show Menu Bar in a window's title bar context menu (Windows, reference manual p. 55).
		m_shell.onMenuBarToggled(
			[this](ui::Window& window) { m_keys.run("window.menuBar", engine::Json::object(), window, nullptr); });
		// The on-screen keyboard's keys show what the new keyboard layout has at their places.
		m_shell.onKeymapChanged(
			[this]
			{
				if (ui::OnScreenKeyboard* keyboard = this->keyboard(); keyboard != nullptr)
					keyboard->relabel();
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
		// Insert Time Signature Event (p. 1469): a key command without a key, whose dialog starts at the signature at
		// the cursor and adds the signature typed at the bar the cursor is in (signature.add).
		m_keys.add({"signature.insertAtCursor",
			"Asks for a time signature and adds a time signature event with it at the bar the project cursor is in.",
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
								const double seconds = static_cast<double>(std::max<int64_t>(
														   transport.value.at("position").get<int64_t>(), 0)) /
									context.sampleRate;
								const time::TimeSignature signature =
									context.signatures.signatureAt(context.tempo.toTicks(seconds));
								openDialog(*found, "insert-signature.rml",
									{{"numerator", std::to_string(signature.numerator)},
										{"denominator", std::to_string(signature.denominator)}});
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
					auto inspector = std::make_shared<ui::Inspector>(
						m_binding, element, [this](uint64_t track)
						{ m_shell.post([this, track] { openChannelSettings(track); }); }, [this](uint64_t track)
						{ m_shell.post([this, track] { openArticulationMaps(m_main, track); }); },
						[this](uint64_t track, bool project)
						{
							m_shell.post(
								[this, track, project]
								{
									if (project)
										openDialog(m_main, "rule-editor.rml", {{"target", "project"}});
									else
										openDialog(m_main, "rule-editor.rml",
											{{"target", "track"}, {"track", std::to_string(track)}});
								});
						},
						[this] { m_shell.post([this] { openArranger(); }); });
					m_inspector = inspector;
					// In a window of its own, the Choose Track Color pane needs a menu in its document (#2026).
					Rml::ElementDocument* document = element.GetOwnerDocument();
					if (document == nullptr || !document->HasAttribute("panel"))
						return std::shared_ptr<void>(inspector);
					auto colors = std::make_shared<ui::ColorMenu>([this] { return m_eventDisplay.get(); },
						[this] { return m_trackList.get(); }, m_binding, *document);
					return std::shared_ptr<void>(
						std::make_shared<std::pair<std::shared_ptr<ui::Inspector>, std::shared_ptr<ui::ColorMenu>>>(
							std::move(inspector), std::move(colors)));
				};
			// The lower zone's editor (reference manual pp. 1132-1156) and, on the Visibility tab's Editor page, its
			// visibility; the Track page has the track visibility (#166).
			else if (added.id == "editor" || added.id == "visibility")
				added.create = [this, editor = added.id == "editor"](Rml::Element& element)
				{
					Rml::Element& page = editor || !m_trackVisibility ? element : m_trackVisibility->showPanel(element);
					auto shown = std::make_shared<ui::MidiEditor>(m_binding, m_editorReads, page, "lower",
						editor ? ui::EditorPart::Editor : ui::EditorPart::Visibility);
					shown->setDrumMapSetup([this] { m_shell.post([this] { openDrumMaps(m_main); }); });
					shown->setPlaybackPosition([this] { return m_engine.playbackPosition(); });
					if (editor)
						m_lowerEditor = shown;
					return std::shared_ptr<void>(shown);
				};
			// The meter display (reference manual pp. 533-538, docs/features/metering.md).
			else if (added.id == "meter")
				added.create = [this](Rml::Element& element)
				{ return std::make_shared<ui::MeterPanel>(m_meters, m_binding, element); };
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
		m_docking.add({"chordPads", "Chord Pads", "project", "lower", [this](Rml::Element& element)
			{
				return std::make_shared<ui::ChordPadsPanel>(
					m_binding, element,
					[this](float x, float y)
					{
						return m_eventDisplay ? m_eventDisplay->trackAt(x, y)
											  : std::optional<std::pair<uint64_t, int64_t>>();
					},
					[this] { return m_eventDisplay ? m_eventDisplay->selection() : std::vector<uint64_t>(); },
					[this](std::function<void(const std::string&)> import)
					{
						// Import from MIDI Loop (p. 1379): the loop chosen in the window the pads show in.
						const ui::Window* window = m_docking.windowOf("chordPads");
						ui::FileDialog dialog;
						dialog.title = "Import from MIDI Loop";
						dialog.filters = {{"MIDI files", "mid;midi;smf"}, {"All files", "*"}};
						dialog.parent = (window != nullptr ? *window : m_main).id();
						m_shell.showFileDialog(dialog,
							[import = std::move(import)](const ui::FileDialogResult& result)
							{
								if (!result.error.empty())
									logging::warning("The file dialog failed: {}", result.error);
								if (!result.files.empty())
									import(result.files.front().u8string());
							});
					});
			}});
		// The sampler panel (reference manual pp. 710-730, docs/features/sampler-track.md) in the lower zone, for the
		// selected sampler track. Import Audio File asks for the file in the window it shows in.
		m_docking.add({"sampler", "Sampler", "project", "lower", [this](Rml::Element& element)
			{
				auto sampler = std::make_shared<ui::SamplerPanel>(m_binding, element,
					[this](uint64_t track, bool keepSettings)
					{
						const ui::Window* window = m_docking.windowOf("sampler");
						ui::FileDialog dialog;
						dialog.title = "Import Audio File";
						dialog.filters = {
							{"Audio files", "wav;w64;rf64;bwf;aif;aiff;aifc;flac;ogg;mp3"}, {"All files", "*"}};
						dialog.parent = (window != nullptr ? *window : m_main).id();
						chooseProjectFile(dialog,
							[this, track, keepSettings](const std::filesystem::path& file)
							{
								// The panel shows why the file didn't load; without it, the file loads all the same.
								const std::string path = file.u8string();
								if (const std::shared_ptr<ui::SamplerPanel> panel = m_sampler.lock())
									panel->load(track, path, keepSettings);
								else
									m_binding.send("sampler.load",
										{{"track", track}, {"path", path}, {"keepSettings", keepSettings}});
							});
					});
				m_sampler = sampler;
				return sampler;
			}});
		// The drum machine (reference manual pp. 938-977, docs/features/drum-track.md) in the lower zone, for the
		// selected drum track. Load asks for the file in the window it shows in, Add Drum Track opens the add track
		// dialog there.
		m_docking.add({"drumMachine", "Drum Machine", "project", "lower", [this](Rml::Element& element)
			{
				auto drums = std::make_shared<ui::DrumMachinePanel>(
					m_binding, element,
					[this](uint64_t track, int pad)
					{
						const ui::Window* window = m_docking.windowOf("drumMachine");
						ui::FileDialog dialog;
						dialog.title = "Load Sample";
						dialog.filters = {
							{"Audio files", "wav;w64;rf64;bwf;aif;aiff;aifc;flac;ogg;mp3"}, {"All files", "*"}};
						dialog.parent = (window != nullptr ? *window : m_main).id();
						chooseProjectFile(dialog,
							[this, track, pad](const std::filesystem::path& file)
							{
								const std::string path = file.u8string();
								if (const std::shared_ptr<ui::DrumMachinePanel> panel = m_drums.lock())
									panel->load(track, pad, path);
								else
									m_binding.send("drum.loadPad", {{"track", track}, {"pad", pad}, {"path", path}});
							});
					},
					[this]
					{
						ui::Window* window = m_docking.windowOf("drumMachine");
						m_keys.run("project.addDrumTrack", engine::Json::object(), window != nullptr ? *window : m_main,
							nullptr);
					},
					[this](uint64_t track, const std::string& mode)
					{
						// The track preset dialog for the drum track shown, as the track list's Load and Save Track
						// Preset open it for the selected tracks.
						ui::Window* window = m_docking.windowOf("drumMachine");
						openDialog(window != nullptr ? *window : m_main, "track-preset.rml",
							{{"mode", mode}, {"tracks", std::to_string(track)}});
					});
				m_drums = drums;
				return drums;
			}});
		m_docking.addKeyCommands(m_keys);
		// What the pool's commands did, sent from its toolbar, the Media menu or Find Missing Files, shows in the
		// pool, which opens for it: its window comes to the front if it is in one of its own.
		for (const char* command :
			{"pool.removeUnused", "media.locate", "media.removeMissing", "media.conform", "media.prepareArchive"})
			m_poolResults.push_back(m_binding.onResult(
				command, [this, command](const engine::Result& result) { reportInPool(command, result); }));

		m_keys.add({"window.chordPads", "Shows or hides the chord pads in the lower zone.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{
				if (m_docking.showing("chordPads") && m_docking.windowOf("chordPads") == nullptr)
					m_docking.setVisible("project", "lower", false);
				else
					m_docking.open("chordPads");
			}});
		m_keys.add({"window.sampler", "Shows the sampler panel, which edits the selected sampler track.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_docking.open("sampler"); }});
		m_keys.add({"window.drumMachine", "Shows the drum machine, which edits the selected drum track.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_docking.open("drumMachine"); }});
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
		m_keys.add({"pool.prepareArchive",
			"Copies the media from outside the project folder into it, after asking in the pool, which opens for it.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{
				m_docking.open("pool");
				if (const std::shared_ptr<ui::PoolPanel> pool = m_pool.lock())
					pool->prepareArchive();
			}});
		m_keys.add({"pool.cleanup",
			"Removes the files of the media folders that nothing uses from the disk, after asking in the pool, which "
			"opens for it.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{
				m_docking.open("pool");
				if (const std::shared_ptr<ui::PoolPanel> pool = m_pool.lock())
					pool->cleanup();
			}});
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
		// For the preferences dialog: the themes there are, each with the name the UI shows for it, and the tool
		// modifiers that ui.toolModifiers overrides.
		m_lua.addFunction("ui.themes",
			[this](const engine::Json&)
			{
				engine::Json themes = engine::Json::array();
				for (const ui::Theme& theme : m_themes.themes())
					themes.push_back({{"id", theme.id}, {"name", theme.name}});
				return engine::Json{{"themes", std::move(themes)}};
			});
		m_lua.addFunction("ui.toolModifierDefaults",
			[](const engine::Json&) { return engine::Json{{"modifiers", ui::EventDisplay::defaultToolModifiers()}}; });
		// For the add track dialog's Time Offset: a position typed in a time format, as position displays show it in
		// the project (ui/timeformat.h), in seconds and ticks; nothing for text that isn't one. The project window's
		// event display has the project's time, so this doesn't wait for the engine (#814).
		m_lua.addFunction("ui.parseTime",
			[this](const engine::Json& parameters)
			{
				const std::optional<ui::TimeFormat> format =
					ui::timeFormatFromName(parameters.value("format", std::string()));
				if (!format || !m_eventDisplay || m_eventDisplay->project().is_null())
					return engine::Json::object();
				const ui::TimeContext& context = m_eventDisplay->time();
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
		// File > Import > MIDI File asks first whether to create a new project (reference manual p. 365, #478).
		m_keys.add({"file.importMidi",
			"Imports a standard MIDI file, into a new project or the open one, as the user chooses, with the options "
			"of the user settings midi.import*.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openDialog(window, "import-midi.rml"); }});
		m_lua.addFunction("ui.importMidi",
			[this](const engine::Json& parameters)
			{
				chooseMidiImport(callingWindow(), parameters.value("newProject", false));
				return engine::Json::object();
			});
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
					[this, id = window.id()](const ui::FileDialogResult& chosen)
					{
						if (!chosen.error.empty())
							logging::warning("The file dialog failed: {}", chosen.error);
						if (chosen.files.empty())
							return;
						std::filesystem::path file = chosen.files.front();
						if (file.extension().empty())
							file += ".wav";
						// A job: m_files shows its progress and why it failed, as for mixdown.export, and
						// quickExportEnded opens the file's folder once it is done.
						m_binding.send("mixdown.quickExport", {{"path", file.u8string()}},
							[this, id](const engine::Result& result)
							{
								if (!result.ok())
								{
									m_files.showError(id, result.error);
									return;
								}
								const auto job = result.value.at("job").at("id").get<uint64_t>();
								m_quickExports.insert(job);
								// The job may have ended before its command's result came: job.get tells.
								m_binding.send("job.get", {{"job", job}},
									[this](const engine::Result& state)
									{
										if (state.ok())
											quickExportEnded(state.value.at("job"));
									});
							});
					});
			}});
		m_quickExportEnded.push_back(m_binding.on(
			"job.finished", [this](const engine::Event& event) { quickExportEnded(event.data.at("job")); }));
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
							[this, kind, exporting, id = window.id()](const ui::FileDialogResult& chosen)
							{
								if (!chosen.error.empty())
									logging::warning("The file dialog failed: {}", chosen.error);
								if (chosen.files.empty())
									return;
								std::filesystem::path file = chosen.files.front();
								if (exporting && file.extension().empty())
									file += std::string(".") + kind.extension;
								// A file that can't be read or written, or one the project refuses, such as a tempo
								// track whose time signatures the locked signature track can't take, says why (#1976).
								m_binding.send(std::string(kind.engine) + (exporting ? ".export" : ".import"),
									{{"path", file.u8string()}},
									[this, id](const engine::Result& result)
									{
										if (!result.ok())
											m_files.showError(id, result.error);
									});
							});
					}});
			}
		}
		// Track import and export and track presets (docs/features/track-presets.md).
		addTrackFileCommands();
		// Auto fades (docs/features/fades.md): the project's, or the first selected audio track's.
		m_keys.add({"project.autoFades", "Opens the auto fades dialog for the whole project.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openAutoFades(window, 0); }});
		// Only audio tracks have auto fades: other selected tracks don't count.
		const auto firstAudioTrack = [this]() -> uint64_t
		{
			if (!m_trackList || !m_eventDisplay || !m_eventDisplay->project().is_object())
				return 0;
			const engine::Json& tracks = m_eventDisplay->project().at("tracks");
			for (const uint64_t id : m_trackList->selection())
				for (const engine::Json& track : tracks)
					if (track.at("id") == id && track.at("type") == "audio")
						return id;
			return 0;
		};
		m_keys.add({"track.autoFades", "Opens the auto fades dialog for the first selected audio track.",
			[this, firstAudioTrack](ui::Window& window, Rml::Element* /*zone*/)
			{
				if (const uint64_t track = firstAudioTrack(); track != 0)
					openAutoFades(window, track);
			}});
		m_keys.enableWhen(
			"track.autoFades", [firstAudioTrack](ui::Window&, Rml::Element*) { return firstAudioTrack() != 0; });
		// New tracks go below the lowest selected track (reference manual p. 198).
		const auto below = [this]
		{
			std::vector<std::pair<std::string, std::string>> attributes;
			if (m_trackList && !m_trackList->selection().empty())
				attributes.emplace_back("below", std::to_string(m_trackList->selection().back()));
			return attributes;
		};
		m_keys.add({"project.addTrack", "Opens the add track dialog.",
			[this, below](ui::Window& window, Rml::Element* /*zone*/)
			{ openOnce(window, "add-track.rml", m_addTrack, below()); }});
		// Add Sampler Track (reference manual pp. 143-144): the add track dialog's Sampler page.
		m_keys.add({"project.addSamplerTrack", "Opens the add track dialog on its Sampler page.",
			[this, below](ui::Window& window, Rml::Element* /*zone*/)
			{
				std::vector<std::pair<std::string, std::string>> attributes = below();
				attributes.emplace_back("type", "sampler");
				openOnce(window, "add-track.rml", m_addTrack, attributes);
			}});
		// Add Drum Track (reference manual pp. 145-146): the add track dialog's Drum page.
		m_keys.add({"project.addDrumTrack", "Opens the add track dialog on its Drum page.",
			[this, below](ui::Window& window, Rml::Element* /*zone*/)
			{
				std::vector<std::pair<std::string, std::string>> attributes = below();
				attributes.emplace_back("type", "drum");
				openOnce(window, "add-track.rml", m_addTrack, attributes);
			}});
		// Group Track, Effect Track and VCA Track to Selected Tracks (reference manual pp. 546, 554, 517): the add
		// track dialog's page for the new track, which the selected tracks play or send into, or which controls them.
		for (const auto& [id, type, description] :
			{std::tuple{"track.addGroupToSelected", "group",
				 "Opens the add track dialog for a group track that the selected tracks play into."},
				std::tuple{"track.addEffectToSelected", "effect",
					"Opens the add track dialog for an effect track that the selected tracks send to."},
				std::tuple{"track.addVcaToSelected", "vca",
					"Opens the add track dialog for a VCA track that controls a new link group of the selected "
					"tracks' channels."}})
		{
			m_keys.add(
				{id, description, [this, below, type = std::string(type)](ui::Window& window, Rml::Element* /*zone*/)
					{
						std::string tracks;
						for (const uint64_t track : m_trackList ? m_trackList->selection() : std::vector<uint64_t>())
							tracks += (tracks.empty() ? "" : ",") + std::to_string(track);
						std::vector<std::pair<std::string, std::string>> attributes = below();
						attributes.emplace_back("type", type);
						attributes.emplace_back("tracks", tracks);
						openOnce(window, "add-track.rml", m_addTrack, attributes);
					}});
			m_keys.enableWhen(id, [this](ui::Window&, Rml::Element*)
				{ return m_trackList != nullptr && !m_trackList->selection().empty(); });
		}
		// Project > Project Colors Setup (docs/features/colors.md).
		m_keys.add({"project.colorsSetup", "Opens the project colors setup dialog, which edits the project's colors.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openDialog(window, "project-colors.rml"); }});
		// Project > Project Rule Editor (#179, docs/features/project-rules.md): the rule editor's document on the
		// project's tracks, parts and events, which reads and sets the project window's selection with these.
		m_keys.add({"project.ruleEditor",
			"Opens the project rule editor, which finds tracks, parts and events and changes, deletes or selects them.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{ openDialog(window, "rule-editor.rml", {{"target", "projectRules"}}); }});
		m_lua.addFunction("ui.projectSelection",
			[this](const engine::Json& /*parameters*/)
			{
				return engine::Json{{"tracks", m_trackList ? m_trackList->selection() : std::vector<uint64_t>{}},
					{"events", m_eventDisplay ? m_eventDisplay->selection() : std::vector<uint64_t>{}},
					{"markers", m_markerLines ? m_markerLines->selection() : std::vector<uint64_t>{}}};
			});
		m_lua.addFunction("ui.selectInProject",
			[this](const engine::Json& parameters)
			{
				// An empty Lua table comes as an object.
				const auto ids = [&parameters](const char* key)
				{
					const auto found = parameters.is_object() ? parameters.find(key) : parameters.end();
					return found != parameters.end() && found->is_array() ? found->get<std::vector<uint64_t>>()
																		  : std::vector<uint64_t>{};
				};
				if (m_trackList)
					m_trackList->select(ids("tracks"));
				const std::vector<uint64_t> events = ids("events");
				if (m_eventDisplay)
					m_eventDisplay->select(events);
				// The project window selects events or markers: markers when no events are given.
				if (m_markerLines)
					m_markerLines->pick(events.empty() ? ids("markers") : std::vector<uint64_t>{});
				return engine::Json::object();
			});
		// For documents' buttons and menu items: gnubase.ui.command{command = "project.addTrack"} runs a key command
		// as its keys would, in the window of the document that called it (callingWindow), else the main window. A
		// command that doesn't exist or parameters it doesn't take are a Lua error at the call (KeyCommands::check), in
		// users' scripts too, instead of an engine error later or parameters a UI command drops (#669).
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
				m_keys.run(command->get<std::string>(), commandParameters, callingWindow(), nullptr);
				return engine::Json::object();
			});
		// Opens the confirmation dialog (resources/ui/confirm.rml) over the calling document: the caller sets the
		// global ConfirmDialog to {title, message, ok, onOk} first, and the dialog runs onOk only for OK, not for
		// Cancel.
		m_lua.addFunction("ui.confirm",
			[this](const engine::Json&)
			{
				openDialog(callingWindow(), "confirm.rml");
				return engine::Json::object();
			});
		for (const ProjectCommand& command : g_projectCommands)
		{
			m_keys.add({command.id, command.description,
				[this, id = std::string(command.id)](ui::Window& window, Rml::Element* zone)
				{
					if (runAudioFunction(id, window, zone))
						return;
					if (window.id() == m_sampleEditor.window && m_sampleEditor.editor &&
						ui::SampleEditor::isProcessCommand(id))
					{
						m_sampleEditor.editor->process(id.substr(5));
						return;
					}
					// Delete removes the selected markers, time signature or arranger events, which the project
					// window selects instead of events.
					if (id == "edit.delete" && m_markerLines && m_markerLines->removeSelected())
						return;
					if (id == "edit.delete" && m_signatureLane && m_signatureLane->row().removeSelected())
						return;
					if (id == "edit.delete" && m_arrangerLane && m_arrangerLane->removeSelected())
						return;
					if (m_eventDisplay)
						m_eventDisplay->actions().at(id)();
				}});
			// Those that only edit the selection range wait for one, and paste for something copied
			// (EventDisplay::canRun).
			m_keys.enableWhen(command.id,
				[this, id = std::string(command.id)](ui::Window& window, Rml::Element* zone)
				{
					if ((id == "audio.spectrumAnalyzer" || id == "audio.statistics") && poolAudioTarget(window, zone))
					{
						const auto pool = m_pool.lock();
						return pool && !pool->analysisClips().empty();
					}
					if (window.id() == m_sampleEditor.window && m_sampleEditor.editor &&
						ui::SampleEditor::isProcessCommand(id))
						return m_sampleEditor.editor->canProcess();
					return m_eventDisplay != nullptr && m_eventDisplay->canRun(id);
				});
			if (command.keys != nullptr)
			{
				m_keys.bind({command.keys, command.id, engine::Json::object(), "project"});
				if (ui::SampleEditor::isProcessCommand(command.id))
				{
					const std::string sampleCommand = "sampleEditor." + std::string(command.id).substr(5);
					m_keys.add({sampleCommand, "Processes the selected clip range in the sample editor.",
						[this, operation = std::string(command.id).substr(5)](ui::Window& window, Rml::Element*)
						{
							if (window.id() == m_sampleEditor.window && m_sampleEditor.editor)
								m_sampleEditor.editor->process(operation);
						}});
					m_keys.enableWhen(sampleCommand,
						[this](ui::Window& window, Rml::Element*)
						{
							return window.id() == m_sampleEditor.window && m_sampleEditor.editor &&
								m_sampleEditor.editor->canProcess();
						});
					m_keys.bind({command.keys, sampleCommand, engine::Json::object(), "sampleEditor"});
				}
			}
		}
		m_keys.bind({"Backspace", "edit.delete", engine::Json::object(), "project"});
		addAudioFunctions();
		m_keys.bind({"Backspace", "sampleEditor.delete", engine::Json::object(), "sampleEditor"});
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
			else if (id.find("Automation") != std::string::npos && id.find("Selected") != std::string::npos)
				// Copy and Paste Automation of Selected Tracks or Parameters wait for their selection, the paste
				// commands for copied automation too (docs/features/copy-automation.md).
				m_keys.enableWhen(command.id,
					[this, parameters = id.find("SelectedParameters") != std::string::npos,
						paste = id.find("paste") != std::string::npos](ui::Window&, Rml::Element*)
					{
						return m_trackList != nullptr && (!paste || m_trackList->automationCopied()) &&
							!(parameters ? m_trackList->selectedLanes(paste) : m_trackList->selection()).empty();
					});
			else if (id == "track.renderSelected" || id == "track.renderSettings")
				// Also for selected events and parts or a selection range (reference manual pp. 1474-1477).
				m_keys.enableWhen(command.id,
					[this](ui::Window&, Rml::Element*)
					{
						return m_trackList != nullptr &&
							(!m_trackList->selection().empty() ||
								(m_eventDisplay &&
									(!m_eventDisplay->selection().empty() || m_eventDisplay->range().has_value())));
					});
			else if (id == "track.pasteAutomationAll")
				m_keys.enableWhen(command.id, [this](ui::Window&, Rml::Element*)
					{ return m_trackList != nullptr && m_trackList->automationCopied(); });
			else
				m_keys.enableWhen(command.id, [this, selected](ui::Window&, Rml::Element*)
					{ return m_trackList != nullptr && (!selected || !m_trackList->selection().empty()); });
			if (command.keys != nullptr)
				m_keys.bind({command.keys, command.id, engine::Json::object(), "project"});
		}
		for (const ProjectCommand& command : g_trackVersionCommands)
		{
			m_keys.add(
				{command.id, command.description, [this, id = std::string(command.id)](ui::Window&, Rml::Element*)
					{
						if (m_trackList)
							m_trackList->actions().at(id)();
					}});
			// They work on the selected audio, instrument and MIDI tracks.
			m_keys.enableWhen(command.id, [this](ui::Window&, Rml::Element*)
				{ return m_trackList != nullptr && m_trackList->selectsVersions(); });
			if (command.keys != nullptr)
				m_keys.bind({command.keys, command.id, engine::Json::object(), "project"});
		}

		// Set Root Key for Unassigned Events in the context menu of the transpose track (reference manual p. 416).
		m_keys.enableWhen("transpose.assignRootKey",
			[this](ui::Window&, Rml::Element*)
			{
				return m_trackList != nullptr && m_trackList->selectsType("transpose") && m_eventDisplay != nullptr &&
					m_eventDisplay->project().is_object() &&
					!m_eventDisplay->project().value("rootKey", std::string()).empty();
			});

		// The Transport menu's record modes show the one that is set (#531); Lock and Unlock Record stay commands.
		m_transportChanged.emplace(
			m_binding.on("transport.changed", [this](const engine::Event& event) { m_transport = event.data; }));
		m_binding.send("transport.get", engine::Json::object(),
			[this](const engine::Result& result)
			{
				if (result.ok())
					m_transport = result.value;
			});
		m_keys.checkWhen("transport.set",
			[this](const engine::Json& parameters)
			{
				if (parameters.size() != 1 || !m_transport.is_object())
					return false;
				const std::string key = parameters.begin().key();
				return (key == "audioRecordMode" || key == "midiRecordMode" || key == "midiCycleRecordMode") &&
					m_transport.value(key, engine::Json()) == parameters.begin().value();
			});

		// Use Pre-roll and Use Post-roll show whether they are on (#583).
		m_keys.checkWhen("transport.toggle",
			[this](const engine::Json& parameters)
			{
				const std::string setting = parameters.value("setting", "");
				return (setting == "preRoll" || setting == "postRoll") && m_transport.is_object() &&
					m_transport.value(setting, false);
			});

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
		// locked asks whether to do it anyway (lock-record.rml), in the window whose key, button or menu item sent it
		// (#645); the engine's reason says which it was.
		for (const char* command : {"transport.stop", "transport.toggle", "transport.record"})
			m_lockRecord.push_back(m_binding.onResult(command,
				[this, command](const engine::Result& result)
				{
					if (result.ok())
						return;
					const bool punchOut = result.error.find("punch out") != std::string::npos;
					m_binding.send("transport.get", engine::Json::object(),
						[this, punchOut, from = m_keys.sentFrom(command)](const engine::Result& status)
						{
							ui::Window* window = m_shell.find(from);
							if (status.ok() && status.value.at("lockRecord").get<bool>() &&
								status.value.at("recording").get<bool>())
								openOnce(window != nullptr ? *window : m_main, "lock-record.rml", m_lockRecordDialog,
									{{"action", punchOut ? "punchOut" : "stop"}});
						});
				}));
		// The status line's remaining record time changes as the disk fills up, also by other programs.
		m_shell.every(std::chrono::seconds(1),
			[this]
			{
				if (m_statusLine)
					m_statusLine->update();
				if (m_maxRecordTime)
					m_maxRecordTime->update();
			});

		// The transport controls' commands (docs/features/transport-bar.md); the engine's transport commands have
		// their keys in the default key map.
		m_keys.add({"window.transportPanel", "Shows or hides the transport panel.",
			[this](ui::Window&, Rml::Element*) { toggleTransportPanel(); }});
		m_keys.bind({"F2", "window.transportPanel"});
		m_keys.add({"window.maxRecordTime",
			"Shows or hides the Max. Record Time window: the remaining record time, as the status line shows it "
			"(reference manual p. 356).",
			[this](ui::Window&, Rml::Element*) { toggleMaxRecordTime(); }});
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
			m_keys.add({wind.id, wind.description,
				[this, id = std::string(wind.id), step = wind.step](ui::Window&, Rml::Element*)
				{ m_winding.start(id, step); }, [this, id = std::string(wind.id)] { m_winding.stop(id); }});
			m_keys.bind({wind.keys, wind.id});
		}
		m_shell.eachStep([this] { return m_winding.step(); });
		m_shell.onUiScale([this](float before, float after) { scaleTimeDisplay(before, after); });
		// The MIDI editors render before their layout and keep their width when their window or zone resizes: their
		// expression lines follow the width the update of their own window gave them (#1671), not the box another
		// window's update finds before theirs is laid out again (#1768). The in-place editors' setTimeRange does so as
		// the project window scrolls and zooms.
		m_shell.afterUpdate(
			[this](ui::Window& window)
			{
				bool changed = false;
				for (ui::MidiEditor* editor : {editorWindow(), lowerZoneEditor()})
					if (ui::KeyEditor* keys = editor != nullptr ? editor->keyEditor() : nullptr)
						changed = keys->fitLines(window.context()) || changed;
				if (m_inPlace)
					for (const uint64_t track : m_inPlace->tracks())
						if (ui::KeyEditor* keys = m_inPlace->keyEditor(track))
							changed = keys->fitLines(window.context()) || changed;
				return changed;
			});
		// Reset All in its setup pane makes the transport panel follow its sections again (#1612).
		m_docking.onReset(
			[this](const std::string& window, const std::string&)
			{
				if (window == "transportPanel")
					m_panelFit.reset();
			});
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
		addWorkspaces();
	}

	void AppUi::showKeyCommandsError(ui::WindowId window, const std::string& title, const std::string& error)
	{
		if (!error.empty())
			logging::warning("{}: {}", title, error);
		Rml::Element* document = m_keyDialog.get();
		Rml::Element* message = document != nullptr && document->GetParentNode() != nullptr
			? document->GetElementById("keycommands-message")
			: nullptr;
		if (message != nullptr)
			message->SetInnerRML(Rml::StringUtilities::EncodeRml(error));
		else if (!error.empty())
			m_files.showNotice(window, title, error);
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
						[this, exporting, id = window.id(), title = dialog.title](const ui::FileDialogResult& result)
						{
							if (!result.error.empty())
								logging::warning("The file dialog failed: {}", result.error);
							if (result.files.empty())
								return;
							const std::filesystem::path& file = result.files.front();
							const auto finished = [this, id, title](const engine::Result& done)
							{
								// A command that worked takes the message of an earlier failure away.
								showKeyCommandsError(id, title, done.ok() ? std::string() : done.error);
							};
							if (exporting)
								m_binding.send("keyCommands.export", {{"path", file.u8string()}}, finished);
							else
								m_binding.send("keyCommands.import",
									{{"path", file.u8string()}, {"name", file.stem().u8string()}}, finished);
						});
				}});
		// File > Export > Key Command Assignments (reference manual p. 1560): an HTML list of every key that runs a
		// command, which the user saves where they choose.
		m_keys.add({"keyCommands.exportAssignments",
			"Exports the list of every key command assignment as an HTML file that the user chooses.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{
				ui::FileDialog dialog;
				dialog.kind = ui::FileDialogKind::Save;
				dialog.title = "Export Key Command Assignments";
				dialog.filters = {{"HTML files", "html"}};
				dialog.parent = window.id();
				m_shell.showFileDialog(dialog,
					[this, id = window.id(), title = dialog.title](const ui::FileDialogResult& result)
					{
						if (!result.error.empty())
							logging::warning("The file dialog failed: {}", result.error);
						if (result.files.empty())
							return;
						std::filesystem::path file = result.files.front();
						if (file.extension().empty())
							file += ".html";
						try
						{
							files::writeAtomically(file, keyAssignmentsHtml(m_keys.assignments()));
						}
						catch (const std::exception& error)
						{
							showKeyCommandsError(
								id, title, std::string("Can't write the key command assignments: ") + error.what());
						}
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
		// The dividers only count where sections go for lack of room, the transport bar (reference manual p. 307);
		// the transport panel takes its sections' width and has none.
		if (fill && !root.HasAttribute("fit"))
			for (int i = root.GetNumChildren() - 1; i >= 0; --i)
				if (root.GetChild(i)->HasAttribute("divider"))
					root.RemoveChild(root.GetChild(i));
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
		// A button held goes on until it is released; a click, a menu item or a script runs a command once.
		controls->holdWith(
			[this, id](const std::string& command, const engine::Json& parameters)
			{
				if (ui::Window* found = m_shell.find(id))
					m_keys.start(command, parameters, *found, nullptr);
			});
		controls->followSelection(
			[this] { return m_eventDisplay ? m_eventDisplay->selectionStart() : std::optional<int64_t>(); });
		return controls;
	}

	void AppUi::toggle(TransportWindow& shown, const char* name, const char* file, const char* title, ui::Size size)
	{
		if (const ui::Window* window = shown.shown(m_shell))
		{
			m_docking.saveWindows(); // its bounds as the user left them
			shown.controls.reset();
			m_shell.close(window->id());
			shown.window = 0;
			return;
		}
		shown.controls.reset(); // its window was closed
		ui::WindowSpec spec;
		spec.title = title;
		spec.size = size;
		spec.bounds = m_docking.bounds(name);
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
		{
			m_docking.attach("transportPanel", *document);
			// Once the controls show the project, their displays grow with their text, and the panel follows (#1474,
			// #1527, #1612).
			m_panelFit.open(window, *controls, !spec.bounds);
			shown.controls->onShown([this] { m_panelFit.follow(); });
		}
		else
		{
			shown.controls->onSize(
				[this, id = window.id()](int size)
				{
					const ui::Window* found = m_shell.find(id);
					if (found == nullptr)
						return;
					ui::Rect bounds = m_shell.bounds(id);
					const ui::Size sized = timeDisplaySize(m_shell, *found, size, found->dpRatio());
					if (bounds.size() == sized)
						return;
					bounds.width = sized.width;
					bounds.height = sized.height;
					m_shell.setBounds(id, bounds);
				});
		}
	}

	void AppUi::toggleTransportPanel()
	{
		toggle(m_transportPanel, "transportPanel", "transport-panel.rml", "Transport", g_transportPanelSize);
		m_docking.saveWindows();
	}

	void AppUi::toggleMaxRecordTime()
	{
		m_docking.saveWindows(); // its bounds as the user left them
		if (m_shell.find(m_maxRecordTimeWindow) != nullptr)
		{
			m_shell.close(m_maxRecordTimeWindow);
			m_maxRecordTime.reset();
			return;
		}
		ui::WindowSpec spec;
		spec.title = "Max. Record Time";
		spec.size = {260, 60};
		spec.bounds = m_docking.bounds("maxRecordTime");
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		m_maxRecordTimeWindow = window.id();
		const std::filesystem::path file = m_resources / "ui" / "max-record-time.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
		{
			m_shell.close(m_maxRecordTimeWindow);
			logging::error("Cannot load {}", file.u8string());
			return;
		}
		document->Show();
		m_maxRecordTime = std::make_unique<ui::StatusLine>(m_binding, *document);
		m_docking.saveWindows();
	}

	// The time display window grows and shrinks with the UI scale, as its digits do (#566): a window at the size of one
	// of its presets stays at that one, one the user sized keeps its proportions. Closed, it keeps the change for when
	// it opens with the bounds it was closed with.
	void AppUi::scaleTimeDisplay(float before, float after)
	{
		if (!(before > 0.0F))
			return;
		if (const ui::Window* window = timeDisplay(); window != nullptr && m_timeDisplay.controls)
			scaleTimeDisplayBounds(*window, m_timeDisplay.controls->size(), after / before);
		else
			m_timeDisplayScaling *= after / before;
	}

	void AppUi::scaleTimeDisplayBounds(const ui::Window& window, int size, float factor)
	{
		ui::Rect bounds = m_shell.bounds(window.id());
		ui::Size sized = timeDisplaySize(m_shell, window, size, window.dpRatio());
		if (!(bounds.size() == timeDisplaySize(m_shell, window, size, window.dpRatio() / factor)))
			sized = {static_cast<int>(std::lround(static_cast<float>(bounds.width) * factor)),
				static_cast<int>(std::lround(static_cast<float>(bounds.height) * factor))};
		if (sized == bounds.size())
			return;
		bounds.width = sized.width;
		bounds.height = sized.height;
		m_shell.setBounds(window.id(), bounds);
	}

	void AppUi::toggleTimeDisplay()
	{
		// A new window opens at the saved size, at the main window's dp ratio and then at its own, rather than at
		// the default size and jumping when ui.timeDisplay is read; one the user sized has its bounds, at the UI scale
		// they have now (#566).
		const bool opens = timeDisplay() == nullptr;
		const bool saved = m_docking.bounds("timeDisplay").has_value();
		int size = 1;
		if (opens)
		{
			const engine::Result options = engine::Client(m_engine).call("settings.get", {{"key", "ui.timeDisplay"}});
			const engine::Json value = options.ok() ? options.value.at("value") : engine::Json::object();
			if (const auto found = value.find("size");
				value.is_object() && found != value.end() && found->is_number_integer())
				size = std::clamp(found->get<int>(), 0, 4);
		}
		toggle(m_timeDisplay, "timeDisplay", "time-display.rml", "Time Display",
			timeDisplaySize(m_shell, m_main, size, m_main.dpRatio()));
		if (const ui::Window* window = timeDisplay(); opens && window != nullptr)
		{
			if (saved)
			{
				scaleTimeDisplayBounds(*window, size, m_timeDisplayScaling);
			}
			else if (ui::Rect bounds = m_shell.bounds(window->id());
				!(bounds.size() == timeDisplaySize(m_shell, *window, size, window->dpRatio())))
			{
				// The main window's monitor may not be the one it opened on.
				bounds.width = timeDisplaySize(m_shell, *window, size, window->dpRatio()).width;
				bounds.height = timeDisplaySize(m_shell, *window, size, window->dpRatio()).height;
				m_shell.setBounds(window->id(), bounds);
			}
			m_timeDisplayScaling = 1.0F;
		}
		m_docking.saveWindows();
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
		m_scaleLane.reset();
		m_arrangerLane.reset();
		m_rulerLanes.reset();
		m_inPlace.reset();
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
		// Imports into the project window that the engine refuses say why, as the project file commands do (#1976).
		m_eventDisplay->onImportFailed([this](const std::string& error) { m_files.showError(m_main.id(), error); });
		// A double click on a part opens it where the preference says (p. 1603).
		m_eventDisplay->onOpenEditor([this](uint64_t part) { openEditor(doubleClickEditor(), {part}); });
		// Paste goes onto the selected track (reference manual p. 270).
		m_eventDisplay->useTrackSelection(
			[this] { return m_trackList ? m_trackList->selection() : std::vector<uint64_t>(); });
		// The quantize panel lists the tracks of the selected audio events with hitpoints (#1344).
		m_eventDisplay->onSelectionShown(
			[this]
			{
				if (const std::shared_ptr<ui::QuantizePanel> panel = m_quantizePanel.lock())
					panel->showHitpointTracks(hitpointTracks());
			});
		if (Rml::Element* quantize = document->GetElementById("toolbar-quantize"))
			m_toolbarQuantize = std::make_unique<ui::QuantizePanel>(m_binding, *quantize,
				ui::QuantizePanel::Layout::Toolbar,
				[this](const std::string& command) { m_keys.run(command, engine::Json::object(), m_main, nullptr); });
		m_statusLine = std::make_unique<ui::StatusLine>(m_binding, *document);
		if (Rml::Element* info = document->GetElementById("info-line"))
		{
			m_infoLine = std::make_unique<ui::InfoLine>(*m_eventDisplay, m_binding, *info);
			m_docking.arrangeSections(*info); // its fields are the sections of its setup pane, there only now
		}
		if (Rml::Element* tracks = document->GetElementById("track-list"))
			m_trackList = std::make_unique<ui::TrackList>(*m_eventDisplay, m_binding, *tracks);
		if (m_trackList)
			m_colorMenu = std::make_unique<ui::ColorMenu>(
				[this] { return m_eventDisplay.get(); }, [this] { return m_trackList.get(); }, m_binding, *document);
		// The color tool's color on its button and its Choose Event Color pane (reference manual pp. 114-115).
		m_eventDisplay->onToolColor(
			[this]
			{
				if (m_colorMenu)
					m_colorMenu->showToolColor();
			});
		m_eventDisplay->onChooseColor(
			[this](float x, float y)
			{
				if (m_colorMenu)
					m_colorMenu->openAt(x, y);
			});
		m_menuDocument = document->GetObserverPtr();
		m_menu = std::make_unique<ui::MainMenu>(m_keys, m_main, *document);
		applyKeySettings(); // Edit > Macros
		m_workspacesMenu.reset();
		if (Rml::Element* workspaces = document->GetElementById("workspaces-menu"))
			m_workspacesMenu = std::make_unique<ui::WorkspacesMenu>(m_binding, *workspaces);
		m_markerLines = std::make_unique<ui::MarkerLines>(*m_eventDisplay, m_binding);
		m_tempoLane = std::make_unique<ui::TempoLane>(*m_eventDisplay, m_binding);
		m_signatureLane = std::make_unique<ui::SignatureLane>(*m_eventDisplay, m_binding);
		m_transposeLane = std::make_unique<ui::GlobalEventLane>(*m_eventDisplay, m_binding);
		m_chordLane = std::make_unique<ui::GlobalEventLane>(*m_eventDisplay, m_binding, ui::LaneKind::Chord);
		m_scaleLane = std::make_unique<ui::GlobalEventLane>(*m_eventDisplay, m_binding, ui::LaneKind::Scale);
		m_arrangerLane = std::make_unique<ui::GlobalEventLane>(*m_eventDisplay, m_binding, ui::LaneKind::Arranger);
		m_rulerLanes = std::make_unique<ui::RulerLanes>(*m_eventDisplay);
		m_inPlace = std::make_unique<ui::InPlaceEditors>(*m_eventDisplay, m_binding);
		if (m_infoLine)
		{
			m_infoLine->followMarkers(
				[this] { return m_markerLines != nullptr ? m_markerLines->selection() : std::vector<uint64_t>(); });
			m_infoLine->followArranger(
				[this] { return m_arrangerLane != nullptr ? m_arrangerLane->selection() : std::vector<uint64_t>(); });
		}
		const auto deselectPoints = [this]
		{
			if (m_eventDisplay && !m_eventDisplay->selectedPoints().empty())
				m_eventDisplay->selectPoints({});
		};
		m_markerLines->onSelect(
			[this, deselectPoints](const std::vector<uint64_t>& selection)
			{
				if (ui::MarkersWindow* window = markers(); window != nullptr && window->syncsSelection())
					window->setSelection(selection);
				// Markers selected deselect the time signature events, the arranger events and the automation points:
				// Delete deletes what was selected last.
				if (selection.empty())
					return;
				if (m_signatureLane)
					m_signatureLane->row().setSelection({});
				if (m_arrangerLane)
					m_arrangerLane->deselect();
				deselectPoints();
			});
		// So do time signature events selected: the markers, the arranger events, the automation points and the
		// selection range go.
		m_signatureLane->onSelect(
			[this, deselectPoints]
			{
				if (m_markerLines)
					m_markerLines->deselect();
				if (m_arrangerLane)
					m_arrangerLane->deselect();
				deselectPoints();
				if (m_eventDisplay && m_eventDisplay->range())
					m_eventDisplay->setRange(std::nullopt);
			});
		// And arranger events selected on their lane: the events and the time signature events too (#2076).
		m_arrangerLane->onSelect(
			[this, deselectPoints]
			{
				if (m_markerLines)
					m_markerLines->deselect();
				if (m_signatureLane)
					m_signatureLane->row().setSelection({});
				if (m_eventDisplay)
				{
					m_eventDisplay->select({});
					if (m_eventDisplay->range())
						m_eventDisplay->setRange(std::nullopt);
				}
				deselectPoints();
			});
		if (m_trackList)
			m_trackList->onArrangerEditor([this] { m_shell.post([this] { openArranger(); }); });
		// The marker tracks' Locate menus select the marker, Shift-click on Add Cycle Marker names the new one.
		if (m_trackList)
			m_trackList->onMarker(
				[this](uint64_t marker, bool rename)
				{
					if (m_markerLines == nullptr)
						return;
					m_markerLines->pick({marker});
					if (rename)
						m_markerLines->rename(marker);
				});
		// A profile switch that GNUbase couldn't finish nor undo at the start: the Profile Manager tells the user
		// (docs/features/profiles.md).
		m_binding.send("profile.list", engine::Json::object(),
			[this](const engine::Result& result)
			{
				if (result.ok() && result.value.value("unfinished", false))
					m_keys.run("window.profiles", engine::Json::object(), m_main, nullptr);
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
			m_docking.saveWindows();
			return nullptr;
		}
		ui::WindowSpec spec;
		spec.title = "Markers";
		spec.size = {720, 360};
		spec.bounds = m_docking.bounds("markers");
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
				if (m_markerLines == nullptr || m_markers == nullptr || !m_markers->syncsSelection())
					return;
				m_markerLines->setSelection(selection);
				if (!selection.empty() && m_eventDisplay && !m_eventDisplay->selectedPoints().empty())
					m_eventDisplay->selectPoints({}); // as the markers' onSelect does (#2011)
			});
		m_markers->syncFrom(
			[this] { return m_markerLines != nullptr ? m_markerLines->selection() : std::vector<uint64_t>(); });
		m_docking.saveWindows();
		return m_markers.get();
	}

	ui::OnScreenKeyboard* AppUi::toggleKeyboard()
	{
		const bool open = m_keyboard != nullptr && m_shell.find(m_keyboardWindow) != nullptr;
		m_keyboard.reset();
		if (open)
		{
			m_shell.close(m_keyboardWindow);
			m_docking.saveWindows();
			return nullptr;
		}
		ui::WindowSpec spec;
		spec.title = "On-Screen Keyboard";
		spec.size = {640, 200};
		spec.bounds = m_docking.bounds("onScreenKeyboard");
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
		// The octave and velocity, user settings (ui.onScreenKeyboard).
		if (const engine::Result saved =
				engine::Client(m_engine).call("settings.get", {{"key", "ui.onScreenKeyboard"}});
			saved.ok())
			m_keyboardSettings = {
				saved.value.at("value").value("octave", 3), saved.value.at("value").value("velocity", 100)};
		m_keyboard = std::make_unique<ui::OnScreenKeyboard>(m_binding, *document, m_keyboardSettings,
			[this](Rml::Input::KeyIdentifier position) { return m_shell.keyLabel(position); });
		m_docking.saveWindows();
		return m_keyboard.get();
	}

	ui::TempoEditor* AppUi::toggleTempoEditor()
	{
		const bool open = m_tempoEditor != nullptr && m_shell.find(m_tempoWindow) != nullptr;
		m_tempoEditor.reset();
		if (open)
		{
			m_shell.close(m_tempoWindow);
			m_docking.saveWindows();
			return nullptr;
		}
		ui::WindowSpec spec;
		spec.title = "Tempo Track";
		spec.size = {800, 400};
		spec.bounds = m_docking.bounds("tempoTrack");
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
		m_tempoEditor = std::make_unique<ui::TempoEditor>(m_engine, m_binding, *document);
		m_docking.saveWindows();
		return m_tempoEditor.get();
	}

	void AppUi::openArranger()
	{
		if (m_shell.find(m_arrangerWindow) != nullptr)
			m_shell.raise(m_arrangerWindow);
		else
			toggleArranger();
	}

	void AppUi::toggleArranger()
	{
		if (m_shell.find(m_arrangerWindow) != nullptr)
		{
			m_shell.close(m_arrangerWindow);
			m_docking.saveWindows();
			return;
		}
		ui::WindowSpec spec;
		spec.title = "Arranger";
		spec.size = {640, 360};
		spec.bounds = m_docking.bounds("arranger");
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
		m_docking.saveWindows();
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

	void AppUi::chooseMidiImport(ui::Window& window, bool newProject)
	{
		ui::FileDialog dialog;
		dialog.title = "Import MIDI File";
		dialog.filters = {{"MIDI files", "mid;midi;smf"}, {"All files", "*"}};
		dialog.parent = window.id();
		m_shell.showFileDialog(dialog,
			[this, newProject, id = window.id()](const ui::FileDialogResult& result)
			{
				if (!result.error.empty())
					logging::warning("The file dialog failed: {}", result.error);
				ui::Window* found = m_shell.find(id);
				if (result.files.empty() || !m_eventDisplay || found == nullptr)
					return;
				const std::vector<std::string> files{result.files.front().u8string()};
				if (!newProject)
				{
					m_eventDisplay->importMidi(files);
					return;
				}
				m_files.newProject(*found,
					[this, files]
					{
						if (m_eventDisplay)
							m_eventDisplay->importMidi(files, true);
					});
			});
	}

	void AppUi::addMixers()
	{
		// The mixer in the lower zone (reference manual pp. 434-436), with a view of its own.
		m_docking.add({"mixer", "Mixer", "project", "lower", [this](Rml::Element& element)
			{
				auto mixer = std::make_shared<ui::Mixer>(
					m_binding, m_meters, m_docking, element, "lower", ui::MixerKind::LowerZone, mixerActions());
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
		actions.choosePicture = [this](const Rml::Element& from, uint64_t track) { choosePicture(from, track); };
		// Later in the step: the click may be in the channel settings window, which opening it would rebuild.
		actions.editChannel = [this](uint64_t track) { m_shell.post([this, track] { openChannelSettings(track); }); };
		// Add Effect Track to Send (p. 554): the add track dialog's effect page, in the mixer's window, later in the
		// step, outside the drop-down list's event. The new track goes below the track whose send it is.
		actions.addEffectTrack = [this](Rml::Element& from, uint64_t track, int send)
		{
			for (const ui::Window* window : m_shell.windows())
				if (&window->context() == from.GetContext())
					m_shell.post(
						[this, id = window->id(), track, send]
						{
							if (ui::Window* found = m_shell.find(id))
								openOnce(*found, "add-track.rml", m_addTrack,
									{{"type", "effect"}, {"tracks", std::to_string(track)},
										{"send", std::to_string(send)}, {"below", std::to_string(track)}});
						});
		};
		actions.wentTo = [this](uint64_t track)
		{
			if (m_trackList)
				m_trackList->select({track});
		};
		return actions;
	}

	void AppUi::updateMenus()
	{
		if (m_menu)
			m_menu->update();
		for (auto it = m_windowMenus.begin(); it != m_windowMenus.end();)
		{
			if (m_shell.find(it->first) == nullptr)
			{
				it = m_windowMenus.erase(it); // the menu holds the window
				continue;
			}
			it->second->update();
			++it;
		}
	}

	void AppUi::updateMixerWindows()
	{
		for (MixerWindow& shown : m_mixers)
			if (shown.mixer && m_shell.find(shown.window) == nullptr)
				shown.mixer.reset(); // the user closed its window
		if (m_channelSettings.mixer && m_shell.find(m_channelSettings.window) == nullptr)
			m_channelSettings.mixer.reset();
		for (auto other = m_moreChannelSettings.begin(); other != m_moreChannelSettings.end();)
			other =
				m_shell.find(other->second.window) == nullptr ? m_moreChannelSettings.erase(other) : std::next(other);
		// The channel settings windows that follow follow the track selection (reference manual p. 499).
		std::vector<ui::Mixer*> following;
		if (channelSettings() != nullptr && channelSettings()->follows())
			following.push_back(channelSettings());
		for (auto& [number, other] : m_moreChannelSettings)
			if (other.mixer->follows())
				following.push_back(other.mixer.get());
		if (!following.empty() && m_trackList)
		{
			// Each window on its own: another one may have gone to a track and selected it.
			const std::vector<uint64_t> tracks = m_trackList->selection();
			for (ui::Mixer* settings : following)
				if (!tracks.empty() && settings->track() != tracks.front())
					settings->showTrack(tracks.front());
		}
	}

	ui::Mixer* AppUi::openChannelSettings(uint64_t track)
	{
		std::vector<MixerWindow*> open; // the first window first
		if (channelSettings() != nullptr)
			open.push_back(&m_channelSettings);
		for (auto& [number, other] : m_moreChannelSettings)
			if (m_shell.find(other.window) != nullptr)
				open.push_back(&other);
		const auto shows = [track](const MixerWindow* window) { return window->mixer->track() == track; };
		const auto follows = [](const MixerWindow* window) { return window->mixer->follows(); };
		auto found = std::find_if(open.begin(), open.end(), shows);
		if (found == open.end())
			found = std::find_if(open.begin(), open.end(), follows);
		MixerWindow* shown = found != open.end() ? *found : nullptr;
		if (shown != nullptr && !m_hidden)
			m_shell.raise(shown->window);
		if (shown == nullptr)
		{
			// The first window, or else another one with the next free number.
			int number = channelSettings() != nullptr ? 2 : 1;
			while (number > 1 && m_moreChannelSettings.count(number) != 0)
				++number;
			shown = number == 1 ? &m_channelSettings : &m_moreChannelSettings[number];
			const std::string name = number == 1 ? "channelSettings" : "channelSettings" + std::to_string(number);
			shown->mixer.reset();
			ui::WindowSpec spec;
			spec.title = "Channel Settings";
			spec.size = {1000, 560};
			spec.bounds = m_docking.bounds(name);
			spec.hidden = m_hidden;
			const ui::Window& window = m_shell.open(spec);
			shown->window = window.id();
			const std::filesystem::path file = m_resources / "ui" / "channel-settings.rml";
			Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
			Rml::Element* root = document != nullptr ? document->GetElementById("mixer") : nullptr;
			if (root == nullptr)
			{
				m_shell.close(shown->window);
				shown->window = 0;
				throw std::runtime_error("cannot load the channel settings window from " + file.u8string());
			}
			document->Show();
			shown->mixer = std::make_unique<ui::Mixer>(
				m_binding, m_meters, m_docking, *root, std::string(), ui::MixerKind::ChannelSettings, mixerActions());
			m_docking.attach(name, *document);
		}
		shown->mixer->showTrack(track);
		if (m_trackList)
			m_trackList->select({track});
		return shown->mixer.get();
	}

	std::vector<ui::WindowId> AppUi::channelSettingsWindows()
	{
		std::vector<ui::WindowId> windows;
		if (channelSettings() != nullptr)
			windows.push_back(m_channelSettings.window);
		for (const auto& [number, window] : m_moreChannelSettings)
			if (m_shell.find(window.window) != nullptr)
				windows.push_back(window.window);
		return windows;
	}

	void AppUi::addEditors()
	{
		// The editor window's left zone: the Visibility tab and the inspector's Global Tracks section (pp. 1144-1149).
		m_docking.add({"editorVisibility", "Visibility", "editorWindow", "left", [this](Rml::Element& element)
			{
				return std::make_shared<ui::MidiEditor>(
					m_binding, m_editorReads, element, "window", ui::EditorPart::Visibility);
			}});
		m_docking.add({"editorInspector", "Inspector", "editorWindow", "left", [this](Rml::Element& element)
			{
				return std::make_shared<ui::MidiEditor>(
					m_binding, m_editorReads, element, "window", ui::EditorPart::GlobalTracks);
			}});
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
		// Open/Close Editor (Mod+E and Return, p. 1189) opens the selected MIDI parts, or else the selected audio parts
		// (p. 692), where a double click opens them, by default in the lower zone (pp. 87, 1603), or closes that editor
		// without any; Open Editor in Lower Zone does the same in the lower zone. Without parts, Open/Close Editor
		// opens the first selected audio event in the sample editor window (p. 613), or closes that too.
		for (const bool lower : {false, true})
		{
			m_keys.add({lower ? "window.lowerZoneEditor" : "editor.openClose",
				lower
					? "Opens the selected MIDI parts or audio parts in the editor in the project window's lower zone, "
					  "or closes it."
					: "Opens the selected MIDI parts or audio parts in the editor where a double click opens them "
					  "(the preference editing.doubleClickOpensEditorIn: the lower zone or the editor window), or else "
					  "the first selected audio event in the sample editor, or closes them.",
				[this, lower](ui::Window&, Rml::Element*)
				{
					const std::string editor = lower ? "lower" : doubleClickEditor();
					const std::vector<uint64_t> parts = selectedParts();
					if (!parts.empty())
					{
						openEditor(editor, parts);
					}
					else if (const std::vector<uint64_t> events = selected("audioEvents"); !events.empty() && !lower)
					{
						m_binding.send("sampleEditor.open", {{"event", events.front()}});
					}
					else
					{
						m_binding.send("editor.close", {{"editor", editor}});
						if (!lower)
							m_binding.send("sampleEditor.close");
					}
				}});
		}
		m_keys.bind({"Mod+E", "editor.openClose", engine::Json::object(), "project"});
		m_keys.bind({"Return", "editor.openClose", engine::Json::object(), "project"});
		// The MIDI menu's Open Key Editor (reference manual p. 1189), Open Drum Editor (p. 1221) and Open List Editor
		// (Mod+G, p. 1243): the selected MIDI parts in the editor window, with that editor.
		for (const auto& [command, content, description] : {
				 std::tuple{"editor.openKey", "key", "Opens the selected MIDI parts in the key editor window."},
				 std::tuple{"editor.openDrum", "drum", "Opens the selected MIDI parts in the drum editor window."},
				 std::tuple{"editor.openList", "list", "Opens the selected MIDI parts in the list editor window."},
			 })
			m_keys.add({command, description, [this, content = std::string(content)](ui::Window&, Rml::Element*)
				{
					const std::vector<uint64_t> parts = selected("midiParts");
					if (!parts.empty())
						m_binding.send("editor.open", {{"editor", "window"}, {"parts", parts}, {"content", content}});
				}});
		// Edit In-Place (Mod+Shift+I, reference manual p. 1261) opens or closes the in-place editors of the selected
		// MIDI and instrument tracks, MIDI > Open In-Place Editor opens them (p. 1259). Both wait for such a track to
		// be selected.
		const auto noteTracks = [this]
		{
			engine::Json tracks = engine::Json::array();
			if (!m_trackList || !m_eventDisplay || !m_eventDisplay->project().is_object())
				return tracks;
			const std::vector<uint64_t> selection = m_trackList->selection();
			for (const engine::Json& track : m_eventDisplay->project().at("tracks"))
				if ((track.at("type") == "midi" || track.at("type") == "instrument") &&
					std::find(selection.begin(), selection.end(), track.at("id").get<uint64_t>()) != selection.end())
					tracks.push_back(track.at("id"));
			return tracks;
		};
		for (const auto& [id, open, description] :
			{std::tuple{"editor.editInPlace", false,
				 "Opens or closes the in-place editors of the selected MIDI and instrument tracks."},
				std::tuple{"editor.openInPlace", true,
					"Opens the in-place editors of the selected MIDI and instrument tracks."}})
		{
			m_keys.add({id, description, [this, noteTracks, opens = open](ui::Window&, Rml::Element*)
				{
					engine::Json tracks = noteTracks();
					if (tracks.empty())
						return;
					engine::Json parameters = {{"tracks", std::move(tracks)}};
					if (opens)
						parameters["open"] = true;
					m_binding.send("editor.inPlace", parameters);
				}});
			m_keys.enableWhen(id, [noteTracks](ui::Window&, Rml::Element*) { return !noteTracks().empty(); });
		}
		m_keys.bind({"Mod+Shift+I", "editor.editInPlace", engine::Json::object(), "project"});
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
		// The Drum Map Setup (pp. 1235-1237), also from the drum editor's Map menu. Its Save chooses the file for the
		// map it shows (gnubase.ui.saveDrumMap); its Load runs drumMap.load.
		m_keys.add({"drumMap.setup",
			"Opens the Drum Map Setup, which lists the project's drum maps and changes them and their sounds.",
			[this](ui::Window& window, Rml::Element*) { openDrumMaps(window); }});
		m_lua.addFunction("ui.saveDrumMap",
			[this](const engine::Json& parameters)
			{
				chooseDrumMapFile(callingWindow().id(), parameters.value("map", uint64_t{0}));
				return engine::Json::object();
			});
		// The pattern editor (#181, reference manual p. 895): the selected MIDI parts in the editor window's pattern
		// editor, and Preset Management's Save Preset and Load Preset (pp. 932-933) for the editor's active track.
		m_keys.add({"editor.openPattern", "Opens the selected MIDI parts in the pattern editor window.",
			[this](ui::Window&, Rml::Element*)
			{
				const std::vector<uint64_t> parts = selected("midiParts");
				if (!parts.empty())
					m_binding.send("editor.open", {{"editor", "window"}, {"parts", parts}, {"content", "pattern"}});
			}});
		for (const bool save : {true, false})
			m_keys.add({save ? "pattern.savePreset" : "pattern.loadPreset",
				save ? "Saves the patterns of the editor's active track as a pattern preset."
					 : "Loads a pattern preset into the editor's active track.",
				[this, save](ui::Window& window, Rml::Element*) { patternPreset(window.id(), save); }});
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
		// By reference, not through Json::value's copy: the main menu asks for it in every frame (#1901).
		const engine::Json& project = m_eventDisplay->project();
		const auto items = project.find(kind);
		if (items == project.end())
			return found;
		const std::vector<uint64_t>& selection = m_eventDisplay->selection();
		for (const engine::Json& item : *items)
		{
			const auto id = item.at("id").get<uint64_t>();
			if (std::find(selection.begin(), selection.end(), id) != selection.end())
				found.push_back(id);
		}
		return found;
	}

	void AppUi::openEditor(const std::string& editor, const std::vector<uint64_t>& parts)
	{
		m_binding.send("editor.open", {{"editor", editor}, {"parts", parts}});
		if (editor != "lower")
			return;
		// Opening it explicitly brings the Editor tab forward, even from behind the Mixer, and the lower zone's editor
		// gets the keyboard focus (p. 100).
		m_docking.open("editor");
		m_docking.focus("project", "lower");
	}

	std::string AppUi::doubleClickEditor()
	{
		const engine::Result result =
			engine::Client(m_engine).call("settings.get", {{"key", "editing.doubleClickOpensEditorIn"}});
		return result.ok() && result.value.at("value") == "window" ? "window" : "lower";
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
		m_sampleEditor.quantize.reset();
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
		spec.bounds = m_docking.bounds("sampleEditor");
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
		m_sampleEditor.editor->onRegionDrop(
			[this](const engine::Json& region, float x, float y) { dropSampleRegion(region, x, y); });
		// The quantize functions of the sample editor's toolbar (p. 367), on the event it edits (quantizeTarget).
		if (Rml::Element* quantize = document->GetElementById("toolbar-quantize"))
			m_sampleEditor.quantize =
				std::make_unique<ui::QuantizePanel>(m_binding, *quantize, ui::QuantizePanel::Layout::Toolbar,
					[this](const std::string& command)
					{
						if (ui::Window* window = m_shell.find(m_sampleEditor.window))
							m_keys.run(command, engine::Json::object(), *window, nullptr);
					});
	}

	void AppUi::followEditor(const engine::Json& view)
	{
		const std::string editor = view.at("editor").get<std::string>();
		if (editor != "lower" && editor != "window")
			return; // an in-place editor (ui/inplaceeditor.h)
		const bool open = view.at("open").get<bool>();
		if (open && m_closing.count(editor) != 0)
			return; // on its way from before the user closed it, which the editor.close sent closes
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
		if (open && m_editor.window != 0 && !shown)
		{
			closeEditor(editor); // the user closed the window before followSelection saw it
			return;
		}
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
		spec.bounds = m_docking.bounds("editor");
		spec.hidden = m_hidden;
		ui::Window& window = m_shell.open(spec);
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
			m_binding, m_editorReads, *root, "window", ui::EditorPart::Editor, [this] { m_docking.open("editor"); });
		m_editor.editor->setPlaybackPosition([this] { return m_engine.playbackPosition(); });
		m_editor.editor->setDrumMapSetup(
			[this]
			{
				m_shell.post(
					[this]
					{
						ui::Window* window = m_shell.find(m_editor.window);
						openDrumMaps(window != nullptr ? *window : m_main);
					});
			});
		m_docking.attach("editorWindow", *document);
		m_windowMenus[window.id()] = std::make_unique<ui::MainMenu>(m_keys, window, *document);
	}

	void AppUi::closeEditor(const std::string& editor)
	{
		if (editor == "window")
		{
			m_editor.editor.reset();
			m_editor.window = 0;
		}
		else
		{
			m_editorViews[editor]["open"] = false; // until editor.changed says so
		}
		// Until the engine answers, an editor.changed with the view open was sent before it closed (#625).
		m_closing.insert(editor);
		m_binding.send("editor.close", {{"editor", editor}},
			[this, editor](const engine::Result& result)
			{
				m_closing.erase(editor);
				if (!result.ok())
					logging::warning("The {} editor didn't close: {}", editor, result.error);
			});
	}

	void AppUi::followSelection()
	{
		// The user closed the editor window or the lower zone's editor: Keep Editor Contents goes off with it (p.
		// 1153).
		if (m_editor.window != 0 && m_shell.find(m_editor.window) == nullptr)
			closeEditor("window");
		if (m_sampleEditor.window != 0 && m_shell.find(m_sampleEditor.window) == nullptr)
		{
			m_sampleEditor.quantize.reset();
			m_sampleEditor.editor.reset();
			m_sampleEditor.window = 0;
			m_binding.send("sampleEditor.close");
		}
		const auto open = [this](const char* editor)
		{ return m_editorViews.value(editor, engine::Json::object()).value("open", false); };
		if (open("lower") && m_docking.windowOf("editor") == nullptr && !m_docking.visible("project", "lower"))
			closeEditor("lower");
		// The Editor tab coming to the front without an open editor, as when the user picks it from the tab bar or
		// shows the lower zone with it, opens the editor with the project window's selected parts (p. 1153).
		const bool editorTab = m_docking.showing("editor");
		if (editorTab && !m_editorTabShown && !open("lower") && m_closing.count("lower") == 0)
			openLowerIfClosed();
		m_editorTabShown = editorTab;
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

	void AppUi::openLowerIfClosed()
	{
		// Asks first: an editor.open on its way, such as the editor window's Open in Lower Zone, which brought the tab
		// forward, comes before the answer, and then the editor keeps its parts.
		m_binding.send("editor.get", {{"editor", "lower"}},
			[this](const engine::Result& result)
			{
				if (!result.ok() || result.value.value("open", false) || !m_docking.showing("editor") ||
					m_closing.count("lower") != 0)
					return;
				if (const std::vector<uint64_t> parts = selectedParts(); !parts.empty())
					m_binding.send("editor.open", {{"editor", "lower"}, {"parts", parts}});
			});
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
			m_docking.saveWindows();
			return nullptr;
		}
		ui::WindowSpec spec;
		spec.title = number == 1 ? std::string("Mixer") : "Mixer " + std::to_string(number);
		spec.size = {1000, 560};
		spec.bounds = m_docking.bounds("mixer" + std::to_string(number));
		spec.hidden = m_hidden;
		ui::Window& window = m_shell.open(spec);
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
		shown.mixer = std::make_unique<ui::Mixer>(m_binding, m_meters, m_docking, *root,
			"window" + std::to_string(number), ui::MixerKind::Window, mixerActions());
		m_docking.attach("mixer" + std::to_string(number), *document);
		m_windowMenus[window.id()] = std::make_unique<ui::MainMenu>(m_keys, window, *document);
		m_docking.saveWindows();
		return shown.mixer.get();
	}

	ui::Mixer* AppUi::mixer(int number)
	{
		return m_mixers.at(static_cast<size_t>(number - 1)).mixer.get(); // the step lets go of it once its window goes
	}

	void AppUi::chooseProjectFile(
		const ui::FileDialog& dialog, std::function<void(const std::filesystem::path&)> chosen)
	{
		m_shell.showFileDialog(dialog,
			[this, chosen = std::move(chosen), resets = m_resets](const ui::FileDialogResult& result)
			{
				if (!result.error.empty())
					logging::warning("The file dialog failed: {}", result.error);
				if (!result.files.empty() && resets == m_resets)
					chosen(result.files.front());
			});
	}

	void AppUi::choosePicture(const Rml::Element& from, uint64_t track)
	{
		ui::FileDialog dialog;
		dialog.title = "Choose Track Picture";
		dialog.filters = {{"Images", "png;jpg;jpeg;svg"}, {"All files", "*"}};
		dialog.parent = m_main.id();
		for (const ui::Window* window : m_shell.windows())
			if (&window->context() == from.GetContext())
				dialog.parent = window->id(); // the mixer's, e.g. on another monitor
		chooseProjectFile(dialog, [this, track](const std::filesystem::path& file)
			{ m_binding.send("track.set", {{"track", track}, {"picture", file.u8string()}}); });
	}

	void AppUi::addTrackFileCommands()
	{
		const auto selection = [this]
		{ return m_trackList != nullptr ? m_trackList->selection() : std::vector<uint64_t>(); };
		const auto hasSelection = [selection](ui::Window&, Rml::Element*) { return !selection().empty(); };
		// File > Import > Tracks from Project or Track Archive (reference manual pp. 201-205): the file chosen opens in
		// the Import Options dialog, which imports below the first selected track or at the bottom.
		m_keys.add({"file.importTracks",
			"Imports tracks of a project or track archive through the Import Options dialog, below the first selected "
			"track or at the bottom.",
			[this, selection](ui::Window& window, Rml::Element* /*zone*/)
			{
				ui::FileDialog dialog;
				dialog.kind = ui::FileDialogKind::Open;
				dialog.title = "Import Tracks";
				dialog.filters = {{"DAWproject files", "dawproject"}, {"All files", "*"}};
				dialog.parent = window.id();
				m_shell.showFileDialog(dialog,
					[this, selection, id = window.id()](const ui::FileDialogResult& chosen)
					{
						if (!chosen.error.empty())
							logging::warning("The file dialog failed: {}", chosen.error);
						ui::Window* parent = m_shell.find(id);
						if (chosen.files.empty() || parent == nullptr)
							return;
						// The selection now: a project.reset while the file dialog was open replaced the one before.
						const std::vector<uint64_t> below = selection();
						openDialog(*parent, "track-import.rml",
							{{"path", chosen.files.front().u8string()},
								{"below", below.empty() ? std::string() : std::to_string(below.front())}});
					});
			}});
		// File > Export > Selected Tracks (reference manual pp. 205-206): a track archive, with copies of the media.
		m_keys.add({"file.exportTracks", "Exports the selected tracks as a track archive.",
			[this, selection](ui::Window& window, Rml::Element* /*zone*/)
			{
				ui::FileDialog dialog;
				dialog.kind = ui::FileDialogKind::Save;
				dialog.title = "Export Selected Tracks";
				dialog.filters = {{"DAWproject files", "dawproject"}};
				dialog.location = "Tracks.dawproject";
				dialog.parent = window.id();
				chooseProjectFile(dialog,
					[this, tracks = selection(), id = window.id()](std::filesystem::path file)
					{
						if (file.extension().empty())
							file += ".dawproject";
						m_binding.send("track.export", {{"tracks", tracks}, {"path", file.u8string()}},
							[this, id](const engine::Result& result)
							{
								if (!result.ok())
								{
									m_files.showError(id, result.error);
									return;
								}
								// The tracks left out and the media files missing, both if both (#1736).
								std::string notice;
								if (const std::string left = leftOut(result.value); !left.empty())
									notice = "The tracks were exported without " + left + ".";
								if (const std::string missing = ui::missingMedia(result.value); !missing.empty())
									notice += (notice.empty() ? "" : " ") + missing;
								if (!notice.empty())
									m_files.showNotice(id, "Export Selected Tracks", notice);
							});
					});
			}});
		m_keys.enableWhen("file.exportTracks", hasSelection);
		// Track presets (reference manual pp. 199, 238-243): the dialog track-preset.rml for the selected tracks.
		for (const auto& [id, mode, description] :
			{std::tuple{"track.saveTrackPreset", "save", "Saves the sound of the selected tracks as a track preset."},
				std::tuple{"track.loadTrackPreset", "load", "Gives the selected tracks the sound of a track preset."},
				std::tuple{"project.addTrackFromPreset", "add",
					"Adds the tracks of a track preset below the first selected track, or at the bottom."}})
		{
			m_keys.add({id, description,
				[this, selection, mode = std::string(mode)](ui::Window& window, Rml::Element* /*zone*/)
				{
					std::string tracks;
					for (const uint64_t track : selection())
						tracks += (tracks.empty() ? "" : ",") + std::to_string(track);
					openDialog(window, "track-preset.rml", {{"mode", mode}, {"tracks", tracks}});
				}});
			if (std::string(mode) != "add")
				m_keys.enableWhen(id, hasSelection);
		}
	}

	void AppUi::patternPreset(ui::WindowId parent, bool save, const std::string& editor)
	{
		// The editor window's active track, else the lower zone's.
		m_binding.send("editor.get", {{"editor", editor}},
			[this, parent, save, editor](const engine::Result& view)
			{
				if (!view.ok())
					return;
				const engine::Json track = view.value.at("activeTrack");
				if (track == 0)
				{
					if (editor == "window")
						patternPreset(parent, save, "lower");
					return;
				}
				ui::FileDialog dialog;
				dialog.kind = save ? ui::FileDialogKind::Save : ui::FileDialogKind::Open;
				dialog.title = save ? "Save Pattern Preset" : "Load Pattern Preset";
				dialog.filters = {{"Pattern presets", "patternpreset"}};
				dialog.parent = parent;
				chooseProjectFile(dialog,
					[this, save, track](const std::filesystem::path& file)
					{
						m_binding.send(
							save ? "pattern.export" : "pattern.import", {{"track", track}, {"path", file.u8string()}});
					});
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

	void AppUi::openDrumMaps(ui::Window& window)
	{
		// The editors' views say whose map to show first; the dialog reads them when it opens.
		openOnce(window, "drum-maps.rml", m_drumMaps);
	}

	void AppUi::chooseDrumMapFile(ui::WindowId parent, uint64_t map)
	{
		ui::FileDialog dialog;
		dialog.kind = ui::FileDialogKind::Save;
		dialog.title = "Save Drum Map";
		dialog.filters = {{"Drum maps", "drm"}};
		dialog.parent = parent;
		chooseProjectFile(dialog,
			[this, map](std::filesystem::path file)
			{
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
				{
					logging::warning("The folder dialog failed: {}", result.error);
					reportInPool("media.locate", {0, nullptr, result.error});
				}
				if (!result.files.empty())
					m_binding.send("media.locate", {{"folder", result.files.front().u8string()}});
			});
	}

	void AppUi::reportInPool(const std::string& command, const engine::Result& result)
	{
		if (!m_docking.showing("pool") || m_docking.windowOf("pool") != nullptr)
			m_docking.open("pool");
		if (const std::shared_ptr<ui::PoolPanel> pool = m_pool.lock())
			pool->report(command, result);
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

	Rml::ElementDocument& AppUi::openOnce(ui::Window& window, const char* name, Rml::ObserverPtr<Rml::Element>& open,
		const std::vector<std::pair<std::string, std::string>>& attributes)
	{
		// One dialog: a second document would share the first one's table in the UI's Lua. A closed document has left
		// its context, until the context releases it.
		auto* document = dynamic_cast<Rml::ElementDocument*>(open.get());
		if (document != nullptr && document->GetParentNode() != nullptr)
		{
			document->PullToFront(); // no Show: its show event would load the dialog again, over what was typed
			return *document;
		}
		Rml::ElementDocument& opened = openDialog(window, name, attributes);
		open = opened.GetObserverPtr();
		return opened;
	}

	void AppUi::openAutoFades(ui::Window& window, uint64_t track)
	{
		// Its Lua reads the track when it shows.
		std::vector<std::pair<std::string, std::string>> attributes;
		if (track != 0)
			attributes.emplace_back("track", std::to_string(track));
		openOnce(window, "auto-fades.rml", m_autoFades, attributes);
	}

	ui::Window& AppUi::callingWindow()
	{
		// The nearest local "document" up the stack of the Lua that called, if it is one of RmlUi's documents: event
		// handlers (onclick, onshow) have one, and functions they call with it. Only in the current run: Lua that a
		// gnubase function runs, such as the script console's prompt (ui.runLua) or a script (ui.command), runs on
		// the same stack above the handler that started it, but runs in the main window.
		lua_State* state = m_lua.caller();
		lua_Debug frame{};
		for (int level = 1; state != nullptr && lua_getstack(state, level, &frame) != 0 &&
			!scripting::Runtime::isGnubaseCall(state, frame);
			++level)
			for (int local = 1;; ++local)
			{
				const char* name = lua_getlocal(state, &frame, local);
				if (name == nullptr)
					break;
				auto* const* document = std::string_view(name) == "document"
					? static_cast<Rml::ElementDocument**>(luaL_testudata(state, -1, "Document"))
					: nullptr;
				lua_pop(state, 1);
				if (document == nullptr || *document == nullptr)
					continue;
				for (ui::Window* window : m_shell.windows())
					if (&window->context() == (*document)->GetContext())
						return *window;
			}
		return m_main;
	}

	Rml::ElementDocument& AppUi::openDialog(
		ui::Window& window, const char* name, const std::vector<std::pair<std::string, std::string>>& attributes)
	{
		const std::filesystem::path file = m_resources / "ui" / name;
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
			throw std::runtime_error("cannot load the dialog " + file.u8string());
		// The attributes with ids of the project's objects. The articulation map setup follows project.reset itself,
		// dropping its track.
		constexpr std::array<std::string_view, 7> ids = {
			"below", "destination", "notes", "parts", "send", "track", "tracks"};
		bool forObjects = false;
		for (const auto& [attribute, value] : attributes)
		{
			document->SetAttribute(attribute, value);
			forObjects = forObjects ||
				(!value.empty() && std::find(ids.begin(), ids.end(), attribute) != ids.end() &&
					std::string_view(name) != "articulation-maps.rml");
		}
		if (forObjects)
		{
			m_projectDialogs.erase(std::remove_if(m_projectDialogs.begin(), m_projectDialogs.end(),
									   [](const Rml::ObserverPtr<Rml::Element>& open) { return !open; }),
				m_projectDialogs.end());
			m_projectDialogs.push_back(document->GetObserverPtr());
		}
		document->Show(Rml::ModalFlag::Modal);
		return *document;
	}

	bool AppUi::runAudioFunction(const std::string& command, ui::Window& window, Rml::Element* zone)
	{
		const char* file = nullptr;
		if (command == "audio.detectSilence")
			file = "detect-silence.rml";
		else if (command == "audio.spectrumAnalyzer")
			file = "spectrum-analyzer.rml";
		else if (command == "audio.statistics")
			file = "statistics.rml";
		if (file == nullptr)
			return false;
		if (command != "audio.detectSilence" && poolAudioTarget(window, zone))
		{
			if (const auto pool = m_pool.lock(); pool && !pool->analysisClips().empty())
				openAudioFunction(file, false, pool->analysisClips());
		}
		else
		{
			openAudioFunction(file, window.id() == m_sampleEditor.window);
		}
		return true;
	}

	void AppUi::addAudioFunctions()
	{
		m_keys.add({"audio.divideAtHitpoints", "Divides the selected folder's audio events at hitpoints.",
			[this](ui::Window&, Rml::Element*)
			{
				if (m_trackList && m_trackList->selection().size() == 1)
					m_binding.send("track.divideAtHitpoints", {{"track", m_trackList->selection().front()}},
						[this](const engine::Result& result)
						{
							if (!result.ok() && m_eventDisplay)
								m_eventDisplay->showRefusal(result.error);
						});
			}});
		m_keys.enableWhen("audio.divideAtHitpoints",
			[this](ui::Window&, Rml::Element*)
			{
				if (!m_trackList || !m_eventDisplay || m_trackList->selection().size() != 1)
					return false;
				for (const auto& track : m_eventDisplay->project().at("tracks"))
					if (track.at("id") == m_trackList->selection().front())
						return track.at("type") == "folder" && track.value("groupEditing", false);
				return false;
			});
	}

	bool AppUi::poolAudioTarget(ui::Window& window, Rml::Element* zone)
	{
		if (const ui::Window* pool = m_docking.windowOf("pool"))
			return pool->id() == window.id();
		return window.id() == m_main.id() && zone != nullptr &&
			zone->GetAttribute<Rml::String>("focuszone", "") == "lower" && m_docking.showing("pool");
	}

	void AppUi::dropSampleRegion(const engine::Json& region, float x, float y)
	{
		const ui::Window* source = m_shell.find(m_sampleEditor.window);
		if (source == nullptr || !m_eventDisplay)
			return;
		const ui::Size pixels = source->pixelSize();
		const double mouseX = x;
		const double mouseY = y;
		if (pixels.width <= 0 || pixels.height <= 0 ||
			(mouseX >= 0 && mouseY >= 0 && mouseX < pixels.width && mouseY < pixels.height))
			return;
		const ui::Rect from = m_shell.bounds(source->id());
		const ui::Rect to = m_shell.bounds(m_main.id());
		const ui::Size targetPixels = m_main.pixelSize();
		if (to.width <= 0 || to.height <= 0)
			return;
		const double screenX = from.x + (mouseX * from.width / pixels.width);
		const double screenY = from.y + (mouseY * from.height / pixels.height);
		const auto targetX = static_cast<float>((screenX - to.x) * targetPixels.width / to.width);
		const auto targetY = static_cast<float>((screenY - to.y) * targetPixels.height / to.height);
		m_eventDisplay->dropRegion(region, targetX, targetY);
	}

	void AppUi::openAudioFunction(const std::string& file, bool sampleEditor, const std::string& clips)
	{
		AudioFunctionWindow& shown = m_audioFunctions[file];
		if (shown.document && shown.document->GetParentNode() != nullptr && m_shell.find(shown.window) != nullptr)
		{
			m_shell.raise(shown.window);
			return;
		}
		std::string events;
		std::pair<int64_t, int64_t> range{};
		if (sampleEditor && m_sampleEditor.editor)
		{
			if (m_sampleEditor.editor->event() != 0)
				events = std::to_string(m_sampleEditor.editor->event());
			range = m_sampleEditor.editor->analysisRange();
		}
		else if (clips.empty() && m_eventDisplay)
		{
			const auto& selection = m_eventDisplay->selection();
			for (const engine::Json& event : m_eventDisplay->project().at("audioEvents"))
				if (std::find(selection.begin(), selection.end(), event.at("id").get<uint64_t>()) != selection.end())
					events += event.at("id").dump() + " ";
		}
		if (events.empty() && clips.empty())
			return;
		ui::WindowSpec spec;
		spec.title = "Statistics";
		if (file == "detect-silence.rml")
			spec.title = "Detect Silence";
		else if (file == "spectrum-analyzer.rml")
			spec.title = "Spectrum Analyzer";
		spec.size = {760, 720};
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		shown.window = window.id();
		auto* document = window.context().LoadDocument((m_resources / "ui" / file).generic_u8string());
		if (document == nullptr)
		{
			m_shell.close(shown.window);
			return;
		}
		document->SetAttribute("events", events);
		document->SetAttribute("clips", clips);
		if (sampleEditor && range.second > range.first)
		{
			document->SetAttribute("start", std::to_string(range.first));
			document->SetAttribute("end", std::to_string(range.second));
		}
		shown.document = document->GetObserverPtr();
		m_projectDialogs.erase(std::remove_if(m_projectDialogs.begin(), m_projectDialogs.end(),
								   [](const Rml::ObserverPtr<Rml::Element>& open) { return !open; }),
			m_projectDialogs.end());
		m_projectDialogs.push_back(shown.document);
		document->Show();
	}

	void AppUi::quickExportEnded(const engine::Json& job)
	{
		if (job.at("state") == "running" || m_quickExports.erase(job.at("id").get<uint64_t>()) == 0)
			return;
		if (job.at("state") == "done")
			m_openExternally(
				std::filesystem::u8path(job.at("result").at("path").get<std::string>()).parent_path(), false);
	}
}
