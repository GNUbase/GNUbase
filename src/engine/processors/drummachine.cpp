// SPDX-License-Identifier: GPL-3.0-or-later
// The drum machine (#182, reference manual pp. 938-980). Each voice plays a pad: its sample, read between frames, or
// one of the percussion synths, all made of sines, squares and noise through a filter. Noise comes from each voice's
// own generator, seeded by the pad, so the same notes render the same way every time (architecture rule 6).
#include "engine/processors/drummachine.h"

#include "engine/dsp/dsp.h"
#include "engine/processors/builtin.h"
#include "platform/rt/denormals.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gnubase::engine::processors
{
	namespace
	{
		constexpr double g_pi = 3.14159265358979323846;
		// How long a voice that a gate, an exclusive group or a new voice ends fades out.
		constexpr double g_fadeMs = 5.0;
		// A voice ends once its envelope is this far down (-80 dB).
		constexpr double g_silent = 1e-4;
		// The frequencies of the classic six-square metal cluster, for hi-hats and cymbals.
		constexpr std::array<double, 6> g_metal = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};

		double square(double phase)
		{
			return phase - std::floor(phase) < 0.5 ? 1.0 : -1.0;
		}
	}

	namespace
	{
		std::vector<double> defaults()
		{
			std::vector<double> values(DrumMachine::ReverbLevel + 1, 0.0);
			if (const BuiltinInfo* info = findBuiltin("drumMachine"))
				for (const ParameterInfo& parameter : info->parameters)
					values.at(parameter.id) = parameter.defaultValue;
			return values;
		}
	}

	DrumMachine::DrumMachine() : m_kit(std::make_unique<const Kit>()), m_values(defaults())
	{
		auto kit = std::make_unique<Kit>();
		kit->values.assign(m_values.begin(), m_values.begin() + MasterLevel);
		kit->automated.assign(MasterLevel, false);
		publish(std::move(kit));
		m_delay.setParameter(dsp::Delay::Mix, 100.0);
		m_reverb.setParameter(dsp::Reverb::Mix, 100.0);
		for (const uint32_t id : {DelayTime, DelaySync, DelayNote, DelayFeedback, DelayPingPong, ReverbPredelay,
				 ReverbSize, ReverbTime, ReverbDamping})
			setParameter(id, m_values[id]);
	}

	void DrumMachine::setSample(size_t pad, std::shared_ptr<const Sample> sample)
	{
		if (pad >= static_cast<size_t>(g_drumPads))
			return;
		auto kit = std::make_unique<Kit>(m_kit.latest());
		kit->samples[pad] = std::move(sample);
		publish(std::move(kit));
	}

	void DrumMachine::setPadValues(const std::map<uint32_t, double>& values, const std::set<uint32_t>& automated)
	{
		static const std::vector<double> standard = defaults();
		std::vector<double> pads(standard.begin(), standard.begin() + MasterLevel);
		std::vector<bool> flags(MasterLevel, false);
		for (const auto& [id, value] : values)
			if (id < MasterLevel)
				pads[id] = value;
		for (const uint32_t id : automated)
			if (id < MasterLevel)
				flags[id] = true;
		const Kit& latest = m_kit.latest();
		if (pads == latest.values && flags == latest.automated)
			return;
		auto kit = std::make_unique<Kit>(latest);
		kit->values = std::move(pads);
		kit->automated = std::move(flags);
		publish(std::move(kit));
	}

	void DrumMachine::publish(std::unique_ptr<Kit> kit)
	{
		kit->serial = m_kit.latest().serial + 1;
		m_kit.publish(std::move(kit));
	}

	void DrumMachine::collect()
	{
		m_kit.collect();
	}

	void DrumMachine::prepare(double sampleRate, uint32_t maxFrames)
	{
		m_sampleRate = sampleRate;
		m_delay.prepare(sampleRate, maxFrames);
		m_reverb.prepare(sampleRate, maxFrames);
		for (auto* bus : {&m_delayBus, &m_reverbBus})
			for (std::vector<float>& channel : *bus)
				channel.assign(maxFrames, 0.0f);
	}

	void DrumMachine::reset() noexcept
	{
		for (Voice& voice : m_voices)
			voice.active = false;
		m_delay.reset();
		m_reverb.reset();
	}

	uint32_t DrumMachine::tail() const
	{
		// The longest decay, and what the effects hold after it.
		const auto voices = static_cast<uint32_t>(m_sampleRate * 10.0);
		const uint32_t delay = m_delay.tail();
		const uint32_t reverb = m_reverb.tail();
		if (delay == UINT32_MAX || reverb == UINT32_MAX)
			return UINT32_MAX;
		return voices + delay + reverb;
	}

	int DrumMachine::activeVoices() const
	{
		return static_cast<int>(
			std::count_if(m_voices.begin(), m_voices.end(), [](const Voice& voice) { return voice.active; }));
	}

	void DrumMachine::setParameter(uint32_t id, double value) noexcept
	{
		if (id >= m_values.size())
			return;
		m_values[id] = value;
		switch (id)
		{
		case DelayTime:
			m_delay.setParameter(dsp::Delay::Time, value);
			break;
		case DelaySync:
			m_delay.setParameter(dsp::Delay::Sync, value);
			break;
		case DelayNote:
			m_delay.setParameter(dsp::Delay::Note, value);
			break;
		case DelayFeedback:
			m_delay.setParameter(dsp::Delay::Feedback, value);
			break;
		case DelayPingPong:
			m_delay.setParameter(dsp::Delay::PingPong, value);
			break;
		case ReverbPredelay:
			m_reverb.setParameter(dsp::Reverb::Predelay, value);
			break;
		case ReverbSize:
			m_reverb.setParameter(dsp::Reverb::Size, value);
			break;
		case ReverbTime:
			m_reverb.setParameter(dsp::Reverb::Decay, value);
			break;
		case ReverbDamping:
			m_reverb.setParameter(dsp::Reverb::Damping, value);
			break;
		default:
			break;
		}
	}

	void DrumMachine::fadeOut(Voice& voice) const noexcept
	{
		voice.held = false;
		voice.fadeStep = std::max(voice.fadeStep, 1.0 / std::max(g_fadeMs * m_sampleRate / 1000.0, 1.0));
	}

	void DrumMachine::noteOn(uint8_t note, uint16_t velocity, const Kit& kit) noexcept
	{
		if (velocity == 0)
		{
			noteOff(note);
			return;
		}
		const int pad = note;
		if (pad >= g_drumPads)
			return;
		const auto layer = static_cast<Layer>(std::clamp<long>(std::lround(value(pad, Instrument)), 0, 8));
		if (layer == Layer::Off)
			return;
		const Sample* sample = kit.samples[static_cast<size_t>(pad)].get();
		if (layer == Layer::Sample && (sample == nullptr || sample->frames() == 0))
			return;

		// Pads of the same exclusive group cut each other off, e.g. a closed hi-hat the open one (p. 974).
		const long group = std::lround(value(pad, ExclusiveGroup));
		if (group > 0)
			for (Voice& voice : m_voices)
				if (voice.active && std::lround(value(voice.pad, ExclusiveGroup)) == group)
					fadeOut(voice);

		Voice* const first = m_voices.data();
		Voice* const last = first + m_voices.size();
		Voice* chosen = std::find_if(first, last, [](const Voice& v) { return !v.active; });
		if (chosen == last)
			chosen =
				std::min_element(first, last, [](const Voice& a, const Voice& b) { return a.started < b.started; });
		Voice& voice = *chosen;
		voice = {};
		voice.active = true;
		voice.held = true;
		voice.pad = static_cast<uint8_t>(pad);
		voice.layer = layer;
		voice.started = ++m_notes;
		const double share = std::clamp(value(pad, VelocityToLevel) / 100.0, 0.0, 1.0);
		voice.amplitude = 1.0 - share + (share * velocity / 65535.0);
		voice.noise = 0x9E3779B9u ^ (static_cast<uint32_t>(pad) * 2654435761u);
		const double pitch = std::pow(2.0, value(pad, Tune) / 12.0);
		if (layer == Layer::Sample && sample != nullptr)
		{
			const auto frames = static_cast<double>(sample->frames());
			voice.from = std::clamp(value(pad, Start), 0.0, 1.0) * frames;
			voice.to = std::clamp(value(pad, End), 0.0, 1.0) * frames;
			voice.position = voice.from;
			voice.step = pitch * sample->sampleRate / m_sampleRate;
			return;
		}
		// The synths' filters: bright noise for snares, hi-hats and cymbals, a band for claps and cowbells.
		const double tone = std::clamp(value(pad, Tone) / 100.0, 0.0, 1.0);
		const double nyquist = 0.45 * m_sampleRate;
		switch (layer)
		{
		case Layer::Snare:
			voice.filter = dsp::Biquad::highPass(m_sampleRate, std::min(800.0 + (tone * 4000.0), nyquist), 0.7071);
			break;
		case Layer::HiHat:
			voice.filter = dsp::Biquad::highPass(m_sampleRate, std::min(5000.0 + (tone * 6000.0), nyquist), 0.7071);
			break;
		case Layer::Cymbal:
			voice.filter = dsp::Biquad::highPass(m_sampleRate, std::min(2500.0 + (tone * 5000.0), nyquist), 0.7071);
			break;
		case Layer::Clap:
			voice.filter = dsp::Biquad::bandPass(m_sampleRate, std::min(800.0 + (tone * 1600.0), nyquist), 1.5);
			break;
		case Layer::Cowbell:
			voice.filter = dsp::Biquad::bandPass(m_sampleRate, std::min(800.0 * pitch, nyquist), 2.0);
			break;
		case Layer::Kick:
		case Layer::Tom:
			voice.filter = dsp::Biquad::lowPass(m_sampleRate, std::min(2000.0 + (tone * 10000.0), nyquist), 0.7071);
			break;
		case Layer::Off:
		case Layer::Sample:
			break;
		}
	}

	void DrumMachine::noteOff(uint8_t note) noexcept
	{
		for (Voice& voice : m_voices)
			if (voice.active && voice.held && voice.pad == note)
			{
				voice.held = false;
				if (value(voice.pad, Gate) >= 0.5) // p. 944
					fadeOut(voice);
			}
	}

	double DrumMachine::nextNoise(Voice& voice) noexcept
	{
		voice.noise = (voice.noise * 1664525u) + 1013904223u;
		return (static_cast<double>(voice.noise >> 8) / 8388608.0) - 1.0;
	}

	// One frame of a synth layer before its envelope, about -1 to 1.
	double DrumMachine::synth(Voice& voice) noexcept
	{
		const int pad = voice.pad;
		const double t = voice.frame / m_sampleRate;
		const double pitch = std::pow(2.0, value(pad, Tune) / 12.0);
		const double noise = std::clamp(value(pad, Noise) / 100.0, 0.0, 1.0);
		const double tone = std::clamp(value(pad, Tone) / 100.0, 0.0, 1.0);
		// A sine whose pitch falls from depth times its own to it.
		const auto body = [&](double frequency, double depth, double fall)
		{
			const double f = frequency * pitch * (1.0 + (depth * std::exp(-t / fall)));
			voice.phases[0] += f / m_sampleRate;
			voice.phases[0] -= std::floor(voice.phases[0]);
			return std::sin(2.0 * g_pi * voice.phases[0]);
		};
		const auto metal = [&](double scale)
		{
			double sum = 0.0;
			for (size_t i = 0; i < g_metal.size(); ++i)
			{
				voice.phases[i] += g_metal[i] * scale * pitch / m_sampleRate;
				voice.phases[i] -= std::floor(voice.phases[i]);
				sum += square(voice.phases[i]);
			}
			return sum / static_cast<double>(g_metal.size());
		};
		const auto filtered = [&](double x) { return voice.state[0].process(voice.filter, x); };
		switch (voice.layer)
		{
		case Layer::Kick:
		{
			const double drive = 1.0 + (3.0 * tone);
			const double click = noise * nextNoise(voice) * std::exp(-t / 0.003);
			return filtered((std::tanh(body(50.0, 3.0, 0.03) * drive) / std::tanh(drive)) + click);
		}
		case Layer::Tom:
			return filtered(body(110.0, 0.6, 0.05) + (0.3 * noise * nextNoise(voice) * std::exp(-t / 0.01)));
		case Layer::Snare:
		{
			const double head = body(180.0, 0.5, 0.02) * std::exp(-t / 0.06);
			return ((1.0 - noise) * head) + (noise * filtered(nextNoise(voice)));
		}
		case Layer::HiHat:
			return filtered(((1.0 - noise) * metal(1.0)) + (noise * nextNoise(voice)));
		case Layer::Cymbal:
			return filtered(((1.0 - noise) * metal(1.3)) + (noise * nextNoise(voice)));
		case Layer::Clap:
		{
			// Three quick claps 10 ms apart, then the tail.
			const double burst = t < 0.03 ? std::exp(-std::fmod(t, 0.01) / 0.002) : 1.0;
			return 2.0 * burst * filtered(nextNoise(voice));
		}
		case Layer::Cowbell:
		{
			voice.phases[0] += 540.0 * pitch / m_sampleRate;
			voice.phases[1] += 800.0 * pitch / m_sampleRate;
			for (size_t i = 0; i < 2; ++i)
				voice.phases[i] -= std::floor(voice.phases[i]);
			const double tones = 0.5 * (square(voice.phases[0]) + square(voice.phases[1]));
			return 2.0 * filtered(((1.0 - noise) * tones) + (noise * nextNoise(voice)));
		}
		case Layer::Off:
		case Layer::Sample:
			break;
		}
		return 0.0;
	}

	void DrumMachine::render(
		const graph::ProcessContext& context, uint32_t begin, uint32_t end, const Kit& kit) noexcept
	{
		for (int c = 0; c < context.channels; ++c)
			std::memset(context.audio[c] + begin, 0, (end - begin) * sizeof(float));
		for (auto* bus : {&m_delayBus, &m_reverbBus})
			for (std::vector<float>& channel : *bus)
				std::fill(channel.begin() + begin, channel.begin() + end, 0.0f);
		if (context.channels == 0)
			return;
		bool solo = false;
		for (int pad = 0; pad < g_drumPads && !solo; ++pad)
			solo = value(pad, Solo) >= 0.5;
		float* left = context.audio[0];
		float* right = context.channels > 1 ? context.audio[1] : nullptr;
		const bool effects = m_values[BypassEffects] < 0.5;
		for (Voice& voice : m_voices)
		{
			if (!voice.active)
				continue;
			const int pad = voice.pad;
			// A muted pad, or one that isn't soloed while others are, plays on silently (p. 942).
			const bool audible = value(pad, Mute) < 0.5 && (!solo || value(pad, Solo) >= 0.5);
			const double gain = dsp::dbToGain(value(pad, Level)) * voice.amplitude * (audible ? 1.0 : 0.0);
			const double pan = std::clamp(value(pad, Pan), -1.0, 1.0);
			const double leftPan = std::min(1.0, 1.0 - pan);
			const double rightPan = std::min(1.0, 1.0 + pan);
			const double delaySend = effects ? std::clamp(value(pad, DelaySend) / 100.0, 0.0, 1.0) : 0.0;
			const double reverbSend = effects ? std::clamp(value(pad, ReverbSend) / 100.0, 0.0, 1.0) : 0.0;
			const double hold = std::max(value(pad, Hold), 0.0) * m_sampleRate / 1000.0;
			const double decay = std::max(value(pad, Decay), 1.0) * m_sampleRate / 1000.0;
			const Sample* sample = kit.samples[voice.pad].get();
			const bool sampled = voice.layer == Layer::Sample;
			// A one-shot sample plays whole without hold and decay; the gate ends it all the same (p. 944).
			const bool shaped = !sampled || value(pad, OneShot) < 0.5;
			const bool reverse = value(pad, Reverse) >= 0.5;
			if (sampled && (sample == nullptr || sample->frames() == 0))
			{
				voice.active = false;
				continue;
			}
			for (uint32_t i = begin; i < end && voice.active; ++i)
			{
				const double envelope =
					shaped && voice.frame > hold ? std::exp(-6.907755 * (voice.frame - hold) / decay) : 1.0;
				double l = 0.0;
				double r = 0.0;
				if (sampled)
				{
					const int64_t frames = sample->frames();
					const auto whole = static_cast<int64_t>(std::floor(voice.position));
					const double t = voice.position - static_cast<double>(whole);
					const auto at = [&](const std::vector<float>& data, int64_t index) -> double
					{
						if (index < 0 || index >= frames)
							return 0.0;
						return data[static_cast<size_t>(reverse ? frames - 1 - index : index)];
					};
					l = ((1.0 - t) * at(sample->left, whole)) + (t * at(sample->left, whole + 1));
					r = sample->right.empty()
						? l
						: ((1.0 - t) * at(sample->right, whole)) + (t * at(sample->right, whole + 1));
					voice.position += voice.step;
					if (voice.position >= std::min(voice.to, static_cast<double>(frames)))
						voice.active = false;
				}
				else
				{
					l = r = synth(voice);
				}
				const double scale = gain * envelope * voice.fade;
				l *= scale * leftPan;
				r *= scale * rightPan;
				if (right != nullptr)
				{
					left[i] += static_cast<float>(l);
					right[i] += static_cast<float>(r);
				}
				else
				{
					left[i] += static_cast<float>(0.5 * (l + r));
				}
				m_delayBus[0][i] += static_cast<float>(l * delaySend);
				m_delayBus[1][i] += static_cast<float>(r * delaySend);
				m_reverbBus[0][i] += static_cast<float>(l * reverbSend);
				m_reverbBus[1][i] += static_cast<float>(r * reverbSend);
				voice.frame += 1.0;
				voice.fade -= voice.fadeStep;
				if (voice.fade <= 1e-9 || (shaped && voice.frame > hold && envelope < g_silent))
					voice.active = false;
			}
			for (dsp::BiquadState& state : voice.state)
				state.flush();
		}

		// The global delay and reverb (pp. 975-978), on the sends, into the pads' sum; the delay sends some of its
		// echoes on to the reverb.
		if (effects)
		{
			std::array<float*, 2> delay = {m_delayBus[0].data(), m_delayBus[1].data()};
			std::array<float*, 2> reverb = {m_reverbBus[0].data(), m_reverbBus[1].data()};
			const double delayLevel = m_values[DelayOn] >= 0.5 ? dsp::dbToGain(m_values[DelayLevel]) : 0.0;
			const double toReverb = std::clamp(m_values[DelayToReverb] / 100.0, 0.0, 1.0);
			if (m_values[DelayOn] >= 0.5)
			{
				m_delay.process(delay.data(), 2, begin, end);
				for (uint32_t i = begin; i < end; ++i)
					for (size_t c = 0; c < 2; ++c)
						reverb[c][i] += static_cast<float>(delay[c][i] * toReverb);
			}
			const double reverbLevel = m_values[ReverbOn] >= 0.5 ? dsp::dbToGain(m_values[ReverbLevel]) : 0.0;
			if (m_values[ReverbOn] >= 0.5)
				m_reverb.process(reverb.data(), 2, begin, end);
			for (uint32_t i = begin; i < end; ++i)
			{
				const double l = (delay[0][i] * delayLevel) + (reverb[0][i] * reverbLevel);
				const double r = (delay[1][i] * delayLevel) + (reverb[1][i] * reverbLevel);
				if (right != nullptr)
				{
					left[i] += static_cast<float>(l);
					right[i] += static_cast<float>(r);
				}
				else
				{
					left[i] += static_cast<float>(0.5 * (l + r));
				}
			}
		}
		const auto master = static_cast<float>(dsp::dbToGain(m_values[MasterLevel]));
		for (int c = 0; c < context.channels; ++c)
			for (uint32_t i = begin; i < end; ++i)
				context.audio[c][i] *= master;
	}

	void DrumMachine::process(const graph::ProcessContext& context) noexcept
	{
		const rt::ScopedDenormalsDisabled denormals;
		const Kit& kit = m_kit.acquire();
		if (kit.serial != m_applied)
		{
			for (size_t id = 0; id < kit.values.size(); ++id)
				if (!kit.automated[id])
					m_values[id] = kit.values[id];
			m_applied = kit.serial;
		}
		m_delay.setTempo(context.transport.tempo);
		splitAtEvents(
			context,
			[this, &kit](const graph::ProcessEvent& event)
			{
				switch (event.type)
				{
				case graph::ProcessEvent::Type::NoteOn:
					noteOn(event.key, event.velocity, kit);
					break;
				case graph::ProcessEvent::Type::NoteOff:
					noteOff(event.key);
					break;
				case graph::ProcessEvent::Type::Parameter:
					setParameter(event.parameter, event.parameterValue);
					break;
				case graph::ProcessEvent::Type::PitchBend:
				case graph::ProcessEvent::Type::Controller:
				case graph::ProcessEvent::Type::ChannelPressure:
				case graph::ProcessEvent::Type::PolyPressure:
				case graph::ProcessEvent::Type::Program:
				case graph::ProcessEvent::Type::SysEx:
				case graph::ProcessEvent::Type::NoteExpression:
					break;
				}
			},
			[this, &context, &kit](uint32_t begin, uint32_t end) { render(context, begin, end, kit); });
	}
}
