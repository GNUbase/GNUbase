// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// Waveform overviews (#83): the smallest and largest sample of every channel over blocks of frames, at several block
// sizes, so a waveform of any length draws from a few thousand values. Nothing here runs on audio threads.
namespace gnubase::engine::peaks
{
	// Peaks are 16-bit: full scale (1.0) is g_fullScale. A peak's minimum is rounded down and its maximum up, so the
	// stored range always contains the samples; samples beyond full scale are clamped to it.
	constexpr int16_t g_fullScale = 32767;

	// Frames per peak of the levels GNUbase builds. Each level is a whole multiple of the one before.
	const std::vector<uint32_t>& defaultLevels();

	// One level: for every block of factor frames (the last one may be shorter), the minimum and maximum per channel.
	struct Level
	{
		uint32_t factor = 0;
		uint64_t peaks = 0;
		std::vector<int16_t> values; // peaks * channels * 2: per peak, per channel, minimum then maximum

		int16_t min(uint64_t peak, int channel, int channels) const
		{
			return values[((peak * static_cast<uint64_t>(channels)) + static_cast<uint64_t>(channel)) * 2];
		}
		int16_t max(uint64_t peak, int channel, int channels) const
		{
			return values[(((peak * static_cast<uint64_t>(channels)) + static_cast<uint64_t>(channel)) * 2) + 1];
		}
	};

	// The peaks of an audio file, or of its first frames while they are being built.
	struct PeakData
	{
		int channels = 0;
		double sampleRate = 0.0;
		uint64_t frames = 0;       // the frames the peaks cover
		std::vector<Level> levels; // finest first
	};

	bool operator==(const PeakData& a, const PeakData& b);
	inline bool operator!=(const PeakData& a, const PeakData& b)
	{
		return !(a == b);
	}

	// Builds peaks from sample frames appended in pieces of any size. After every append the data is complete for the
	// frames so far: the last peak of each level covers what its block has received.
	class PeakBuilder
	{
	public:
		// levels: frames per peak, ascending, each a whole multiple of the one before.
		PeakBuilder(int channels, double sampleRate, const std::vector<uint32_t>& levels = defaultLevels());

		// frames sample frames, channels interleaved.
		void append(const float* samples, size_t frames);

		const PeakData& data() const { return m_data; }

		// Moves the data out; the builder is done afterwards.
		PeakData take() { return std::move(m_data); }

	private:
		struct Accumulator
		{
			std::vector<int16_t> min, max; // per channel, over the completed child peaks of the current block
			uint64_t count = 0;            // child peaks (frames for the finest level) in the current block
			uint64_t done = 0;             // completed peaks
		};

		// A level's current block is complete: stores its peak and adds it to the next level's block.
		void complete(size_t level, const int16_t* min, const int16_t* max);
		// Stores each level's incomplete last peak.
		void updatePartials();

		PeakData m_data;
		std::vector<float> m_min, m_max; // per channel, over the finest level's current block
		std::vector<Accumulator> m_levels;
		std::vector<int16_t> m_partialMin, m_partialMax, m_scratchMin, m_scratchMax; // per channel
	};

	// Peaks for drawing: frames frames from start, in pixels columns, per channel the smallest and largest value.
	struct Columns
	{
		uint32_t factor = 0; // the level they come from
		int channels = 0;    // the channels asked for
		size_t pixels = 0;
		uint64_t covered = 0;     // the leading columns that have peaks; the rest are 0
		std::vector<int16_t> min; // channels * pixels: per channel, per column
		std::vector<int16_t> max;
	};

	// The peaks that columns are made of: a copy of the part of one level they read, for some of the channels. Cheap
	// to take while the peaks are locked; the columns are then computed from it without the lock.
	struct ColumnSource
	{
		uint32_t factor = 0;
		uint64_t frames = 0;         // the frames the peaks cover
		uint64_t first = 0;          // the level's first peak in values
		uint64_t peaks = 0;          // the peaks in values
		int channels = 0;            // the channels in values
		std::vector<int16_t> values; // peaks * channels * 2, as in Level
	};

	// Column i covers frames [start + i * frames / pixels, start + (i + 1) * frames / pixels), widened to whole peaks
	// of the coarsest level that still has at least one peak per column. Costs a few peaks per column at any length.
	// channels: the channels to return, in that order; empty for all of them. Throws std::invalid_argument for a
	// channel the peaks don't have.
	ColumnSource columnSource(
		const PeakData& data, uint64_t start, uint64_t frames, size_t pixels, const std::vector<int>& channels = {});
	Columns columns(const ColumnSource& source, uint64_t start, uint64_t frames, size_t pixels);
	Columns columns(
		const PeakData& data, uint64_t start, uint64_t frames, size_t pixels, const std::vector<int>& channels = {});

	// What a peak file says about its audio file, to notice when the audio file changed.
	struct SourceStamp
	{
		uint64_t size = 0;
		int64_t modified = 0;    // the modification time in ns since 1970 (UTC), on every system
		uint64_t headerHash = 0; // FNV-1a of the first g_headerBytes bytes

		bool operator==(const SourceStamp& other) const
		{
			return size == other.size && modified == other.modified && headerHash == other.headerHash;
		}
		bool operator!=(const SourceStamp& other) const { return !(*this == other); }
	};

	constexpr size_t g_headerBytes = 65536;

	// The audio file's stamp, or nothing if it can't be read.
	std::optional<SourceStamp> stampOf(const std::filesystem::path& file);

	// The file's size and modification time, with no header hash, without reading it; nothing if it isn't a file.
	std::optional<SourceStamp> sizeAndTime(const std::filesystem::path& file);

	// Size and modification time only, without reading the file: cheap enough to check before every query.
	bool sameSizeAndTime(const std::filesystem::path& file, const SourceStamp& stamp);

	class PeakFileError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// The contents of a peak file.
	struct PeakFile
	{
		std::string source; // the audio file's path, UTF-8
		SourceStamp stamp;
		PeakData data;
	};

	// The binary format, version g_peakFileVersion, little-endian: a header (the magic "GBPEAKS" and a zero byte, the
	// version, channels, sample rate, frames, the source's stamp and path), the level table (factor and peak count per
	// level) and the levels' values. Writing goes to a temporary file that is then renamed, so readers never see half a
	// file. Both throw PeakFileError.
	constexpr uint32_t g_peakFileVersion = 1;
	void writePeakFile(
		const std::filesystem::path& file, const std::string& source, const SourceStamp& stamp, const PeakData& data);
	PeakFile readPeakFile(const std::filesystem::path& file);

	// Removes the temporary files "<name>.gbpeaks.<16 hex digits>.tmp" in folder that a writer left behind when it
	// crashed: those last modified more than age ago, so a write still going on keeps its file. Returns how many.
	size_t removeStaleTemporaries(const std::filesystem::path& folder, std::chrono::seconds age);

	// Where an audio file's peaks are kept: next to it as "<file name>.gbpeaks", or, where that folder can't be
	// written, in the fallback folder under a name made from a hash of its path.
	std::filesystem::path besidePath(const std::filesystem::path& audioFile);
	std::filesystem::path fallbackPath(const std::filesystem::path& folder, const std::filesystem::path& audioFile);

	// The per-user cache folder for peak files that can't be stored next to their audio file:
	// %LOCALAPPDATA%\GNUbase\peaks, ~/Library/Caches/GNUbase/peaks or ~/.cache/gnubase/peaks.
	std::filesystem::path defaultFallbackFolder();
}
