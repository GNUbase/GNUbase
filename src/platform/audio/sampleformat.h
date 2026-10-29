// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace gnubase::audio
{
	// Sample formats of drivers, little-endian. Int24 is packed in 3 bytes; Int32 also carries 24-bit samples in its
	// upper bits.
	enum class SampleFormat : uint8_t
	{
		Float32,
		Int16,
		Int24,
		Int32,
	};

	size_t bytesPerSample(SampleFormat format) noexcept;

	// Interleaved driver samples (channels per frame) to one float buffer per channel.
	void toFloat(
		const void* source, SampleFormat format, int channels, uint32_t frames, float* const* destination) noexcept;

	// One float buffer per channel to interleaved driver samples, clipped to the format's range.
	void fromFloat(
		const float* const* source, int channels, uint32_t frames, SampleFormat format, void* destination) noexcept;
}
