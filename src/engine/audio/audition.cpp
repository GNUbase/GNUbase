// SPDX-License-Identifier: GPL-3.0-or-later
// Auditioning and scrubbing one audio track (#470, docs/features/audition.md) or one media file (#730), and the
// commands audition.start, audition.scrub and audition.stop.
#include "engine/audio/audition.h"

#include "engine/audio/audioengine.h"
#include "engine/document/channelformat.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <thread>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		constexpr auto g_startWait = std::chrono::seconds(2); // for the streams before auditioning starts
		// For the audio thread to finish the block it plays the state before a stop in.
		constexpr auto g_releaseWait = std::chrono::milliseconds(200);
		constexpr double g_minScrubSeconds = 0.01;
		constexpr double g_maxScrubSeconds = 0.1;
		constexpr double g_maxScrubMaterial = 4.0; // seconds of the track one scrub plays at most

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}
	}

	Audition::Audition() : m_states(std::make_unique<const State>())
	{
	}

	void Audition::publish(std::unique_ptr<State> state)
	{
		state->serial = ++m_serial;
		{
			const std::scoped_lock lock(m_mutex);
			m_cuePlaying = state->player != nullptr;
			m_cueSerial = state->serial;
			m_cueStart = state->start;
		}
		m_states.publish(std::move(state));
	}

	void Audition::play(const Subject& subject, const Playback::Track& source,
		std::map<ObjectId, std::string> mediaPaths, double sampleRate, time::Samples position, time::Samples end,
		int channels, uint32_t maxFrames)
	{
		// Where the audition before plays on from until this one is out, if one plays.
		time::Samples playsOn = -1;
		{
			// The cue thread leaves the streams alone until this state is out: a cue it made for the last one would
			// move what this one cues (cue holds the lock while it cues).
			const std::scoped_lock lock(m_mutex);
			if (m_cuePlaying)
			{
				const auto [played, at] = m_played.load();
				playsOn = played == m_cueSerial ? at : m_cueStart;
			}
			m_cuePlaying = false;
		}
		m_subject = subject;
		m_scrubbing = false;
		m_readers.clear();
		auto state = std::make_unique<State>();
		state->channels = channels;
		state->start = position;
		state->end = end;
		state->maxFrames = maxFrames;
		state->buffer.assign(static_cast<size_t>(channels) * maxFrames, 0.0f);
		for (int c = 0; c < channels; ++c)
			state->audio.push_back(state->buffer.data() + (static_cast<size_t>(c) * maxFrames));
		state->player = std::make_shared<TrackPlayer>(source.playlist, source.channel, m_playback);
		state->player->prepareFor(sampleRate, maxFrames);
		m_playback.setTracks({source}, std::move(mediaPaths), sampleRate);
		// Exactly at the position before the audio thread starts reading, as a transport start does.
		Playback::Cue cue;
		cue.position = position;
		cue.window = maxFrames;
		cue.playsOn = playsOn;
		m_playback.cue(cue);
		m_playback.waitUntilBuffered(g_startWait);
		publish(std::move(state));
	}

	void Audition::scrub(const Subject& subject, const Playback::Track& source,
		const std::map<ObjectId, std::string>& mediaPaths, double sampleRate, time::Samples position,
		std::optional<double> seconds, int channels)
	{
		const auto now = std::chrono::steady_clock::now();
		if (m_subject != subject || !m_scrubbing)
		{
			stop();
			m_subject = subject;
			m_scrubbing = true;
			m_scrubFrom = position;
			m_scrubTo = position;
			m_scrubFrames = 0;
			m_lastScrub = now;
			return;
		}
		const double elapsed = std::chrono::duration<double>(now - m_lastScrub).count();
		const double duration = seconds.value_or(std::clamp(elapsed, g_minScrubSeconds, g_maxScrubSeconds));
		m_lastScrub = now;
		// On from where the audio thread got to in the last scrub, or where that one starts if it hasn't played it:
		// scrubs that come faster than they play, or late, leave nothing out. (It may play a few more frames of the
		// last one meanwhile, which this one then plays again.)
		time::Samples reached = m_scrubTo;
		if (m_scrubFrames > 0)
		{
			const auto [serial, played] = m_played.load();
			const double done = serial == m_scrubSerial
				? std::min(static_cast<double>(played), static_cast<double>(m_scrubFrames)) /
					static_cast<double>(m_scrubFrames)
				: 0.0;
			reached = m_scrubFrom + std::llround(static_cast<double>(m_scrubTo - m_scrubFrom) * done);
		}
		const auto most = static_cast<time::Samples>(g_maxScrubMaterial * sampleRate);
		const time::Samples from = std::clamp(reached, position - most, position + most);
		const auto frames = static_cast<size_t>(std::llround(duration * sampleRate));
		if (from == position || frames == 0 || channels <= 0)
			return; // standing still: what it played runs out
		// The track between both positions, then read through at the speed it takes from one to the other.
		const time::Samples low = std::min(from, position);
		const time::Samples length = std::max(from, position) - low + 1;
		if (mediaPaths != m_readerPaths)
		{
			m_readers.clear(); // a media file changed, as a freeze file does on undo
			m_readerPaths = mediaPaths;
		}
		const std::vector<float> material = read(*source.playlist, mediaPaths, sampleRate, low, length, channels);
		const auto width = static_cast<size_t>(channels);
		auto audio = std::make_shared<std::vector<float>>(frames * width);
		for (size_t k = 0; k < frames; ++k)
		{
			const double at = static_cast<double>(from - low) +
				(static_cast<double>(position - from) * static_cast<double>(k) / static_cast<double>(frames));
			const auto before = static_cast<size_t>(at);
			const size_t after = std::min(before + 1, static_cast<size_t>(length) - 1);
			const auto fraction = static_cast<float>(at - static_cast<double>(before));
			for (size_t c = 0; c < width; ++c)
			{
				const float a = material[(before * width) + c];
				const float b = material[(after * width) + c];
				(*audio)[(k * width) + c] = a + ((b - a) * fraction);
			}
		}
		auto state = std::make_unique<State>();
		state->channels = channels;
		state->scrub = std::move(audio);
		publish(std::move(state));
		m_scrubFrom = from;
		m_scrubTo = position;
		m_scrubFrames = frames;
		m_scrubSerial = m_serial;
	}

	void Audition::stop()
	{
		m_subject = Subject();
		m_scrubbing = false;
		m_readers.clear();
		publish(std::make_unique<State>());
		// The audio thread may be in the middle of a block with the state before, which publishing then can't free,
		// and no state may come after this one for a long time: it lets go of it at the block's end, so the state
		// and its player are freed here, not kept until the next audition. Past the wait (an audio thread that
		// stalls), the next publish frees it.
		const auto deadline = std::chrono::steady_clock::now() + g_releaseWait;
		while (m_states.retired() > 0 && std::chrono::steady_clock::now() < deadline)
		{
			std::this_thread::sleep_for(std::chrono::microseconds(200));
			m_states.collect();
		}
		m_playback.setTracks({}, {}, 0.0); // closes the streams' files and frees their buffers
	}

	std::vector<float> Audition::read(const Playlist& playlist, const std::map<ObjectId, std::string>& mediaPaths,
		double sampleRate, time::Samples start, time::Samples frames, int channels)
	{
		const auto width = static_cast<size_t>(channels);
		std::vector<float> result(static_cast<size_t>(frames) * width, 0.0f);
		if (playlist.layers.empty())
			return result;
		// ponytail: the front layer only, each clip frame to the nearest; what plays behind during crossfades and
		// resampling as playback does it when scrubbing needs to sound better.
		for (const PlaylistEntry& entry : playlist.layers.front())
		{
			const time::Samples from = std::max(start, entry.start);
			const time::Samples to = std::min(start + frames, entry.end);
			if (from >= to)
				continue;
			auto found = m_readers.find(entry.media);
			if (found == m_readers.end())
			{
				std::unique_ptr<AudioFileReader> reader;
				if (const auto path = mediaPaths.find(entry.media); path != mediaPaths.end())
				{
					try
					{
						reader = AudioFileReader::open(path->second);
					}
					catch (const AudioFileError&)
					{
						reader = nullptr; // silent, like a clip that playback can't read
					}
				}
				found = m_readers.emplace(entry.media, std::move(reader)).first;
			}
			AudioFileReader* reader = found->second.get();
			if (reader == nullptr)
				continue;
			const AudioFileInfo& info = reader->info();
			const double ratio = info.sampleRate / sampleRate;
			const auto clipFrame = [&entry, ratio](time::Samples position)
			{
				return static_cast<time::Samples>(
					std::floor(static_cast<double>(entry.frame + (position - entry.start)) * ratio));
			};
			const time::Samples first = clipFrame(from);
			const time::Samples count = clipFrame(to - 1) - first + 1;
			const auto clipChannels = static_cast<size_t>(info.channels);
			if (first < 0 || first >= info.frames || clipChannels == 0)
				continue;
			std::vector<float> clip(static_cast<size_t>(count) * clipChannels, 0.0f);
			try
			{
				reader->seek(first);
				reader->read(clip.data(), static_cast<size_t>(count));
			}
			catch (const AudioFileError&)
			{
				continue;
			}
			// A mono clip on every channel, the others one to one, as playback has them.
			for (time::Samples position = from; position < to; ++position)
			{
				const auto frame = static_cast<size_t>(clipFrame(position) - first);
				const float gain = entry.gainAt(position);
				for (size_t c = 0; c < width; ++c)
				{
					const size_t source = clipChannels == 1 ? 0 : c;
					if (source < clipChannels)
						result[(static_cast<size_t>(position - start) * width) + c] =
							clip[(frame * clipChannels) + source] * gain;
				}
			}
		}
		return result;
	}

	void Audition::cue(time::Samples window, bool reading)
	{
		// Under the lock, so that play can hold off a cue made for the state before.
		const std::scoped_lock lock(m_mutex);
		if (!m_cuePlaying)
			return;
		const auto [played, position] = m_played.load();
		Playback::Cue cue;
		cue.playing = reading && played == m_cueSerial;
		cue.position = played == m_cueSerial ? position : m_cueStart;
		cue.window = window;
		m_playback.cue(cue);
	}

	void Audition::process(
		float* const* outputs, int outputChannels, const std::vector<int>& ports, uint32_t frames) noexcept
	{
		const State& state = m_states.acquire();
		if (state.serial != m_playing)
		{
			m_playing = state.serial;
			m_position = state.start;
			m_scrubbed = 0;
		}
		const size_t channels = std::min(ports.size(), static_cast<size_t>(state.channels));
		const auto add = [&](size_t channel, const float* audio, size_t stride, size_t count)
		{
			const int port = ports[channel];
			if (port < 0 || port >= outputChannels)
				return;
			for (size_t i = 0; i < count; ++i)
				outputs[port][i] += audio[i * stride];
		};
		if (state.player && m_position < state.end)
		{
			const uint32_t count = std::min(frames, state.maxFrames);
			for (size_t c = 0; c < channels; ++c)
				std::fill_n(state.audio[c], count, 0.0f);
			const graph::TransportSegment segment{0, count, m_position};
			graph::TransportBlock transport;
			transport.playing = true;
			transport.position = m_position;
			transport.segments = &segment;
			transport.segmentCount = 1;
			m_playback.beginBlock();
			state.player->process(
				{state.audio.data(), static_cast<int>(channels), count, m_eventsIn, m_eventsOut, transport});
			m_playback.endBlock();
			// Up to the end, where the position stays, so that the streams aren't read any further.
			const auto audible = static_cast<size_t>(std::min<time::Samples>(count, state.end - m_position));
			for (size_t c = 0; c < channels; ++c)
				add(c, state.audio[c], 1, audible);
			m_position += static_cast<time::Samples>(audible);
		}
		else if (state.scrub)
		{
			const auto width = static_cast<size_t>(state.channels);
			const size_t total = state.scrub->size() / width;
			const size_t count = std::min<size_t>(frames, total - std::min(m_scrubbed, total));
			for (size_t c = 0; c < channels && count > 0; ++c)
				add(c, state.scrub->data() + (m_scrubbed * width) + c, width, count);
			m_scrubbed += count;
		}
		m_played.store(m_playing, state.scrub ? static_cast<time::Samples>(m_scrubbed) : m_position);
		m_states.release();
	}

	void AudioEngine::registerAuditionCommands(Engine& engine)
	{
		const Json status = Json::parse(R"({"type": "object", "additionalProperties": false,
			"required": ["track", "media", "mode"], "properties": {"track": {"type": "integer"},
			"media": {"type": "integer"}, "mode": {"type": "string", "enum": ["off", "play", "scrub"]}}})");
		const Json parameters = Json::parse(R"({"type": "object", "additionalProperties": false,
			"required": ["position"], "properties": {"track": {"type": "integer"}, "media": {"type": "integer"},
			"clip": {"type": "integer"}, "position": {"type": "integer"}}})");
		Json startParameters = parameters;
		startParameters["properties"]["end"] = {{"type", "integer"}};
		Json scrubParameters = parameters;
		scrubParameters["properties"]["seconds"] = {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}};
		const auto report = [this](CommandContext& context)
		{
			const Audition::Subject& subject = m_audition.subject();
			std::string mode = "off";
			if (subject != Audition::Subject())
				mode = m_audition.scrubbing() ? "scrub" : "play";
			Json result = {{"track", subject.track}, {"media", subject.media}, {"mode", mode}};
			context.emit("audition.changed", result);
			return result;
		};
		// The audio track, media file or clip, its playlist and the main mix's channels for a command's parameters,
		// with position and end in the playlist's samples. A media file plays on its own from the project's sample
		// 0, so that its frames, at its own sample rate, are converted to the project's; a clip's plays with the
		// clip's fades at its start and end.
		struct Source
		{
			Audition::Subject subject;
			Playback::Track playback;
			time::Samples position = 0;
			std::optional<time::Samples> end;
			int channels = 0;
		};
		const auto source = [this](const Json& given)
		{
			require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
			require(given.count("track") + given.count("media") + given.count("clip") == 1,
				"give either a track, a media file or a clip");
			Source result;
			const RenderSnapshot& snapshot = m_snapshots.latest();
			const auto position = given.at("position").get<time::Samples>();
			const auto end = given.find("end");
			if (end != given.end())
				require(end->get<time::Samples>() > position, "end must be after position");
			if (given.contains("track"))
			{
				result.subject.track = given.at("track").get<ObjectId>();
				require(position >= 0 && position <= g_maxPosition,
					"position must be 0 to " + std::to_string(g_maxPosition) + " samples");
				const auto found = m_auditionTracks.find(result.subject.track);
				require(found != m_auditionTracks.end(), "no audio track " + std::to_string(result.subject.track));
				result.playback = found->second;
				result.position = position;
				if (end != given.end())
					result.end = end->get<time::Samples>();
			}
			else
			{
				const RenderClip* clip = nullptr;
				if (given.contains("clip"))
				{
					result.subject.clip = given.at("clip").get<ObjectId>();
					const auto found = std::find_if(snapshot.clips.begin(), snapshot.clips.end(),
						[&result](const RenderClip& each) { return each.id == result.subject.clip; });
					require(found != snapshot.clips.end(), "no clip " + std::to_string(result.subject.clip));
					clip = &*found;
					result.subject.media = clip->media;
				}
				else
				{
					result.subject.media = given.at("media").get<ObjectId>();
				}
				// The project's media files: not a warped event's, nor a frozen channel's freeze file, which has the
				// channel's id.
				const auto found = std::find_if(snapshot.media.begin(), snapshot.media.end(),
					[&result, &snapshot](const RenderMedia& media)
					{
						return media.id == result.subject.media && media.warp.empty() &&
							std::none_of(snapshot.channels.begin(), snapshot.channels.end(),
								[&media](const RenderChannel& channel) { return channel.id == media.id; });
					});
				require(found != snapshot.media.end(), "no media file " + std::to_string(result.subject.media));
				require(found->sampleRate > 0.0 && found->length >= 0, // a hand-edited project's, say
					"media file " + std::to_string(found->id) + " has no sample rate or length");
				require(position >= 0 && position <= found->length,
					"position must be 0 to " + std::to_string(found->length) + " sample frames of the media file");
				const double ratio = snapshot.sampleRate / found->sampleRate;
				const auto samples = [ratio](time::Samples frames)
				{ return static_cast<time::Samples>(std::llround(static_cast<double>(frames) * ratio)); };
				PlaylistEntry entry;
				entry.end = samples(found->length);
				entry.media = found->id;
				entry.channels = found->channels;
				if (clip != nullptr && clip->fadeIn.length > 0)
					entry.ramps.push_back({0, samples(clip->fadeIn.length), clip->fadeIn.shape, false});
				if (clip != nullptr && clip->fadeOut.length > 0)
					entry.ramps.push_back(
						{entry.end - samples(clip->fadeOut.length), entry.end, clip->fadeOut.shape, true});
				Playlist playlist;
				playlist.layers.push_back({entry});
				result.playback = {found->id, std::make_shared<const Playlist>(std::move(playlist))};
				result.position = samples(position);
				if (end != given.end())
					result.end = std::max( // past the file's end, nothing plays
						result.position + 1, samples(std::min(end->get<time::Samples>(), found->length)));
			}
			for (const RenderBus& bus : snapshot.busses)
				if (bus.mainMix)
					result.channels = channelCount(bus.format);
			return result;
		};

		engine.registerCommand(
			{"audition.start", 1,
				"Auditions an audio track, as the play tool does while the mouse is pressed: plays its events from "
				"position (samples) until audition.stop, straight to the main mix's outputs, without its channel's "
				"fader, pan, mute and effects, and without the transport. With end (samples, after position) it plays "
				"up to there and then nothing, as the sample editor plays a slice, while its mode stays play until "
				"audition.stop or another audition. A mono clip plays on every channel. Or, instead of track, a media "
				"file on its own: position and end are then its sample frames, from 0 to its length, without the "
				"events that play it. Or, instead of either, a clip: its media file so, with the clip's fades, as the "
				"sample editor plays its clip; media is then the clip's media file. Ends what was auditioned before. "
				"Announces audition.changed.",
				startParameters, status, false},
			[this, source, report](CommandContext& context)
			{
				const Source given = source(context.parameters());
				m_audition.play(given.subject, given.playback, m_mediaPaths, m_snapshots.latest().sampleRate,
					given.position, given.end.value_or(g_maxPosition + 1), given.channels,
					m_device->format().blockSize);
				return report(context);
			});
		engine.registerCommand(
			{"audition.scrub", 1,
				"Scrubs an audio track, as the scrub tool does while the mouse drags: plays its events from where the "
				"audio got to in the last scrub of the track (where that one starts if it hasn't played yet), to "
				"position (samples), forwards or backwards, in seconds (default: the time since that scrub, 0.01 to "
				"0.1 seconds), so speed and pitch follow how fast the positions come, and scrubs that come faster "
				"than they play carry on without gaps. The first scrub only sets where the next one starts, and is the "
				"only one of a drag in engine.commandLog. Like audition.start it goes straight to the main mix's "
				"outputs; it also moves the project cursor to position (transport.changed), while the transport plays "
				"cued exactly as transport.locate does, so that the other tracks play on without a dropout. Or, "
				"instead of track, a media file on its own, or a clip, its media file with the clip's fades, as the "
				"sample editor scrubs its clip: position is then one of the file's sample frames, and the cursor "
				"stays. Ends with audition.stop. Announces audition.changed.",
				scrubParameters, status, false},
			[this, source, report](CommandContext& context)
			{
				const Source given = source(context.parameters());
				std::optional<double> seconds;
				if (const auto found = context.parameters().find("seconds"); found != context.parameters().end())
					seconds = found->get<double>();
				// One per mouse move would push everything else out of the log: only the first scrub of a drag goes in.
				if (m_audition.scrubbing() && m_audition.subject() == given.subject)
					context.leaveOutOfLog();
				// The cursor first, so that a busy transport fails the scrub before it changes the audition. While the
				// transport plays, cued exactly as transport.locate does it, so that the other tracks play on without
				// a dropout; while it stands, the cue thread follows, which keeps scrubs quick.
				if (given.subject.track != 0)
				{
					if (m_transport.playing())
						locate({{"position", given.position}});
					else
						sent(m_transport.locate(given.position));
				}
				m_audition.scrub(given.subject, given.playback, m_mediaPaths, m_snapshots.latest().sampleRate,
					given.position, seconds, given.channels);
				if (given.subject.track != 0)
					context.emit("transport.changed", transportStatus());
				return report(context);
			});
		engine.registerCommand(
			{"audition.stop", 1,
				"Ends auditioning and scrubbing. Does nothing if neither runs. Announces audition.changed.",
				Json{{"type", "object"}, {"additionalProperties", false}}, status, false},
			[this, report](CommandContext& context)
			{
				if (m_audition.subject() != Audition::Subject())
					m_audition.stop();
				return report(context);
			});
	}
}
