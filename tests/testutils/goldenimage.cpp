// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/goldenimage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

namespace gnubase::test
{
	namespace
	{
		std::string sizeOf(const ui::Image& image)
		{
			return std::to_string(image.width) + "x" + std::to_string(image.height);
		}

		// The largest difference of the pixel's channels.
		int difference(const ui::Image& actual, const ui::Image& expected, size_t pixel)
		{
			int largest = 0;
			for (size_t channel = pixel * 4; channel < (pixel + 1) * 4; ++channel)
				largest = std::max(largest,
					std::abs(static_cast<int>(actual.pixels[channel]) - static_cast<int>(expected.pixels[channel])));
			return largest;
		}

		// Each pixel averaged with its neighbours within the radius, as far as the image reaches.
		ui::Image average(const ui::Image& image, int radius)
		{
			if (radius <= 0)
				return image;
			ui::Image result{image.width, image.height, std::vector<uint8_t>(image.pixels.size())};
			for (int y = 0; y < image.height; ++y)
			{
				for (int x = 0; x < image.width; ++x)
				{
					std::array<int, 4> sum{};
					int count = 0;
					for (int j = std::max(0, y - radius); j <= std::min(image.height - 1, y + radius); ++j)
					{
						for (int i = std::max(0, x - radius); i <= std::min(image.width - 1, x + radius); ++i)
						{
							const std::array<uint8_t, 4> pixel = image.pixel(i, j);
							for (size_t channel = 0; channel < 4; ++channel)
								sum.at(channel) += pixel.at(channel);
							++count;
						}
					}
					const size_t at =
						((static_cast<size_t>(y) * static_cast<size_t>(image.width)) + static_cast<size_t>(x)) * 4;
					for (size_t channel = 0; channel < 4; ++channel)
						result.pixels[at + channel] = static_cast<uint8_t>((sum.at(channel) + (count / 2)) / count);
				}
			}
			return result;
		}

		// Write a PNG, creating the folder; the name may contain folders.
		std::filesystem::path save(const ui::Image& image, const std::filesystem::path& file)
		{
			std::filesystem::create_directories(file.parent_path());
			ui::writePng(image, file);
			return file;
		}
	}

	ImageDiff compareImages(const ui::Image& actual, const ui::Image& expected, const ImageTolerance& tolerance)
	{
		ImageDiff diff;
		if (actual.width != expected.width || actual.height != expected.height ||
			actual.pixels.size() != expected.pixels.size())
		{
			diff.equal = false;
			diff.report = "size " + sizeOf(actual) + " instead of " + sizeOf(expected) + ".";
			return diff;
		}
		const ui::Image averagedActual = average(actual, tolerance.averageRadius);
		const ui::Image averagedExpected = average(expected, tolerance.averageRadius);
		std::optional<size_t> first;
		const size_t pixels = actual.pixels.size() / 4;
		for (size_t pixel = 0; pixel < pixels; ++pixel)
		{
			const int different = difference(averagedActual, averagedExpected, pixel);
			diff.maxDifference = std::max(diff.maxDifference, different);
			if (different <= tolerance.channel)
				continue;
			++diff.differentPixels;
			if (!first)
				first = pixel;
		}
		if (diff.differentPixels <= tolerance.maxDifferentPixels)
			return diff;
		diff.equal = false;
		const auto width = static_cast<size_t>(actual.width);
		diff.report = std::to_string(diff.differentPixels) + " pixels differ by more than " +
			std::to_string(tolerance.channel) + " (" + std::to_string(tolerance.maxDifferentPixels) +
			" may), the first at (" + std::to_string(first.value_or(0) % width) + ", " +
			std::to_string(first.value_or(0) / width) + "); the largest difference is " +
			std::to_string(diff.maxDifference) + (tolerance.averageRadius > 0 ? " (averaged)." : ".");
		return diff;
	}

	ImageDiff compareWithGolden(const std::filesystem::path& goldenDir, const std::filesystem::path& outputDir,
		const std::string& name, const ui::Image& actual, const ImageTolerance& tolerance, bool update)
	{
		const std::filesystem::path golden = goldenDir / (name + ".png");
		const std::filesystem::path actualFile = outputDir / (name + ".actual.png");
		const std::string accept = " Run with GNUBASE_UPDATE_GOLDEN=1 to accept the actual image.";

		if (update)
		{
			save(actual, golden);
			return {};
		}

		const std::optional<ui::Image> expected = ui::readPng(golden);
		if (!expected)
		{
			ImageDiff diff;
			diff.equal = false;
			diff.report = "golden file " + golden.string() + " can't be read; actual image written to " +
				save(actual, actualFile).string() + "." + accept;
			return diff;
		}

		ImageDiff diff = compareImages(actual, *expected, tolerance);
		if (diff.equal)
			return diff;

		diff.report +=
			" Golden file: " + golden.string() + ". Actual image: " + save(actual, actualFile).string() + ".";
		if (actual.width == expected->width && actual.height == expected->height)
		{
			const ui::Image averagedActual = average(actual, tolerance.averageRadius);
			const ui::Image averagedExpected = average(*expected, tolerance.averageRadius);
			ui::Image marked = *expected;
			for (size_t pixel = 0; pixel < marked.pixels.size() / 4; ++pixel)
			{
				uint8_t* rgba = &marked.pixels[pixel * 4];
				if (difference(averagedActual, averagedExpected, pixel) > tolerance.channel)
				{
					rgba[0] = 255;
					rgba[1] = 0;
					rgba[2] = 255;
				}
				else
				{
					for (int channel = 0; channel < 3; ++channel)
						rgba[channel] = static_cast<uint8_t>(rgba[channel] / 4);
				}
				rgba[3] = 255;
			}
			diff.report += " Difference: " + save(marked, outputDir / (name + ".diff.png")).string() + ".";
		}
		diff.report += accept;
		return diff;
	}

	ImageDiff compareWithGolden(const std::string& name, const ui::Image& actual, const ImageTolerance& tolerance)
	{
		const char* update = std::getenv("GNUBASE_UPDATE_GOLDEN");
		return compareWithGolden(GNUBASE_GOLDEN_DIR, GNUBASE_TEST_OUTPUT_DIR, name, actual, tolerance,
			update != nullptr && std::string(update) == "1");
	}
}
