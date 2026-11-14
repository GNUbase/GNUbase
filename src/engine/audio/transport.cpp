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

	namespace
	{
		// Where playback from stop would start without the pre-roll.
		time::Samples startWithoutPreRoll(const TransportSettings& settings, time::Samples cursor)
		{
			return settings.startMode == StartMode::CycleStart ? settings.left : cursor;
		}

		time::Samples withPreRoll(const TransportSettings& settings, time::Samples start)
		{
			return std::max<time::Samples>(0, start - (settings.preRoll ? settings.preRollAmount : 0));
		}
	}

	time::Samples Transport::recordStart(const TransportSettings& settings, time::Samples start, time::Samples from)
	{
		const time::Samples at = settings.punchIn ? std::max(start, settings.left) : start;
		// A cycle that the pre-roll starts in jumps back before it reaches a record start after it: from there, or
		// from the punch in.
		const bool cycles = settings.cycle && settings.left < settings.right;
		if (cycles && from < settings.right && at >= settings.right)
			return settings.punchIn ? std::max(from, settings.left) : from;
		// One that skips the range from the right to the left locator jumps over a record start in it: at the left.
		const bool skips = settings.cycle && settings.right < settings.left;
		if (skips && from < settings.right && at >= settings.right && at < settings.left)
			return settings.left;
		return at;
	}

	time::Samples Transport::startPosition(time::Samples from) const
	{
		return withPreRoll(m_requested, playStart(from));
	}

	time::Samples Transport::playStart(time::Samples from) const
	{
		return from >= 0 ? from : startWithoutPreRoll(m_requested, position());
	}

	bool Transport::sync()
	{
		if (m_appliedRequests.load(std::memory_order_acquire) != m_sentRequests)
			return false; // what was asked last comes first
		const uint8_t state = m_state.load(std::memory_order_acquire);
		const bool playing = (state & 1U) != 0;
		const bool recording = (state & 2U) != 0;
		const bool countingIn = (state & 4U) != 0;
		if (playing == m_requestedPlaying && recording == m_requestedRecording && countingIn == m_requestedCountingIn)
			return false;
		m_requestedPlaying = playing;
		m_requestedRecording = recording;
		m_requestedCountingIn = countingIn;
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

	bool Transport::play(time::Samples from)
	{
		const time::Samples start = playStart(from);
		if (!send({Request::Type::Play, from, {}, 0}))
			return false;
		if (!m_requestedPlaying)
			m_playbackStart = start;
		m_requestedPlaying = true;
		return true;
	}

	bool Transport::stop()
	{
		if (!send({Request::Type::Stop, 0, {}, 0}))
			return false;
		m_requestedPlaying = false;
		m_requestedRecording = false;
		m_requestedCountingIn = false;
		return true;
	}

	bool Transport::record(bool on, const graph::CountIn& countIn)
	{
		Request request{Request::Type::Record, 0, {}, 0};
		request.record = on;
		request.countIn = countIn;
		if (!send(request))
			return false;
		// What the audio thread will do, unless playback reaches a locator meanwhile; sync() corrects it.
		if (!on && m_requestedCountingIn) // nothing played yet
		{
			m_requestedPlaying = false;
			m_requestedRecording = false;
			m_requestedCountingIn = false;
			return true;
		}
		bool now = !m_requested.punchIn || position() >= m_requested.left; // while playing
		const bool fromStop = !m_requestedPlaying || m_requestedCountingIn;
		if (fromStop)
		{
			m_playbackStart = startWithoutPreRoll(m_requested, position());
			const time::Samples from = startPosition();
			now = from == recordStart(m_requested, startWithoutPreRoll(m_requested, position()), from);
		}
		m_requestedCountingIn = on && (m_requestedCountingIn || (fromStop && countIn.length > 0));
		m_requestedRecording = on && now;
		m_requestedPlaying = m_requestedPlaying || on;
		return true;
	}

	bool Transport::locate(time::Samples position)
	{
		position =
			std::clamp<time::Samples>(position, 0, g_maxPosition); // whoever asks: the audio thread can't overflow
		if (!send({Request::Type::Locate, position, {}, m_locates + 1}))
			return false;
		++m_locates;
		m_sent.store(m_locates, position);
		return true;
	}

	bool Transport::set(const TransportSettings& settings)
	{
		TransportSettings clamped = settings;
		clamped.left = std::clamp<time::Samples>(clamped.left, 0, g_maxPosition);
		clamped.right = std::clamp<time::Samples>(clamped.right, 0, g_maxPosition);
		if (!send({Request::Type::Set, 0, clamped, 0}))
			return false;
		m_requested = clamped;
		return true;
	}

	bool Transport::arrange(const ArrangerState& state, bool pending, uint64_t generation)
	{
		Request request{Request::Type::Arrange, 0, {}, 0};
		request.arranger = state;
		request.record = pending;
		request.arrangerGeneration = generation;
		return send(request);
	}

	ArrangerState Transport::arranger() const
	{
		const auto [a, b] = m_arrangerShared.load();
		return unpackArranger(a, b);
	}

	std::pair<ArrangerState, uint64_t> Transport::arrangerAndGeneration() const
	{
		const auto [a, b] = m_arrangerShared.load();
		return {unpackArranger(a, b), unpackGeneration(a, b)};
	}

	void Transport::locateTo(time::Samples position) noexcept
	{
		m_position = position;
		m_start = position;
		m_inPreRoll = false;
		m_arranger.entering = true;
	}

	const ArrangerPlan* Transport::remapBasis(const ArrangerPlan* plan) const noexcept
	{
		// The state came from the plan played last unless an arrange tagged it with the block's plan. m_plan is safe
		// to read while its generation is the published one: the engine keeps that plan alive for exactly this
		// hand-over, and frees it on the command thread once the transport has moved on (#885).
		if (m_plan != nullptr && m_plan != plan && m_planGeneration == m_stateGeneration)
			return m_plan;
		if (plan != nullptr && plan->generation == m_stateGeneration)
			return plan;
		return nullptr; // no plan to remap from: the position decides
	}

	// Starts playback from the cursor or the left locator, or from where the caller says with from at or after 0 (the
	// arranger's chain start, #880), less the pre-roll; returnToStart comes back to the former.
	void Transport::start(time::Samples from) noexcept
	{
		m_start = from >= 0 ? from : startWithoutPreRoll(m_settings, m_position);
		m_position = withPreRoll(m_settings, m_start);
		m_inPreRoll = m_position < m_start; // the pre-roll plays toward the start
		m_playing = true;
		m_stopAt = -1;
		m_arranger.entering = true;
	}

	// Recording from stop: playback starts as play does, recording where it would start without the pre-roll.
	void Transport::startRecording() noexcept
	{
		start();
		m_recordAt = recordStart(m_settings, m_start, m_position);
		m_recording = m_position == m_recordAt;
		if (m_recording)
			m_recordAt = -1;
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
			if (!m_playing && !m_countingIn)
				start(request.position);
			return;
		case Request::Type::Stop:
			m_countingIn = false;
			m_inPreRoll = false; // playback isn't rolling in anymore
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
			if (m_recordAt >= 0) // waiting for the record start: it moves along, but not before the punch in
				m_recordAt = recordStart(m_settings, request.position, request.position);
			return;
		case Request::Type::Set:
			m_settings = request.settings;
			return;
		case Request::Type::Arrange:
			if (request.record)
			{
				m_arranger.pendingStep = request.arranger.step;
				m_arranger.pendingRepeat = request.arranger.repeat;
				m_arranger.pendingSection = request.arranger.step >= 0 ? -1 : request.arranger.section;
			}
			else
			{
				m_arranger = request.arranger;
				m_arranger.entering = false;
				if (request.arrangerGeneration != 0)
					m_stateGeneration = request.arrangerGeneration; // the state's indices walk that plan
			}
			return;
		case Request::Type::Record:
			if (!request.record)
			{
				m_recording = false;
				m_recordAt = -1;
				m_countingIn = false;
				return;
			}
			m_stopAt = -1; // recording again during the post-roll
			if (m_countingIn)
			{
				if (request.countIn.length > 0) // from where playback now starts
				{
					m_countIn = request.countIn;
					m_countedIn = 0;
				}
				return;
			}
			if (!m_playing && request.countIn.length > 0)
			{
				m_countingIn = true;
				m_countIn = request.countIn;
				m_countedIn = 0;
				return;
			}
			if (!m_playing)
			{
				startRecording();
				return;
			}
			// With punch in before the left locator, the punch in starts recording.
			if (!m_settings.punchIn || m_position >= m_settings.left)
			{
				m_recording = true;
				m_recordAt = -1; // started by hand during the pre-roll: no second start later
			}
			return;
		}
	}

	void Transport::publish(bool changed) noexcept
	{
		// A count-in plays toward a recording: it counts as playing, and as recording if recording starts where
		// playback starts, so recording stays as it is when the count-in ends.
		bool recording = m_recording;
		time::Samples countInStart = -1;
		if (m_countingIn)
		{
			const time::Samples start = startWithoutPreRoll(m_settings, m_position);
			countInStart = withPreRoll(m_settings, start);
			recording = recordStart(m_settings, start, countInStart) == countInStart;
		}
		m_countInStart.store(countInStart, std::memory_order_release);
		if (m_arranger != m_publishedArranger || m_stateGeneration != m_publishedGeneration)
		{
			// Only where playback is counts as a change, not whether it still has to find that out.
			changed = changed || !samePlace(m_arranger, m_publishedArranger);
			m_publishedArranger = m_arranger;
			m_publishedGeneration = m_stateGeneration;
			const auto [a, b] = pack(m_arranger, m_stateGeneration);
			m_arrangerShared.store(a, b);
		}
		const auto state = static_cast<uint8_t>(
			(m_playing || m_countingIn ? 1U : 0U) | (recording ? 2U : 0U) | (m_countingIn ? 4U : 0U));
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

	const graph::TransportBlock& Transport::advance(uint32_t frames, const ArrangerPlan* plan) noexcept
	{
		update();
		const bool arranged = plan != nullptr && plan->plays();
		// Playback enters the chain where it starts without the pre-roll, so that the pre-roll plays into the step. A
		// new plan (a chain edit, another active chain) has other step indices: playback finds its place again.
		if (!arranged)
			m_arranger.entering = false; // linear playback is where it is: arranger mode switched on enters there
		else if (m_arranger.entering)
			m_arranger = arrangerEnter(*plan, remapBasis(plan), m_arranger, m_start);
		else if (plan != m_plan)
			// A new plan finds the step playing again, by the event its indices walk in the plan they come from.
			// During the pre-roll the position is before the start, where no step's event holds it yet, so enter at
			// the start without the pre-roll to keep the chain playing (#885). After it, playback is at the position,
			// even where the chain has played back before the start.
			m_arranger = arrangerEnter(*plan, remapBasis(plan), m_arranger, m_inPreRoll ? m_start : m_position);
		m_plan = plan;
		m_planGeneration = plan != nullptr ? plan->generation : 0;
		m_stateGeneration = m_planGeneration; // every state this block computes walks it
		GNUBASE_ASSERT(frames <= m_segments.size());
		frames = std::min(frames, static_cast<uint32_t>(m_segments.size()));
		if (m_playing && m_stopAt >= 0 && m_position >= m_stopAt)
			autoStop();
		m_block.recording = false;
		m_block.position = m_position;
		m_block.segmentCount = 0;
		m_block.countInFrames = 0;
		m_block.click = m_settings.metronome;
		uint32_t offset = 0;
		if (m_countingIn)
		{
			offset = static_cast<uint32_t>(std::min<time::Samples>(frames, m_countIn.length - m_countedIn));
			m_block.countInFrames = offset;
			m_block.countInPosition = m_countedIn;
			m_block.countIn = m_countIn;
			m_countedIn += offset;
			if (m_countedIn >= m_countIn.length)
			{
				m_countingIn = false;
				startRecording();
				m_block.position = m_position;
			}
		}
		m_block.playing = m_playing && offset < frames;
		if (!m_block.playing)
		{
			m_applied.store(m_locate, m_position);
			publish(false);
			return m_block;
		}

		// In arranger mode the chain decides where playback goes, not the cycle.
		const bool cycles = !arranged && m_settings.cycle && m_settings.left != m_settings.right;
		const bool punchesOut = m_settings.punchOut && !m_settings.lockRecord; // Lock Record ignores it
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
			ArrangerTurn turn;
			if (arranged)
			{
				turn = arrangerNext(*plan, m_arranger, m_position);
				if (turn.at == m_position) // a turn here: no frames before it
				{
					m_arranger = turn.next;
					m_position = turn.to;
					m_inPreRoll = false; // the chain has taken over
					if (turn.stops)
					{
						m_playing = false;
						m_recording = false;
						m_recordAt = -1;
						break;
					}
					continue;
				}
				if (turn.at > m_position && turn.at - m_position <= length)
				{
					length = static_cast<uint32_t>(turn.at - m_position);
					jumps = true;
				}
			}
			else if (cycles && m_position < m_settings.right && m_settings.right - m_position <= length)
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
			if (m_recording && punchesOut)
				until(m_settings.right);
			m_segments[m_block.segmentCount++] = {offset, length, m_position, m_recording};
			m_block.recording = m_block.recording || m_recording;
			offset += length;
			m_position += length;
			if (m_inPreRoll && m_position >= m_start)
				m_inPreRoll = false; // the pre-roll has reached where playback starts
			if (m_recording && punchesOut && m_position == m_settings.right)
			{
				m_recording = false;
				if (m_settings.stopAfterPunchOut && !cycles)
					m_stopAt = m_settings.right + (m_settings.postRoll ? m_settings.postRollAmount : 0);
			}
			if (jumps && arranged)
			{
				m_arranger = turn.next;
				m_position = turn.to;
				m_inPreRoll = false; // the chain has taken over
				if (turn.stops)      // Pause After Repeats: the rest of the block is silent
				{
					m_playing = false;
					m_recording = false;
					m_recordAt = -1;
					break;
				}
			}
			else if (jumps)
			{
				m_position = m_settings.left;
				m_inPreRoll = false; // the cycle jumped: playback isn't rolling in anymore
			}
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
