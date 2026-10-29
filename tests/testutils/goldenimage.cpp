// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/goldenimage.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>

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

		// Write a PNG, creating the folder; the name may contain folders.
		std::filesystem::path save(const ui::Image& image, const std::filesystem::path& file)
		{
			std::filesystem::create_directories(file.parent_path());
			ui::writePng(image, file);
			return file;
		}
	}

	ImageDiff compareImages(
		const ui::Image& actual, const ui::Image& expected, int tolerance, size_t maxDifferentPixels)
	{
		ImageDiff diff;
		if (actual.width != expected.width || actual.height != expected.height ||
			actual.pixels.size() != expected.pixels.size())
		{
			diff.equal = false;
			diff.report = "size " + sizeOf(actual) + " instead of " + sizeOf(expected) + ".";
			return diff;
		}
		std::optional<size_t> first;
		const size_t pixels = actual.pixels.size() / 4;
		for (size_t pixel = 0; pixel < pixels; ++pixel)
		{
			const int different = difference(actual, expected, pixel);
			diff.maxDifference = std::max(diff.maxDifference, different);
			if (different <= tolerance)
				continue;
			++diff.differentPixels;
			if (!first)
				first = pixel;
		}
		if (diff.differentPixels <= maxDifferentPixels)
			return diff;
		diff.equal = false;
		const auto width = static_cast<size_t>(actual.width);
		diff.report = std::to_string(diff.differentPixels) + " pixels differ by more than " +
			std::to_string(tolerance) + " (" + std::to_string(maxDifferentPixels) + " may), the first at (" +
			std::to_string(first.value_or(0) % width) + ", " + std::to_string(first.value_or(0) / width) +
			"); the largest difference is " + std::to_string(diff.maxDifference) + ".";
		return diff;
	}

	ImageDiff compareWithGolden(const std::filesystem::path& goldenDir, const std::filesystem::path& outputDir,
		const std::string& name, const ui::Image& actual, int tolerance, size_t maxDifferentPixels, bool update)
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

		ImageDiff diff = compareImages(actual, *expected, tolerance, maxDifferentPixels);
		if (diff.equal)
			return diff;

		diff.report +=
			" Golden file: " + golden.string() + ". Actual image: " + save(actual, actualFile).string() + ".";
		if (actual.width == expected->width && actual.height == expected->height)
		{
			ui::Image marked = *expected;
			for (size_t pixel = 0; pixel < marked.pixels.size() / 4; ++pixel)
			{
				uint8_t* rgba = &marked.pixels[pixel * 4];
				if (difference(actual, *expected, pixel) > tolerance)
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

	ImageDiff compareWithGolden(
		const std::string& name, const ui::Image& actual, int tolerance, size_t maxDifferentPixels)
	{
		const char* update = std::getenv("GNUBASE_UPDATE_GOLDEN");
		return compareWithGolden(GNUBASE_GOLDEN_DIR, GNUBASE_TEST_OUTPUT_DIR, name, actual, tolerance,
			maxDifferentPixels, update != nullptr && std::string(update) == "1");
	}
}
