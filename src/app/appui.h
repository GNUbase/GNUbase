// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/gpurenderer.h"
#include "ui/keycommands.h"
#include "ui/rmlui.h"
#include "ui/sdlwindowsystem.h"
#include "ui/shell.h"
#include "ui/themes.h"
#include "ui/uisettings.h"

#include <filesystem>
#include <memory>

namespace gnubase::app
{
	// GNUbase's UI for an engine: SDL3 windows drawn through SDL_GPU, RmlUi with GNUbase's fonts, themes and widgets,
	// and the shell, which follows the user settings and the engine's events and runs key commands, with the main
	// window open. runWindowed
	// runs it; UI tests (tests/uiapp.h) drive the same UI. Throws std::runtime_error if this machine can't show it
	// (ui::GpuUnavailable without an SDL_GPU device) or the fonts and themes in the resources can't be loaded.
	class AppUi
	{
	public:
		AppUi(engine::Engine& engine, const std::filesystem::path& resources, const ui::WindowSpec& main,
			int msaaSamples = ui::g_defaultMsaaSamples);
		AppUi(const AppUi&) = delete;
		AppUi& operator=(const AppUi&) = delete;
		AppUi(AppUi&&) = delete;
		AppUi& operator=(AppUi&&) = delete;
		~AppUi() = default;

		// Shows the project window in the main window (resources/ui/project.rml) with the event display. Throws
		// std::runtime_error if the document can't be loaded.
		ui::EventDisplay& showProject();

		ui::Shell& shell() { return m_shell; }
		ui::Window& mainWindow() { return m_main; }
		ui::RmlUi& rmlui() { return m_rmlui; }
		ui::KeyCommands& keyCommands() { return m_keys; }
		const ui::GpuRenderer& renderer() const { return m_renderer; }

	private:
		// Made in this order and destroyed in the reverse: RmlUi releases its resources through the renderer, the
		// shell's windows need all three, the settings and the binding tell the shell, and key commands send through
		// the binding.
		ui::SdlWindowSystem m_windows;
		ui::GpuRenderer m_renderer;
		ui::RmlUi m_rmlui;
		ui::Themes m_themes;
		ui::Shell m_shell;
		ui::UiSettings m_settings;
		ui::EngineBinding m_binding;
		ui::KeyCommands m_keys;
		ui::Window& m_main;
		engine::Engine& m_engine;
		std::filesystem::path m_resources;
		std::unique_ptr<ui::EventDisplay> m_eventDisplay; // goes before the binding
	};
}
