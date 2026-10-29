// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/sdlwindowsystem.h"

#include <RmlUi_Platform_SDL.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace gnubase::ui
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::runtime_error sdlError(const std::string& what)
		{
			return std::runtime_error(what + ": " + SDL_GetError());
		}

		Rect toRect(const SDL_Rect& rect)
		{
			return {rect.x, rect.y, rect.w, rect.h};
		}

		bool isInput(Uint32 type)
		{
			switch (type)
			{
			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
			case SDL_EVENT_MOUSE_WHEEL:
			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
			case SDL_EVENT_TEXT_INPUT:
			case SDL_EVENT_FINGER_DOWN:
			case SDL_EVENT_FINGER_UP:
			case SDL_EVENT_FINGER_MOTION:
			case SDL_EVENT_WINDOW_MOUSE_LEAVE:
				return true;
			default:
				return false;
			}
		}

		// What an SDL event means to the shell; empty for events it doesn't care about.
		std::optional<WindowEventKind> kindOf(Uint32 type)
		{
			switch (type)
			{
			case SDL_EVENT_QUIT:
				return WindowEventKind::Quit;
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				return WindowEventKind::CloseRequested;
			case SDL_EVENT_WINDOW_MOVED:
				return WindowEventKind::Moved;
			case SDL_EVENT_WINDOW_RESIZED:
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				return WindowEventKind::Resized;
			case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
			case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
				return WindowEventKind::ScaleChanged;
			case SDL_EVENT_WINDOW_SHOWN:
			case SDL_EVENT_WINDOW_EXPOSED:
			case SDL_EVENT_WINDOW_RESTORED:
			case SDL_EVENT_WINDOW_MAXIMIZED:
				return WindowEventKind::Exposed;
			case SDL_EVENT_DISPLAY_ADDED:
			case SDL_EVENT_DISPLAY_REMOVED:
			case SDL_EVENT_DISPLAY_MOVED:
			case SDL_EVENT_DISPLAY_ORIENTATION:
			case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
			case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
			case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
				return WindowEventKind::DisplaysChanged;
			default:
				return std::nullopt;
			}
		}
	}

	SdlWindowSystem::SdlWindowSystem()
	{
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
			throw sdlError("SDL cannot start its video subsystem");
	}

	SdlWindowSystem::~SdlWindowSystem()
	{
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
	}

	const char* SdlWindowSystem::driver()
	{
		const char* name = SDL_GetCurrentVideoDriver();
		return name != nullptr ? name : "";
	}

	std::vector<Display> SdlWindowSystem::displays() const
	{
		std::vector<Display> result;
		int count = 0;
		SDL_DisplayID* ids = SDL_GetDisplays(&count);
		if (ids == nullptr)
			return result;
		const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
		for (int i = 0; i < count; ++i)
		{
			Display display;
			display.id = ids[i];
			const char* name = SDL_GetDisplayName(ids[i]);
			display.name = name != nullptr ? name : "";
			SDL_Rect rect;
			if (SDL_GetDisplayBounds(ids[i], &rect))
				display.bounds = toRect(rect);
			display.usable = SDL_GetDisplayUsableBounds(ids[i], &rect) ? toRect(rect) : display.bounds;
			const float scale = SDL_GetDisplayContentScale(ids[i]);
			display.scale = scale > 0.0F ? scale : 1.0F;
			result.push_back(display);
		}
		SDL_free(ids);
		std::stable_partition(result.begin(), result.end(), [primary](const Display& d) { return d.id == primary; });
		return result;
	}

	WindowId SdlWindowSystem::createWindow(const WindowSpec& spec)
	{
		const Rect bounds = spec.bounds.value_or(Rect{0, 0, spec.size.width, spec.size.height});
		const SDL_PropertiesID properties = SDL_CreateProperties();
		SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, spec.title.c_str());
		if (spec.bounds)
		{
			SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X_NUMBER, bounds.x);
			SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER, bounds.y);
		}
		SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, bounds.width);
		SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, bounds.height);
		SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
		SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
		SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, spec.hidden);
		SDL_Window* window = SDL_CreateWindowWithProperties(properties);
		SDL_DestroyProperties(properties);
		if (window == nullptr)
			throw sdlError("SDL cannot open a window");
		return SDL_GetWindowID(window);
	}

	void SdlWindowSystem::destroyWindow(WindowId window)
	{
		m_inputTargets.erase(window);
		SDL_DestroyWindow(sdlWindow(window));
	}

	SDL_Window* SdlWindowSystem::sdlWindow(WindowId window)
	{
		return SDL_GetWindowFromID(window);
	}

	Rect SdlWindowSystem::bounds(WindowId window) const
	{
		Rect result;
		SDL_Window* sdl = sdlWindow(window);
		SDL_GetWindowPosition(sdl, &result.x, &result.y);
		SDL_GetWindowSize(sdl, &result.width, &result.height);
		return result;
	}

	void SdlWindowSystem::setBounds(WindowId window, const Rect& bounds)
	{
		SDL_Window* sdl = sdlWindow(window);
		SDL_SetWindowPosition(sdl, bounds.x, bounds.y);
		SDL_SetWindowSize(sdl, bounds.width, bounds.height);
	}

	Size SdlWindowSystem::pixelSize(WindowId window) const
	{
		Size result;
		SDL_GetWindowSizeInPixels(sdlWindow(window), &result.width, &result.height);
		return result;
	}

	float SdlWindowSystem::scale(WindowId window) const
	{
		const float scale = SDL_GetWindowDisplayScale(sdlWindow(window));
		return scale > 0.0F ? scale : 1.0F;
	}

	void SdlWindowSystem::setInputTarget(WindowId window, Rml::Context* context)
	{
		if (context != nullptr)
			m_inputTargets[window] = context;
		else
			m_inputTargets.erase(window);
	}

	std::optional<WindowEvent> SdlWindowSystem::nextEvent(std::optional<std::chrono::milliseconds> timeout)
	{
		const Clock::time_point deadline = timeout ? Clock::now() + *timeout : Clock::time_point::max();
		for (;;)
		{
			Sint32 wait = -1;
			if (timeout)
			{
				const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
				wait = static_cast<Sint32>(std::max<std::chrono::milliseconds::rep>(left.count(), 0));
			}
			SDL_Event event;
			if (!SDL_WaitEventTimeout(&event, wait))
				return std::nullopt;

			SDL_Window* window = SDL_GetWindowFromEvent(&event);
			const WindowId id = window != nullptr ? SDL_GetWindowID(window) : 0;
			if (isInput(event.type))
			{
				const auto target = m_inputTargets.find(id);
				if (target == m_inputTargets.end())
					continue;
				RmlSDL::InputEventHandler(target->second, window, event);
				return WindowEvent{WindowEventKind::Input, id};
			}
			if (const std::optional<WindowEventKind> kind = kindOf(event.type))
				return WindowEvent{*kind, id};
		}
	}
}
