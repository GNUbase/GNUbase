// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/windowsystem.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

struct SDL_Cursor;
struct SDL_Window;
union SDL_Event;

namespace gnubase::ui
{
	class SdlTextInput;

	// The WindowSystem of the app: SDL3's video subsystem, started while the object lives. Like SDL's video, only
	// for the main thread. Throws std::runtime_error if SDL's video can't start.
	class SdlWindowSystem final : public WindowSystem
	{
	public:
		SdlWindowSystem();
		~SdlWindowSystem() override;
		SdlWindowSystem(const SdlWindowSystem&) = delete;
		SdlWindowSystem& operator=(const SdlWindowSystem&) = delete;
		SdlWindowSystem(SdlWindowSystem&&) = delete;
		SdlWindowSystem& operator=(SdlWindowSystem&&) = delete;

		std::vector<Display> displays() const override;
		WindowId createWindow(const WindowSpec& spec) override;
		void destroyWindow(WindowId window) override;
		Rect bounds(WindowId window) const override;
		void setBounds(WindowId window, const Rect& bounds) override;
		void raise(WindowId window) override;
		void minimize(WindowId window) override;
		void restore(WindowId window) override;
		bool maximized(WindowId window) const override;
		void setMaximized(WindowId window, bool maximized) override;
		Size pixelSize(WindowId window) const override;
		float scale(WindowId window) const override;
		void setInputTarget(WindowId window, Rml::Context* context) override;
		Rml::TextInputHandler* textInputHandler(WindowId window) override;
		std::string clipboardText() const override;
		void setClipboardText(const std::string& text) override;
		void setCursor(const Cursor& cursor) override;
		void showFileDialog(uint64_t id, const FileDialog& dialog) override;
		std::optional<WindowEvent> nextEvent(std::optional<std::chrono::milliseconds> timeout) override;
		void wake() override;

		// The SDL window, for the renderer; null if there is none.
		static SDL_Window* sdlWindow(WindowId window);

		// SDL's video driver, e.g. "x11", "wayland", "windows", "cocoa" or "offscreen".
		static const char* driver();

		// Shows one of the OS's message boxes with the buttons, the first the default for Return and Escape, and waits
		// for the user; works before any window opens, e.g. for the safe start mode (#163). The index of the button
		// chosen, or -1 if it couldn't be shown. A warning shows the OS's warning icon.
		enum class Severity : uint8_t
		{
			Information,
			Warning,
		};
		static int ask(const std::string& title, const std::string& message, const std::vector<std::string>& buttons,
			Severity severity = Severity::Information);

	private:
		// Files being dropped on a window: SDL reports one event per file, then the drop's end.
		struct Drop
		{
			std::vector<std::filesystem::path> files;
			Point position; // in the window's pixels
		};

		std::optional<WindowEvent> translate(SDL_Event& event);

		struct DestroyCursor
		{
			void operator()(SDL_Cursor* cursor) const;
		};

		std::map<WindowId, Rml::Context*> m_inputTargets;
		std::map<WindowId, std::unique_ptr<SdlTextInput>> m_textInputs;
		std::map<WindowId, Drop> m_drops;
		std::map<std::string, std::unique_ptr<SDL_Cursor, DestroyCursor>> m_cursors; // made once per name; null: arrow
		uint32_t m_dialogEvent = 0;      // the SDL event type that brings file dialog results to the main thread
		uint32_t m_wakeEvent = 0;        // the SDL event type that wake pushes
		WindowId m_keyboardGained = 0;   // the window that just got the keyboard, until an event other than a key-down
		uint64_t m_keyboardGainedAt = 0; // when, in SDL's nanoseconds
		WindowId m_altHeldOnGaining = 0; // the window whose Alt was held as it got the keyboard, until Alt is released
		std::set<WindowId> m_leftHeld;   // the windows whose left mouse button went down and not up yet
	};
}
