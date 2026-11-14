// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/biquad.h"
#include "engine/dsp/delay.h"
#include "engine/dsp/reverb.h"
#include "engine/processors/sampler.h"
#include "platform/rt/snapshotpublisher.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace gnubase::engine::processors
{
	inline constexpr int g_drumPads = 128;
	inline constexpr int g_drumVoices = 64;
	// A pad's parameters have the ids pad * g_drumPadStride + DrumMachine::PadParameter.
	inline constexpr uint32_t g_drumPadStride = 32;

	// The drum track's instrument (#182, reference manual pp. 938-980): 128 pads, one per MIDI note, each playing a
	// pool clip or one of the built-in percussion synths, with its own level, pan, tuning, envelope, exclusive group
	// and sends to the global delay and reverb. The pads' samples come from the track (drum.loadPad).
	//
	// The pads have 2432 parameters, more than the events a block can hold, so the automation player sends only
	// those of the pads that have automation (and the global ones); the others reach the drum machine with its
	// samples, from the channel's values (setPadValues). Parameter events for a pad's parameter hold until new pad
	// values come, unless the parameter is automated.
	class DrumMachine : public SamplePlayer
	{
	public:
		// What a pad plays (its layer, p. 945).
		enum class Layer : uint8_t
		{
			Off,
			Sample,
			Kick,
			Snare,
			HiHat,
			Tom,
			Clap,
			Cymbal,
			Cowbell,
		};

		enum PadParameter : uint8_t
		{
			Instrument, // Layer
			Level,      // dB
			Pan,        // -1 to 1
			Mute,
			Solo,
			Tune,            // semitones
			Tone,            // %, the synths' brightness
			Noise,           // %, the synths' noise
			Hold,            // ms before the decay
			Decay,           // ms to fall by 60 dB
			VelocityToLevel, // %
			OneShot,         // samples play whole, without hold and decay
			Gate,            // the note's end ends the sound
			Start,           // shares of the sample
			End,
			Reverse,
			ExclusiveGroup, // 0 for none: pads of a group cut each other off
			DelaySend,      // %
			ReverbSend,     // %
		};

		enum GlobalParameter : uint16_t
		{
			MasterLevel = g_drumPads * g_drumPadStride, // dB
			BypassEffects,
			DelayOn,
			DelayTime, // ms
			DelaySync,
			DelayNote,     // dsp::Delay::noteValues()
			DelayFeedback, // %
			DelayPingPong,
			DelayToReverb, // %
			DelayLevel,    // dB
			ReverbOn,
			ReverbPredelay, // ms
			ReverbSize,     // %
			ReverbTime,     // s
			ReverbDamping,  // %
			ReverbLevel,    // dB
		};

		static constexpr uint32_t padParameter(int pad, PadParameter parameter)
		{
			return (static_cast<uint32_t>(pad) * g_drumPadStride) + parameter;
		}

		DrumMachine();

		// Command thread: the sample of a pad to play from its next note on.
		void setSample(size_t pad, std::shared_ptr<const Sample> sample) override;
		// Command thread: the pads' parameter values (those not given: their defaults), from the next block on, except
		// for the automated ones, whose parameter events set them.
		void setPadValues(const std::map<uint32_t, double>& values, const std::set<uint32_t>& automated);
		void collect() override;

		void process(const graph::ProcessContext& context) noexcept override;
		void reset() noexcept override;
		uint32_t tail() const override;

		// Voices that are sounding.
		int activeVoices() const;

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		// What the command thread hands over: the pads' samples and parameter values.
		struct Kit
		{
			uint64_t serial = 0; // which hand-over it is
			std::array<std::shared_ptr<const Sample>, g_drumPads> samples{};
			std::vector<double> values;  // the pads' parameters, by id
			std::vector<bool> automated; // the pads' parameters that their events set
		};

		struct Voice
		{
			bool active = false;
			bool held = false;
			uint8_t pad = 0;
			Layer layer = Layer::Off;
			uint64_t started = 0;
			double amplitude = 1.0; // from the velocity
			double frame = 0.0;     // frames since the note started
			double fade = 1.0;      // a fade out after a gate, a cut or a stolen voice
			double fadeStep = 0.0;
			// Sample layers: where it reads in the pad's sample, which is the kit's of the block that plays (a sample
			// loaded meanwhile plays on from there), counted from the sample's end when reversed.
			double position = 0.0;
			double step = 1.0;
			double from = 0.0;
			double to = 0.0;
			std::array<double, 6> phases{};
			uint32_t noise = 1; // the voice's own noise generator, so that every render is the same
			dsp::Biquad filter;
			std::array<dsp::BiquadState, 2> state{};
		};

		double value(int pad, PadParameter parameter) const noexcept { return m_values[padParameter(pad, parameter)]; }
		void setParameter(uint32_t id, double value) noexcept;
		void noteOn(uint8_t note, uint16_t velocity, const Kit& kit) noexcept;
		void noteOff(uint8_t note) noexcept;
		void fadeOut(Voice& voice) const noexcept;
		void render(const graph::ProcessContext& context, uint32_t begin, uint32_t end, const Kit& kit) noexcept;
		double synth(Voice& voice) noexcept;
		static double nextNoise(Voice& voice) noexcept;

		void publish(std::unique_ptr<Kit> kit);

		rt::SnapshotPublisher<Kit> m_kit;
		uint64_t m_applied = 0; // the serial of the kit whose values m_values has
		double m_sampleRate = 48000.0;
		std::vector<double> m_values;
		uint64_t m_notes = 0;
		std::array<Voice, g_drumVoices> m_voices{};
		dsp::Delay m_delay;
		dsp::Reverb m_reverb;
		std::array<std::vector<float>, 2> m_delayBus;
		std::array<std::vector<float>, 2> m_reverbBus;
	};
}
