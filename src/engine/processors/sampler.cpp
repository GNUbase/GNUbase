// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/processors/sampler.h"

#include "engine/audiofile/audiofile.h"
#include "engine/dsp/dsp.h"
#include "engine/processors/builtin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <map>
#include <mutex>

namespace gnubase::engine::processors
{
	namespace
	{
		// Catmull-Rom between y1 and y2 at t (0 to 1): exactly y1 at 0, so a sample played at its own pitch comes out
		// unchanged.
		double hermite(double y0, double y1, double y2, double y3, double t)
		{
			const double c1 = 0.5 * (y2 - y0);
			const double c2 = y0 - (2.5 * y1) + (2.0 * y2) - (0.5 * y3);
			const double c3 = (0.5 * (y3 - y0)) + (1.5 * (y1 - y2));
			return (((((c3 * t) + c2) * t) + c1) * t) + y1;
		}

		double milliseconds(double ms, double sampleRate)
		{
			return std::max(ms, 0.0) * sampleRate / 1000.0;
		}

		// How long a voice that a monophonic note takes over fades out.
		constexpr double g_takeOverMs = 5.0;

		std::shared_ptr<const Sample> loadSample(const std::string& path)
		{
			auto sample = std::make_shared<Sample>();
			try
			{
				const std::unique_ptr<AudioFileReader> reader = AudioFileReader::open(path);
				const AudioFileInfo& info = reader->info();
				const auto channels = static_cast<size_t>(std::max(info.channels, 1));
				std::vector<float> interleaved(static_cast<size_t>(info.frames) * channels);
				const size_t frames = reader->read(interleaved.data(), static_cast<size_t>(info.frames));
				sample->sampleRate = info.sampleRate;
				sample->left.resize(frames);
				if (channels > 1)
					sample->right.resize(frames);
				for (size_t i = 0; i < frames; ++i)
				{
					sample->left[i] = interleaved[i * channels];
					if (channels > 1)
						sample->right[i] = interleaved[(i * channels) + 1];
				}
			}
			catch (const std::exception&)
			{
				return std::make_shared<const Sample>(); // a missing or unreadable file plays nothing
			}
			return sample;
		}
	}

	std::shared_ptr<const Sample> sharedSample(const std::string& path)
	{
		namespace fs = std::filesystem;
		struct Cached
		{
			fs::file_time_type written;
			uintmax_t size = 0;
			std::weak_ptr<const Sample> sample;
		};
		// ponytail: one lock for every file, read under it; a lock per file if samplers load in parallel.
		static std::mutex mutex;
		static std::map<std::string, Cached> cache;
		std::error_code error;
		const fs::path file = fs::u8path(path);
		const fs::file_time_type written = fs::last_write_time(file, error);
		const uintmax_t size = error ? 0 : fs::file_size(file, error);
		if (error)
			return std::make_shared<const Sample>();
		const std::lock_guard<std::mutex> lock(mutex);
		for (auto entry = cache.begin(); entry != cache.end();)
			entry = entry->second.sample.expired() ? cache.erase(entry) : std::next(entry);
		Cached& cached = cache[path];
		std::shared_ptr<const Sample> sample = cached.sample.lock();
		if (!sample || cached.written != written || cached.size != size)
		{
			sample = loadSample(path);
			cached = {written, size, sample};
		}
		return sample;
	}

	Sampler::Sampler()
		: m_sample(std::make_unique<const std::shared_ptr<const Sample>>(std::make_shared<const Sample>()))
	{
		if (const BuiltinInfo* info = findBuiltin("sampler"))
			for (const ParameterInfo& parameter : info->parameters)
				m_values.at(parameter.id) = parameter.defaultValue;
	}

	void Sampler::setSample(std::shared_ptr<const Sample> sample)
	{
		m_sample.publish(std::make_unique<const std::shared_ptr<const Sample>>(std::move(sample)));
	}

	void Sampler::collect()
	{
		m_sample.collect();
	}

	void Sampler::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
	}

	void Sampler::reset() noexcept
	{
		for (Voice& voice : m_voices)
			voice.active = false;
		m_heldSince.fill(0);
		m_bend = 0.0;
	}

	int Sampler::activeVoices() const
	{
		return static_cast<int>(
			std::count_if(m_voices.begin(), m_voices.end(), [](const Voice& voice) { return voice.active; }));
	}

	void Sampler::setParameter(uint32_t id, double value) noexcept
	{
		if (id < m_values.size())
			m_values[id] = value;
	}

	// Starts a note on a free voice, or else on the oldest.
	void Sampler::start(uint8_t channel, uint8_t note, uint16_t velocity, const Sample& sample) noexcept
	{
		Voice* const first = m_voices.data();
		Voice* const last = first + m_voices.size();
		Voice* chosen = std::find_if(first, last, [](const Voice& v) { return !v.active; });
		if (chosen == last)
			chosen =
				std::min_element(first, last, [](const Voice& a, const Voice& b) { return a.started < b.started; });
		Voice& voice = *chosen;
		const auto frames = static_cast<double>(sample.frames());
		const double start = std::clamp(m_values[Start], 0.0, 1.0) * frames;
		const double end = std::clamp(m_values[End], 0.0, 1.0) * frames;
		voice = {};
		voice.active = true;
		voice.held = true;
		voice.note = note;
		voice.channel = channel;
		voice.started = ++m_notes;
		voice.from = start;
		voice.to = end;
		if (m_values[PlaybackMode] >= 0.5)
		{
			const double slices = std::max(std::round(m_values[Slices]), 1.0);
			const double index = static_cast<double>(note) - std::round(m_values[RootKey]);
			const double length = (end - start) / slices;
			voice.slice = true;
			voice.from = start + (index * length);
			voice.to = voice.from + length;
		}
		voice.position = voice.from;
		voice.amplitude = velocity / 65535.0;
		voice.level = m_values[Attack] > 0.0 ? 0.0 : 1.0;
		voice.stage = m_values[Attack] > 0.0 ? Stage::Attack : Stage::Decay;
		voice.filter = filterFor(voice);
	}

	void Sampler::release(Voice& voice) noexcept
	{
		voice.held = false;
		if (m_values[OneShot] >= 0.5 || voice.stage == Stage::Release)
			return;
		voice.stage = Stage::Release;
		const double frames = std::round(milliseconds(m_values[Release], m_sampleRate));
		if (frames < 1.0)
			voice.active = false;
		else
			voice.releaseStep = voice.level / frames;
	}

	// Ends a voice quickly but without a click, also a one-shot one: the envelope ends it once its level is 0.
	void Sampler::fadeOut(Voice& voice) noexcept
	{
		voice.held = false;
		voice.stage = Stage::Release;
		voice.releaseStep = voice.level / std::max(std::round(milliseconds(g_takeOverMs, m_sampleRate)), 1.0);
	}

	// Monophonic mode: the voice that plays the note, the one started last that isn't released.
	Sampler::Voice* Sampler::monophonicVoice() noexcept
	{
		Voice* playing = nullptr;
		for (Voice& voice : m_voices)
			if (voice.active && voice.stage != Stage::Release &&
				(playing == nullptr || voice.started > playing->started))
				playing = &voice;
		return playing;
	}

	void Sampler::noteOn(uint8_t channel, uint8_t note, uint16_t velocity) noexcept
	{
		if (velocity == 0)
		{
			noteOff(channel, note);
			return;
		}
		const Sample& sample = *m_playing;
		if (sample.frames() == 0 || note >= m_heldSince.size())
			return;
		const bool slices = m_values[PlaybackMode] >= 0.5;
		const auto root = static_cast<int>(std::round(m_values[RootKey]));
		if (slices ? (note < root || note - root >= static_cast<int>(std::round(m_values[Slices])))
				   : (note < std::round(m_values[KeyLow]) || note > std::round(m_values[KeyHigh])))
			return;

		if (m_values[Monophonic] >= 0.5)
		{
			m_heldSince[note] = m_notes + 1;
			Voice* playing = monophonicVoice();
			// Legato: a note played while another is held only changes the pitch; the envelopes run on.
			if (playing != nullptr && m_values[Legato] >= 0.5 && playing->held && !slices)
			{
				playing->note = note;
				playing->channel = channel;
				playing->started = ++m_notes;
				playing->filter = filterFor(*playing);
				return;
			}
			// Every voice fades out, also those of notes from before monophonic mode or still releasing.
			for (Voice& voice : m_voices)
				if (voice.active)
					fadeOut(voice);
		}
		start(channel, note, velocity, sample);
	}

	void Sampler::noteOff(uint8_t channel, uint8_t note) noexcept
	{
		if (note >= m_heldSince.size())
			return;
		const bool monophonic = m_values[Monophonic] >= 0.5;
		if (monophonic)
			m_heldSince[note] = 0;
		// Voices of notes played before monophonic mode came on end like polyphonic ones.
		const Voice* const playing = monophonic ? monophonicVoice() : nullptr;
		for (Voice& voice : m_voices)
		{
			if (!voice.active || !voice.held || voice.note != note || voice.channel != channel)
				continue;
			if (&voice != playing)
			{
				release(voice);
				continue;
			}
			// A note it took over that is still held plays again.
			size_t latest = 0;
			for (size_t key = 1; key < m_heldSince.size(); ++key)
				if (m_heldSince[key] > m_heldSince[latest])
					latest = key;
			if (m_heldSince[latest] == 0)
			{
				release(voice);
				continue;
			}
			const auto back = static_cast<uint8_t>(latest);
			if (m_values[Legato] >= 0.5 && !voice.slice)
			{
				voice.note = back;
				voice.filter = filterFor(voice);
				continue;
			}
			const auto velocity = static_cast<uint16_t>(std::lround(voice.amplitude * 65535.0));
			fadeOut(voice);
			start(channel, back, velocity, *m_playing);
		}
	}

	dsp::Biquad Sampler::filterFor(const Voice& voice) const noexcept
	{
		const double keys = static_cast<double>(voice.note) - std::round(m_values[RootKey]);
		const double cutoff = std::clamp(
			m_values[Cutoff] * std::pow(2.0, keys * m_values[KeyFollow] / 1200.0), 20.0, 0.49 * m_sampleRate);
		// 0 % resonance is a Butterworth response, 100 % a Q of about 23.
		const double q = 0.7071 * std::pow(2.0, m_values[Resonance] / 20.0);
		const auto type = static_cast<int>(std::round(m_values[FilterType]));
		if (type == 1)
			return dsp::Biquad::highPass(m_sampleRate, cutoff, q);
		if (type == 2)
			return dsp::Biquad::bandPass(m_sampleRate, cutoff, q);
		return dsp::Biquad::lowPass(m_sampleRate, cutoff, q);
	}

	double Sampler::increment(const Voice& voice, const Sample& sample) const noexcept
	{
		const bool keyed = !voice.slice && m_values[FixedPitch] < 0.5;
		const double semitones = (keyed ? static_cast<double>(voice.note) - std::round(m_values[RootKey]) : 0.0) +
			(12.0 * std::round(m_values[Octave])) + std::round(m_values[Coarse]) + (m_values[Fine] / 100.0) +
			(m_bend * std::round(m_values[PitchBendRange]));
		return std::pow(2.0, semitones / 12.0) * sample.sampleRate / m_sampleRate;
	}

	// Moves the voice on by step frames; false once it has played its range.
	bool Sampler::advance(Voice& voice, double step, double frames) const noexcept
	{
		voice.position += voice.direction * step;
		const auto loop = static_cast<Loop>(std::lround(m_values[LoopMode]));
		const double loopStart = std::clamp(m_values[LoopStart] * frames, voice.from, voice.to);
		const double loopEnd = std::clamp(m_values[LoopEnd] * frames, voice.from, voice.to);
		const bool looping =
			!voice.slice && m_values[OneShot] < 0.5 && loop != Loop::None && loopEnd - loopStart >= 1.0;
		if (looping)
		{
			const bool held = voice.held && voice.stage != Stage::Release;
			const bool alternate = loop == Loop::Alternate || (loop == Loop::AlternateUntilRelease && held);
			const bool wraps = loop == Loop::Continuous || (loop == Loop::UntilRelease && held) ||
				(loop == Loop::Once && !voice.loopedOnce);
			const double length = loopEnd - loopStart;
			if (alternate)
			{
				// Wherever a step goes, also past both ends of a short loop at a high pitch: the loop unfolded is a
				// cycle twice its length, forwards and then backwards.
				if ((voice.direction > 0.0 && voice.position >= loopEnd) ||
					(voice.direction < 0.0 && voice.position <= loopStart))
				{
					const double x = voice.position - loopStart;
					double cycle = std::fmod(voice.direction > 0.0 ? x : (2.0 * length) - x, 2.0 * length);
					if (cycle < 0.0)
						cycle += 2.0 * length;
					voice.direction = cycle < length ? 1.0 : -1.0;
					voice.position = loopStart + (cycle < length ? cycle : (2.0 * length) - cycle);
				}
				return true;
			}
			voice.direction = 1.0; // a released alternating loop plays on to the end
			if (wraps && voice.position >= loopEnd)
			{
				voice.position = loopStart + std::fmod(voice.position - loopStart, length);
				voice.loopedOnce = true;
			}
		}
		return voice.position < voice.to;
	}

	double Sampler::envelope(Voice& voice) noexcept
	{
		switch (voice.stage)
		{
		case Stage::Attack:
		{
			voice.level += 1.0 / std::max(milliseconds(m_values[Attack], m_sampleRate), 1.0);
			if (voice.level >= 1.0)
			{
				voice.level = 1.0;
				voice.stage = Stage::Decay;
			}
			break;
		}
		case Stage::Decay:
		{
			const double sustain = std::clamp(m_values[Sustain] / 100.0, 0.0, 1.0);
			voice.level -= (1.0 - sustain) / std::max(milliseconds(m_values[Decay], m_sampleRate), 1.0);
			if (voice.level <= sustain)
			{
				voice.level = sustain;
				voice.stage = Stage::Sustain;
			}
			break;
		}
		case Stage::Sustain:
			voice.level = std::clamp(m_values[Sustain] / 100.0, 0.0, 1.0);
			break;
		case Stage::Release:
			voice.level -= voice.releaseStep;
			if (voice.level <= 0.0)
			{
				voice.level = 0.0;
				voice.active = false;
			}
			break;
		}
		return voice.level;
	}

	void Sampler::render(
		const graph::ProcessContext& context, uint32_t begin, uint32_t end, const Sample& sample) noexcept
	{
		for (int c = 0; c < context.channels; ++c)
			std::memset(context.audio[c] + begin, 0, (end - begin) * sizeof(float));
		if (context.channels == 0 || sample.frames() == 0)
			return;
		const int64_t frames = sample.frames();
		const bool reverse = m_values[Reverse] >= 0.5;
		const bool stereo = !sample.right.empty();
		// The sample's frame index (from its end when reversed) of channel, silence outside it.
		const auto at = [&](const std::vector<float>& data, int64_t index) -> double
		{
			if (index < 0 || index >= frames)
				return 0.0;
			return data[static_cast<size_t>(reverse ? frames - 1 - index : index)];
		};
		const auto read = [&](const std::vector<float>& data, double position)
		{
			const double whole = std::floor(position);
			const auto i = static_cast<int64_t>(whole);
			return hermite(at(data, i - 1), at(data, i), at(data, i + 1), at(data, i + 2), position - whole);
		};
		const double gain = dsp::dbToGain(m_values[Volume] + m_values[Gain]);
		const double pan = std::clamp(m_values[Pan], -1.0, 1.0);
		const double leftPan = std::min(1.0, 1.0 - pan);
		const double rightPan = std::min(1.0, 1.0 + pan);
		const bool filter = m_values[FilterOn] >= 0.5;
		const double drive = filter && m_values[Drive] > 0.0 ? dsp::dbToGain(m_values[Drive]) : 0.0; // p. 722
		float* left = context.audio[0];
		float* right = context.channels > 1 ? context.audio[1] : nullptr;
		for (Voice& voice : m_voices)
		{
			if (!voice.active)
				continue;
			const double step = increment(voice, sample);
			if (filter)
				voice.filter = filterFor(voice);
			for (uint32_t i = begin; i < end && voice.active; ++i)
			{
				double l = read(sample.left, voice.position);
				double r = stereo ? read(sample.right, voice.position) : l;
				const double scale = voice.amplitude * gain * envelope(voice);
				l *= scale;
				r *= scale;
				if (drive > 0.0)
				{
					l = std::tanh(l * drive);
					r = std::tanh(r * drive);
				}
				if (filter)
				{
					l = voice.state[0].process(voice.filter, l);
					r = voice.state[1].process(voice.filter, r);
				}
				if (right != nullptr)
				{
					left[i] += static_cast<float>(l * leftPan);
					right[i] += static_cast<float>(r * rightPan);
				}
				else
				{
					left[i] += static_cast<float>(0.5 * (l + r));
				}
				if (!advance(voice, step, static_cast<double>(frames)))
					voice.active = false;
			}
			for (dsp::BiquadState& state : voice.state)
				state.flush();
		}
	}

	void Sampler::process(const graph::ProcessContext& context) noexcept
	{
		const Sample& sample = *m_sample.acquire();
		m_playing = &sample;
		splitAtEvents(
			context,
			[this](const graph::ProcessEvent& event)
			{
				switch (event.type)
				{
				case graph::ProcessEvent::Type::NoteOn:
					noteOn(event.channel, event.key, event.velocity);
					break;
				case graph::ProcessEvent::Type::NoteOff:
					noteOff(event.channel, event.key);
					break;
				case graph::ProcessEvent::Type::Parameter:
					setParameter(event.parameter, event.parameterValue);
					break;
				case graph::ProcessEvent::Type::PitchBend:
					m_bend = (static_cast<double>(event.value) - 2147483648.0) / 2147483648.0;
					break;
				case graph::ProcessEvent::Type::Controller:
				case graph::ProcessEvent::Type::ChannelPressure:
				case graph::ProcessEvent::Type::PolyPressure:
				case graph::ProcessEvent::Type::Program:
				case graph::ProcessEvent::Type::SysEx:
					break;
				}
			},
			[this, &context, &sample](uint32_t begin, uint32_t end) { render(context, begin, end, sample); });
	}
}
