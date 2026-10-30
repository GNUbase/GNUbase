// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/transport.h"

#include "platform/log.h"

#include <algorithm>

namespace gnubase::engine
{
	namespace
	{
		constexpr size_t g_maxRequests = 256;
	}

	// A block has at most one segment per frame: a jump at every frame, with a one-sample cycle.
	Transport::Transport(uint32_t maxFrames) : m_requests(g_maxRequests), m_segments(maxFrames)
	{
		m_block.segments = m_segments.data();
	}

	bool Transport::send(const Request& request)
	{
		return m_requests.tryPush(request);
	}

	time::Samples Transport::position() const
	{
		// The audio thread may apply a locate before its count is stored, so a count it has reached or passed is no
		// longer waiting. Applied counts read after the sent one are at least as new.
		const auto [locates, target] = m_sent.load();
		const auto [applied, cursor] = m_applied.load();
		return applied < locates ? target : cursor;
	}

	bool Transport::play(const graph::CountIn& countIn)
	{
		if (!send({Request::Type::Play, 0, {}, 0, countIn}))
			return false;
		m_requestedPlaying = true;
		return true;
	}

	bool Transport::stop()
	{
		if (!send({Request::Type::Stop, 0, {}, 0, {}}))
			return false;
		m_requestedPlaying = false;
		return true;
	}

	bool Transport::locate(time::Samples position)
	{
		if (!send({Request::Type::Locate, position, {}, m_locates + 1, {}}))
			return false;
		++m_locates;
		m_sent.store(m_locates, position);
		return true;
	}

	bool Transport::set(const TransportSettings& settings)
	{
		if (!send({Request::Type::Set, 0, settings, 0, {}}))
			return false;
		m_requested = settings;
		return true;
	}

	void Transport::locateTo(time::Samples position) noexcept
	{
		m_position = position;
		m_start = position;
	}

	void Transport::apply(const Request& request) noexcept
	{
		switch (request.type)
		{
		case Request::Type::Play:
			if (m_playing || m_countingIn)
				return;
			if (m_settings.startMode == StartMode::CycleStart)
				locateTo(m_settings.left);
			m_start = m_position;
			m_countingIn = request.countIn.length > 0;
			m_countIn = request.countIn;
			m_countedIn = 0;
			m_playing = !m_countingIn;
			return;
		case Request::Type::Stop:
			m_countingIn = false;
			if (!m_playing)
				return;
			m_playing = false;
			if (m_settings.returnToStart)
				m_position = m_start;
			return;
		case Request::Type::Locate:
			locateTo(request.position);
			m_locate = request.locate;
			return;
		case Request::Type::Set:
			m_settings = request.settings;
			return;
		}
	}

	void Transport::update() noexcept
	{
		Request request;
		while (m_requests.tryPop(request))
			apply(request);
		m_applied.store(m_locate, m_position);
	}

	const graph::TransportBlock& Transport::advance(uint32_t frames) noexcept
	{
		update();
		GNUBASE_ASSERT(frames <= m_segments.size());
		frames = std::min(frames, static_cast<uint32_t>(m_segments.size()));
		m_block.position = m_position;
		m_block.segmentCount = 0;
		m_block.countInFrames = 0;
		uint32_t offset = 0;
		if (m_countingIn)
		{
			offset = static_cast<uint32_t>(std::min<time::Samples>(frames, m_countIn.length - m_countedIn));
			m_block.countInFrames = offset;
			m_block.countInPosition = m_countedIn;
			m_block.countIn = m_countIn;
			m_countedIn += offset;
			m_countingIn = m_countedIn < m_countIn.length;
			m_playing = !m_countingIn;
		}
		m_block.playing = m_playing && offset < frames;
		if (!m_block.playing)
			return m_block;

		const bool cycles = m_settings.cycle && m_settings.left != m_settings.right;
		while (offset < frames)
		{
			uint32_t length = frames - offset;
			bool jumps = false;
			if (cycles && m_position < m_settings.right && m_settings.right - m_position <= length)
			{
				length = static_cast<uint32_t>(m_settings.right - m_position);
				jumps = true;
			}
			m_segments[m_block.segmentCount++] = {offset, length, m_position};
			offset += length;
			m_position = jumps ? m_settings.left : m_position + length;
		}
		m_applied.store(m_locate, m_position);
		return m_block;
	}
}
