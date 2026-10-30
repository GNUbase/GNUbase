// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/renderer.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <vector>

namespace gnubase::ui
{
	std::vector<uint8_t> encodePng(const Image& image)
	{
		std::vector<uint8_t> png;
		const auto write = [](void* context, void* data, int size)
		{
			const auto* bytes = static_cast<const uint8_t*>(data);
			static_cast<std::vector<uint8_t>*>(context)->insert(
				static_cast<std::vector<uint8_t>*>(context)->end(), bytes, bytes + size);
		};
		if (stbi_write_png_to_func(write, &png, image.width, image.height, 4, image.pixels.data(), image.width * 4) ==
			0)
			png.clear();
		return png;
	}

	std::optional<Image> decodePng(const std::vector<uint8_t>& png)
	{
		Image image;
		int channels = 0;
		const std::unique_ptr<stbi_uc, void (*)(void*)> pixels(
			stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &image.width, &image.height, &channels, 4),
			stbi_image_free);
		if (pixels == nullptr)
			return std::nullopt;
		image.pixels.assign(
			pixels.get(), pixels.get() + (static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4));
		return image;
	}

	bool writePng(const Image& image, const std::filesystem::path& file)
	{
		const std::vector<uint8_t> png = encodePng(image);
		std::ofstream out(file, std::ios::binary);
		out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
		return !png.empty() && out.flush().good();
	}

	std::optional<Image> readPng(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return std::nullopt;
		return decodePng({std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});
	}
}
