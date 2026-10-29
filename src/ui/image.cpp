// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/renderer.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>

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

	std::optional<Image> readPng(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return std::nullopt;
		const std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
		Image image;
		int channels = 0;
		const std::unique_ptr<stbi_uc, void (*)(void*)> pixels(
			stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()), static_cast<int>(data.size()),
				&image.width, &image.height, &channels, 4),
			stbi_image_free);
		if (pixels == nullptr)
			return std::nullopt;
		image.pixels.assign(
			pixels.get(), pixels.get() + (static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4));
		return image;
	}
}
