// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "scripting/runtime.h"
#include "ui/colormenu.h"
#include "ui/docking.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/gpurenderer.h"
#include "ui/infoline.h"
#include "ui/inspector.h"
#include "ui/keycommands.h"
#include "ui/mainmenu.h"
#include "ui/markers.h"
#include "ui/midieditor.h"
#include "ui/mixer.h"
#include "ui/onscreenkeyboard.h"
#include "ui/poolpanel.h"
#include "ui/projectfiles.h"
#include "ui/quantizepanel.h"
#include "ui/rmlui.h"
#include "ui/sampleeditor.h"
#include "ui/sdlwindowsystem.h"
#include "ui/shell.h"
#include "ui/signaturetrack.h"
#include "ui/statusline.h"
#include "ui/tempotrack.h"
#include "ui/themes.h"
#include "ui/tracklist.h"
#include "ui/transport.h"
#include "ui/uisettings.h"

#include <RmlUi/Core/ObserverPtr.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
	class ElementDocument;
}

namespace gnubase::app
{
	// GNUbase's UI for an engine: SDL3 windows drawn through SDL_GPU, RmlUi with GNUbase's fonts, themes and widgets,
	// and the shell, which follows the user settings and the engine's events and runs key commands, with the main
	// window open. runWindowed
	// runs it; UI tests (tests/uiapp.h) drive the same UI. Throws std::runtime_error if this machine can't show it
	// (ui::GpuUnavailable without an SDL_GPU device) or the fonts and themes in the resources can't be loaded.
	//
	// The UI's Lua (#208) runs on the UI thread, trusted: RmlUi's Lua plugin runs documents' scripts and event
	// handlers on it, with GNUbase's API (scripting/runtime.h) and the UI tools as gnubase.ui (app/uitools.h). Its
	// gnubase.on handlers get the engine's events in the shell's steps; print writes to output.
	class AppUi
	{
	public:
		AppUi(engine::Engine& engine, const std::filesystem::path& resources, const ui::WindowSpec& main,
			std::ostream& output, int msaaSamples = ui::g_defaultMsaaSamples);
		AppUi(const AppUi&) = delete;
		AppUi& operator=(const AppUi&) = delete;
		AppUi(AppUi&&) = delete;
		AppUi& operator=(AppUi&&) = delete;
		~AppUi() = default;

		// Shows the project window in the main window (resources/ui/project.rml), a docking window with the main menu
		// bar (ui/mainmenu.h) and the track list beside the event display in its main zone, whose commands are key
		// commands in the focus zone "project". Throws std::runtime_error if the document can't be loaded.
		// Also displays the project markers and status line.
		ui::EventDisplay& showProject();
		// The project window's track list, null before showProject.
		ui::TrackList* trackList() { return m_trackList.get(); }

		// Opens the markers window in a window of its own (resources/ui/markers.rml), hidden like the main window, or
		// closes it if it is open: the UI command window.markers, Mod+M. Returns it, or null when it closed it.
		// Throws std::runtime_error if the document can't be loaded.
		ui::MarkersWindow* toggleMarkers();
		// The markers window while it is open, or null.
		ui::MarkersWindow* markers() { return m_shell.find(m_markersWindow) != nullptr ? m_markers.get() : nullptr; }
		ui::WindowId markersWindow() const { return m_markersWindow; }
		// Opens the on-screen keyboard (#103, docs/features/onscreen-keyboard.md) in a window of its own
		// (resources/ui/onscreen-keyboard.rml), hidden like the main window, or closes it if it is open: the UI
		// command window.onScreenKeyboard, Alt+K, which the Studio menu runs. While it is open, it takes the keys
		// that no element used before the key commands. Returns it, or null when it closed it. Throws
		// std::runtime_error if the document can't be loaded.
		ui::OnScreenKeyboard* toggleKeyboard();
		// The on-screen keyboard while it is open, or null.
		ui::OnScreenKeyboard* keyboard()
		{
			return m_shell.find(m_keyboardWindow) != nullptr ? m_keyboard.get() : nullptr;
		}
		ui::WindowId keyboardWindow() const { return m_keyboardWindow; }
		// The markers in the project window's event display, null before showProject.
		ui::MarkerLines* markerLines() { return m_markerLines.get(); }

		// Opens the tempo track editor in a window of its own (resources/ui/tempo-editor.rml), hidden like the main
		// window, or closes it if it is open: the UI command window.tempoTrack, Mod+T (reference manual p. 1447).
		// Returns it, or null when it closed it. Throws std::runtime_error if the document can't be loaded.
		ui::TempoEditor* toggleTempoEditor();
		ui::TempoEditor* tempoEditor()
		{
			return m_shell.find(m_tempoWindow) != nullptr ? m_tempoEditor.get() : nullptr;
		}
		ui::WindowId tempoWindow() const { return m_tempoWindow; }
		// Opens the arranger editor in a window of its own (resources/ui/arranger-editor.rml,
		// docs/features/arranger.md), or closes it if it is open: the UI command window.arranger. Its script follows
		// the engine on its own. Throws std::runtime_error if the document can't be loaded.
		void toggleArranger();
		ui::WindowId arrangerWindow() const { return m_arrangerWindow; }
		// The tempo curve on the tempo track's lane in the project window, null before showProject.
		ui::TempoLane* tempoLane() { return m_tempoLane.get(); }
		// The time signature events on the signature track's lane in the project window, null before showProject.
		ui::SignatureLane* signatureLane() { return m_signatureLane.get(); }

		// The mixers (#106, docs/features/mixer-window.md): mixer window number 1 to 4 opens in a window of its own
		// (resources/ui/mixer.rml), hidden like the main window, or closes if it is open: the UI commands window.mixer
		// (F3), window.mixer2, window.mixer3 and window.mixer4. Returns it, or null when it closed it. Throws
		// std::runtime_error if the document can't be loaded. mixer is the mixer of an open window, or null;
		// lowerZoneMixer the project window's, while the docking system shows its panel somewhere.
		ui::Mixer* toggleMixer(int number);
		ui::Mixer* mixer(int number);
		ui::WindowId mixerWindow(int number) const { return m_mixers.at(static_cast<size_t>(number - 1)).window; }
		ui::Mixer* lowerZoneMixer() { return m_lowerMixer.lock().get(); }
		// The channel settings window (#112, docs/features/channel-settings.md, resources/ui/channel-settings.rml):
		// Edit Channel Settings opens it for a track's channel, or shows that channel in it while it is open, and
		// selects the track; while it follows, it shows the first selected track's channel. The UI command
		// window.channelSettings opens it for the first selected track. Throws std::runtime_error if the document
		// can't be loaded. channelSettings is its mixer while it is open, or null.
		ui::Mixer* openChannelSettings(uint64_t track);
		ui::Mixer* channelSettings()
		{
			return m_shell.find(m_channelSettings.window) != nullptr ? m_channelSettings.mixer.get() : nullptr;
		}
		ui::WindowId channelSettingsWindow() const { return m_channelSettings.window; }
		// The MIDI editors (#126, docs/features/midi-editors.md): the editor window (resources/ui/editor.rml), which
		// opens and closes as the engine's editor view window does (editor.open, editor.close), and the project
		// window's Editor panel in the lower zone, which shows as the view lower opens. Null while they don't show.
		ui::MidiEditor* editorWindow()
		{
			return m_shell.find(m_editor.window) != nullptr ? m_editor.editor.get() : nullptr;
		}
		ui::WindowId editorWindowId() const { return m_editor.window; }
		ui::MidiEditor* lowerZoneEditor() { return m_lowerEditor.lock().get(); }
		// The sample editor window (#140, docs/features/sample-editor.md, resources/ui/sample-editor.rml), which opens
		// and closes as the engine's sampleEditor view does. Null while it doesn't show.
		ui::SampleEditor* sampleEditorWindow()
		{
			return m_shell.find(m_sampleEditor.window) != nullptr ? m_sampleEditor.editor.get() : nullptr;
		}
		ui::WindowId sampleEditorWindowId() const { return m_sampleEditor.window; }

		// Opens the studio setup dialog (resources/ui/studio-setup.rml, #79) in the window, modal: the UI command
		// window.studioSetup, which the main menu's Studio menu runs. While it is open, this brings it to the front
		// instead: MCP and Lua can run the command then. Throws std::runtime_error if the document can't be loaded.
		Rml::ElementDocument& openStudioSetup(ui::Window& window);
		// Opens the project setup dialog (resources/ui/project-setup.rml, #78) the same way: the UI command
		// window.projectSetup, which Project > Project Setup and a click on the status line's record format run.
		Rml::ElementDocument& openProjectSetup(ui::Window& window);

		// Opens the start screen in the main window (ui/projectfiles.h, #77), as the app does when it starts without
		// a project.
		void openStartScreen() { m_files.openStartScreen(m_main); }

		// The transport controls (#90, docs/features/transport-bar.md): the project window's transport bar, once it
		// shows, and the transport panel and the time display window, windows of their own that the UI commands
		// window.transportPanel (F2) and window.timeDisplay show and hide. Null or none while they don't show.
		ui::TransportControls* transportBar() { return m_transportBar.get(); }
		void toggleTransportPanel();
		void toggleTimeDisplay();
		ui::Window* transportPanel() { return m_transportPanel.shown(m_shell); }
		ui::Window* timeDisplay() { return m_timeDisplay.shown(m_shell); }

		ui::Shell& shell() { return m_shell; }
		ui::Window& mainWindow() { return m_main; }
		ui::RmlUi& rmlui() { return m_rmlui; }
		scripting::Runtime& lua() { return m_lua; }
		ui::KeyCommands& keyCommands() { return m_keys; }
		ui::Docking& docking() { return m_docking; }
		const ui::GpuRenderer& renderer() const { return m_renderer; }

	private:
		// A window of its own with transport controls.
		struct TransportWindow
		{
			ui::WindowId window = 0;
			std::unique_ptr<ui::TransportControls> controls;

			// The window, or null if it is closed, also when the user closed it.
			ui::Window* shown(ui::Shell& shell) const { return window == 0 ? nullptr : shell.find(window); }
		};

		// Fills an element with the transport controls (resources/ui/transport.rml) and drives them.
		std::unique_ptr<ui::TransportControls> transportControls(Rml::Element& root, ui::Window& window, bool fill);
		void toggle(TransportWindow& shown, const char* file, const char* title, ui::Size size);
		// Edits a display of the transport bar, or of the transport panel, which opens if the bar doesn't show.
		void enter(const std::string& field);
		// File > Import: the OS's file dialog for the window, then the event display imports what the user chose;
		// with clipsOnly into the pool only (Media > Import Medium).
		void chooseImport(
			ui::Window& window, std::string title, std::vector<ui::FileFilter> filters, bool many, bool clipsOnly);
		// Media > Find Missing Files: the OS's folder dialog, then media.locate looks for the missing files there.
		void chooseMissingFolder(ui::Window& window);
		// The lower zone's mixer panel, the mixer windows' Visibility tabs and their UI commands, and Edit Channel
		// Settings.
		void addMixers();
		// What the mixers ask of the app: pictures, Edit Channel Settings, Add Effect Track to Send, and the channel
		// settings window going to another track, which the track list then selects.
		ui::MixerActions mixerActions();
		// The editors' panels and UI commands, and the windows and zones that follow the engine's editor views.
		void addEditors();
		// Shows or closes the editor window or the lower zone's editor as the engine's view says.
		void followEditor(const engine::Json& view);
		// The selected MIDI parts, or else audio parts, open in the editors that follow the project window's
		// selection (p. 1153).
		void followSelection();
		// What of a kind in project.get (midiParts, audioParts, audioEvents) is selected in the project window's event
		// display.
		std::vector<uint64_t> selected(const char* kind) const;
		// The parts an editor opens: the selected MIDI parts, or else the selected audio parts.
		std::vector<uint64_t> selectedParts() const;
		// Shows or closes the sample editor window as the engine's sampleEditor view says.
		void followSampleEditor(const engine::Json& view);
		// Quantizing (#132, docs/features/quantize.md, appquantize.cpp): the quantize panel, the Edit menu's quantize
		// commands and what they quantize.
		void addQuantize();
		// The commands that quantize what a command run in the window and zone takes: the notes of an editor, by part,
		// or all notes of each part (none), and the editor's length quantize (empty for the project window).
		struct QuantizeTarget
		{
			std::vector<std::pair<uint64_t, std::vector<uint64_t>>> parts;
			std::string lengthQuantize;
			double insertVelocity = 100.0 / 127.0; // the editor's, for Fixed Velocity
		};
		QuantizeTarget quantizeTarget(ui::Window& window, Rml::Element* zone) const;
		void quantize(const std::string& command, const std::string& step, const QuantizeTarget& target);
		// The MIDI menu (#133, docs/features/midi-functions.md, appmidifunctions.cpp): MIDI > Functions on what
		// quantizeTarget gives, and the dialogs of Transpose Setup, Merge MIDI in Loop, Dissolve Part and Restrict
		// Polyphony (resources/ui/midi-function.rml).
		void addMidiFunctions();
		// The OS's file dialog for a track's picture in the mixer, then track.set.
		void choosePicture(uint64_t track);
		// The drum map of the active track of an editor (window, else lower), then the OS's file dialog to save it in
		// and drumMap.export.
		void saveDrumMap(ui::WindowId parent, const std::string& editor = "window");
		void chooseDrumMapFile(ui::WindowId parent, uint64_t map);
		// Opens resources/ui/<name> in the window, modal. Throws std::runtime_error if it can't be loaded.
		// attributes go on the document before it shows, for its Lua.
		Rml::ElementDocument& openDialog(ui::Window& window, const char* name,
			const std::vector<std::pair<std::string, std::string>>& attributes = {}) const;
		// The Auto Fades dialog (resources/ui/auto-fades.rml) for the project, or for a track (not 0).
		void openAutoFades(ui::Window& window, uint64_t track) const;
		// openDialog for a dialog whose Lua keeps its state in a global table, so that there is only one: while open
		// is open, brings it to the front instead.
		Rml::ElementDocument& openOnce(
			ui::Window& window, const char* name, Rml::ObserverPtr<Rml::Element>& open) const;

		// Made in this order and destroyed in the reverse: RmlUi releases its resources through the renderer and
		// its Lua plugin's in Lua, the shell's windows need all of them, the settings and the binding tell the shell,
		// key commands send through the binding, and the docking system follows the binding and opens windows.
		ui::SdlWindowSystem m_windows;
		ui::GpuRenderer m_renderer;
		scripting::Runtime m_lua;
		ui::RmlUi m_rmlui;
		ui::Themes m_themes;
		ui::Shell m_shell;
		ui::UiSettings m_settings;
		ui::EngineBinding m_binding;
		ui::KeyCommands m_keys;
		ui::Window& m_main;
		ui::Docking m_docking;
		engine::Engine& m_engine;
		std::filesystem::path m_resources;
		bool m_hidden = false; // the main window, and so the others
		std::unique_ptr<ui::MarkersWindow> m_markers;
		ui::WindowId m_markersWindow = 0;
		ui::OnScreenKeyboard::Settings m_keyboardSettings; // kept while its window is closed
		std::unique_ptr<ui::OnScreenKeyboard> m_keyboard;
		ui::WindowId m_keyboardWindow = 0;
		std::unique_ptr<ui::EventDisplay> m_eventDisplay;   // goes before the binding
		std::unique_ptr<ui::MarkerLines> m_markerLines;     // and before the display
		std::unique_ptr<ui::TempoLane> m_tempoLane;         // as well
		std::unique_ptr<ui::SignatureLane> m_signatureLane; // as well
		std::unique_ptr<ui::TempoEditor> m_tempoEditor;
		ui::WindowId m_tempoWindow = 0;
		ui::WindowId m_arrangerWindow = 0;
		std::vector<uint64_t> m_eventSelection; // the display's, as the last step saw it
		ui::Winding m_winding{m_binding};       // Rewind and Forward held, which the transport controls end
		std::unique_ptr<ui::TransportControls> m_transportBar;
		TransportWindow m_transportPanel;
		TransportWindow m_timeDisplay;
		std::unique_ptr<ui::StatusLine> m_statusLine;         // too
		std::unique_ptr<ui::TrackList> m_trackList;           // and before the display
		std::unique_ptr<ui::ColorMenu> m_colorMenu;           // before the track list
		std::unique_ptr<ui::InfoLine> m_infoLine;             // and this too
		std::unique_ptr<ui::QuantizePanel> m_toolbarQuantize; // the toolbar's quantize section
		// What the undo history can undo and redo now, for the Edit menu's Undo and Redo.
		bool m_canUndo = false;
		bool m_canRedo = false;
		ui::Subscription m_history;
		ui::ProjectFiles m_files;                          // the File menu's project commands
		std::unique_ptr<ui::MainMenu> m_menu;              // the project window's
		Rml::ObserverPtr<Rml::Element> m_studioSetup;      // the open studio setup dialog's document
		Rml::ObserverPtr<Rml::Element> m_projectSetup;     // the open project setup dialog's document
		std::weak_ptr<ui::PoolPanel> m_pool;               // the docking system owns it while it is there
		std::weak_ptr<ui::Inspector> m_inspector;          // so too
		std::vector<ui::Subscription> m_poolResults;       // what the pool commands did, for the pool
		std::vector<ui::Subscription> m_lockRecord;        // stops refused while recording is locked
		Rml::ObserverPtr<Rml::Element> m_lockRecordDialog; // the open lock record dialog's document
		// A mixer window: open while the shell has it; the app lets go of the mixer once the user closed the window.
		struct MixerWindow
		{
			ui::WindowId window = 0;
			std::unique_ptr<ui::Mixer> mixer;
		};
		std::array<MixerWindow, 4> m_mixers;
		MixerWindow m_channelSettings;         // the channel settings window
		uint64_t m_settingsTrack = 0;          // the track it last went to or followed
		std::weak_ptr<ui::Mixer> m_lowerMixer; // the docking system owns it while it is there
		struct EditorWindow
		{
			ui::WindowId window = 0;
			std::unique_ptr<ui::MidiEditor> editor;
		};
		EditorWindow m_editor;
		struct SampleEditorWindow
		{
			ui::WindowId window = 0;
			std::unique_ptr<ui::SampleEditor> editor;
		};
		SampleEditorWindow m_sampleEditor;
		std::weak_ptr<ui::MidiEditor> m_lowerEditor;         // the docking system owns it while it is there
		engine::Json m_editorViews = engine::Json::object(); // the engine's, by editor
		std::vector<uint64_t> m_followed;                    // the selection the editors last followed
		std::vector<ui::Subscription> m_editorSubscriptions;
	};
}
