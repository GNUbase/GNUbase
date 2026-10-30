// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/metronome/metronome.h"

#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/resampler.h"
#include "engine/dsp/dsp.h"
#include "platform/log.h"

#include <algorithm>
#include <cmath>
#include <system_error>

namespace gnubase::engine::metronome
{
	namespace
	{
		constexpr size_t g_readFrames = 4096;
		constexpr double g_maxVelocity = 65535.0;

		std::shared_ptr<const Sound> scaled(const Sound& sound, double gain)
		{
			auto result = std::make_shared<Sound>(sound);
			for (float& sample : *result)
				sample = static_cast<float>(sample * gain);
			return result;
		}

		// Mono at the file's rate, at most g_maxSampleSeconds long.
		Sound decode(const std::string& path, double& sampleRate)
		{
			const std::unique_ptr<AudioFileReader> reader = AudioFileReader::open(path);
			const AudioFileInfo& info = reader->info();
			sampleRate = info.sampleRate;
			const auto channels = static_cast<size_t>(std::max(info.channels, 1));
			const auto maxFrames = static_cast<size_t>(
				std::min<double>(static_cast<double>(info.frames), std::ceil(g_maxSampleSeconds * info.sampleRate)));
			Sound mono;
			mono.reserve(maxFrames);
			std::vector<float> buffer(g_readFrames * channels);
			while (mono.size() < maxFrames)
			{
				const size_t read = reader->read(buffer.data(), std::min(g_readFrames, maxFrames - mono.size()));
				if (read == 0)
					break;
				for (size_t frame = 0; frame < read; ++frame)
				{
					double sum = 0.0;
					for (size_t c = 0; c < channels; ++c)
						sum += buffer[(frame * channels) + c];
					mono.push_back(static_cast<float>(sum / static_cast<double>(channels)));
				}
			}
			return mono;
		}

		Sound resample(const Sound& sound, double from, double to)
		{
			if (from == to || sound.empty())
				return sound;
			Resampler resampler(from, to, 1, sound.size());
			Sound result;
			resampler.process(sound.data(), sound.size(), result);
			resampler.finish(result);
			return result;
		}
	}

	std::shared_ptr<const Sound> SoundLibrary::beep(const Beep& beep, double gain, double sampleRate)
	{
		// A sine tone that starts within a millisecond and decays by 60 dB over its length.
		const auto frames = static_cast<size_t>(std::llround(g_beepSeconds * sampleRate));
		const double attack = std::max(1.0, std::round(0.001 * sampleRate));
		auto sound = std::make_shared<Sound>(frames);
		for (size_t i = 0; i < frames; ++i)
		{
			const auto at = static_cast<double>(i);
			const double envelope = std::min(1.0, at / attack) * std::exp(-6.907755 * at / static_cast<double>(frames));
			(*sound)[i] =
				static_cast<float>(gain * envelope * std::sin(2.0 * dsp::g_pi * beep.frequency * at / sampleRate));
		}
		return sound;
	}

	std::shared_ptr<const Sound> SoundLibrary::sample(const std::string& path, double gain, double sampleRate)
	{
		const Stamp now = stamp(path);
		Decoded& decoded = m_decoded[{path, sampleRate}];
		if (!decoded.sound || !(decoded.stamp == now))
		{
			double rate = sampleRate;
			const Sound sound = decode(path, rate);
			decoded = {now, std::make_shared<const Sound>(resample(sound, rate, sampleRate))};
		}
		return scaled(*decoded.sound, gain);
	}

	void SoundLibrary::check(const std::string& path)
	{
		AudioFileReader::open(path);
	}

	SoundLibrary::Stamp SoundLibrary::stamp(const std::string& path)
	{
		const std::filesystem::path file = std::filesystem::u8path(path);
		std::error_code error;
		Stamp result;
		result.modified = std::filesystem::last_write_time(file, error);
		if (error)
			return {};
		result.size = std::filesystem::file_size(file, error);
		result.exists = !error;
		return result.exists ? result : Stamp{};
	}

	std::unique_ptr<const ClickConfig> compile(const MetronomeSettings& settings, const time::TempoMap& tempo,
		const time::SignatureMap& signatures, double projectRate, double deviceRate, SoundLibrary& sounds)
	{
		auto config = std::make_unique<ClickConfig>();
		config->clickWhilePlaying = settings.click && settings.whilePlaying;
		config->sampleRate = projectRate;
		config->tempo = tempo;
		config->signatures = signatures;
		for (const ClickPattern& pattern : settings.patterns)
		{
			PatternTable::Entry entry;
			entry.signature = pattern.signature;
			entry.clicks = static_cast<int>(std::min<size_t>(pattern.accents.size(), g_maxClicks));
			std::copy_n(pattern.accents.begin(), entry.clicks, entry.accents.begin());
			config->patterns.entries.push_back(entry);
		}

		config->audio = settings.audio;
		for (size_t accent = 0; accent < g_accents; ++accent)
		{
			const Sample& sample = settings.samples[accent];
			std::shared_ptr<const Sound>& sound = config->sounds[accent];
			if (settings.sounds == Sounds::Samples && !sample.path.empty())
			{
				try
				{
					sound = sounds.sample(sample.path, dsp::dbToGain(settings.level + sample.level), deviceRate);
				}
				catch (const std::exception& error)
				{
					// A file that is gone or broken plays the beep.
					logging::warning("The metronome plays a beep for accent {}: {}", accent + 1, error.what());
				}
			}
			if (!sound)
				sound = SoundLibrary::beep(
					settings.beeps[accent], dsp::dbToGain(settings.level + settings.beeps[accent].level), deviceRate);
		}

		config->midi = settings.midi;
		config->channel = static_cast<uint8_t>(std::clamp(settings.channel, 1, 16) - 1);
		for (size_t accent = 0; accent < g_accents; ++accent)
		{
			config->notes[accent] = static_cast<uint8_t>(std::clamp(settings.notes[accent].note, 0, 127));
			config->velocities[accent] = static_cast<uint16_t>(
				std::lround(std::clamp(settings.notes[accent].velocity, 0.0, 1.0) * g_maxVelocity));
		}
		config->noteLength = static_cast<uint32_t>(std::max<int64_t>(1, std::llround(g_midiNoteSeconds * deviceRate)));
		return config;
	}

	Metronome::Metronome() : m_config(std::make_unique<const ClickConfig>())
	{
	}

	void Metronome::setConfig(std::unique_ptr<const ClickConfig> config)
	{
		m_config.publish(std::move(config));
	}

	size_t Metronome::length(const ClickConfig& config, int accent) noexcept
	{
		const std::shared_ptr<const Sound>& sound = config.sounds[static_cast<size_t>(accent - 1)];
		return sound ? sound->size() : 0;
	}

	void Metronome::render(const ClickConfig& config, const graph::ProcessContext& context, uint32_t until) noexcept
	{
		// Voice by voice, so every frame adds up the voices in the same order however the block is split.
		const uint32_t from = m_rendered;
		m_rendered = std::max(m_rendered, until);
		if (!config.audio || context.channels < 1 || until <= from)
			return;
		float* out = context.audio[0];
		for (const Voice& voice : m_voices)
		{
			if (voice.accent == 0)
				continue;
			const int64_t start = voice.start - m_frame; // in the block
			const int64_t first = std::max<int64_t>(from, start);
			const int64_t last = std::min<int64_t>(until, start + static_cast<int64_t>(length(config, voice.accent)));
			const float* sound = last > first ? config.sounds[static_cast<size_t>(voice.accent - 1)]->data() : nullptr;
			for (int64_t i = first; i < last; ++i)
				out[i] += sound[i - start];
		}
	}

	void Metronome::endNote(graph::EventBuffer& out, uint32_t offset) noexcept
	{
		graph::ProcessEvent event;
		event.offset = offset;
		event.type = graph::ProcessEvent::Type::NoteOff;
		event.channel = m_noteChannel;
		event.key = m_noteKey;
		if (out.add(event))
			m_noteOn = false; // otherwise it ends in the next block
	}

	void Metronome::click(
		const ClickConfig& config, const graph::ProcessContext& context, uint32_t offset, int accent) noexcept
	{
		const int64_t now = m_frame + offset;
		render(config, context, offset);

		// The voice that is free, or else the one that started first.
		Voice* voice = &m_voices.front();
		for (Voice& candidate : m_voices)
		{
			const bool free = candidate.accent == 0 ||
				now >= candidate.start + static_cast<int64_t>(length(config, candidate.accent));
			if (free)
			{
				voice = &candidate;
				break;
			}
			if (candidate.start < voice->start)
				voice = &candidate;
		}
		*voice = {accent, now};

		if (!config.midi)
			return;
		if (m_noteOn)
			endNote(
				context.eventsOut, std::min(offset, static_cast<uint32_t>(std::max<int64_t>(0, m_noteEnd - m_frame))));
		if (m_noteOn)
			return; // no room for both
		graph::ProcessEvent event;
		event.offset = offset;
		event.type = graph::ProcessEvent::Type::NoteOn;
		event.channel = config.channel;
		event.key = config.notes[static_cast<size_t>(accent - 1)];
		event.velocity = config.velocities[static_cast<size_t>(accent - 1)];
		if (!context.eventsOut.add(event))
			return;
		m_noteOn = true;
		m_noteChannel = event.channel;
		m_noteKey = event.key;
		m_noteEnd = now + config.noteLength;
	}

	void Metronome::process(const graph::ProcessContext& context) noexcept
	{
		const ClickConfig& config = m_config.acquire();
		const graph::TransportBlock& transport = context.transport;
		m_rendered = 0;
		if (context.channels >= 1)
			std::fill_n(context.audio[0], context.frames, 0.0f);
		const auto emit = [this, &config, &context](uint32_t offset, int accent)
		{
			// A note that ends before the click ends first, so the events stay in order.
			if (m_noteOn && m_noteEnd <= m_frame + offset)
				endNote(context.eventsOut, static_cast<uint32_t>(std::max<int64_t>(0, m_noteEnd - m_frame)));
			click(config, context, offset, accent);
		};

		const ProjectTimeline project{config.tempo, config.signatures, config.sampleRate};
		if (transport.countInFrames > 0)
		{
			const graph::CountIn& countIn = transport.countIn;
			const uint32_t frames = std::min(transport.countInFrames, context.frames);
			const int64_t begin = (countIn.projectTime ? countIn.origin : 0) + transport.countInPosition;
			const auto at = [&emit, begin](time::Samples position, int accent)
			{ emit(static_cast<uint32_t>(position - begin), accent); };
			if (countIn.projectTime)
				clicksIn(project, config.patterns, begin, begin + frames, at);
			else
				clicksIn(ConstantTimeline{countIn.bpm, {countIn.numerator, countIn.denominator}, config.sampleRate},
					config.patterns, begin, begin + frames, at);
		}
		if (transport.playing && config.clickWhilePlaying)
			for (size_t s = 0; s < transport.segmentCount; ++s)
			{
				const graph::TransportSegment& segment = transport.segments[s];
				clicksIn(project, config.patterns, segment.position, segment.position + segment.frames,
					[&emit, &segment](time::Samples position, int accent)
					{ emit(segment.offset + static_cast<uint32_t>(position - segment.position), accent); });
			}

		render(config, context, context.frames);
		if (m_noteOn && m_noteEnd < m_frame + context.frames)
			endNote(context.eventsOut, static_cast<uint32_t>(std::max<int64_t>(0, m_noteEnd - m_frame)));
		m_frame += context.frames;
		m_config.release();
	}
}
