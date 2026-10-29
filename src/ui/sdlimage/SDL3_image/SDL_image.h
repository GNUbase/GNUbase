// SPDX-License-Identifier: GPL-3.0-or-later
// The one SDL_image function RmlUi's SDL_GPU renderer calls, to load image files for textures. GNUbase doesn't build
// SDL_image; ui/thirdparty.c implements this with stb_image. Icons and UI graphics are SVG and don't come this way
// (#210).
#pragma once

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C"
{
#endif

	SDL_Surface* IMG_LoadTyped_IO(SDL_IOStream* source, bool closeio, const char* type);

#ifdef __cplusplus
}
#endif
