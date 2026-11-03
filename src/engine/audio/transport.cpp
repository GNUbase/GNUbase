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
		if (!m_requests.tryPush(request))
			return false;
		++m_sentRequests;
		return true;
	}

	time::Samples Transport::startPosition() const
	{
		const time::Samples from = m_requested.startMode == StartMode::CycleStart ? m_requested.left : position();
		return std::max<time::Samples>(0, from - (m_requested.preRoll ? m_requested.preRollAmount : 0));
	}

	bool Transport::sync()
	{
		if (m_appliedRequests.load(std::memory_order_acquire) != m_sentRequests)
			return false; // what was asked last comes first
		const uint8_t state = m_state.load(std::memory_order_acquire);
		const bool playing = (state & 1U) != 0;
		const bool recording = (state & 2U) != 0;
		if (playing == m_requestedPlaying && recording == m_requestedRecording)
			return false;
		m_requestedPlaying = playing;
		m_requestedRecording = recording;
		return true;
	}

	time::Samples Transport::position() const
	{
		// The audio thread may apply a locate before its count is stored, so a count it has reached or passed is no
		// longer waiting. Applied counts read after the sent one are at least as new.
		const auto [locates, target] = m_sent.load();
		const auto [applied, cursor] = m_applied.load();
		return applied < locates ? target : cursor;
	}

	bool Transport::play()
	{
		if (!send({Request::Type::Play, 0, {}, 0}))
			return false;
		m_requestedPlaying = true;
		return true;
	}

	bool Transport::stop()
	{
		if (!send({Request::Type::Stop, 0, {}, 0}))
			return false;
		m_requestedPlaying = false;
		m_requestedRecording = false;
		return true;
	}

	bool Transport::record(bool on)
	{
		Request request{Request::Type::Record, 0, {}, 0};
		request.record = on;
		if (!send(request))
			return false;
		// What the audio thread will do, unless playback reaches a locator meanwhile; sync() corrects it.
		const time::Samples from = m_requestedPlaying ? position() : startPosition();
		const bool waits = m_requested.punchIn && from < m_requested.left;
		const bool rollsBack = !m_requestedPlaying && m_requested.preRoll && m_requested.preRollAmount > 0;
		m_requestedRecording = on && !waits && !rollsBack;
		m_requestedPlaying = m_requestedPlaying || on;
		return true;
	}

	bool Transport::locate(time::Samples position)
	{
		if (!send({Request::Type::Locate, position, {}, m_locates + 1}))
			return false;
		++m_locates;
		m_sent.store(m_locates, position);
		return true;
	}

	bool Transport::set(const TransportSettings& settings)
	{
		if (!send({Request::Type::Set, 0, settings, 0}))
			return false;
		m_requested = settings;
		return true;
	}

	void Transport::locateTo(time::Samples position) noexcept
	{
		m_position = position;
		m_start = position;
	}

	// Starts playback from the cursor or the left locator, less the pre-roll; returnToStart comes back to the former.
	void Transport::start() noexcept
	{
		if (m_settings.startMode == StartMode::CycleStart)
			m_position = m_settings.left;
		m_start = m_position;
		m_position = std::max<time::Samples>(0, m_position - (m_settings.preRoll ? m_settings.preRollAmount : 0));
		m_playing = true;
		m_stopAt = -1;
	}

	void Transport::autoStop() noexcept
	{
		m_playing = false;
		m_recording = false;
		m_recordAt = -1;
		m_position = m_settings.returnToStart ? m_start : m_stopAt;
		m_stopAt = -1;
	}

	void Transport::apply(const Request& request) noexcept
	{
		switch (request.type)
		{
		case Request::Type::Play:
			if (!m_playing)
				start();
			return;
		case Request::Type::Stop:
			if (!m_playing)
				return;
			m_playing = false;
			m_recording = false;
			m_recordAt = -1;
			m_stopAt = -1;
			if (m_settings.returnToStart)
				m_position = m_start;
			return;
		case Request::Type::Locate:
			locateTo(request.position);
			m_locate = request.locate;
			m_stopAt = -1;
			return;
		case Request::Type::Set:
			m_settings = request.settings;
			return;
		case Request::Type::Record:
			if (!request.record)
			{
				m_recording = false;
				m_recordAt = -1;
				return;
			}
			if (!m_playing)
			{
				start();
				m_recordAt = m_settings.punchIn ? std::max(m_start, m_settings.left) : m_start;
				m_recording = m_position == m_recordAt;
				if (m_recording)
					m_recordAt = -1;
				return;
			}
			// With punch in before the left locator, the punch in starts recording.
			if (!m_settings.punchIn || m_position >= m_settings.left)
				m_recording = true;
			return;
		}
	}

	void Transport::publish(bool changed) noexcept
	{
		const auto state = static_cast<uint8_t>((m_playing ? 1U : 0U) | (m_recording ? 2U : 0U));
		if (state == m_published && !changed)
			return;
		m_published = state;
		m_state.store(state, std::memory_order_release);
		m_changes.store(m_changes.load(std::memory_order_relaxed) + 1, std::memory_order_release);
	}

	void Transport::update() noexcept
	{
		Request request;
		uint64_t applied = 0;
		while (m_requests.tryPop(request))
		{
			apply(request);
			++applied;
		}
		m_applied.store(m_locate, m_position);
		if (applied == 0)
			return;
		// The state first: a sync that sees every request applied also sees what they did.
		publish(true);
		m_appliedRequests.store(m_appliedRequests.load(std::memory_order_relaxed) + applied, std::memory_order_release);
	}

	const graph::TransportBlock& Transport::advance(uint32_t frames) noexcept
	{
		update();
		GNUBASE_ASSERT(frames <= m_segments.size());
		frames = std::min(frames, static_cast<uint32_t>(m_segments.size()));
		if (m_playing && m_stopAt >= 0 && m_position >= m_stopAt)
			autoStop();
		m_block.playing = m_playing;
		m_block.recording = false;
		m_block.position = m_position;
		m_block.segmentCount = 0;
		if (!m_playing)
		{
			m_applied.store(m_locate, m_position);
			publish(false);
			return m_block;
		}

		const bool cycles = m_settings.cycle && m_settings.left != m_settings.right;
		uint32_t offset = 0;
		while (offset < frames)
		{
			// Punching in where recording from stop starts or at the left locator.
			if (!m_recording && (m_position == m_recordAt || (m_settings.punchIn && m_position == m_settings.left)))
			{
				m_recording = true;
				m_recordAt = -1;
			}
			uint32_t length = frames - offset;
			bool jumps = false;
			if (cycles && m_position < m_settings.right && m_settings.right - m_position <= length)
			{
				length = static_cast<uint32_t>(m_settings.right - m_position);
				jumps = true;
			}
			// The segment ends where recording punches in or out.
			const auto until = [this, &length, &jumps](time::Samples point)
			{
				if (point > m_position && point - m_position < length)
				{
					length = static_cast<uint32_t>(point - m_position);
					jumps = false;
				}
			};
			if (!m_recording && m_recordAt >= 0)
				until(m_recordAt);
			if (!m_recording && m_settings.punchIn)
				until(m_settings.left);
			if (m_recording && m_settings.punchOut)
				until(m_settings.right);
			m_segments[m_block.segmentCount++] = {offset, length, m_position, m_recording};
			m_block.recording = m_block.recording || m_recording;
			offset += length;
			m_position += length;
			if (m_recording && m_settings.punchOut && m_position == m_settings.right)
			{
				m_recording = false;
				if (m_settings.stopAfterPunchOut && !cycles)
					m_stopAt = m_settings.right + (m_settings.postRoll ? m_settings.postRollAmount : 0);
			}
			if (jumps)
				m_position = m_settings.left;
		}
		m_applied.store(m_locate, m_position);
		publish(false);
		return m_block;
	}

	void PlayedPositions::add(uint64_t start, uint32_t frames, const graph::TransportBlock& transport) noexcept
	{
		const size_t size = m_positions.size();
		if (size == 0)
			return;
		for (uint32_t i = 0; i < frames; ++i)
		{
			m_positions[(start + i) % size] = -1;
			m_recording[(start + i) % size] = 0;
		}
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			for (uint32_t i = 0; i < segment.frames; ++i)
			{
				m_positions[(start + segment.offset + i) % size] = segment.position + i;
				m_recording[(start + segment.offset + i) % size] = segment.recording ? 1 : 0;
			}
		}
		m_end = static_cast<int64_t>(start + frames);
	}

	time::Samples PlayedPositions::at(int64_t frame) const noexcept
	{
		const auto size = static_cast<int64_t>(m_positions.size());
		if (frame < 0 || frame >= m_end || frame < m_end - size)
			return -1;
		return m_positions[static_cast<size_t>(frame % size)];
	}

	time::Samples PlayedPositions::recordedAt(int64_t frame) const noexcept
	{
		const time::Samples position = at(frame);
		return position >= 0 && m_recording[static_cast<size_t>(frame % static_cast<int64_t>(m_recording.size()))] != 0
			? position
			: -1;
	}
}
