// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/testutils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <limits>

using namespace gnubase::test;

namespace
{
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
