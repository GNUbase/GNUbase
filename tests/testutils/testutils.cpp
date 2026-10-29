// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/testutils.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace gnubase::test
{
	TempDir::TempDir()
	{
		std::random_device random;
		do
			m_path = std::filesystem::temp_directory_path() / ("gnubase-test-" + std::to_string(random()));
		while (!std::filesystem::create_directory(m_path));
	}

	// NOLINTNEXTLINE(bugprone-exception-escape): only std::bad_alloc could escape, which ends the test run anyway
	TempDir::~TempDir()
	{
		std::error_code ignored;
		std::filesystem::remove_all(m_path, ignored);
	}

	AudioDiff compareAudio(const Audio& actual, const Audio& expected, float tolerance)
	{
		AudioDiff diff;
		std::ostringstream report;

		if (actual.channels != expected.channels || actual.sampleRate != expected.sampleRate ||
			actual.samples.size() != expected.samples.size())
		{
			diff.equal = false;
			report << "format differs: actual " << actual.channels << " ch, " << actual.sampleRate << " Hz, "
				   << actual.frames() << " frames; expected " << expected.channels << " ch, " << expected.sampleRate
				   << " Hz, " << expected.frames() << " frames";
			diff.report = report.str();
			return diff;
		}

		size_t mismatches = 0;
		size_t maxIndex = 0;
		for (size_t i = 0; i < actual.samples.size(); ++i)
		{
			const float difference = std::abs(actual.samples[i] - expected.samples[i]);
			if (difference <= tolerance) // false for NaN, so NaN always counts as a mismatch
				continue;
			if (mismatches++ == 0)
				diff.firstMismatch = i;
			if (difference > diff.maxDifference)
			{
				diff.maxDifference = difference;
				maxIndex = i;
			}
		}
		if (mismatches == 0)
			return diff;

		const auto channels = static_cast<size_t>(actual.channels);
		const auto describe = [&](size_t i)
		{
			const size_t frame = i / channels;
			std::ostringstream s;
			s << "sample " << i << " (frame " << frame << ", channel " << i % channels << ", "
			  << static_cast<double>(frame) / actual.sampleRate << " s): actual " << actual.samples[i] << ", expected "
			  << expected.samples[i];
			return s.str();
		};

		diff.equal = false;
		report << mismatches << " of " << actual.samples.size() << " samples differ by more than " << tolerance
			   << ". First: " << describe(diff.firstMismatch) << ". Largest difference " << diff.maxDifference << " ("
			   << 20.0 * std::log10(diff.maxDifference) << " dB) at " << describe(maxIndex) << ".";
		diff.report = report.str();
		return diff;
	}

	// WAV fields are little-endian, like every platform GNUbase targets, so values are written and read as they are.
	namespace
	{
		template <typename T> void put(std::ostream& out, T value)
		{
			out.write(reinterpret_cast<const char*>(&value), sizeof(value));
		}

		template <typename T> T get(std::istream& in)
		{
			T value{};
			in.read(reinterpret_cast<char*>(&value), sizeof(value));
			return value;
		}

		constexpr uint16_t g_formatFloat = 3; // WAVE_FORMAT_IEEE_FLOAT
	}

	void writeWav(const std::filesystem::path& file, const Audio& audio)
	{
		std::filesystem::create_directories(file.parent_path());
		std::ofstream out(file, std::ios::binary);
		if (!out)
			throw std::runtime_error("cannot write " + file.string());

		const auto channels = static_cast<uint16_t>(audio.channels);
		const auto rate = static_cast<uint32_t>(audio.sampleRate);
		const auto dataBytes = static_cast<uint32_t>(audio.samples.size() * sizeof(float));

		out.write("RIFF", 4);
		put<uint32_t>(out, 4 + (8 + 18) + (8 + 4) + (8 + dataBytes));
		out.write("WAVE", 4);

		out.write("fmt ", 4);
		put<uint32_t>(out, 18);
		put<uint16_t>(out, g_formatFloat);
		put<uint16_t>(out, channels);
		put<uint32_t>(out, rate);
		put<uint32_t>(out, rate * channels * 4u);
		put<uint16_t>(out, static_cast<uint16_t>(channels * 4u));
		put<uint16_t>(out, 32);
		put<uint16_t>(out, 0); // no extension

		out.write("fact", 4); // required for non-PCM formats
		put<uint32_t>(out, 4);
		put<uint32_t>(out, static_cast<uint32_t>(audio.frames()));

		out.write("data", 4);
		put<uint32_t>(out, dataBytes);
		out.write(reinterpret_cast<const char*>(audio.samples.data()), dataBytes);
		if (!out)
			throw std::runtime_error("cannot write " + file.string());
	}

	Audio readWav(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		char id[4] = {};
		const auto readId = [&] { return static_cast<bool>(in.read(id, 4)); };

		const bool isWav =
			readId() && std::memcmp(id, "RIFF", 4) == 0 && in.ignore(4) && readId() && std::memcmp(id, "WAVE", 4) == 0;
		if (!isWav)
			throw std::runtime_error(file.string() + " is not a WAV file");

		Audio audio;
		bool haveFormat = false;
		while (readId())
		{
			const auto size = get<uint32_t>(in);
			if (std::memcmp(id, "fmt ", 4) == 0)
			{
				const auto format = get<uint16_t>(in);
				audio.channels = get<uint16_t>(in);
				audio.sampleRate = static_cast<int>(get<uint32_t>(in));
				in.ignore(6); // byte rate, block align
				const auto bits = get<uint16_t>(in);
				if (format != g_formatFloat || bits != 32)
					throw std::runtime_error(file.string() + ": only 32-bit float WAV files are supported");
				in.ignore(size - 16 + (size & 1));
				haveFormat = true;
			}
			else if (std::memcmp(id, "data", 4) == 0)
			{
				if (!haveFormat)
					throw std::runtime_error(file.string() + ": data chunk before fmt chunk");
				audio.samples.resize(size / sizeof(float));
				in.read(reinterpret_cast<char*>(audio.samples.data()), static_cast<std::streamsize>(size));
				if (!in)
					throw std::runtime_error(file.string() + ": truncated data chunk");
				return audio;
			}
			else
			{
				in.ignore(size + (size & 1));
			}
		}
		throw std::runtime_error(file.string() + " has no data chunk");
	}

	AudioDiff compareWithGolden(const std::filesystem::path& goldenDir, const std::filesystem::path& outputDir,
		const std::string& name, const Audio& actual, float tolerance, bool update)
	{
		const auto golden = goldenDir / (name + ".wav");
		const auto actualFile = outputDir / (name + ".actual.wav");
		const std::string accept = " Run with GNUBASE_UPDATE_GOLDEN=1 to accept the actual audio.";

		if (update)
		{
			writeWav(golden, actual);
			return {};
		}

		if (!std::filesystem::exists(golden))
		{
			writeWav(actualFile, actual);
			AudioDiff diff;
			diff.equal = false;
			diff.report = "golden file " + golden.string() + " does not exist; actual audio written to " +
				actualFile.string() + "." + accept;
			return diff;
		}

		const Audio expected = readWav(golden);
		AudioDiff diff = compareAudio(actual, expected, tolerance);
		if (diff.equal)
			return diff;

		writeWav(actualFile, actual);
		diff.report += " Golden file: " + golden.string() + ". Actual audio: " + actualFile.string() + ".";
		if (actual.samples.size() == expected.samples.size() && actual.channels == expected.channels &&
			actual.sampleRate == expected.sampleRate)
		{
			Audio difference = actual;
			for (size_t i = 0; i < difference.samples.size(); ++i)
				difference.samples[i] = actual.samples[i] - expected.samples[i];
			const auto diffFile = outputDir / (name + ".diff.wav");
			writeWav(diffFile, difference);
			diff.report += " Difference: " + diffFile.string() + ".";
		}
		diff.report += accept;
		return diff;
	}

	AudioDiff compareWithGolden(const std::string& name, const Audio& actual, float tolerance)
	{
		const char* update = std::getenv("GNUBASE_UPDATE_GOLDEN");
		return compareWithGolden(GNUBASE_GOLDEN_DIR, GNUBASE_TEST_OUTPUT_DIR, name, actual, tolerance,
			update != nullptr && std::string(update) == "1");
	}

	std::string readText(const std::filesystem::path& file)
	{
		const std::ifstream in(file, std::ios::binary);
		std::ostringstream text;
		text << in.rdbuf();
		return text.str();
	}

	std::filesystem::path outputDirectory()
	{
		return GNUBASE_TEST_OUTPUT_DIR;
	}
}
