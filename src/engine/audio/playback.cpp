// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/playback.h"

#include "engine/audiofile/audiofile.h"
#include "engine/document/rendersnapshot.h"

#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		// The entry playing at position, or null.
		const PlaylistEntry* entryAt(const Playlist& playlist, time::Samples position)
		{
			const auto entry = std::upper_bound(playlist.begin(), playlist.end(), position,
				[](time::Samples at, const PlaylistEntry& e) { return at < e.end; });
			return entry != playlist.end() && entry->start <= position ? &*entry : nullptr;
		}

		// A voice's state (Playback::Voice): waiting after its nth cue, read in a block (blocks count from 1), or
		// being repositioned by a cue.
		constexpr uint64_t g_cueing = ~uint64_t{0};

		constexpr uint64_t waitingAfter(uint64_t cues)
		{
			return (cues << 1U) | 1U;
		}

		constexpr uint64_t readIn(uint64_t block)
		{
			return block << 1U;
		}

		constexpr bool isWaiting(uint64_t state)
		{
			return (state & 1U) != 0 && state != g_cueing;
		}

		constexpr uint64_t blockOf(uint64_t state)
		{
			return state >> 1U;
		}
	}

	Playlist makePlaylist(const RenderSnapshot& snapshot, const RenderChannel& channel)
	{
		std::vector<const RenderAudioEvent*> events;
		for (const RenderAudioEvent& event : channel.audioEvents)
			if (!event.muted && event.media >= 0 && static_cast<size_t>(event.media) < snapshot.media.size())
				events.push_back(&event);
		std::sort(events.begin(), events.end(),
			[](const RenderAudioEvent* a, const RenderAudioEvent* b) { return a->id < b->id; });

		// Each event covers what the events before it play where they overlap. Quadratic in the number of a track's
		// overlapping events, which stays small.
		Playlist playlist;
		for (const RenderAudioEvent* event : events)
		{
			const RenderMedia& media = snapshot.media[static_cast<size_t>(event->media)];
			const double ratio = snapshot.sampleRate / media.sampleRate;
			const auto length = static_cast<time::Samples>(std::llround(static_cast<double>(event->length) * ratio));
			if (length <= 0)
				continue;
			const PlaylistEntry entry{event->start, event->start + length, media.id,
				static_cast<time::Samples>(std::llround(static_cast<double>(event->offset) * ratio)), event->gain,
				std::max(1, media.channels)};
			Playlist covered;
			for (const PlaylistEntry& other : playlist)
			{
				if (other.end <= entry.start || other.start >= entry.end)
				{
					covered.push_back(other);
					continue;
				}
				if (other.start < entry.start)
				{
					PlaylistEntry before = other;
					before.end = entry.start;
					covered.push_back(before);
				}
				if (other.end > entry.end)
				{
					PlaylistEntry after = other;
					after.start = entry.end;
					after.frame = other.frame + (entry.end - other.start);
					covered.push_back(after);
				}
			}
			covered.push_back(entry);
			std::sort(covered.begin(), covered.end(),
				[](const PlaylistEntry& a, const PlaylistEntry& b) { return a.start < b.start; });
			playlist = std::move(covered);
		}
		return playlist;
	}

	Playback::Playback(size_t voicesPerTrack, const DiskStreamSettings& streams)
		: m_voicesPerTrack(voicesPerTrack), m_streamSettings(streams), m_tables(std::make_unique<const VoiceTable>())
	{
	}

	Playback::~Playback() = default;

	void Playback::setTracks(std::vector<Track> tracks, std::map<ObjectId, std::string> mediaPaths, double sampleRate)
	{
		const std::scoped_lock lock(m_mutex);
		m_tracks = std::move(tracks);
		m_mediaPaths = std::move(mediaPaths);
		m_unreadable.clear();
		if (sampleRate != m_sampleRate)
		{
			// Streams convert to one sample rate; open new ones for the new rate.
			for (const auto& voice : m_voices)
				retire(voice);
			m_voices.clear();
			m_tables.publish(std::make_unique<const VoiceTable>());
		}
		m_sampleRate = sampleRate;
	}

	std::vector<Playback::Point> Playback::points(const Playlist& playlist, const Cue& cue) const
	{
		std::vector<Point> result;
		const TransportSettings& settings = cue.settings;
		const bool cycles = settings.cycle && settings.left != settings.right;
		time::Samples position = cue.position;
		time::Samples remaining = cue.window;
		bool jumped = false;
		bool lapped = false; // past a cycle jump, where entries behind the cursor play again
		while (remaining > 0 && result.size() < m_voicesPerTrack)
		{
			const bool bounded = cycles && position < settings.right;
			const time::Samples end = bounded ? std::min(settings.right, position + remaining) : position + remaining;
			auto entry = std::upper_bound(playlist.begin(), playlist.end(), position,
				[](time::Samples at, const PlaylistEntry& e) { return at < e.end; });
			for (; entry != playlist.end() && entry->start < end && result.size() < m_voicesPerTrack; ++entry)
			{
				const time::Samples from = std::max(position, entry->start);
				// After a cycle jump playback enters wherever the left locator is. Otherwise reaching an entry's
				// start plays it from a new voice, unless the one before ends there and its clip goes on: then the
				// same voice reads on. The entry at the cursor itself is cue()'s.
				bool starts = from == entry->start && (lapped || from > cue.position);
				if (starts && entry != playlist.begin())
				{
					const PlaylistEntry& before = *(entry - 1);
					starts = before.end != entry->start || before.media != entry->media ||
						before.frame + (before.end - before.start) != entry->frame;
				}
				if ((jumped && from == position) || starts)
					result.push_back({entry->media, entry->frame + (from - entry->start)});
			}
			remaining -= end - position;
			jumped = bounded && end == settings.right;
			lapped = lapped || jumped;
			position = jumped ? settings.left : end;
		}
		return result;
	}

	time::Samples Playback::ahead(const Cue& cue, time::Samples frames)
	{
		const TransportSettings& settings = cue.settings;
		const bool cycles = settings.cycle && settings.left != settings.right;
		time::Samples position = cue.position;
		while (frames > 0)
		{
			if (cycles && position < settings.right && settings.right - position <= frames)
			{
				frames -= settings.right - position;
				position = settings.left;
				if (settings.right - settings.left <= 0)
					return position + frames; // the range is skipped: from the left locator on, no more jumps
				continue;
			}
			return position + frames;
		}
		return position;
	}

	bool Playback::claim(Voice& voice, uint64_t seen)
	{
		return voice.state.compare_exchange_strong(seen, g_cueing, std::memory_order_acq_rel);
	}

	void Playback::place(Voice& voice, time::Samples frame)
	{
		voice.stream->seek(frame);
		voice.cueFrame.store(frame, std::memory_order_relaxed);
		voice.state.store(waitingAfter(++voice.cues), std::memory_order_release);
	}

	std::shared_ptr<Playback::Voice> Playback::open(ObjectId channel, ObjectId media, time::Samples frame)
	{
		const auto path = m_mediaPaths.find(media);
		if (path == m_mediaPaths.end() || m_unreadable.count(media) != 0)
			return nullptr;
		std::unique_ptr<AudioFileReader> reader;
		try
		{
			reader = AudioFileReader::open(path->second);
		}
		catch (const AudioFileError&)
		{
			m_unreadable.insert(media); // plays as silence
			return nullptr;
		}
		if (!m_streamer)
			m_streamer = std::make_unique<DiskStreamer>();
		auto voice = std::make_shared<Voice>();
		voice->channel = channel;
		voice->media = media;
		voice->stream = m_streamer->open(std::move(reader), m_sampleRate, m_streamSettings);
		place(*voice, frame);
		return voice;
	}

	void Playback::retire(const std::shared_ptr<Voice>& voice)
	{
		m_retiredDropouts += voice->stream->dropouts();
		m_streamer->close(voice->stream); // the audio thread may still read it until the next table
	}

	void Playback::cue(const Cue& cue)
	{
		const std::scoped_lock lock(m_mutex);
		const uint64_t now = m_block.load(std::memory_order_acquire);
		bool changed = false;

		// Voices of tracks that are gone.
		const auto kept = std::stable_partition(m_voices.begin(), m_voices.end(),
			[this](const std::shared_ptr<Voice>& voice)
			{
				return std::any_of(m_tracks.begin(), m_tracks.end(),
					[&voice](const Track& track) { return track.channel == voice->channel; });
			});
		for (auto voice = kept; voice != m_voices.end(); ++voice)
			retire(*voice);
		if (kept != m_voices.end())
		{
			m_voices.erase(kept, m_voices.end());
			changed = true;
		}

		// What every voice was doing, read once. The audio thread goes on meanwhile and may start reading a voice
		// that looks free here: repositioning a voice claims it from exactly the state read here, which fails then.
		struct State
		{
			uint64_t seen = 0;
			bool started = false;       // read since its last cue
			time::Samples next = 0;     // where it reads on, once started
			time::Samples cueFrame = 0; // where it waits, until then
			uint64_t lastBlock = 0;     // the block it was last read in, once started
		};
		const auto waitingAt = [](uint64_t seen, time::Samples frame) { return State{seen, false, frame, frame, 0}; };
		std::vector<State> states;
		states.reserve(m_voices.size());
		for (const auto& voice : m_voices)
		{
			State state;
			state.seen = voice->state.load(std::memory_order_acquire);
			state.started = !isWaiting(state.seen);
			state.lastBlock = state.started ? blockOf(state.seen) : 0;
			state.next = voice->next.load(std::memory_order_relaxed);
			state.cueFrame = voice->cueFrame.load(std::memory_order_relaxed);
			states.push_back(state);
		}
		voicesRead();

		for (const Track& track : m_tracks)
		{
			std::vector<size_t> mine; // indices into m_voices
			for (size_t i = 0; i < m_voices.size(); ++i)
				if (m_voices[i]->channel == track.channel)
					mine.push_back(i);
			// A voice the audio thread is reading from stays where it is.
			std::vector<bool> taken(m_voices.size(), false);
			for (const size_t i : mine)
				taken[i] = cue.playing && states[i].started && states[i].lastBlock + 2 > now;

			std::vector<Point> unmet;

			// The entry at the cursor: while playing, a voice reading near it or waiting just ahead of it has it.
			// Otherwise it needs one: while stopped exactly there, while playing a little ahead, so the voice is
			// ready by the time playback gets there.
			const Playlist& playlist = *track.playlist;
			const auto slack = static_cast<time::Samples>(m_sampleRate / 10.0);
			if (const PlaylistEntry* at = entryAt(playlist, cue.position))
			{
				const time::Samples frame = at->frame + (cue.position - at->start);
				const bool covered = cue.playing &&
					std::any_of(mine.begin(), mine.end(),
						[&](size_t i)
						{
							if (m_voices[i]->media != at->media)
								return false;
							const time::Samples next = taken[i] ? states[i].next : states[i].cueFrame;
							return std::abs(next - frame) <= 2 * slack;
						});
				if (covered)
				{
					for (const size_t i : mine)
						if (!taken[i] && m_voices[i]->media == at->media &&
							std::abs(states[i].cueFrame - frame) <= 2 * slack)
							taken[i] = true; // keep the voice waiting there
				}
				else if (!cue.playing)
				{
					const auto waiting = std::find_if(mine.begin(), mine.end(),
						[&](size_t i)
						{
							return m_voices[i]->media == at->media &&
								(states[i].started ? states[i].next : states[i].cueFrame) == frame;
						});
					if (waiting != mine.end())
						taken[*waiting] = true;
					else
						unmet.push_back({at->media, frame});
				}
				else
				{
					Cue later = cue;
					later.position = ahead(cue, slack);
					if (const PlaylistEntry* next = entryAt(playlist, later.position))
						unmet.push_back({next->media, next->frame + (later.position - next->start)});
				}
			}

			// Points a voice already waits at keep it.
			for (const Point& point : points(playlist, cue))
			{
				const auto waiting = std::find_if(mine.begin(), mine.end(),
					[&](size_t i)
					{
						return !taken[i] && m_voices[i]->media == point.media && !states[i].started &&
							states[i].cueFrame == point.frame;
					});
				if (waiting != mine.end())
					taken[*waiting] = true;
				else
					unmet.push_back(point);
			}

			// A free voice that the audio thread hasn't started reading since its state was read: claimed for the
			// cue, or the end of mine.
			const auto claimFree = [&](bool sameMedia, ObjectId media)
			{
				for (;;)
				{
					const auto free = std::find_if(mine.begin(), mine.end(),
						[&](size_t i) { return !taken[i] && (!sameMedia || m_voices[i]->media == media); });
					if (free == mine.end())
						return free;
					taken[*free] = true;
					if (claim(*m_voices[*free], states[*free].seen))
						return free;
				}
			};

			// The others get a free voice: one of the same clip if there is one, else a new stream.
			for (const Point& point : unmet)
			{
				auto free = claimFree(true, point.media);
				if (free != mine.end())
				{
					Voice& voice = *m_voices[*free];
					place(voice, point.frame);
					states[*free] = waitingAt(voice.state.load(std::memory_order_relaxed), point.frame);
					continue;
				}
				free = claimFree(false, point.media);
				if (free == mine.end() && mine.size() >= m_voicesPerTrack)
					continue; // every voice is busy; the point gets one in a later cue
				std::shared_ptr<Voice> voice = open(track.channel, point.media, point.frame);
				if (!voice)
				{
					if (free != mine.end())
						m_voices[*free]->state.store(states[*free].seen, std::memory_order_release); // unclaimed
					continue;
				}
				const State state = waitingAt(voice->state.load(std::memory_order_relaxed), point.frame);
				if (free != mine.end())
				{
					retire(m_voices[*free]); // stays claimed: the audio thread leaves it alone until the next table
					m_voices[*free] = std::move(voice);
					states[*free] = state;
				}
				else
				{
					mine.push_back(m_voices.size());
					m_voices.push_back(std::move(voice));
					states.push_back(state);
					taken.push_back(true);
				}
				changed = true;
			}
		}

		if (changed)
		{
			std::stable_sort(m_voices.begin(), m_voices.end(),
				[](const std::shared_ptr<Voice>& a, const std::shared_ptr<Voice>& b)
				{ return a->channel < b->channel; });
			auto table = std::make_unique<VoiceTable>();
			table->voices = m_voices;
			m_tables.publish(std::move(table));
		}
	}

	bool Playback::waitUntilBuffered(std::chrono::milliseconds timeout)
	{
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for (;;)
		{
			bool buffered = true;
			{
				const std::scoped_lock lock(m_mutex);
				for (const auto& voice : m_voices)
					buffered = buffered && (voice->stream->full() || voice->stream->failed());
			}
			if (buffered)
				return true;
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
	}

	uint64_t Playback::dropouts() const
	{
		const std::scoped_lock lock(m_mutex);
		uint64_t total = m_retiredDropouts + m_missedFrames.load(std::memory_order_relaxed);
		for (const auto& voice : m_voices)
			total += voice->stream->dropouts();
		return total;
	}

	void Playback::beginBlock() noexcept
	{
		m_block.fetch_add(1, std::memory_order_acq_rel);
		m_table = &m_tables.acquire();
	}

	void Playback::endBlock() noexcept
	{
		m_tables.release();
		m_table = nullptr;
	}

	Playback::Voice* Playback::take(
		ObjectId channel, ObjectId media, time::Samples frame, time::Samples frames, time::Samples& lead) noexcept
	{
		if (m_table == nullptr)
			return nullptr;
		const auto& voices = m_table->voices;
		auto voice = std::lower_bound(voices.begin(), voices.end(), channel,
			[](const std::shared_ptr<Voice>& v, ObjectId id) { return v->channel < id; });
		for (; voice != voices.end() && (*voice)->channel == channel; ++voice)
		{
			Voice& v = **voice;
			if (v.media != media)
				continue;
			uint64_t state = v.state.load(std::memory_order_acquire);
			if (state == g_cueing)
				continue;
			time::Samples next = 0;
			if (isWaiting(state))
			{
				// Until its first read after a cue, a voice is only there once its stream has the frames.
				if (!v.stream->ready())
					continue;
				next = v.cueFrame.load(std::memory_order_relaxed);
			}
			else
			{
				next = v.next.load(std::memory_order_relaxed);
			}
			if (next < frame || next >= frame + frames)
				continue;
			// Read from it, unless a cue has just claimed it.
			v.next.store(next, std::memory_order_relaxed);
			if (!v.state.compare_exchange_strong(state, readIn(block()), std::memory_order_acq_rel))
				continue;
			lead = next - frame;
			return &v;
		}
		return nullptr;
	}

	void Playback::missed(time::Samples frames) noexcept
	{
		m_missedFrames.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
	}

	TrackPlayer::TrackPlayer(std::shared_ptr<const Playlist> playlist, ObjectId channel, Playback& playback)
		: m_playlist(std::move(playlist)), m_channel(channel), m_playback(playback)
	{
	}

	void TrackPlayer::prepare(double /*sampleRate*/, uint32_t maxFrames)
	{
		int channels = 1;
		for (const PlaylistEntry& entry : *m_playlist)
			channels = std::max(channels, entry.channels);
		m_bufferFrames = maxFrames;
		m_buffer.assign(static_cast<size_t>(maxFrames) * static_cast<size_t>(channels), 0.0f);
	}

	void TrackPlayer::process(const graph::ProcessContext& context) noexcept
	{
		const graph::TransportBlock& transport = context.transport;
		if (!transport.playing)
			return;
		const Playlist& playlist = *m_playlist;
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			const time::Samples begin = segment.position;
			const time::Samples end = begin + segment.frames;
			auto entry = std::upper_bound(playlist.begin(), playlist.end(), begin,
				[](time::Samples at, const PlaylistEntry& e) { return at < e.end; });
			for (; entry != playlist.end() && entry->start < end; ++entry)
			{
				const time::Samples from = std::max(begin, entry->start);
				const time::Samples to = std::min(end, entry->end);
				play(context, *entry, from, to - from, segment.offset + static_cast<uint32_t>(from - begin));
			}
		}
	}

	void TrackPlayer::play(const graph::ProcessContext& context, const PlaylistEntry& entry, time::Samples position,
		time::Samples frames, uint32_t offset) noexcept
	{
		const time::Samples frame = entry.frame + (position - entry.start);
		time::Samples lead = 0;
		Playback::Voice* voice = m_playback.take(m_channel, entry.media, frame, frames, lead);
		if (voice == nullptr)
		{
			m_playback.missed(frames);
			return;
		}
		if (lead > 0)
			m_playback.missed(lead);
		const auto count = static_cast<size_t>(std::min(frames - lead, static_cast<time::Samples>(m_bufferFrames)));
		const int channels = voice->stream->channels();
		if (static_cast<size_t>(channels) * count > m_buffer.size())
			return; // a clip with more channels than prepared for; the next graph has room
		voice->stream->read(m_buffer.data(), count);
		voice->next.store(frame + lead + static_cast<time::Samples>(count), std::memory_order_relaxed);

		// One to one, a mono clip on every channel, extra clip channels unused.
		const auto start = offset + static_cast<uint32_t>(lead);
		for (int c = 0; c < context.channels; ++c)
		{
			const int source = channels == 1 ? 0 : c;
			if (source >= channels)
				continue;
			float* out = context.audio[c] + start;
			for (size_t i = 0; i < count; ++i)
				out[i] += m_buffer[(i * static_cast<size_t>(channels)) + static_cast<size_t>(source)] * entry.gain;
		}
	}
}
