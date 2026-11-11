// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/biquad.h"
#include "engine/graph/processor.h"
#include "platform/rt/snapshotpublisher.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::engine::processors
{
	// The audio a sampler plays (#180): one or two channels at the file's sample rate. Mono samples have right empty.
	struct Sample
	{
		double sampleRate = 48000.0;
		std::vector<float> left;
		std::vector<float> right;

		int64_t frames() const { return static_cast<int64_t>(left.size()); }
	};

	// The first two channels of an audio file, or an empty sample if it can't be read. A file is read once while
	// something holds its sample, e.g. a sampler that plays it and sampler.normalize, and again once it changed. Not
	// real-time safe; thread-safe.
	std::shared_ptr<const Sample> sharedSample(const std::string& path);

	inline constexpr int g_samplerVoices = 128;

	// The sampler track's instrument (#180, reference manual pp. 712-735): plays a sample chromatically from its root
	// key, or in slice mode a slice per key from the root key up. Positions (start, end, loop start and end) are
	// shares of the sample's length, so they stay where they are when the sample's sample rate changes. Reverse plays
	// the sample backwards with every position measured from its end. Each voice has an amp envelope (attack, decay,
	// sustain, release) and its own filter, whose cutoff follows the key by the key follow share.
	class Sampler : public graph::Processor
	{
	public:
		enum Parameter : uint8_t
		{
			RootKey,
			KeyLow,
			KeyHigh,
			FixedPitch,
			Octave,
			Coarse, // semitones
			Fine,   // cents
			PitchBendRange,
			Start,
			End,
			LoopMode,
			LoopStart,
			LoopEnd,
			Reverse,
			OneShot,
			PlaybackMode, // Normal, Slice
			Slices,
			Monophonic,
			Legato,
			FilterOn,
			FilterType, // Low pass, High pass, Band pass
			Cutoff,     // Hz
			Resonance,  // %
			Drive,      // dB
			KeyFollow,  // %
			Volume,     // dB
			Pan,
			Attack,  // ms
			Decay,   // ms
			Sustain, // %
			Release, // ms
			Gain,    // the sample's gain, dB, which sampler.normalize sets
		};

		enum class Loop : uint8_t
		{
			None,
			Continuous,
			Alternate,
			Once,
			UntilRelease,
			AlternateUntilRelease,
		};

		Sampler();

		// Command thread: the sample to play from the next block on. Voices that play keep their positions.
		void setSample(std::shared_ptr<const Sample> sample);

		// Command thread: lets go of the samples played before the last setSample once the audio thread has moved on.
		void collect();

		void process(const graph::ProcessContext& context) noexcept override;
		void reset() noexcept override;

		// Voices that are sounding, including releasing ones.
		int activeVoices() const;

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		enum class Stage : uint8_t
		{
			Attack,
			Decay,
			Sustain,
			Release,
		};

		struct Voice
		{
			bool active = false;
			bool held = false; // its key is down (one-shot voices ignore note-offs)
			uint8_t note = 0;
			uint8_t channel = 0;
			uint64_t started = 0;
			double position = 0.0; // in the sample's frames, counted from its end when reversed
			double direction = 1.0;
			double from = 0.0; // the range it plays: the sample's start and end, or its slice
			double to = 0.0;
			bool slice = false;
			bool loopedOnce = false;
			double amplitude = 1.0;
			Stage stage = Stage::Attack;
			double level = 0.0;
			double releaseStep = 0.0;
			dsp::Biquad filter;
			std::array<dsp::BiquadState, 2> state{};
		};

		void noteOn(uint8_t channel, uint8_t note, uint16_t velocity) noexcept;
		void noteOff(uint8_t channel, uint8_t note) noexcept;
		void start(uint8_t channel, uint8_t note, uint16_t velocity, const Sample& sample) noexcept;
		void release(Voice& voice) noexcept;
		void fadeOut(Voice& voice) noexcept;
		Voice* monophonicVoice() noexcept;
		void setParameter(uint32_t id, double value) noexcept;
		void render(const graph::ProcessContext& context, uint32_t begin, uint32_t end, const Sample& sample) noexcept;
		bool advance(Voice& voice, double step, double frames) const noexcept;
		double increment(const Voice& voice, const Sample& sample) const noexcept;
		double envelope(Voice& voice) noexcept;
		dsp::Biquad filterFor(const Voice& voice) const noexcept;

		rt::SnapshotPublisher<std::shared_ptr<const Sample>> m_sample;
		const Sample* m_playing = nullptr; // what the block that runs plays: m_sample's, acquired once per block
		double m_sampleRate = 48000.0;
		std::array<double, Gain + 1> m_values{};
		double m_bend = 0.0; // -1 to 1
		uint64_t m_notes = 0;
		// Monophonic mode: when each key went down (0: up), so releasing the playing note goes back to the latest held.
		std::array<uint64_t, 128> m_heldSince{};
		std::array<Voice, g_samplerVoices> m_voices{};
	};
}
