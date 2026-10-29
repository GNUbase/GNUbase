// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/windowsystem.h"

#include <map>

struct SDL_Window;

namespace gnubase::ui
{
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
		std::optional<WindowEvent> nextEvent(std::optional<std::chrono::milliseconds> timeout) override;

		// The SDL window, for the renderer; null if there is none.
		static SDL_Window* sdlWindow(WindowId window);

		// SDL's video driver, e.g. "x11", "wayland", "windows", "cocoa" or "offscreen".
		static const char* driver();

	private:
		std::map<WindowId, Rml::Context*> m_inputTargets;
	};
}
