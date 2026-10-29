// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/playback.h"

#include "engine/audio/fades.h"
#include "engine/audiofile/audiofile.h"
#include "engine/document/rendersnapshot.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <thread>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		// The entry playing at position, or null.
		const PlaylistEntry* entryAt(const PlaylistLayer& layer, time::Samples position)
		{
			const auto entry = std::upper_bound(layer.begin(), layer.end(), position,
				[](time::Samples at, const PlaylistEntry& e) { return at < e.end; });
			return entry != layer.end() && entry->start <= position ? &*entry : nullptr;
		}

		// An entry of any layer that ends where entry starts and whose clip entry continues, other than the ones
		// already continued, or null.
		const PlaylistEntry* continued(
			const Playlist& playlist, const PlaylistEntry& entry, const std::vector<const PlaylistEntry*>& taken)
		{
			for (const PlaylistLayer& layer : playlist.layers)
			{
				const auto before = std::lower_bound(layer.begin(), layer.end(), entry.start,
					[](const PlaylistEntry& e, time::Samples at) { return e.end < at; });
				if (before != layer.end() && before->end == entry.start && before->media == entry.media &&
					before->frame + (before->end - before->start) == entry.frame &&
					std::find(taken.begin(), taken.end(), &*before) == taken.end())
					return &*before;
			}
			return nullptr;
		}

		time::Samples scaled(time::Samples frames, double ratio)
		{
			return static_cast<time::Samples>(std::llround(static_cast<double>(frames) * ratio));
		}

		// An event on the track, in the project's samples, with the fades at its start and end.
		struct Placed
		{
			const RenderAudioEvent* event = nullptr;
			const RenderMedia* media = nullptr;
			double ratio = 1.0; // project samples per clip frame
			time::Samples start = 0;
			time::Samples end = 0;
			time::Samples frame = 0; // the clip's frame at start, at the project's sample rate
			std::optional<GainRamp> fadeIn;
			std::optional<GainRamp> fadeOut;
			bool crossfadeIn = false; // the fade in belongs to a crossfade
			bool crossfadeOut = false;
		};

		// Where two events play together: out fades out and in fades in. Fades that only apply where an event plays
		// behind the other one come with the transition; the others are the events' own.
		struct Transition
		{
			size_t out = 0; // indices of Placed
			size_t in = 0;
			time::Samples start = 0;
			time::Samples end = 0;
			std::optional<GainRamp> outBehind;
			std::optional<GainRamp> inBehind;
		};

		void addRamp(PlaylistEntry& entry, const std::optional<GainRamp>& ramp)
		{
			if (ramp)
				entry.ramps.push_back(*ramp);
		}

		// A section of an event, with its fades and its clip's.
		PlaylistEntry section(const Placed& placed, time::Samples start, time::Samples end)
		{
			PlaylistEntry entry;
			entry.start = start;
			entry.end = end;
			entry.media = placed.media->id;
			entry.frame = placed.frame + (start - placed.start);
			entry.gain = placed.event->gain;
			entry.channels = std::max(1, placed.media->channels);
			entry.event = placed.event->id;
			addRamp(entry, placed.fadeIn);
			addRamp(entry, placed.fadeOut);
			const time::Samples clipStart = placed.start - placed.frame;
			const time::Samples clipEnd = clipStart + scaled(placed.media->length, placed.ratio);
			if (const time::Samples length = scaled(placed.event->clipFadeIn.length, placed.ratio);
				length > 0 && clipStart + length > placed.start)
				addRamp(entry, GainRamp{clipStart, clipStart + length, placed.event->clipFadeIn.shape, false});
			if (const time::Samples length = scaled(placed.event->clipFadeOut.length, placed.ratio);
				length > 0 && clipEnd - length < placed.end)
				addRamp(entry, GainRamp{clipEnd - length, clipEnd, placed.event->clipFadeOut.shape, true});
			return entry;
		}

		// The fade out of an auto crossfade with the given fade in: for equal gain its mirror image, for equal power
		// a quarter sine too.
		FadeShape crossfadeOut(CrossfadeMode mode, FadeShape in)
		{
			FadeShape mirror = FadeShape::EqualPower;
			if (mode == CrossfadeMode::EqualGain && mirrorShape(in, mirror))
				return mirror;
			return FadeShape::EqualPower;
		}

		std::map<ObjectId, size_t> indexById(const std::vector<Placed>& events)
		{
			std::map<ObjectId, size_t> index;
			for (size_t i = 0; i < events.size(); ++i)
				index[events[i].event->id] = i;
			return index;
		}

		// Crossfades of the events' own: where the two overlap, the one before fades out up to its end and the
		// other one fades in from its start.
		void crossfades(std::vector<Placed>& events, std::vector<Transition>& transitions)
		{
			const std::map<ObjectId, size_t> index = indexById(events);
			for (size_t i = 0; i < events.size(); ++i)
			{
				Placed& in = events[i];
				const Crossfade& crossfade = in.event->crossfade;
				const auto from = index.find(crossfade.from);
				if (crossfade.from == 0 || from == index.end())
					continue;
				Placed& out = events[from->second];
				// The snapshot only has crossfades that apply (crossfadeApplies): between events that overlap, the
				// second one starting inside the first and ending after it. An edit that ends this rests a crossfade.
				if (out.crossfadeOut || out.end <= in.start)
					continue;
				const time::Samples overlap = out.end - in.start;
				const auto length = [overlap](time::Samples frames, double ratio)
				{ return frames > 0 ? std::clamp<time::Samples>(scaled(frames, ratio), 1, overlap) : overlap; };
				in.fadeIn = GainRamp{in.start, in.start + length(crossfade.length, in.ratio), crossfade.shape, false};
				out.fadeOut =
					GainRamp{out.end - length(crossfade.outLength, out.ratio), out.end, crossfade.outShape, true};
				in.crossfadeIn = true;
				out.crossfadeOut = true;
				transitions.push_back({from->second, i, in.start, out.end, std::nullopt, std::nullopt});
			}
		}

		// An event's own fade in or out, shortened to the event, if it has one.
		std::optional<GainRamp> ownFade(const Placed& placed, bool out)
		{
			const Fade& fade = out ? placed.event->fadeOut : placed.event->fadeIn;
			const time::Samples n = std::min(placed.end - placed.start, scaled(fade.length, placed.ratio));
			if (n <= 0)
				return std::nullopt;
			return out ? GainRamp{placed.end - n, placed.end, fade.shape, true}
					   : GainRamp{placed.start, placed.start + n, fade.shape, false};
		}

		// The mirror image of a fade's shape, which keeps the sum of the two gains at 1; an equal power fade's is
		// one too, which keeps the sum of their power at 1.
		FadeShape mirrored(FadeShape shape)
		{
			return crossfadeOut(CrossfadeMode::EqualGain, shape);
		}

		// Auto crossfades where the front passes from one event to another one that goes on behind it. Where an
		// event starts on top of one that goes on, the one behind plays on for the auto fade length, fading out;
		// where an event ends on top of one that goes on, the one behind fades in during its last stretch.
		//
		// Auto fades don't stack with an event's own fades. Where the event in front has a fade of its own at that
		// end, that fade is the crossfade's: the event behind fades with its mirror image over the same stretch, as
		// far as it reaches.
		void autoCrossfades(const PlaylistLayer& front, std::vector<Placed>& events, const AutoFades& settings,
			time::Samples length, std::vector<Transition>& transitions)
		{
			const std::map<ObjectId, size_t> index = indexById(events);
			const FadeShape inShape =
				settings.crossfadeMode == CrossfadeMode::EqualPower ? FadeShape::EqualPower : settings.crossfadeShape;
			const FadeShape outShape = crossfadeOut(settings.crossfadeMode, inShape);
			for (size_t k = 1; k < front.size(); ++k)
			{
				const PlaylistEntry& before = front[k - 1];
				const PlaylistEntry& after = front[k];
				if (before.end != after.start || before.event == after.event)
					continue;
				const size_t a = index.at(before.event);
				const size_t b = index.at(after.event);
				Placed& out = events[a];
				Placed& in = events[b];
				const time::Samples at = after.start;
				if (in.start == at && out.end > at && !in.crossfadeIn)
				{
					in.crossfadeIn = true;
					if (const std::optional<GainRamp> own = ownFade(in, false))
					{
						in.fadeIn = own;
						const time::Samples end = std::min(own->end, out.end);
						transitions.push_back(
							{a, b, at, end, GainRamp{at, end, mirrored(own->shape), true}, std::nullopt});
						continue;
					}
					const time::Samples n = std::min({length, out.end - at, in.end - at});
					in.fadeIn = GainRamp{at, at + n, inShape, false};
					transitions.push_back({a, b, at, at + n, GainRamp{at, at + n, outShape, true}, std::nullopt});
				}
				else if (out.end == at && in.start < at && !out.crossfadeOut)
				{
					out.crossfadeOut = true;
					if (const std::optional<GainRamp> own = ownFade(out, true))
					{
						out.fadeOut = own;
						const time::Samples start = std::max(own->start, in.start);
						transitions.push_back(
							{a, b, start, at, std::nullopt, GainRamp{start, at, mirrored(own->shape), false}});
						continue;
					}
					const time::Samples n = std::min({length, at - in.start, at - out.start});
					out.fadeOut = GainRamp{at - n, at, outShape, true};
					transitions.push_back({a, b, at - n, at, std::nullopt, GainRamp{at - n, at, inShape, false}});
				}
			}
		}

		// Fades of the events' own, or else auto fades: an auto fade only goes where an event has no fade of its own
		// at that end, never on top of one.
		void fades(std::vector<Placed>& events, const AutoFades& settings, time::Samples autoLength)
		{
			for (Placed& placed : events)
			{
				const time::Samples length = placed.end - placed.start;
				if (!placed.fadeIn)
				{
					placed.fadeIn = ownFade(placed, false);
					if (!placed.fadeIn && settings.fadeIn)
						placed.fadeIn = GainRamp{
							placed.start, placed.start + std::min(length, autoLength), settings.fadeInShape, false};
				}
				if (!placed.fadeOut)
				{
					placed.fadeOut = ownFade(placed, true);
					if (!placed.fadeOut && settings.fadeOut)
						placed.fadeOut = GainRamp{
							placed.end - std::min(length, autoLength), placed.end, settings.fadeOutShape, true};
				}
			}
		}

		// The front layer: each event covers what the events before it play where they overlap. Quadratic in the
		// number of a track's overlapping events, which stays small.
		PlaylistLayer front(const std::vector<Placed>& events)
		{
			PlaylistLayer layer;
			for (const Placed& placed : events)
			{
				const PlaylistEntry entry = section(placed, placed.start, placed.end);
				PlaylistLayer covered;
				for (const PlaylistEntry& other : layer)
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
				layer = std::move(covered);
			}
			return layer;
		}

		// Whether transitions active from start to end join event e to event f, directly or through other events.
		// If so, ramps gets the fades behind the front that apply to e on the way: at each link of the shortest chain,
		// those of the transitions between its two events on the side away from f. So an event behind another one
		// fades out and in with it.
		bool joined(size_t e, size_t f, size_t count, const std::vector<Transition>& transitions, time::Samples start,
			time::Samples end, std::vector<const GainRamp*>& ramps)
		{
			std::vector<const Transition*> active;
			for (const Transition& transition : transitions)
				if (transition.start <= start && transition.end >= end)
					active.push_back(&transition);
			// Breadth first from f: each event reached knows the next one on its way to f.
			constexpr size_t none = std::numeric_limits<size_t>::max();
			std::vector<size_t> towards(count, none);
			towards[f] = f;
			std::vector<size_t> queue{f};
			for (size_t q = 0; q < queue.size() && towards[e] == none; ++q)
				for (const Transition* transition : active)
				{
					const size_t at = queue[q];
					const size_t other = transition->out == at ? transition->in : transition->out;
					if ((transition->out == at || transition->in == at) && towards[other] == none)
					{
						towards[other] = at;
						queue.push_back(other);
					}
				}
			if (towards[e] == none)
				return false;
			ramps.clear();
			for (size_t at = e; at != f; at = towards[at])
				for (const Transition* transition : active)
				{
					if (transition->out == at && transition->in == towards[at] && transition->outBehind)
						ramps.push_back(&*transition->outBehind);
					else if (transition->in == at && transition->out == towards[at] && transition->inBehind)
						ramps.push_back(&*transition->inBehind);
				}
			return true;
		}

		// What plays behind the front during transitions, sorted by start. An event plays behind the front only where
		// transitions active there join it to the event in front: where that is its partner in a transition, or is
		// joined to its partner by further transitions, as when a third event takes the front during a crossfade. It
		// plays with its own fades and those behind the front along the way (joined()). Once those have faded it out,
		// it stays silent until the event in front changes, unless one fades it in again from silence.
		std::vector<PlaylistEntry> hidden(
			const std::vector<Placed>& events, const std::vector<Transition>& transitions, const PlaylistLayer& front)
		{
			const std::map<ObjectId, size_t> index = indexById(events);
			std::vector<PlaylistEntry> result;
			for (size_t e = 0; e < events.size(); ++e)
			{
				const Placed& placed = events[e];
				if (std::none_of(transitions.begin(), transitions.end(),
						[e](const Transition& transition) { return transition.out == e || transition.in == e; }))
					continue;
				// Cut where a transition or a front entry starts or ends, so each piece has one set of active
				// transitions and one event in front.
				std::vector<time::Samples> cuts{placed.start, placed.end};
				const auto cut = [&](time::Samples at)
				{
					if (at > placed.start && at < placed.end)
						cuts.push_back(at);
				};
				for (const Transition& transition : transitions)
				{
					cut(transition.start);
					cut(transition.end);
				}
				for (const PlaylistEntry& entry : front)
				{
					cut(entry.start);
					cut(entry.end);
				}
				std::sort(cuts.begin(), cuts.end());
				cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

				std::vector<const GainRamp*> ramps;
				std::vector<const GainRamp*> previous; // the ramps of the piece before, while it goes on
				bool goesOn = false;
				ObjectId silentUnder = 0; // the event in front when its fades behind the front faded it out
				for (size_t k = 0; k + 1 < cuts.size(); ++k)
				{
					const time::Samples start = cuts[k];
					const time::Samples end = cuts[k + 1];
					const PlaylistEntry* inFront = entryAt(front, start);
					if (inFront == nullptr || inFront->event == placed.event->id)
					{
						goesOn = false;
						silentUnder = 0;
						continue;
					}
					if (inFront->event != silentUnder)
						silentUnder = 0;
					if (!joined(e, index.at(inFront->event), events.size(), transitions, start, end, ramps) ||
						(silentUnder != 0 &&
							std::all_of(ramps.begin(), ramps.end(),
								[start](const GainRamp* ramp) { return ramp->gainAt(start) > 0.0; })))
					{
						goesOn = false;
						continue;
					}
					silentUnder = 0;
					if (goesOn && ramps == previous)
					{
						result.back().end = end;
					}
					else
					{
						PlaylistEntry piece = section(placed, start, end);
						for (const GainRamp* ramp : ramps)
							piece.ramps.push_back(*ramp);
						result.push_back(std::move(piece));
						previous = ramps;
					}
					goesOn = true;
					if (std::any_of(ramps.begin(), ramps.end(),
							[end](const GainRamp* ramp) { return ramp->out && ramp->end <= end; }))
						silentUnder = inFront->event;
				}
			}
			std::stable_sort(result.begin(), result.end(),
				[](const PlaylistEntry& a, const PlaylistEntry& b) { return a.start < b.start; });
			return result;
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

	double GainRamp::gainAt(time::Samples position) const noexcept
	{
		const auto length = static_cast<double>(end - start);
		if (!out)
		{
			if (position < start)
				return 0.0;
			return position >= end ? 1.0 : fadeGain(shape, static_cast<double>(position - start) / length);
		}
		if (position >= end)
			return 0.0;
		return position < start ? 1.0 : fadeGain(shape, static_cast<double>(end - position) / length);
	}

	float PlaylistEntry::gainAt(time::Samples position) const noexcept
	{
		double result = gain;
		for (const GainRamp& ramp : ramps)
			result *= ramp.gainAt(position);
		return static_cast<float>(result);
	}

	bool PlaylistEntry::fades(time::Samples position, time::Samples frames) const noexcept
	{
		return std::any_of(ramps.begin(), ramps.end(),
			[&](const GainRamp& ramp) { return ramp.start < position + frames && ramp.end > position; });
	}

	Playlist makePlaylist(const RenderSnapshot& snapshot, const RenderChannel& channel)
	{
		std::vector<Placed> events;
		for (const RenderAudioEvent& event : channel.audioEvents)
		{
			if (event.muted || event.media < 0 || static_cast<size_t>(event.media) >= snapshot.media.size())
				continue;
			Placed placed;
			placed.event = &event;
			placed.media = &snapshot.media[static_cast<size_t>(event.media)];
			placed.ratio = snapshot.sampleRate / placed.media->sampleRate;
			placed.start = event.start;
			placed.end = event.start + scaled(event.length, placed.ratio);
			placed.frame = scaled(event.offset, placed.ratio);
			if (placed.end > placed.start)
				events.push_back(placed);
		}
		std::sort(
			events.begin(), events.end(), [](const Placed& a, const Placed& b) { return a.event->id < b.event->id; });

		// Crossfades first: they replace the fades at the ends they join. Auto crossfades go where the front layer
		// passes from one event to another, which the fades don't change.
		const AutoFades& settings = channel.autoFades;
		const time::Samples autoLength = std::max<time::Samples>(
			1, static_cast<time::Samples>(std::llround(settings.length * snapshot.sampleRate / 1000.0)));
		std::vector<Transition> transitions;
		crossfades(events, transitions);
		if (settings.crossfades)
			autoCrossfades(front(events), events, settings, autoLength, transitions);
		fades(events, settings, autoLength);

		Playlist playlist;
		playlist.layers.push_back(front(events));

		// Where a transition's events play together, the one that isn't in front plays in a layer behind, as long as
		// transitions join it to the event in front: also where a third event has taken the front.
		const std::vector<PlaylistEntry> behind = hidden(events, transitions, playlist.layers.front());
		for (const PlaylistEntry& entry : behind)
		{
			auto layer = std::find_if(playlist.layers.begin() + 1, playlist.layers.end(),
				[&entry](const PlaylistLayer& l) { return l.back().end <= entry.start; });
			if (layer == playlist.layers.end())
				layer = playlist.layers.insert(playlist.layers.end(), PlaylistLayer());
			layer->push_back(entry);
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
			// After a cycle jump playback enters wherever the left locator is. Otherwise reaching an entry's start
			// plays it from a new voice, unless an entry ends there whose clip it continues: then that voice reads on,
			// for one entry. The entries at the cursor are cue()'s.
			std::vector<const PlaylistEntry*> starting;
			for (const PlaylistLayer& layer : playlist.layers)
			{
				auto entry = std::upper_bound(layer.begin(), layer.end(), position,
					[](time::Samples at, const PlaylistEntry& e) { return at < e.end; });
				for (; entry != layer.end() && entry->start < end; ++entry)
				{
					const time::Samples from = std::max(position, entry->start);
					if (jumped && from == position)
						result.push_back({entry->media, entry->frame + (from - entry->start)});
					else if (from == entry->start && (lapped || from > cue.position))
						starting.push_back(&*entry);
				}
			}
			std::stable_sort(starting.begin(), starting.end(),
				[](const PlaylistEntry* a, const PlaylistEntry* b) { return a->start < b->start; });
			std::vector<const PlaylistEntry*> readOn;
			for (const PlaylistEntry* entry : starting)
			{
				if (const PlaylistEntry* before = continued(playlist, *entry, readOn))
					readOn.push_back(before);
				else
					result.push_back({entry->media, entry->frame});
			}
			remaining -= end - position;
			jumped = bounded && end == settings.right;
			lapped = lapped || jumped;
			position = jumped ? settings.left : end;
		}
		if (result.size() > m_voicesPerTrack)
			result.resize(m_voicesPerTrack);
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
			std::vector<bool> reading(m_voices.size(), false);
			for (const size_t i : mine)
				reading[i] = cue.playing && states[i].started && states[i].lastBlock + 2 > now;
			std::vector<bool> taken = reading;
			std::vector<bool> claimed(m_voices.size(), false); // has one of the entries at the cursor

			std::vector<Point> unmet;

			// The entries at the cursor, one per layer, each with a voice of its own: while playing, a voice reading
			// near it or waiting just ahead of it has it. Otherwise it needs one: while stopped exactly there, while
			// playing a little ahead, so the voice is ready by the time playback gets there.
			const Playlist& playlist = *track.playlist;
			const auto slack = static_cast<time::Samples>(m_sampleRate / 10.0);
			for (const PlaylistLayer& layer : playlist.layers)
			{
				const PlaylistEntry* at = entryAt(layer, cue.position);
				if (at == nullptr)
					continue;
				const time::Samples frame = at->frame + (cue.position - at->start);
				if (cue.playing)
				{
					const auto near = std::find_if(mine.begin(), mine.end(),
						[&](size_t i)
						{
							if (claimed[i] || m_voices[i]->media != at->media)
								return false;
							const time::Samples next = reading[i] ? states[i].next : states[i].cueFrame;
							return std::abs(next - frame) <= 2 * slack;
						});
					if (near != mine.end())
					{
						claimed[*near] = true;
						taken[*near] = true;
						for (const size_t i : mine)
							if (!taken[i] && m_voices[i]->media == at->media &&
								std::abs(states[i].cueFrame - frame) <= 2 * slack)
								taken[i] = true; // keep the voice waiting there
						continue;
					}
					Cue later = cue;
					later.position = ahead(cue, slack);
					if (const PlaylistEntry* next = entryAt(layer, later.position))
						unmet.push_back({next->media, next->frame + (later.position - next->start)});
					continue;
				}
				const auto waiting = std::find_if(mine.begin(), mine.end(),
					[&](size_t i)
					{
						return !claimed[i] && m_voices[i]->media == at->media &&
							(states[i].started ? states[i].next : states[i].cueFrame) == frame;
					});
				if (waiting != mine.end())
				{
					claimed[*waiting] = true;
					taken[*waiting] = true;
				}
				else
				{
					unmet.push_back({at->media, frame});
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

	double Playback::diskLoad() const
	{
		const std::scoped_lock lock(m_mutex);
		double load = 0.0;
		for (const auto& voice : m_voices)
			if (voice->stream)
				load = std::max(load, voice->stream->load());
		return load;
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
		// Voices of the same clip can be close together, as where a clip crossfades with itself: one exactly at the
		// frame is the right one.
		Voice* soonest = nullptr;
		uint64_t soonestState = 0;
		time::Samples soonestNext = 0;
		const auto& voices = m_table->voices;
		auto voice = std::lower_bound(voices.begin(), voices.end(), channel,
			[](const std::shared_ptr<Voice>& v, ObjectId id) { return v->channel < id; });
		for (; voice != voices.end() && (*voice)->channel == channel; ++voice)
		{
			Voice& v = **voice;
			if (v.media != media)
				continue;
			const uint64_t state = v.state.load(std::memory_order_acquire);
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
			if (next == frame)
			{
				if (!startReading(v, state, next))
					continue; // a cue has just claimed it
				lead = 0;
				return &v;
			}
			if (soonest == nullptr || next - frame < soonestNext - frame)
			{
				soonest = &v;
				soonestState = state;
				soonestNext = next;
			}
		}
		if (soonest == nullptr || !startReading(*soonest, soonestState, soonestNext))
			return nullptr;
		lead = soonestNext - frame;
		return soonest;
	}

	bool Playback::startReading(Voice& voice, uint64_t seen, time::Samples next) const noexcept
	{
		// Read from it, unless a cue has just claimed it.
		voice.next.store(next, std::memory_order_relaxed);
		return voice.state.compare_exchange_strong(seen, readIn(block()), std::memory_order_acq_rel);
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
		for (const PlaylistLayer& layer : m_playlist->layers)
			for (const PlaylistEntry& entry : layer)
				channels = std::max(channels, entry.channels);
		const size_t layers = m_playlist->layers.size();
		m_bufferFrames = maxFrames;
		m_clipChannels = static_cast<size_t>(channels);
		m_buffer.assign(static_cast<size_t>(maxFrames) * m_clipChannels, 0.0f);
		m_gains.assign(maxFrames, 0.0f);
		m_next.assign(layers, 0);
		m_layerAudio.assign(layers * static_cast<size_t>(maxFrames) * m_clipChannels, 0.0f);
		m_layerChannels.assign(layers * static_cast<size_t>(maxFrames), 0);
		m_layerUsed.assign(layers, 0);
	}

	void TrackPlayer::process(const graph::ProcessContext& context) noexcept
	{
		const graph::TransportBlock& transport = context.transport;
		if (!transport.playing)
			return;
		const std::vector<PlaylistLayer>& layers = m_playlist->layers;
		const size_t layerCount = std::min(layers.size(), m_next.size());
		const size_t blockFrames = std::min<size_t>(context.frames, m_bufferFrames);
		std::fill(m_layerUsed.begin(), m_layerUsed.end(), 0);
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			const time::Samples begin = segment.position;
			const time::Samples end = begin + segment.frames;
			for (size_t l = 0; l < layerCount; ++l)
				m_next[l] =
					static_cast<size_t>(std::upper_bound(layers[l].begin(), layers[l].end(), begin,
											[](time::Samples at, const PlaylistEntry& e) { return at < e.end; }) -
						layers[l].begin());
			// The entries of every layer in the order they begin, so that where one entry goes on reading from the
			// voice of another, in another layer, the other one has read up to there.
			for (;;)
			{
				const PlaylistEntry* entry = nullptr;
				size_t next = 0;
				for (size_t l = 0; l < layerCount; ++l)
				{
					if (m_next[l] >= layers[l].size() || layers[l][m_next[l]].start >= end)
						continue;
					if (entry == nullptr || std::max(begin, layers[l][m_next[l]].start) < std::max(begin, entry->start))
					{
						entry = &layers[l][m_next[l]];
						next = l;
					}
				}
				if (entry == nullptr)
					break;
				++m_next[next];
				const time::Samples start = std::max(begin, entry->start);
				const time::Samples stop = std::min(end, entry->end);
				if (m_layerUsed[next] == 0)
				{
					// The layer's first entry in this block: nothing of it plays in the block yet.
					m_layerUsed[next] = 1;
					std::fill_n(m_layerChannels.begin() + static_cast<std::ptrdiff_t>(next * m_bufferFrames),
						blockFrames, uint8_t{0});
				}
				play(*entry, next, start, stop - start, segment.offset + static_cast<uint32_t>(start - begin));
			}
		}

		// The layers add up in a fixed order, the front first, so a sum of floats is the same wherever a block starts.
		// One to one, a mono clip on every channel, extra clip channels unused.
		for (size_t l = 0; l < layerCount; ++l)
		{
			if (m_layerUsed[l] == 0)
				continue;
			const float* audio = m_layerAudio.data() + (l * m_bufferFrames * m_clipChannels);
			const uint8_t* channels = m_layerChannels.data() + (l * m_bufferFrames);
			for (int c = 0; c < context.channels; ++c)
			{
				float* out = context.audio[c];
				const auto channel = static_cast<size_t>(c);
				for (size_t i = 0; i < blockFrames; ++i)
				{
					const size_t clipChannels = channels[i];
					const size_t source = clipChannels == 1 ? 0 : channel;
					if (source < clipChannels)
						out[i] += audio[(i * m_clipChannels) + source];
				}
			}
		}
	}

	void TrackPlayer::play(const PlaylistEntry& entry, size_t layer, time::Samples position, time::Samples frames,
		uint32_t offset) noexcept
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
		const auto start = static_cast<size_t>(offset) + static_cast<size_t>(lead);
		if (start >= m_bufferFrames)
			return;
		const auto count = std::min(static_cast<size_t>(frames - lead), m_bufferFrames - start);
		const auto channels = static_cast<size_t>(voice->stream->channels());
		if (channels > m_clipChannels || channels > std::numeric_limits<uint8_t>::max())
			return; // a clip with more channels than prepared for; the next graph has room
		voice->stream->read(m_buffer.data(), count);
		voice->next.store(frame + lead + static_cast<time::Samples>(count), std::memory_order_relaxed);

		// The gain of every frame where the entry fades, else one for all of them.
		const time::Samples first = position + lead;
		const bool fading = entry.fades(first, static_cast<time::Samples>(count)) && count <= m_gains.size();
		const float gain = entry.gainAt(first);
		if (fading)
			for (size_t i = 0; i < count; ++i)
				m_gains[i] = entry.gainAt(first + static_cast<time::Samples>(i));

		// Into the layer's frames, as the clip's channels.
		float* audio = m_layerAudio.data() + (((layer * m_bufferFrames) + start) * m_clipChannels);
		uint8_t* layerChannels = m_layerChannels.data() + (layer * m_bufferFrames) + start;
		for (size_t i = 0; i < count; ++i)
		{
			const float frameGain = fading ? m_gains[i] : gain;
			for (size_t c = 0; c < channels; ++c)
				audio[(i * m_clipChannels) + c] = m_buffer[(i * channels) + c] * frameGain;
			layerChannels[i] = static_cast<uint8_t>(channels);
		}
	}
}
