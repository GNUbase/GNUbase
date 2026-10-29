// SPDX-License-Identifier: GPL-3.0-or-later
// The one SDL_image function RmlUi's SDL_GPU renderer uses, implemented with stb_image in image.cpp.
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
