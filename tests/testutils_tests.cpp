// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/goldenimage.h"
#include "testutils/testutils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>

using namespace gnubase::test;

namespace
{
	// A 4x3 image with a different color in every pixel.
	gnubase::ui::Image gradient()
	{
		gnubase::ui::Image image{4, 3, {}};
		for (int y = 0; y < image.height; ++y)
			for (int x = 0; x < image.width; ++x)
				image.pixels.insert(
					image.pixels.end(), {static_cast<uint8_t>(x * 60), static_cast<uint8_t>(y * 100), 50, 255});
		return image;
	}

	Audio ramp(int channels, size_t frames)
	{
		Audio audio;
		audio.channels = channels;
		const size_t count = frames * static_cast<size_t>(channels);
		for (size_t i = 0; i < count; ++i)
			audio.samples.push_back(static_cast<float>(i) / static_cast<float>(count));
		return audio;
	}
}

TEST_CASE("TempDir creates a folder and deletes it with its contents", "[unit]")
{
	std::filesystem::path path;
	{
		const TempDir dir;
		path = dir.path();
		REQUIRE(std::filesystem::is_directory(path));
		std::ofstream(path / "file.txt") << "content";
	}
	REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("compareAudio accepts differences within the tolerance", "[unit]")
{
	const Audio expected = ramp(2, 100);
	Audio actual = expected;
	actual.samples[10] += 0.0005f;

	const AudioDiff diff = compareAudio(actual, expected, 0.001f);
	CHECK(diff.equal);
	CHECK(diff.report.empty());
}

TEST_CASE("compareAudio reports the first mismatch and the largest difference", "[unit]")
{
	const Audio expected = ramp(2, 100);
	Audio actual = expected;
	actual.samples[21] += 0.1f;
	actual.samples[40] -= 0.5f;

	const AudioDiff diff = compareAudio(actual, expected, 0.001f);
	REQUIRE_FALSE(diff.equal);
	CHECK(diff.firstMismatch == 21);
	CHECK(diff.maxDifference == Catch::Approx(0.5f));
	CHECK(diff.report.find("sample 21") != std::string::npos);
}

TEST_CASE("compareAudio rejects different channel counts, sample rates and lengths", "[unit]")
{
	const Audio expected = ramp(2, 100);
	CHECK_FALSE(compareAudio(ramp(1, 200), expected, 0.0f).equal);
	CHECK_FALSE(compareAudio(ramp(2, 99), expected, 0.0f).equal);

	Audio otherRate = expected;
	otherRate.sampleRate = 44100;
	CHECK_FALSE(compareAudio(otherRate, expected, 0.0f).equal);
}

TEST_CASE("compareAudio treats NaN as a mismatch", "[unit]")
{
	const Audio expected = ramp(1, 10);
	Audio actual = expected;
	actual.samples[3] = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(compareAudio(actual, expected, 1.0f).equal);
}

TEST_CASE("WAV files round-trip float samples exactly", "[unit]")
{
	const TempDir dir;
	const Audio audio = ramp(2, 1000);
	writeWav(dir.path() / "ramp.wav", audio);

	const Audio read = readWav(dir.path() / "ramp.wav");
	CHECK(read.channels == 2);
	CHECK(read.sampleRate == 48000);
	CHECK(read.samples == audio.samples);
}

TEST_CASE("compareWithGolden fails for a missing golden file and saves the actual audio", "[unit]")
{
	const TempDir golden;
	const TempDir output;
	const AudioDiff diff = compareWithGolden(golden.path(), output.path(), "ramp", ramp(1, 10), 0.0f);
	CHECK_FALSE(diff.equal);
	CHECK(readWav(output.path() / "ramp.actual.wav").samples == ramp(1, 10).samples);
}

TEST_CASE("compareWithGolden passes when the audio matches the golden file", "[unit]")
{
	const TempDir golden;
	const TempDir output;
	writeWav(golden.path() / "ramp.wav", ramp(2, 100));
	CHECK(compareWithGolden(golden.path(), output.path(), "ramp", ramp(2, 100), 0.0f).equal);
}

TEST_CASE("compareWithGolden saves the actual audio and the difference on a mismatch", "[unit]")
{
	const TempDir golden;
	const TempDir output;
	const Audio expected = ramp(2, 100);
	writeWav(golden.path() / "ramp.wav", expected);
	Audio actual = expected;
	actual.samples[7] = 1.0f;

	const AudioDiff diff = compareWithGolden(golden.path(), output.path(), "ramp", actual, 0.0f);
	CHECK_FALSE(diff.equal);
	CHECK(readWav(output.path() / "ramp.actual.wav").samples == actual.samples);
	const Audio difference = readWav(output.path() / "ramp.diff.wav");
	CHECK(difference.samples[7] == actual.samples[7] - expected.samples[7]);
	CHECK(difference.samples[8] == 0.0f);
}

TEST_CASE("compareWithGolden writes the golden file in update mode", "[unit]")
{
	const TempDir golden;
	const TempDir output;
	CHECK(compareWithGolden(golden.path(), output.path(), "ramp", ramp(1, 10), 0.0f, true).equal);
	CHECK(readWav(golden.path() / "ramp.wav").samples == ramp(1, 10).samples);
}

TEST_CASE("images compare equal within the tolerance, allowing a number of pixels beyond it", "[unit]")
{
	const gnubase::ui::Image expected = gradient();
	gnubase::ui::Image actual = expected;
	actual.pixels[0] += 3;
	CHECK(compareImages(actual, expected, {3}).equal);
	CHECK(compareImages(actual, expected, {2, 1}).equal);

	const ImageDiff diff = compareImages(actual, expected, {2});
	CHECK_FALSE(diff.equal);
	CHECK(diff.maxDifference == 3);
	CHECK(diff.differentPixels == 1);
	CHECK(diff.report.find("(0, 0)") != std::string::npos);

	actual.width = 3;
	CHECK_FALSE(compareImages(actual, expected, {255, 100}).equal);
}

TEST_CASE("images compared on averages ignore single pixels of an edge antialiased differently", "[unit]")
{
	// A vertical edge between black and white; one of its pixels is half covered in the actual image only.
	gnubase::ui::Image expected{8, 8, {}};
	for (int y = 0; y < 8; ++y)
	{
		for (int x = 0; x < 8; ++x)
		{
			const auto value = static_cast<uint8_t>(x < 4 ? 0 : 255);
			expected.pixels.insert(expected.pixels.end(), {value, value, value, 255});
		}
	}
	const auto at = [](int x, int y) { return ((static_cast<size_t>(y) * 8) + static_cast<size_t>(x)) * 4; };
	gnubase::ui::Image actual = expected;
	for (size_t channel = 0; channel < 3; ++channel)
		actual.pixels[at(4, 3) + channel] = 128;
	CHECK_FALSE(compareImages(actual, expected, {16}).equal);
	CHECK(compareImages(actual, expected, {16, 0, 1}).equal);

	// Averaging doesn't hide a line that is missing.
	gnubase::ui::Image missing = expected;
	for (int y = 0; y < 8; ++y)
		for (size_t channel = 0; channel < 3; ++channel)
			missing.pixels[at(5, y) + channel] = 0;
	CHECK_FALSE(compareImages(missing, expected, {16, 0, 1}).equal);
}

TEST_CASE("compareWithGolden saves the actual image and marks the differences", "[unit]")
{
	const TempDir golden;
	const TempDir output;
	const gnubase::ui::Image expected = gradient();
	CHECK(compareWithGolden(golden.path(), output.path(), "ui/gradient", expected, {}, true).equal);
	CHECK(compareWithGolden(golden.path(), output.path(), "ui/gradient", expected, {}).equal);

	gnubase::ui::Image actual = expected;
	actual.pixels[(5 * 4) + 1] = 255; // the pixel at (1, 1)
	const ImageDiff diff = compareWithGolden(golden.path(), output.path(), "ui/gradient", actual, {});
	CHECK_FALSE(diff.equal);
	CHECK(gnubase::ui::readPng(output.path() / "ui/gradient.actual.png").value_or(gnubase::ui::Image{}).pixels ==
		actual.pixels);
	const std::optional<gnubase::ui::Image> marked = gnubase::ui::readPng(output.path() / "ui/gradient.diff.png");
	REQUIRE(marked.has_value());
	CHECK(marked.value_or(gnubase::ui::Image{}).pixel(1, 1) == std::array<uint8_t, 4>{255, 0, 255, 255});
	CHECK(marked.value_or(gnubase::ui::Image{}).pixel(0, 0)[3] == 255);

	CHECK_FALSE(compareWithGolden(golden.path(), output.path(), "missing", actual, {}).equal);
	CHECK(std::filesystem::exists(output.path() / "missing.actual.png"));
}
