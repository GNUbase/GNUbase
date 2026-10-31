// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/appui.h"
#include "engine/engine.h"
#include "platform/midi/port.h"
#include "testutils/goldenimage.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/gpurenderer.h"

#include <RmlUi/Core/ElementDocument.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The harness for UI tests (#59), and what they share with tests/window_tests.cpp: the display and the GPU.
namespace gnubase::test
{
	// SDL's offscreen video driver on Linux, unless SDL_VIDEO_DRIVER names another, so UI tests need no display; CI's
	// runners have none. Call it before SDL's video starts.
	void useTestDisplay();

	// Skips the test because this machine has no SDL_GPU device, such as CI's macOS Intel runner, or fails it if
	// GNUBASE_REQUIRE_GPU=1 says this machine must have one.
	void gpuUnavailable(const ui::GpuUnavailable& error);

	// Sanitized Linux builds: LeakSanitizer can't see what an unloaded library still referenced, so once SDL_GPU's
	// Vulkan driver is unloaded, the memory Mesa keeps for the life of the process shows up as leaks. Call it once
	// SDL_GPU runs: it loads the drivers once more and never closes them. Does nothing elsewhere.
	void keepVulkanDriversLoaded();

	// GNUbase's UI as the app shows it (app::AppUi), with the engine on the dummy device and the user settings in a
	// temporary folder: GNUbase's fonts and themes, the user settings and the engine binding, and the main window,
	// hidden and drawn with 4x MSAA, which every GPU has with the same sample positions. Tests drive it with
	// ui::Automation, as MCP's UI tools do. They run on the UI thread: waitUntil steps the shell as the app's event
	// loop does. Skips the test without an SDL_GPU device (gpuUnavailable).
	class UiApp
	{
	public:
		// The main window's size in desktop coordinates; in pixels at a monitor scale of 1. The engine takes MIDI from
		// these backends, by default the system's.
		explicit UiApp(ui::Size size = {640, 480},
			std::vector<std::unique_ptr<midi::Backend>> midiBackends = midi::makeBackends());

		engine::Engine& engine() { return *m_engine; }
		app::AppUi& appUi() { return *m_ui; }
		ui::Shell& shell() { return m_ui->shell(); }
		ui::Window& main() { return m_ui->mainWindow(); }
		ui::Automation& ui() { return *m_automation; }
		scripting::Runtime& lua() { return m_ui->lua(); }
		ui::KeyCommands& keys() { return m_ui->keyCommands(); }

		// Quits the UI and the engine and starts them again with the same user settings, as a user restarting GNUbase
		// would, with the main window as it was first opened. The engine takes MIDI from the system's backends then.
		void restart();

		// Runs an engine command that must succeed; returns its result.
		engine::Json call(const std::string& command, const engine::Json& parameters = engine::Json::object());

		// Loads a document from tests/data/ui into the main window and shows it.
		Rml::ElementDocument& show(const std::string& name);

		// Steps the shell until done() is true; false if it isn't within the timeout.
		bool waitUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds(5));

		// Lays the windows out at this dp ratio, whatever the monitor's scale, so that screenshots have the same pixels
		// everywhere.
		void setDpRatio(float ratio);

		// The main window's top-left corner, dp in size, as it looks now; also written to the test output folder as
		// ui/<name>.png for a look when a check fails.
		ui::Image screenshot(const std::string& name, ui::Size dp);

		// Checks the image against tests/golden/ui/<name>.png, allowing what GPUs round differently: one pixel in 500
		// may differ by more than 8 of 255.
		static void checkGolden(const std::string& name, const ui::Image& image);

	private:
		void start(std::vector<std::unique_ptr<midi::Backend>> midiBackends);

		TempDir m_settings;
		ui::Size m_size;
		std::unique_ptr<engine::Engine> m_engine;
		std::unique_ptr<app::AppUi> m_ui;
		std::unique_ptr<ui::Automation> m_automation;
	};
}
