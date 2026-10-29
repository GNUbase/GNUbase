// SPDX-License-Identifier: GPL-3.0-or-later
#include "image.h"

#include <SDL3_image/SDL_image.h>

#include <stb_image.h>
#include <stb_image_write.h>

#include <cstddef>
#include <vector>

namespace gnubase::spike
{
	bool writePng(const Image& image, const std::string& path)
	{
		return stbi_write_png(path.c_str(), image.width, image.height, 4, image.pixels.data(), image.width * 4) != 0;
	}
}

extern "C" SDL_Surface* IMG_LoadTyped_IO(SDL_IOStream* source, bool closeio, const char* /*type*/)
{
	size_t size = 0;
	void* data = SDL_LoadFile_IO(source, &size, closeio);
	if (data == nullptr)
		return nullptr;
	int width = 0;
	int height = 0;
	int channels = 0;
	unsigned char* pixels = stbi_load_from_memory(
		static_cast<const unsigned char*>(data), static_cast<int>(size), &width, &height, &channels, 4);
	SDL_free(data);
	if (pixels == nullptr)
		return nullptr;
	SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
	if (surface != nullptr)
	{
		for (int row = 0; row < height; ++row)
			SDL_memcpy(static_cast<unsigned char*>(surface->pixels) + (static_cast<ptrdiff_t>(row) * surface->pitch),
				pixels + (static_cast<ptrdiff_t>(row) * width * 4), static_cast<size_t>(width) * 4);
	}
	stbi_image_free(pixels);
	return surface;
}
