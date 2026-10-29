// SPDX-License-Identifier: GPL-3.0-or-later
#include "backend.h"

namespace gnubase::spike
{
	Rml::Vector2i Backend::pixelSize() const
	{
		int width = 0;
		int height = 0;
		SDL_GetWindowSizeInPixels(window(), &width, &height);
		return {width, height};
	}

	SDL_Window* createWindow(const WindowOptions& options, SDL_WindowFlags flags, std::string& error)
	{
		flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
		if (!options.visible)
			flags |= SDL_WINDOW_HIDDEN;
		SDL_Window* window = SDL_CreateWindow(options.title.c_str(), options.width, options.height, flags);
		if (window == nullptr)
			error = std::string("cannot create a window: ") + SDL_GetError();
		return window;
	}
}
