// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace gnubase::spike
{
	// An 8-bit RGBA image, top row first.
	struct Image
	{
		int width = 0;
		int height = 0;
		std::vector<unsigned char> pixels;
	};

	// Writes the image as PNG; false if it can't.
	bool writePng(const Image& image, const std::string& path);
}
