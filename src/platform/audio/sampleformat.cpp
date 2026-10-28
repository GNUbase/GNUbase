// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/sampleformat.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gnubase::audio
{
	namespace
	{
		// Full scale of the integer formats.
		constexpr double g_int16Scale = 32768.0;
		constexpr double g_int24Scale = 8388608.0;
		constexpr double g_int32Scale = 2147483648.0;

		int32_t quantize(float sample, double scale) noexcept
		{
			return static_cast<int32_t>(
				std::clamp(std::round(static_cast<double>(sample) * scale), -scale, scale - 1.0));
		}

		// One sample at a byte position, as a float.
		float read(const uint8_t* bytes, SampleFormat format) noexcept
		{
			switch (format)
			{
			case SampleFormat::Float32:
			{
				float value = 0.0f;
				std::memcpy(&value, bytes, sizeof(value));
				return value;
			}
			case SampleFormat::Int16:
			{
				int16_t value = 0;
				std::memcpy(&value, bytes, sizeof(value));
				return static_cast<float>(value / g_int16Scale);
			}
			case SampleFormat::Int24:
			{
				const auto value = static_cast<int32_t>(
					(uint32_t{bytes[0]} << 8U) | (uint32_t{bytes[1]} << 16U) | (uint32_t{bytes[2]} << 24U));
				return static_cast<float>(value / g_int32Scale);
			}
			case SampleFormat::Int32:
			{
				int32_t value = 0;
				std::memcpy(&value, bytes, sizeof(value));
				return static_cast<float>(value / g_int32Scale);
			}
			}
			return 0.0f;
		}

		void write(float sample, SampleFormat format, uint8_t* bytes) noexcept
		{
			switch (format)
			{
			case SampleFormat::Float32:
				std::memcpy(bytes, &sample, sizeof(sample));
				return;
			case SampleFormat::Int16:
			{
				const auto value = static_cast<int16_t>(quantize(sample, g_int16Scale));
				std::memcpy(bytes, &value, sizeof(value));
				return;
			}
			case SampleFormat::Int24:
			{
				const auto value = static_cast<uint32_t>(quantize(sample, g_int24Scale));
				bytes[0] = static_cast<uint8_t>(value);
				bytes[1] = static_cast<uint8_t>(value >> 8U);
				bytes[2] = static_cast<uint8_t>(value >> 16U);
				return;
			}
			case SampleFormat::Int32:
			{
				const int32_t value = quantize(sample, g_int32Scale);
				std::memcpy(bytes, &value, sizeof(value));
				return;
			}
			}
		}
	}

	size_t bytesPerSample(SampleFormat format) noexcept
	{
		switch (format)
		{
		case SampleFormat::Int16:
			return 2;
		case SampleFormat::Int24:
			return 3;
		case SampleFormat::Float32:
		case SampleFormat::Int32:
			return 4;
		}
		return 4;
	}

	void toFloat(
		const void* source, SampleFormat format, int channels, uint32_t frames, float* const* destination) noexcept
	{
		const size_t sampleBytes = bytesPerSample(format);
		const auto* bytes = static_cast<const uint8_t*>(source);
		for (uint32_t frame = 0; frame < frames; ++frame)
			for (int c = 0; c < channels; ++c, bytes += sampleBytes)
				destination[c][frame] = read(bytes, format);
	}

	void fromFloat(
		const float* const* source, int channels, uint32_t frames, SampleFormat format, void* destination) noexcept
	{
		const size_t sampleBytes = bytesPerSample(format);
		auto* bytes = static_cast<uint8_t*>(destination);
		for (uint32_t frame = 0; frame < frames; ++frame)
			for (int c = 0; c < channels; ++c, bytes += sampleBytes)
				write(source[c][frame], format, bytes);
	}
}
