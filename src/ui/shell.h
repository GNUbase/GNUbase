// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/renderer.h"
#include "ui/windowsystem.h"

#include <RmlUi/Core/Types.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The application shell (#48): GNUbase's top-level windows, each with its own RmlUi context, and the event loop that
// draws them only when something changed. docs/architecture.md, "UI shell", has the reasoning.
namespace gnubase::ui
{
	class Shell;

	// About a frame at 60 Hz: how often the shell steps while a step task follows values that change without events.
	inline constexpr std::chrono::milliseconds g_framePeriod{16};

	class Window
	{
	public:
		WindowId id() const { return m_id; }
		bool main() const { return m_main; }
		Rml::Context& context() const { return *m_context; }

		// RmlUi's dp ratio for the window: its monitor's scale times the user's UI scale.
		float dpRatio() const { return m_dpRatio; }
		Size pixelSize() const { return m_pixelSize; }

		// Makes the shell update and draw the window in its next step, e.g. after a document changed. Elements and
		// data models can ask the same through RmlUi: Rml::Context::RequestNextUpdate(0).
		void requestRender() { m_renderRequested = true; }
		bool renderRequested() const { return m_renderRequested; }

		// How many frames the window has shown.
		uint64_t frames() const { return m_frames; }

	private:
		friend class Shell;

		WindowId m_id = 0;
		bool m_main = false;
		std::string m_contextName;
		Rml::Context* m_context = nullptr;
		float m_dpRatio = 1.0F;
		Size m_pixelSize;
		bool m_renderRequested = true;
		std::chrono::steady_clock::time_point m_nextUpdate = std::chrono::steady_clock::time_point::max();
		uint64_t m_frames = 0;
	};

	// What a file dialog returned: the files the user chose, none if they cancelled, or why it failed.
	struct FileDialogResult
	{
		std::vector<std::filesystem::path> files;
		std::string error;

		bool cancelled() const { return files.empty() && error.empty(); }
	};

	class Shell
	{
	public:
		// RmlUi must be initialised (RmlUi in ui/rmlui.h) for as long as the shell has windows.
		Shell(WindowSystem& windows, Renderer& renderer);
		~Shell();
		Shell(const Shell&) = delete;
		Shell& operator=(const Shell&) = delete;
		Shell(Shell&&) = delete;
		Shell& operator=(Shell&&) = delete;

		// Opens a window with an empty RmlUi context. The first window is the main window: closing it quits.
		// Without a size it takes 80 % of the primary monitor's usable area. Bounds on a monitor that isn't
		// connected are moved onto one. Throws std::runtime_error if the window can't be opened.
		Window& open(const WindowSpec& spec);
		void close(WindowId id);
		Window* find(WindowId id);
		std::vector<Window*> windows();

		// The user's UI scale (the user setting ui.scale, #53), on top of each monitor's scale.
		void setUiScale(float scale);
		float uiScale() const { return m_uiScale; }

		// The theme (ui/themes.h): its rules, inside "@media (theme: <name>)", apply in every window, also in windows
		// opened later. Documents restyle without loading again. Empty for none.
		void setTheme(const std::string& name);
		const std::string& theme() const { return m_theme; }

		// Runs task on the UI thread in the shell's next step, waking the shell if it waits for events. Safe to call
		// from any thread, e.g. from an engine listener (ui/uisettings.h).
		void post(std::function<void()> task);

		// Handles the events that arrive within timeout (all that are waiting, without timeout), then updates and
		// draws every window that needs it. Throws std::logic_error inside a step, e.g. from an event handler.
		void step(std::optional<std::chrono::milliseconds> timeout);

		// Steps until the shell quits, sleeping while no window needs drawing. Returns the exit code, 0.
		int run();

		// Closes every window and ends run(). Work posted but not run yet is dropped.
		void quit();
		bool running() const { return m_running; }

		// The window's content as it is now, drawn offscreen at its pixel size.
		std::optional<Image> screenshot(WindowId id);

		// Shows the OS's file dialog. done runs later on the UI thread, in a step, with what the user chose. Dialogs
		// still open when the shell is destroyed never call done.
		void showFileDialog(const FileDialog& dialog, std::function<void(const FileDialogResult&)> done);

		// Runs task in the steps after each interval has passed, e.g. to look for changed files.
		void every(std::chrono::milliseconds interval, std::function<void()> task);

		// Runs task in every step, after the periodic tasks and before drawing, e.g. to take what the engine sent
		// (ui/enginebinding.h). While it returns true, because it follows values that change without events such as
		// the playback position, the shell steps at least once a frame (g_framePeriod).
		void eachStep(std::function<bool()> task);

		// Runs handler for every key pressed in a window that none of its elements used, e.g. for key commands
		// (ui/keycommands.h). Elements that use a key stop its propagation.
		void onUnusedKey(std::function<void(Window&, const KeyPress&)> handler) { m_unusedKey = std::move(handler); }

		// Hands a key that no element of the window used to that handler. The shell calls it for the window system's
		// input, and ui::Automation for the keys it presses.
		void unusedKey(Window& window, const KeyPress& keys);

		// From any thread: makes the shell step soon, e.g. when the engine sent the UI something.
		void wake() { m_windows.wake(); }

		// How long run() may wait for events before a window needs drawing or a task is due; empty to wait for the
		// next event.
		std::optional<std::chrono::milliseconds> idleTimeout() const;

	private:
		using Clock = std::chrono::steady_clock;

		struct Task
		{
			std::chrono::milliseconds interval;
			std::function<void()> run;
			Clock::time_point next;
		};

		void handle(const WindowEvent& event);
		void refresh(Window& window);
		void draw(Window& window);
		// When the window needs updating next, also for updates its context requested since it was drawn, e.g. by an
		// element that changed (Rml::Context::RequestNextUpdate).
		static Clock::time_point nextUpdate(const Window& window, Clock::time_point now);

		WindowSystem& m_windows;
		Renderer& m_renderer;
		std::map<WindowId, std::unique_ptr<Window>> m_open;
		float m_uiScale = 1.0F;
		std::string m_theme;
		bool m_running = true;
		bool m_stepping = false;
		std::map<uint64_t, std::function<void(const FileDialogResult&)>> m_dialogs; // open file dialogs by id
		uint64_t m_nextDialog = 1;
		std::vector<Task> m_tasks;
		std::mutex m_postedMutex; // guards m_posted, which other threads add to
		std::vector<std::function<void()>> m_posted;
		std::vector<std::function<bool()>> m_stepTasks;
		Clock::time_point m_nextFrame = Clock::time_point::max(); // when a step task that follows values runs again
		std::function<void(Window&, const KeyPress&)> m_unusedKey;
	};
}
