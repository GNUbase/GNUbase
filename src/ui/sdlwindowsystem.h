// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/windowsystem.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

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
		Size pixelSize(WindowId window) const override;
		float scale(WindowId window) const override;
		void setInputTarget(WindowId window, Rml::Context* context) override;
		Rml::TextInputHandler* textInputHandler(WindowId window) override;
		std::string clipboardText() const override;
		void setClipboardText(const std::string& text) override;
		void showFileDialog(uint64_t id, const FileDialog& dialog) override;
		std::optional<WindowEvent> nextEvent(std::optional<std::chrono::milliseconds> timeout) override;

		// The SDL window, for the renderer; null if there is none.
		static SDL_Window* sdlWindow(WindowId window);

		// SDL's video driver, e.g. "x11", "wayland", "windows", "cocoa" or "offscreen".
		static const char* driver();

	private:
		// Files being dropped on a window: SDL reports one event per file, then the drop's end.
		struct Drop
		{
			std::vector<std::filesystem::path> files;
			Point position; // in the window's pixels
		};

		std::optional<WindowEvent> translate(SDL_Event& event);

		std::map<WindowId, Rml::Context*> m_inputTargets;
		std::map<WindowId, std::unique_ptr<SdlTextInput>> m_textInputs;
		std::map<WindowId, Drop> m_drops;
		uint32_t m_dialogEvent = 0; // the SDL event type that brings file dialog results to the main thread
	};
}
