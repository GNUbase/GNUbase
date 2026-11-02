// SPDX-License-Identifier: GPL-3.0-or-later
// Auditioning and scrubbing one audio track (#470, docs/features/audition.md), and the commands audition.start,
// audition.scrub and audition.stop.
#include "engine/audio/audition.h"

#include "engine/audio/audioengine.h"
#include "engine/document/channelformat.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		constexpr auto g_startWait = std::chrono::seconds(2); // for the streams before auditioning starts
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

	void Audition::play(ObjectId track, const Playback::Track& source, std::map<ObjectId, std::string> mediaPaths,
		double sampleRate, time::Samples position, int channels, uint32_t maxFrames)
	{
		m_track = track;
		m_scrubbing = false;
		m_readers.clear();
		auto state = std::make_unique<State>();
		state->channels = channels;
		state->start = position;
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
		m_playback.cue(cue);
		m_playback.waitUntilBuffered(g_startWait);
		publish(std::move(state));
	}

	void Audition::scrub(ObjectId track, const Playback::Track& source,
		const std::map<ObjectId, std::string>& mediaPaths, double sampleRate, time::Samples position,
		std::optional<double> seconds, int channels)
	{
		const auto now = std::chrono::steady_clock::now();
		if (m_track != track || !m_scrubbing)
		{
			stop();
			m_track = track;
			m_scrubbing = true;
			m_head = position;
			m_lastScrub = now;
			return;
		}
		const double elapsed = std::chrono::duration<double>(now - m_lastScrub).count();
		const double duration = seconds.value_or(std::clamp(elapsed, g_minScrubSeconds, g_maxScrubSeconds));
		m_lastScrub = now;
		const auto most = static_cast<time::Samples>(g_maxScrubMaterial * sampleRate);
		const time::Samples from = std::clamp(m_head, position - most, position + most);
		m_head = position;
		const auto frames = static_cast<size_t>(std::llround(duration * sampleRate));
		if (from == position || frames == 0 || channels <= 0)
			return; // standing still: what it played runs out
		// The track between both positions, then read through at the speed it takes from one to the other.
		const time::Samples low = std::min(from, position);
		const time::Samples length = std::max(from, position) - low + 1;
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
	}

	void Audition::stop()
	{
		m_track = 0;
		m_scrubbing = false;
		m_readers.clear();
		publish(std::make_unique<State>());
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
		uint64_t serial = 0;
		time::Samples start = 0;
		{
			const std::scoped_lock lock(m_mutex);
			if (!m_cuePlaying)
				return;
			serial = m_cueSerial;
			start = m_cueStart;
		}
		const auto [played, position] = m_played.load();
		Playback::Cue cue;
		cue.playing = reading && played == serial;
		cue.position = played == serial ? position : start;
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
		if (state.player)
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
			for (size_t c = 0; c < channels; ++c)
				add(c, state.audio[c], 1, count);
			m_position += count;
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
		m_played.store(m_playing, m_position);
		m_states.release();
	}

	void AudioEngine::registerAuditionCommands(Engine& engine)
	{
		const Json status = Json::parse(R"({"type": "object", "additionalProperties": false,
			"required": ["track", "mode"], "properties": {"track": {"type": "integer"},
			"mode": {"type": "string", "enum": ["off", "play", "scrub"]}}})");
		const Json parameters = Json::parse(R"({"type": "object", "additionalProperties": false,
			"required": ["track", "position"],
			"properties": {"track": {"type": "integer"}, "position": {"type": "integer"}}})");
		Json scrubParameters = parameters;
		scrubParameters["properties"]["seconds"] = {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}};
		const auto report = [this](CommandContext& context)
		{
			Json result = {{"track", m_audition.track()},
				{"mode",
					m_audition.track() == 0      ? "off"
						: m_audition.scrubbing() ? "scrub"
												 : "play"}};
			context.emit("audition.changed", result);
			return result;
		};
		// The audio track, its playlist and the main mix's channels for a command's parameters.
		struct Source
		{
			ObjectId track = 0;
			const Playback::Track* playback = nullptr;
			time::Samples position = 0;
			int channels = 0;
		};
		const auto source = [this](const Json& given)
		{
			require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
			Source result;
			result.track = given.at("track").get<ObjectId>();
			result.position = given.at("position").get<time::Samples>();
			require(result.position >= 0, "position must not be negative");
			const auto found = m_auditionTracks.find(result.track);
			require(found != m_auditionTracks.end(), "no audio track " + std::to_string(result.track));
			result.playback = &found->second;
			for (const RenderBus& bus : m_snapshots.latest().busses)
				if (bus.mainMix)
					result.channels = channelCount(bus.format);
			return result;
		};

		engine.registerCommand(
			{"audition.start", 1,
				"Auditions an audio track, as the play tool does while the mouse is pressed: plays its events from "
				"position (samples) until audition.stop, straight to the main mix's outputs, without its channel's "
				"fader, pan, mute and effects, and without the transport. A mono clip plays on every channel. Ends "
				"what was auditioned before. Announces audition.changed.",
				parameters, status, false},
			[this, source, report](CommandContext& context)
			{
				const Source given = source(context.parameters());
				m_audition.play(given.track, *given.playback, m_mediaPaths, m_snapshots.latest().sampleRate,
					given.position, given.channels, m_device->format().blockSize);
				return report(context);
			});
		engine.registerCommand(
			{"audition.scrub", 1,
				"Scrubs an audio track, as the scrub tool does while the mouse drags: plays its events from where the "
				"last scrub of the track got to, to position (samples), forwards or backwards, in seconds (default: "
				"the time since that scrub, 0.01 to 0.1 seconds), so speed and pitch follow how fast the positions "
				"come; the first scrub only sets where the next one starts. Like audition.start it goes straight to "
				"the main mix's outputs; it also moves the project cursor to position (transport.changed). Ends with "
				"audition.stop. Announces audition.changed.",
				scrubParameters, status, false},
			[this, source, report](CommandContext& context)
			{
				const Source given = source(context.parameters());
				std::optional<double> seconds;
				if (const auto found = context.parameters().find("seconds"); found != context.parameters().end())
					seconds = found->get<double>();
				m_audition.scrub(given.track, *given.playback, m_mediaPaths, m_snapshots.latest().sampleRate,
					given.position, seconds, given.channels);
				sent(m_transport.locate(given.position));
				context.emit("transport.changed", transportStatus());
				return report(context);
			});
		engine.registerCommand(
			{"audition.stop", 1,
				"Ends auditioning and scrubbing. Does nothing if neither runs. Announces audition.changed.",
				Json{{"type", "object"}, {"additionalProperties", false}}, status, false},
			[this, report](CommandContext& context)
			{
				if (m_audition.track() != 0)
					m_audition.stop();
				return report(context);
			});
	}
}
