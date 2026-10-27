// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace gnubase::test
{
	// Unique temporary folder, deleted with all its contents when the object goes out of scope.
	class TempDir
	{
	public:
		TempDir();
		~TempDir();
		TempDir(const TempDir&) = delete;
		TempDir& operator=(const TempDir&) = delete;

		const std::filesystem::path& path() const { return m_path; }

	private:
		std::filesystem::path m_path;
	};

	// Interleaved float audio.
	struct Audio
	{
		std::vector<float> samples;
		int channels = 1;
		int sampleRate = 48000;

		size_t frames() const { return channels > 0 ? samples.size() / static_cast<size_t>(channels) : 0; }
	};

	struct AudioDiff
	{
		bool equal = true;
		std::string report; // empty when equal
		size_t firstMismatch = 0; // sample index
		float maxDifference = 0.0f;
	};

	// Equal when channels, sample rate and length match and every sample differs by at most the tolerance.
	AudioDiff compareAudio(const Audio& actual, const Audio& expected, float tolerance);

	// 32-bit float WAV files. Only the format written by writeWav can be read.
	void writeWav(const std::filesystem::path& file, const Audio& audio);
	Audio readWav(const std::filesystem::path& file);

	// Compares against <goldenDir>/<name>.wav. On a mismatch or a missing golden file, writes <name>.actual.wav and,
	// if the formats match, <name>.diff.wav (actual minus expected) to the output folder. In update mode, (re)writes
	// the golden file instead and reports equal.
	AudioDiff compareWithGolden(const std::filesystem::path& goldenDir, const std::filesystem::path& outputDir,
		const std::string& name, const Audio& actual, float tolerance, bool update = false);

	// Same, using tests/golden and the build's test output folder. GNUBASE_UPDATE_GOLDEN=1 enables update mode.
	AudioDiff compareWithGolden(const std::string& name, const Audio& actual, float tolerance);
}
