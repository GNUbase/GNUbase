// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/renderer.h"

#include <stb_image_write.h>

#include <fstream>

namespace gnubase::ui
{
	bool writePng(const Image& image, const std::filesystem::path& file)
	{
		std::ofstream out(file, std::ios::binary);
		if (!out)
			return false;
		const auto write = [](void* stream, void* data, int size)
		{ static_cast<std::ofstream*>(stream)->write(static_cast<const char*>(data), size); };
		const bool encoded = stbi_write_png_to_func(
								 write, &out, image.width, image.height, 4, image.pixels.data(), image.width * 4) != 0;
		return encoded && out.flush().good();
	}
}
