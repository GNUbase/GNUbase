// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/appui.h"
#include "engine/engine.h"
#include "platform/audio/device.h"
#include "platform/midi/port.h"
#include "testutils/goldenimage.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/gpurenderer.h"

#include <RmlUi/Core/ElementDocument.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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

	// Copies the shipped resources into folder/resources and replaces one string in a UI document there. A missing
	// string fails the test. The caller keeps folder alive while an app uses the returned resources path.
	std::filesystem::path patchedResources(const std::filesystem::path& folder, const std::string& document,
		const std::string& from, const std::string& to);

	// GNUbase's UI as the app shows it (app::AppUi), with the engine on the dummy device and the user settings in a
	// temporary folder: GNUbase's fonts and themes, the user settings and the engine binding, and the main window,
	// hidden and drawn with 4x MSAA, which every GPU has with the same sample positions. Tests drive it with
	// ui::Automation, as MCP's UI tools do. They run on the UI thread: waitUntil steps the shell as the app's event
	// loop does. Skips the test without an SDL_GPU device (gpuUnavailable).
	class UiApp
	{
	public:
		// The main window's size in desktop coordinates; in pixels at a monitor scale of 1. The engine takes MIDI and
		// audio from these backends, by default the system's; the audio backends must include the dummy device's.
		explicit UiApp(ui::Size size = {640, 480},
			std::vector<std::unique_ptr<midi::Backend>> midiBackends = midi::makeBackends(),
			std::vector<std::unique_ptr<audio::Backend>> audioBackends = audio::makeBackends());
		// The same with the fonts, themes and documents of another resources folder than GNUbase's, such as a copy
		// with a document changed.
		UiApp(ui::Size size, std::filesystem::path resources);

		engine::Engine& engine() { return *m_engine; }
		app::AppUi& appUi() { return *m_ui; }
		ui::Shell& shell() { return m_ui->shell(); }
		ui::Window& main() { return m_ui->mainWindow(); }
		ui::Automation& ui() { return *m_automation; }
		scripting::Runtime& lua() { return m_ui->lua(); }
		ui::KeyCommands& keys() { return m_ui->keyCommands(); }

		// Quits the UI and the engine and starts them again with the same user settings, as a user restarting GNUbase
		// would, with the main window as it was first opened and the resources folder it was started with. The engine
		// takes MIDI and audio from the system's backends then.
		void restart();

		// Runs an engine command that must succeed; returns its result.
		engine::Json call(const std::string& command, const engine::Json& parameters = engine::Json::object());

		// Loads a document from tests/data/ui into the main window and shows it.
		Rml::ElementDocument& show(const std::string& name);

		// Steps the shell until done() is true; false if it isn't within the timeout.
		// A JSON error from done() means "not ready yet"; if it still throws past the timeout, the error is rethrown.
		// Predicates may read UI state that hasn't arrived yet without a JSON type guard.
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
		void start(std::vector<std::unique_ptr<midi::Backend>> midiBackends,
			std::vector<std::unique_ptr<audio::Backend>> audioBackends);

		TempDir m_settings;
		ui::Size m_size;
		std::filesystem::path m_resources{GNUBASE_RESOURCES_DIR};
		std::unique_ptr<engine::Engine> m_engine;
		std::unique_ptr<app::AppUi> m_ui;
		std::unique_ptr<ui::Automation> m_automation;
	};

	// The instrument noteProbe on a track, rendered by hand: plays() hears the note ons and offs that reach the
	// track's instrument, such as those of a key of a key editor's keyboard. Sets the dummy device up and captures
	// MIDI.
	class NoteProbe
	{
	public:
		NoteProbe(UiApp& app, uint64_t track);

		// Renders until the note ons and offs heard since clear() are the expected ones (bytes of each message).
		bool plays(const std::vector<std::vector<int>>& expected);

		// Forgets what was heard.
		void clear() { m_played.clear(); }

	private:
		UiApp& m_app;
		std::vector<std::vector<int>> m_played;
	};

	// The value attribute of a value control (a fader, a knob, a value field) as a number.
	double controlValue(UiApp& app, std::optional<ui::WindowId> window, const std::string& selector);

	// Changes a value control step by step while the engine answers late (#716): after each step the engine has done
	// what the UI sent, and the UI hands the answers on only after the next step sent its commands, so that the
	// answers to earlier steps arrive during the gesture. Checks after every step that the control is where the step
	// put it, never back toward an earlier step: down (direction 1) is less, up (-1) more. Returns the control's
	// value.
	double lateSteps(UiApp& app, std::optional<ui::WindowId> window, const std::string& selector, int steps,
		int direction, const std::function<void()>& step);

	// A drag of a value control through its window's context, with lateSteps.
	class LateDrag
	{
	public:
		// Presses the left button on the control that the selector matches.
		LateDrag(UiApp& app, std::optional<ui::WindowId> window, std::string selector);

		// Moves the mouse by steps of two pixels, down (1) or up (-1), perAnswer steps for each time the engine
		// answers. Returns the control's value.
		double move(int steps, int direction, int perAnswer = 1);

		// Moves the mouse by one step of two pixels, down (1) or up (-1), without waiting for the engine, e.g. while
		// the test holds it.
		void nudge(int direction);

		// Releases the button. If stays, checks that the control then stays where the drag left it while the engine
		// answers late; it doesn't if another client set its value during the drag.
		void release(bool stays = true);

		double value() const { return controlValue(m_app, m_window, m_selector); }

	private:
		UiApp& m_app;
		std::optional<ui::WindowId> m_window;
		std::string m_selector;
		Rml::Context& m_context;
		ui::Point m_at;
		int m_moved = 0;
	};
}
