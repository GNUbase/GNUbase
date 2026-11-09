// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
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

	// Keeps a file from being read while it exists, as another program that locks it (Windows) or a missing read
	// permission (elsewhere) would. Its size and time stay readable. Not active where it can't be done, e.g. for root,
	// who reads anything.
	class Unreadable
	{
	public:
		explicit Unreadable(std::filesystem::path file);
		~Unreadable();
		Unreadable(const Unreadable&) = delete;
		Unreadable& operator=(const Unreadable&) = delete;
		Unreadable(Unreadable&&) = delete;
		Unreadable& operator=(Unreadable&&) = delete;

		bool active() const { return m_active; }
		void release();

	private:
		std::filesystem::path m_file;
		bool m_active = false;
#ifdef _WIN32
		void* m_handle = nullptr; // the file's HANDLE
#endif
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
		std::string report;       // empty when equal
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

	// Compares text, such as MIDI events written a line each, against tests/golden/<name>.txt, ignoring carriage
	// returns. On a mismatch or a missing golden file, writes <name>.actual.txt to the build's test output folder and
	// returns a report; GNUBASE_UPDATE_GOLDEN=1 (re)writes the golden file instead. Empty when equal.
	std::string compareTextWithGolden(const std::string& name, const std::string& actual);

	// The whole file, or an empty string if it cannot be read.
	std::string readText(const std::filesystem::path& file);

	// The build's test output folder, for files a test writes to look at, such as screenshots.
	std::filesystem::path outputDirectory();

	// A zip archive's entries, such as a project file's, by name. Throws if the file isn't a zip archive.
	std::map<std::string, std::string> readZip(const std::string& path);

	// Writes the entries as a zip archive, replacing the file. Throws if it can't be written.
	void writeZip(const std::string& path, const std::map<std::string, std::string>& entries);
}
