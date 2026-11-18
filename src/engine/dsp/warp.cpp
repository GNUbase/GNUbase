// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/warp.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>

namespace gnubase::engine::dsp
{
	namespace
	{
		constexpr size_t g_blockFrames = 1024;
		constexpr double g_fadeSeconds = 0.005; // the crossfade where the reader starts over while reading on

		// The segment of a map that key falls in: the index of its first point.
		template <typename Key> size_t segmentOf(const std::vector<WarpPoint>& map, double value, Key key)
		{
			const auto next = std::upper_bound(map.begin() + 1, map.end() - 1, value,
				[key](double v, const WarpPoint& point) { return v < point.*key; });
			return static_cast<size_t>(next - map.begin()) - 1;
		}

		double interpolate(
			const WarpPoint& a, const WarpPoint& b, double value, double WarpPoint::* from, double WarpPoint::* to)
		{
			const double span = b.*from - a.*from;
			if (span <= 0.0)
				return a.*to;
			return a.*to + ((value - a.*from) * (b.*to - a.*to) / span);
		}
	}

	double warpTarget(const std::vector<WarpPoint>& map, double source)
	{
		const size_t s = segmentOf(map, source, &WarpPoint::source);
		return interpolate(map[s], map[s + 1], source, &WarpPoint::source, &WarpPoint::target);
	}

	double warpSource(const std::vector<WarpPoint>& map, double target)
	{
		const size_t s = segmentOf(map, target, &WarpPoint::target);
		return interpolate(map[s], map[s + 1], target, &WarpPoint::target, &WarpPoint::source);
	}

	WarpReader::WarpReader(std::unique_ptr<AudioFileReader> source, std::vector<WarpPoint> map, StretchPreset preset,
		const std::vector<PitchPoint>& pitch)
		: m_source(std::move(source)), m_map(std::move(map))
	{
		if (m_map.size() < 2)
			throw std::invalid_argument("a warp map needs two points at least");
		for (const PitchPoint& point : pitch)
			m_pitch.emplace_back(std::llround(point.target), point.semitones);
		m_info = m_source->info();
		m_info.frames = std::max<time::Samples>(0, std::llround(m_map.back().target));
		StretchSettings settings;
		settings.channels = m_info.channels;
		settings.sampleRate = m_info.sampleRate;
		settings.mode = StretchMode::RealTime;
		settings.preset = preset;
		settings.maxBlockFrames = g_blockFrames;
		m_stretcher = std::make_unique<TimeStretcher>(settings);
		const auto channels = static_cast<size_t>(m_info.channels);
		m_interleaved.resize(g_blockFrames * channels);
		m_planar.assign(channels, std::vector<float>(g_blockFrames));
		for (std::vector<float>& channel : m_planar)
			m_pointers.push_back(channel.data());
		m_fade = std::max<size_t>(1, static_cast<size_t>(std::llround(g_fadeSeconds * m_info.sampleRate)));
		m_tail.resize(m_fade * channels);
		m_nextTail.resize(m_fade * channels);
		restart(0);
	}

	WarpReader::~WarpReader() = default;

	bool WarpReader::seekTo(time::Samples frame)
	{
		restart(frame);
		return true;
	}

	double WarpReader::ratio(size_t segment) const
	{
		const WarpPoint& a = m_map[segment];
		const WarpPoint& b = m_map[segment + 1];
		return (b.target - a.target) / (b.source - a.source);
	}

	void WarpReader::restart(time::Samples frame)
	{
		(*m_stretcher).reset();
		const double source = warpSource(m_map, static_cast<double>(frame));
		m_stretcher->setRatio(ratio(segmentOf(m_map, source, &WarpPoint::source)));
		// The stretcher's latency depends on the pitch: it starts over at each pitch point, so that the latency it
		// drops is the one of the pitch it plays.
		double semitones = 0.0;
		m_restartAt = m_info.frames;
		for (const auto& [at, shift] : m_pitch)
		{
			if (at > frame)
			{
				m_restartAt = std::min(at, m_info.frames);
				break;
			}
			semitones = shift;
		}
		m_stretcher->setPitch(semitones, 0.0);
		m_input = std::llround(source) - static_cast<time::Samples>(m_stretcher->startPad());
		m_drop = m_stretcher->latency();
		m_position = frame;
		m_final = false;
		m_faded = m_fade;
		for (size_t s = segmentOf(m_map, static_cast<double>(frame), &WarpPoint::target); s + 1 < m_map.size(); ++s)
			if (const double r = ratio(s); r > g_maxRealtimeRatio || r < g_minRealtimeRatio)
				if (const time::Samples end = std::llround(m_map[s + 1].target); end > frame)
				{
					m_restartAt = std::min(end, m_restartAt);
					break;
				}
	}

	void WarpReader::feed()
	{
		// A ratio takes effect on the input that the stretcher outputs next, which is its start pad behind the input
		// it takes: the ratio of each segment is set that much early.
		const auto pad = static_cast<double>(m_stretcher->startPad());
		const double at = static_cast<double>(m_input) - pad;
		const size_t s = segmentOf(m_map, at, &WarpPoint::source);
		const WarpPoint& b = m_map[s + 1];
		m_stretcher->setRatio(ratio(s));
		size_t frames = std::clamp<size_t>(m_stretcher->inputRequired(), 1, g_blockFrames);
		if (at < b.source && s + 2 < m_map.size())
			frames = std::min(frames, static_cast<size_t>(std::ceil(b.source - at)));
		const auto channels = static_cast<size_t>(m_info.channels);
		size_t silent = 0; // frames before the source's start
		if (m_input < 0)
			silent = std::min(frames, static_cast<size_t>(-m_input));
		std::fill_n(m_interleaved.begin(), silent * channels, 0.0F);
		size_t got = silent;
		if (silent < frames)
		{
			const time::Samples from = m_input + static_cast<time::Samples>(silent);
			if (m_sourceAt != from)
				m_source->seek(std::min(from, m_source->info().frames));
			got += m_source->read(m_interleaved.data() + (silent * channels), frames - silent);
			m_sourceAt = m_input + static_cast<time::Samples>(got);
			m_final = got < frames;
		}
		for (size_t f = 0; f < got; ++f)
			for (size_t c = 0; c < channels; ++c)
				m_planar[c][f] = m_interleaved[(f * channels) + c];
		m_stretcher->process(m_pointers.data(), got, m_final);
		m_input += static_cast<time::Samples>(got);
	}

	size_t WarpReader::pull(float* buffer, size_t frames)
	{
		const auto channels = static_cast<size_t>(m_info.channels);
		size_t done = 0;
		while (done < frames)
		{
			const size_t available = m_stretcher->available();
			if (available == 0)
			{
				if (m_final)
					break;
				feed();
				continue;
			}
			const size_t got =
				m_stretcher->retrieve(m_pointers.data(), std::min({available, g_blockFrames, m_drop + frames - done}));
			const size_t dropped = std::min(got, m_drop);
			m_drop -= dropped;
			for (size_t f = dropped; f < got; ++f)
				for (size_t c = 0; c < channels; ++c)
					buffer[((done + f - dropped) * channels) + c] = m_planar[c][f];
			done += got - dropped;
		}
		return done;
	}

	void WarpReader::fadeIn(float* buffer, size_t frames)
	{
		const auto channels = static_cast<size_t>(m_info.channels);
		for (size_t f = 0; f < frames && m_faded < m_fade; ++f, ++m_faded)
		{
			const float in = (static_cast<float>(m_faded) + 0.5F) / static_cast<float>(m_fade);
			for (size_t c = 0; c < channels; ++c)
			{
				float& sample = buffer[(f * channels) + c];
				sample = (sample * in) + (m_tail[(m_faded * channels) + c] * (1.0F - in));
			}
		}
	}

	size_t WarpReader::read(float* buffer, size_t frames)
	{
		const auto channels = static_cast<size_t>(m_info.channels);
		size_t done = 0;
		while (done < frames && m_position < m_info.frames)
		{
			if (m_position >= m_restartAt)
			{
				// The ended run would play on with other audio than the new one starts with: its next frames fade out
				// over the new run's first, after the rest of a fade still going, and silence after the source's end.
				const size_t tail = pull(m_nextTail.data(), m_fade);
				std::fill(m_nextTail.begin() + static_cast<std::ptrdiff_t>(tail * channels), m_nextTail.end(), 0.0F);
				fadeIn(m_nextTail.data(), m_fade);
				m_tail.swap(m_nextTail);
				restart(m_position);
				m_faded = 0;
				continue;
			}
			const size_t wanted = std::min<size_t>(frames - done, static_cast<size_t>(m_restartAt - m_position));
			float* out = buffer + (done * channels);
			const size_t got = pull(out, wanted);
			fadeIn(out, got);
			done += got;
			m_position += static_cast<time::Samples>(got);
			if (got < wanted)
				break;
		}
		// The stretcher's output may end a little early: silence up to the length.
		const size_t rest = std::min<size_t>(frames - done, static_cast<size_t>(m_info.frames - m_position));
		std::fill_n(buffer + (done * channels), rest * channels, 0.0F);
		m_position += static_cast<time::Samples>(rest);
		return done + rest;
	}
}
