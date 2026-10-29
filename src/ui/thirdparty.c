/* SPDX-License-Identifier: GPL-3.0-or-later */
/* stb_image and stb_image_write, and the one SDL_image function RmlUi's SDL_GPU renderer calls, compiled with the
   third-party code, outside GNUbase's warnings and clang-tidy. */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_write.h>

#include <SDL3_image/SDL_image.h>

/* Loads an image file into an RGBA surface for a texture. */
SDL_Surface* IMG_LoadTyped_IO(SDL_IOStream* source, bool closeio, const char* type)
{
	size_t size = 0;
	void* data = SDL_LoadFile_IO(source, &size, closeio);
	int width = 0;
	int height = 0;
	int channels = 0;
	unsigned char* pixels = NULL;
	SDL_Surface* surface = NULL;
	(void)type;
	if (data == NULL)
		return NULL;
	pixels = stbi_load_from_memory((const unsigned char*)data, (int)size, &width, &height, &channels, 4);
	SDL_free(data);
	if (pixels == NULL)
		return NULL;
	surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
	if (surface != NULL)
		for (int row = 0; row < height; ++row)
			SDL_memcpy((unsigned char*)surface->pixels + ((ptrdiff_t)row * surface->pitch),
				pixels + ((ptrdiff_t)row * width * 4), (size_t)width * 4);
	stbi_image_free(pixels);
	return surface;
}
