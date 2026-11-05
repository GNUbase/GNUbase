// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/warp.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace gnubase::engine::dsp
{
	namespace
	{
		constexpr size_t g_blockFrames = 1024;

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

	WarpReader::WarpReader(std::unique_ptr<AudioFileReader> source, std::vector<WarpPoint> map, StretchPreset preset)
		: m_source(std::move(source)), m_map(std::move(map))
	{
		if (m_map.size() < 2)
			throw std::invalid_argument("a warp map needs two points at least");
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
		seekTo(0);
	}

	WarpReader::~WarpReader() = default;

	bool WarpReader::seekTo(time::Samples frame)
	{
		m_stretcher->reset();
		const double source = warpSource(m_map, static_cast<double>(frame));
		const size_t s = segmentOf(m_map, source, &WarpPoint::source);
		m_stretcher->setRatio((m_map[s + 1].target - m_map[s].target) / (m_map[s + 1].source - m_map[s].source));
		m_input = std::llround(source) - static_cast<time::Samples>(m_stretcher->startPad());
		m_drop = m_stretcher->latency();
		m_position = frame;
		m_final = false;
		return true;
	}

	void WarpReader::feed()
	{
		// A ratio takes effect on the input that the stretcher outputs next, which is its start pad behind the input
		// it takes: the ratio of each segment is set that much early.
		const double pad = static_cast<double>(m_stretcher->startPad());
		const double at = static_cast<double>(m_input) - pad;
		const size_t s = segmentOf(m_map, at, &WarpPoint::source);
		const WarpPoint& a = m_map[s];
		const WarpPoint& b = m_map[s + 1];
		m_stretcher->setRatio((b.target - a.target) / (b.source - a.source));
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

	size_t WarpReader::read(float* buffer, size_t frames)
	{
		const auto channels = static_cast<size_t>(m_info.channels);
		size_t done = 0;
		while (done < frames && m_position < m_info.frames)
		{
			const size_t available = m_stretcher->available();
			if (available == 0)
			{
				if (m_final)
					break;
				feed();
				continue;
			}
			const size_t wanted = std::min<size_t>(frames - done, static_cast<size_t>(m_info.frames - m_position));
			const size_t got =
				m_stretcher->retrieve(m_pointers.data(), std::min({available, g_blockFrames, m_drop + wanted}));
			const size_t dropped = std::min(got, m_drop);
			m_drop -= dropped;
			for (size_t f = dropped; f < got; ++f)
				for (size_t c = 0; c < channels; ++c)
					buffer[((done + f - dropped) * channels) + c] = m_planar[c][f];
			done += got - dropped;
			m_position += static_cast<time::Samples>(got - dropped);
		}
		// The stretcher's output may end a little early: silence up to the length.
		const size_t rest = std::min<size_t>(frames - done, static_cast<size_t>(m_info.frames - m_position));
		std::fill_n(buffer + (done * channels), rest * channels, 0.0F);
		m_position += static_cast<time::Samples>(rest);
		return done + rest;
	}
}
