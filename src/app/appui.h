// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "scripting/runtime.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/gpurenderer.h"
#include "ui/keycommands.h"
#include "ui/markers.h"
#include "ui/rmlui.h"
#include "ui/sdlwindowsystem.h"
#include "ui/shell.h"
#include "ui/themes.h"
#include "ui/uisettings.h"

#include <filesystem>
#include <iosfwd>
#include <memory>

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

		// Shows the project window in the main window (resources/ui/project.rml) with the event display and its
		// markers, whose commands are key commands in the focus zone "project". Throws std::runtime_error if the
		// document can't be loaded.
		ui::EventDisplay& showProject();

		// Opens the markers window in a window of its own (resources/ui/markers.rml), hidden like the main window, or
		// closes it if it is open: the UI command window.markers, Mod+M. Returns it, or null when it closed it.
		// Throws std::runtime_error if the document can't be loaded.
		ui::MarkersWindow* toggleMarkers();
		// The markers window while it is open, or null.
		ui::MarkersWindow* markers() { return m_shell.find(m_markersWindow) != nullptr ? m_markers.get() : nullptr; }
		ui::WindowId markersWindow() const { return m_markersWindow; }

		ui::Shell& shell() { return m_shell; }
		ui::Window& mainWindow() { return m_main; }
		ui::RmlUi& rmlui() { return m_rmlui; }
		scripting::Runtime& lua() { return m_lua; }
		ui::KeyCommands& keyCommands() { return m_keys; }
		const ui::GpuRenderer& renderer() const { return m_renderer; }

	private:
		// Made in this order and destroyed in the reverse: RmlUi releases its resources through the renderer and
		// its Lua plugin's in Lua, the shell's windows need all of them, the settings and the binding tell the shell,
		// and key commands send through the binding.
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
		engine::Engine& m_engine;
		std::filesystem::path m_resources;
		bool m_hidden = false;                            // the main window, and so the others
		std::unique_ptr<ui::EventDisplay> m_eventDisplay; // these go before the binding
		std::unique_ptr<ui::MarkerLines> m_markerLines;   // and before the display
		std::unique_ptr<ui::MarkersWindow> m_markers;
		ui::WindowId m_markersWindow = 0;
	};
}
