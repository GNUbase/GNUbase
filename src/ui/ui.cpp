// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/ui.h"

#include <RmlUi/Core.h>
#include <SDL3/SDL.h>

namespace gnubase::ui
{
	std::string libraryVersions()
	{
		const int sdl = SDL_GetVersion();
		return "SDL " + std::to_string(SDL_VERSIONNUM_MAJOR(sdl)) + "." + std::to_string(SDL_VERSIONNUM_MINOR(sdl)) + "." +
			std::to_string(SDL_VERSIONNUM_MICRO(sdl)) + ", RmlUi " + Rml::GetVersion();
	}
}
