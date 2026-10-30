// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/renderer.h"

#include <cstddef>
#include <filesystem>
#include <string>

// Screenshots compared with golden PNG images, as golden WAV files are for audio (testutils/testutils.h).
namespace gnubase::test
{
	struct ImageDiff
	{
		bool equal = true;
		std::string report;         // empty when equal
		int maxDifference = 0;      // of a color or alpha channel, 0 to 255
		size_t differentPixels = 0; // that have a channel differing by more than the tolerance
	};

	// How much two images may differ. Different GPUs may round an edge or a blend differently, and renderers may
	// place antialiasing samples differently along edges.
	struct ImageTolerance
	{
		int channel = 0;               // how much a color or alpha channel may differ, 0 to 255
		size_t maxDifferentPixels = 0; // how many pixels may differ by more
		int averageRadius = 0;         // compare each pixel's average with its neighbours this far away, not the pixel
	};

	// Equal when the sizes match and the images differ within the tolerance.
	ImageDiff compareImages(const ui::Image& actual, const ui::Image& expected, const ImageTolerance& tolerance);

	// Compares against <goldenDir>/<name>.png. On a mismatch or a missing golden file, writes <name>.actual.png and,
	// if the sizes match, <name>.diff.png to the output folder: the golden image darkened, with the pixels that
	// differ in magenta. In update mode, (re)writes the golden file instead and reports equal.
	ImageDiff compareWithGolden(const std::filesystem::path& goldenDir, const std::filesystem::path& outputDir,
		const std::string& name, const ui::Image& actual, const ImageTolerance& tolerance, bool update = false);

	// Same, using tests/golden and the build's test output folder. GNUBASE_UPDATE_GOLDEN=1 enables update mode.
	ImageDiff compareWithGolden(const std::string& name, const ui::Image& actual, const ImageTolerance& tolerance);
}
