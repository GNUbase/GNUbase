// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gnubase::audio
{
	// Input from a capture device with a clock of its own, for an output device that drives processing. One thread
	// writes frames as they arrive and reads them block by block. Reading starts once target frames are buffered; then
	// what is buffered before a read stays around target: when the two clocks drift apart and the smoothed fill strays
	// more than slack frames from it, a read drops or repeats one frame. Too little input is padded with silence (an
	// underrun, after which it fills up to target again), and input that doesn't fit is dropped (an overrun). Allocates
	// in the constructor only.
	//
	// ponytail: drift is corrected by dropping or repeating single frames, inaudible at the usual 10 to 100 ppm of
	// drift; resample if that ever shows.
	class InputFifo
	{
	public:
		InputFifo(int channels, uint32_t capacity, uint32_t target, uint32_t slack = 32)
			: m_samples(static_cast<size_t>(std::max(channels, 0)) * capacity), m_channels(channels),
			  m_capacity(capacity), m_target(std::min(target, capacity)), m_slack(slack)
		{
		}

		void write(const float* const* channels, uint32_t frames) noexcept
		{
			const uint32_t keep = std::min(frames, m_capacity - size());
			if (keep < frames)
				++m_overruns;
			for (uint32_t i = 0; i < keep; ++i)
			{
				const size_t at = (m_written + i) % m_capacity;
				for (int c = 0; c < m_channels; ++c)
					m_samples[(static_cast<size_t>(c) * m_capacity) + at] = channels[c][i];
			}
			m_written += keep;
		}

		void read(float* const* channels, uint32_t frames) noexcept
		{
			const uint32_t available = size();
			if (!m_primed && available >= std::max(m_target, frames))
			{
				m_primed = true;
				m_average = available;
			}
			if (!m_primed || available < frames)
			{
				const uint32_t copied = m_primed ? available : 0;
				copy(channels, copied);
				for (int c = 0; c < m_channels; ++c)
					std::fill(channels[c] + copied, channels[c] + frames, 0.0f);
				m_read += copied;
				if (m_primed)
					++m_underruns;
				m_primed = false;
				return;
			}

			m_average += (available - m_average) * g_smoothing;
			if (m_average > m_target + m_slack && available > frames)
			{
				copy(channels, frames);
				m_read += frames + 1; // drop one
				m_average -= 1.0;
				++m_drops;
			}
			else if (m_average < static_cast<double>(m_target) - m_slack && frames >= 2)
			{
				copy(channels, frames - 1);
				for (int c = 0; c < m_channels; ++c)
					channels[c][frames - 1] = channels[c][frames - 2]; // repeat one
				m_read += frames - 1;
				m_average += 1.0;
				++m_repeats;
			}
			else
			{
				copy(channels, frames);
				m_read += frames;
			}
		}

		uint32_t size() const { return static_cast<uint32_t>(m_written - m_read); }
		size_t underruns() const { return m_underruns; }
		size_t overruns() const { return m_overruns; }
		size_t drops() const { return m_drops; }
		size_t repeats() const { return m_repeats; }

	private:
		static constexpr double g_smoothing = 0.01; // per read: the fill is averaged over about 100 reads

		void copy(float* const* channels, uint32_t frames) const noexcept
		{
			for (uint32_t i = 0; i < frames; ++i)
			{
				const size_t at = (m_read + i) % m_capacity;
				for (int c = 0; c < m_channels; ++c)
					channels[c][i] = m_samples[(static_cast<size_t>(c) * m_capacity) + at];
			}
		}

		std::vector<float> m_samples; // one ring of capacity frames per channel
		int m_channels;
		uint32_t m_capacity;
		uint32_t m_target;
		uint32_t m_slack;
		uint64_t m_written = 0;
		uint64_t m_read = 0;
		bool m_primed = false;
		double m_average = 0.0; // the fill before a read, smoothed
		size_t m_underruns = 0;
		size_t m_overruns = 0;
		size_t m_drops = 0;
		size_t m_repeats = 0;
	};
}
