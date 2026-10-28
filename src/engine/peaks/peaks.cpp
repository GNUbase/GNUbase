// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/peaks/peaks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace gnubase::engine::peaks
{
	namespace
	{
		constexpr std::array<char, 8> g_magic = {'G', 'B', 'P', 'E', 'A', 'K', 'S', '\0'};
		constexpr size_t g_fixedHeader = 64; // up to the source path
		constexpr size_t g_levelEntry = 16;
		constexpr uint32_t g_maxChannels = 256;
		constexpr uint32_t g_maxLevels = 16;
		constexpr uint32_t g_maxSourceLength = 65536;
		constexpr int16_t g_emptyMin = std::numeric_limits<int16_t>::max();
		constexpr int16_t g_emptyMax = std::numeric_limits<int16_t>::min();

		int16_t quantizeDown(float sample)
		{
			return static_cast<int16_t>(std::clamp(std::floor(static_cast<double>(sample) * g_fullScale),
				-static_cast<double>(g_fullScale), static_cast<double>(g_fullScale)));
		}

		int16_t quantizeUp(float sample)
		{
			return static_cast<int16_t>(std::clamp(std::ceil(static_cast<double>(sample) * g_fullScale),
				-static_cast<double>(g_fullScale), static_cast<double>(g_fullScale)));
		}

		// A block's float minimum and maximum as a peak. A block of nothing but NaN reads as silence.
		void quantize(float low, float high, int16_t& min, int16_t& max)
		{
			if (low <= high)
			{
				min = quantizeDown(low);
				max = quantizeUp(high);
			}
			else
			{
				min = 0;
				max = 0;
			}
		}

		uint64_t fnv1a(const unsigned char* bytes, size_t size, uint64_t hash = 14695981039346656037ULL)
		{
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		// i * frames / pixels without overflowing.
		uint64_t scale(uint64_t i, uint64_t frames, uint64_t pixels)
		{
			return (i * (frames / pixels)) + (i * (frames % pixels) / pixels);
		}

		class Writer
		{
		public:
			void put(uint64_t value, int bytes)
			{
				for (int i = 0; i < bytes; ++i)
					m_bytes.push_back(static_cast<char>((value >> (8 * i)) & 0xff));
			}
			void putDouble(double value)
			{
				uint64_t bits = 0;
				std::memcpy(&bits, &value, sizeof bits);
				put(bits, 8);
			}
			void putBytes(const char* bytes, size_t size) { m_bytes.insert(m_bytes.end(), bytes, bytes + size); }
			void reserve(size_t size) { m_bytes.reserve(size); }
			const std::string& bytes() const { return m_bytes; }

		private:
			std::string m_bytes;
		};

		class Reader
		{
		public:
			Reader(const std::string& bytes, const std::filesystem::path& file) : m_bytes(bytes), m_file(file) {}

			uint64_t get(int bytes)
			{
				need(static_cast<size_t>(bytes));
				uint64_t value = 0;
				for (int i = 0; i < bytes; ++i)
					value |=
						static_cast<uint64_t>(static_cast<unsigned char>(m_bytes[m_offset + static_cast<size_t>(i)]))
						<< (8 * i);
				m_offset += static_cast<size_t>(bytes);
				return value;
			}
			double getDouble()
			{
				const uint64_t bits = get(8);
				double value = 0.0;
				std::memcpy(&value, &bits, sizeof value);
				return value;
			}
			std::string getBytes(size_t size)
			{
				need(size);
				std::string bytes = m_bytes.substr(m_offset, size);
				m_offset += size;
				return bytes;
			}
			size_t left() const { return m_bytes.size() - m_offset; }
			[[noreturn]] void fail(const std::string& reason) const
			{
				throw PeakFileError(m_file.u8string() + " is not a valid peak file: " + reason);
			}

		private:
			void need(size_t size) const
			{
				if (left() < size)
					fail("it is too short");
			}

			const std::string& m_bytes;
			const std::filesystem::path& m_file;
			size_t m_offset = 0;
		};

		// A file's size and modification time, without its header hash; nothing if it isn't a readable file.
		std::optional<SourceStamp> sizeAndTime(const std::filesystem::path& file)
		{
			// The OS directly: clang-analyzer reports std::filesystem's status flags in MSVC's own headers.
			SourceStamp stamp;
#ifdef _WIN32
			WIN32_FILE_ATTRIBUTE_DATA attributes{};
			if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &attributes) == 0 ||
				(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
				return std::nullopt;
			stamp.size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
			stamp.modified =
				static_cast<int64_t>((static_cast<uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) |
					attributes.ftLastWriteTime.dwLowDateTime); // 100 ns since 1601
#else
			struct stat status{};
			if (::stat(file.c_str(), &status) != 0 || !S_ISREG(status.st_mode))
				return std::nullopt;
			stamp.size = static_cast<uint64_t>(status.st_size);
#ifdef __APPLE__
			const timespec& time = status.st_mtimespec;
#else
			const timespec& time = status.st_mtim;
#endif
			const int64_t seconds = time.tv_sec;
			const int64_t nanoseconds = time.tv_nsec;
			stamp.modified = (seconds * 1000000000) + nanoseconds;
#endif
			return stamp;
		}

		std::string hex(uint64_t value)
		{
			static constexpr char digits[] = "0123456789abcdef";
			std::string text(16, '0');
			for (size_t i = 0; i < 16; ++i)
				text[15 - i] = digits[(value >> (4 * i)) & 0xf];
			return text;
		}
	}

	const std::vector<uint32_t>& defaultLevels()
	{
		static const std::vector<uint32_t> levels = {256, 4096, 65536};
		return levels;
	}

	bool operator==(const PeakData& a, const PeakData& b)
	{
		if (a.channels != b.channels || a.sampleRate != b.sampleRate || a.frames != b.frames ||
			a.levels.size() != b.levels.size())
			return false;
		for (size_t i = 0; i < a.levels.size(); ++i)
			if (a.levels[i].factor != b.levels[i].factor || a.levels[i].peaks != b.levels[i].peaks ||
				a.levels[i].values != b.levels[i].values)
				return false;
		return true;
	}

	PeakBuilder::PeakBuilder(int channels, double sampleRate, const std::vector<uint32_t>& levels)
	{
		if (channels < 1 || static_cast<uint32_t>(channels) > g_maxChannels)
			throw std::invalid_argument("peaks need 1 to " + std::to_string(g_maxChannels) + " channels");
		if (levels.empty() || levels.size() > g_maxLevels || levels[0] == 0)
			throw std::invalid_argument("peaks need 1 to " + std::to_string(g_maxLevels) + " levels");
		for (size_t i = 1; i < levels.size(); ++i)
			if (levels[i] <= levels[i - 1] || levels[i] % levels[i - 1] != 0)
				throw std::invalid_argument("each peak level must be a multiple of the one before");
		m_data.channels = channels;
		m_data.sampleRate = sampleRate;
		const auto count = static_cast<size_t>(channels);
		for (const uint32_t factor : levels)
		{
			m_data.levels.push_back({factor, 0, {}});
			m_levels.push_back(
				{std::vector<int16_t>(count, g_emptyMin), std::vector<int16_t>(count, g_emptyMax), 0, 0});
		}
		m_min.assign(count, std::numeric_limits<float>::infinity());
		m_max.assign(count, -std::numeric_limits<float>::infinity());
		m_partialMin.resize(count);
		m_partialMax.resize(count);
		m_scratchMin.resize(count);
		m_scratchMax.resize(count);
	}

	void PeakBuilder::append(const float* samples, size_t frames)
	{
		const auto channels = static_cast<size_t>(m_data.channels);
		const uint64_t factor = m_data.levels[0].factor;
		Accumulator& finest = m_levels[0];
		float* low = m_min.data();
		float* high = m_max.data();
		while (frames > 0)
		{
			const auto run = static_cast<size_t>(std::min<uint64_t>(frames, factor - finest.count));
			for (size_t frame = 0; frame < run; ++frame)
				for (size_t channel = 0; channel < channels; ++channel)
				{
					// NaN compares false and is left out.
					const float sample = samples[(frame * channels) + channel];
					low[channel] = std::min(low[channel], sample);
					high[channel] = std::max(high[channel], sample);
				}
			samples += run * channels;
			frames -= run;
			finest.count += run;
			m_data.frames += run;
			if (finest.count == factor)
			{
				for (size_t channel = 0; channel < channels; ++channel)
				{
					quantize(low[channel], high[channel], m_scratchMin[channel], m_scratchMax[channel]);
					low[channel] = std::numeric_limits<float>::infinity();
					high[channel] = -std::numeric_limits<float>::infinity();
				}
				finest.count = 0;
				complete(0, m_scratchMin.data(), m_scratchMax.data());
			}
		}
		updatePartials();
	}

	void PeakBuilder::complete(size_t level, const int16_t* min, const int16_t* max)
	{
		const auto channels = static_cast<size_t>(m_data.channels);
		Level& data = m_data.levels[level];
		Accumulator& accumulator = m_levels[level];
		const size_t at = static_cast<size_t>(accumulator.done) * channels * 2;
		if (data.values.size() < at + (channels * 2))
			data.values.resize(at + (channels * 2));
		for (size_t channel = 0; channel < channels; ++channel)
		{
			data.values[at + (channel * 2)] = min[channel];
			data.values[at + (channel * 2) + 1] = max[channel];
		}
		++accumulator.done;

		if (level + 1 == m_levels.size())
			return;
		Accumulator& parent = m_levels[level + 1];
		for (size_t channel = 0; channel < channels; ++channel)
		{
			parent.min[channel] = std::min(parent.min[channel], min[channel]);
			parent.max[channel] = std::max(parent.max[channel], max[channel]);
		}
		++parent.count;
		if (parent.count == m_data.levels[level + 1].factor / data.factor)
		{
			complete(level + 1, parent.min.data(), parent.max.data());
			std::fill(parent.min.begin(), parent.min.end(), g_emptyMin);
			std::fill(parent.max.begin(), parent.max.end(), g_emptyMax);
			parent.count = 0;
		}
	}

	void PeakBuilder::updatePartials()
	{
		const auto channels = static_cast<size_t>(m_data.channels);
		bool partial = m_levels[0].count > 0;
		if (partial)
			for (size_t channel = 0; channel < channels; ++channel)
				quantize(m_min[channel], m_max[channel], m_partialMin[channel], m_partialMax[channel]);
		for (size_t level = 0; level < m_levels.size(); ++level)
		{
			Level& data = m_data.levels[level];
			const Accumulator& accumulator = m_levels[level];
			if (level > 0)
			{
				// The completed child peaks of this block, and the child level's partial peak.
				if (!partial)
				{
					std::copy(accumulator.min.begin(), accumulator.min.end(), m_partialMin.begin());
					std::copy(accumulator.max.begin(), accumulator.max.end(), m_partialMax.begin());
				}
				else
				{
					for (size_t channel = 0; channel < channels; ++channel)
					{
						m_partialMin[channel] = std::min(m_partialMin[channel], accumulator.min[channel]);
						m_partialMax[channel] = std::max(m_partialMax[channel], accumulator.max[channel]);
					}
				}
				partial = partial || accumulator.count > 0;
			}
			data.peaks = accumulator.done + (partial ? 1 : 0);
			data.values.resize(static_cast<size_t>(data.peaks) * channels * 2);
			if (partial)
			{
				const size_t at = static_cast<size_t>(accumulator.done) * channels * 2;
				for (size_t channel = 0; channel < channels; ++channel)
				{
					data.values[at + (channel * 2)] = m_partialMin[channel];
					data.values[at + (channel * 2) + 1] = m_partialMax[channel];
				}
			}
		}
	}

	Columns columns(const PeakData& data, uint64_t start, uint64_t frames, size_t pixels)
	{
		if (pixels == 0 || frames == 0)
			throw std::invalid_argument("columns need at least one pixel and one frame");
		if (data.levels.empty())
			throw std::invalid_argument("peaks without levels");
		Columns result;
		result.channels = data.channels;
		result.pixels = pixels;
		const auto channels = static_cast<size_t>(data.channels);
		result.min.assign(channels * pixels, 0);
		result.max.assign(channels * pixels, 0);

		// The coarsest level that still has a peak per column.
		const Level* level = data.levels.data();
		for (const Level& candidate : data.levels)
			if (static_cast<uint64_t>(candidate.factor) <= frames / pixels)
				level = &candidate;
		result.factor = level->factor;
		const uint64_t factor = level->factor;

		for (size_t i = 0; i < pixels; ++i)
		{
			const uint64_t begin = start + scale(i, frames, pixels);
			const uint64_t end = std::max(begin + 1, start + scale(i + 1, frames, pixels));
			if (begin >= data.frames)
				break;
			const uint64_t first = begin / factor;
			const uint64_t last = std::min((end + factor - 1) / factor, level->peaks);
			for (size_t channel = 0; channel < channels; ++channel)
			{
				int16_t low = g_emptyMin;
				int16_t high = g_emptyMax;
				for (uint64_t peak = first; peak < last; ++peak)
				{
					low = std::min(low, level->min(peak, static_cast<int>(channel), data.channels));
					high = std::max(high, level->max(peak, static_cast<int>(channel), data.channels));
				}
				result.min[(channel * pixels) + i] = low;
				result.max[(channel * pixels) + i] = high;
			}
			result.covered = i + 1;
		}
		return result;
	}

	std::optional<SourceStamp> stampOf(const std::filesystem::path& file)
	{
		const std::optional<SourceStamp> found = sizeAndTime(file);
		if (!found)
			return std::nullopt;
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return std::nullopt;
		std::string header(static_cast<size_t>(std::min<uint64_t>(found->size, g_headerBytes)), '\0');
		in.read(header.data(), static_cast<std::streamsize>(header.size()));
		if (static_cast<size_t>(in.gcount()) != header.size())
			return std::nullopt;
		SourceStamp stamp = *found;
		stamp.headerHash = fnv1a(reinterpret_cast<const unsigned char*>(header.data()), header.size());
		return stamp;
	}

	bool sameSizeAndTime(const std::filesystem::path& file, const SourceStamp& stamp)
	{
		const std::optional<SourceStamp> found = sizeAndTime(file);
		return found && found->size == stamp.size && found->modified == stamp.modified;
	}

	void writePeakFile(
		const std::filesystem::path& file, const std::string& source, const SourceStamp& stamp, const PeakData& data)
	{
		if (data.channels < 1 || static_cast<uint32_t>(data.channels) > g_maxChannels || data.levels.empty() ||
			data.levels.size() > g_maxLevels || source.size() > g_maxSourceLength)
			throw PeakFileError("can't write peaks of this shape to " + file.u8string());
		Writer out;
		size_t values = 0;
		for (const Level& level : data.levels)
			values += level.values.size();
		out.reserve(g_fixedHeader + source.size() + (data.levels.size() * g_levelEntry) + (values * 2));
		out.putBytes(g_magic.data(), g_magic.size());
		out.put(g_peakFileVersion, 4);
		out.put(static_cast<uint64_t>(data.channels), 4);
		out.putDouble(data.sampleRate);
		out.put(data.frames, 8);
		out.put(stamp.size, 8);
		out.put(static_cast<uint64_t>(stamp.modified), 8);
		out.put(stamp.headerHash, 8);
		out.put(data.levels.size(), 4);
		out.put(source.size(), 4);
		out.putBytes(source.data(), source.size());
		for (const Level& level : data.levels)
		{
			out.put(level.factor, 4);
			out.put(0, 4);
			out.put(level.peaks, 8);
		}
		for (const Level& level : data.levels)
			for (const int16_t value : level.values)
				out.put(static_cast<uint16_t>(value), 2);

		// A temporary file next to it first, so no reader ever sees a half-written peak file.
		std::filesystem::path temporary = file;
		temporary += "." + hex(std::random_device()()) + ".tmp";
		{
			std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
			stream.write(out.bytes().data(), static_cast<std::streamsize>(out.bytes().size()));
			stream.close();
			if (!stream)
			{
				std::error_code ignored;
				std::filesystem::remove(temporary, ignored);
				throw PeakFileError("can't write " + file.u8string());
			}
		}
		std::error_code error;
		std::filesystem::rename(temporary, file, error);
		if (error)
		{
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			throw PeakFileError("can't write " + file.u8string() + ": " + error.message());
		}
	}

	PeakFile readPeakFile(const std::filesystem::path& file)
	{
		std::ifstream stream(file, std::ios::binary);
		if (!stream)
			throw PeakFileError("can't open " + file.u8string());
		const std::string bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
		Reader in(bytes, file);

		if (in.getBytes(g_magic.size()) != std::string(g_magic.data(), g_magic.size()))
			in.fail("it doesn't start with the magic bytes");
		if (const uint64_t version = in.get(4); version != g_peakFileVersion)
			in.fail("version " + std::to_string(version) + " isn't supported");
		PeakFile contents;
		PeakData& data = contents.data;
		const uint64_t channels = in.get(4);
		if (channels < 1 || channels > g_maxChannels)
			in.fail("it has " + std::to_string(channels) + " channels");
		data.channels = static_cast<int>(channels);
		data.sampleRate = in.getDouble();
		data.frames = in.get(8);
		contents.stamp.size = in.get(8);
		contents.stamp.modified = static_cast<int64_t>(in.get(8));
		contents.stamp.headerHash = in.get(8);
		const uint64_t levels = in.get(4);
		if (levels < 1 || levels > g_maxLevels)
			in.fail("it has " + std::to_string(levels) + " levels");
		const uint64_t sourceLength = in.get(4);
		if (sourceLength > g_maxSourceLength)
			in.fail("its source path is too long");
		contents.source = in.getBytes(static_cast<size_t>(sourceLength));

		uint64_t values = 0;
		for (uint64_t i = 0; i < levels; ++i)
		{
			Level level;
			level.factor = static_cast<uint32_t>(in.get(4));
			in.get(4);
			level.peaks = in.get(8);
			if (level.peaks > in.left())
				in.fail("it is too short");
			if (level.factor == 0 ||
				(i > 0 && (level.factor <= data.levels.back().factor || level.factor % data.levels.back().factor != 0)))
				in.fail("its levels are out of order");
			if (level.peaks != (data.frames / level.factor) + (data.frames % level.factor != 0 ? 1 : 0))
				in.fail("a level's peak count doesn't match its frames");
			values += level.peaks * channels * 2;
			data.levels.push_back(std::move(level));
		}
		if (in.left() != values * 2)
			in.fail(in.left() < values * 2 ? "it is too short" : "it is too long");
		for (Level& level : data.levels)
		{
			level.values.resize(static_cast<size_t>(level.peaks * channels * 2));
			for (int16_t& value : level.values)
				value = static_cast<int16_t>(static_cast<uint16_t>(in.get(2)));
		}
		return contents;
	}

	std::filesystem::path besidePath(const std::filesystem::path& audioFile)
	{
		std::filesystem::path path = audioFile;
		path += ".gbpeaks";
		return path;
	}

	std::filesystem::path fallbackPath(const std::filesystem::path& folder, const std::filesystem::path& audioFile)
	{
		std::error_code error;
		std::filesystem::path absolute = std::filesystem::absolute(audioFile, error);
		if (error)
			absolute = audioFile;
		const std::string key = absolute.lexically_normal().u8string();
		const uint64_t hash = fnv1a(reinterpret_cast<const unsigned char*>(key.data()), key.size());
		return folder / std::filesystem::u8path(audioFile.filename().u8string() + "-" + hex(hash) + ".gbpeaks");
	}

	std::filesystem::path defaultFallbackFolder()
	{
#ifdef _WIN32
		if (const wchar_t* appData = _wgetenv(L"LOCALAPPDATA"); appData != nullptr)
			return std::filesystem::path(appData) / "GNUbase" / "peaks";
#elif defined(__APPLE__)
		if (const char* home = std::getenv("HOME"); home != nullptr)
			return std::filesystem::path(home) / "Library" / "Caches" / "GNUbase" / "peaks";
#else
		if (const char* cache = std::getenv("XDG_CACHE_HOME"); cache != nullptr && *cache != '\0')
			return std::filesystem::path(cache) / "gnubase" / "peaks";
		if (const char* home = std::getenv("HOME"); home != nullptr)
			return std::filesystem::path(home) / ".cache" / "gnubase" / "peaks";
#endif
		std::error_code error;
		return std::filesystem::temp_directory_path(error) / "gnubase" / "peaks";
	}
}
