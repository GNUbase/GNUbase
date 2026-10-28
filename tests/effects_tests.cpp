// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/delay.h"
#include "engine/dsp/dither.h"
#include "engine/dsp/dynamics.h"
#include "engine/dsp/equalizer.h"
#include "engine/dsp/reverb.h"
#include "engine/graph/events.h"
#include "engine/graph/graph.h"
#include "engine/graph/processor.h"
#include "engine/processors/builtin.h"
#include "engine/processors/effects.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// The mixing effects (#114): their DSP measured against analytic curves, their timing, and their real-time behavior.
using namespace gnubase::engine;
using namespace gnubase::engine::graph;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr uint32_t g_block = 256;
	constexpr double g_pi = 3.14159265358979323846;

	using Signal = std::vector<std::vector<float>>; // channel after channel

	struct Change
	{
		uint64_t frame = 0; // from the start of the signal
		uint32_t parameter = 0;
		double value = 0.0;
	};

	ProcessEvent parameter(uint32_t offset, uint32_t id, double value)
	{
		ProcessEvent event;
		event.offset = offset;
		event.type = ProcessEvent::Type::Parameter;
		event.parameter = id;
		event.parameterValue = value;
		return event;
	}

	// Runs a processor over a whole signal in blocks, with parameter changes at their frames.
	class Runner
	{
	public:
		explicit Runner(Processor& processor, double tempo = 120.0) : m_processor(processor)
		{
			m_transport.tempo = tempo;
			processor.prepareFor(g_sampleRate, g_block);
		}

		// Applies the values at once, as a block without frames.
		void set(const std::vector<std::pair<uint32_t, double>>& values)
		{
			m_events.clear();
			for (const auto& [id, value] : values)
				REQUIRE(m_events.add(parameter(0, id, value)));
			m_out.clear();
			m_processor.process({nullptr, 0, 0, m_events, m_out, m_transport});
			m_events.clear();
		}

		Signal run(Signal signal, const std::vector<Change>& changes = {})
		{
			const size_t channels = signal.size();
			const size_t length = signal[0].size();
			std::vector<float*> pointers(channels);
			size_t next = 0;
			for (size_t start = 0; start < length; start += g_block)
			{
				const auto frames = static_cast<uint32_t>(std::min<size_t>(g_block, length - start));
				for (size_t c = 0; c < channels; ++c)
					pointers[c] = signal[c].data() + start;
				m_events.clear();
				for (; next < changes.size() && changes[next].frame < start + frames; ++next)
					REQUIRE(m_events.add(parameter(static_cast<uint32_t>(changes[next].frame - start),
						changes[next].parameter, changes[next].value)));
				m_out.clear();
				m_processor.process(
					{pointers.data(), static_cast<int>(channels), frames, m_events, m_out, m_transport});
			}
			return signal;
		}

	private:
		Processor& m_processor;
		TransportBlock m_transport;
		EventBuffer m_events{g_maxEventsPerBlock};
		EventBuffer m_out{g_maxEventsPerBlock};
	};

	Signal silence(size_t channels, size_t frames)
	{
		return {channels, std::vector<float>(frames, 0.0f)};
	}

	Signal impulse(size_t channels, size_t frames, float level = 1.0f)
	{
		Signal signal = silence(channels, frames);
		for (std::vector<float>& channel : signal)
			channel[0] = level;
		return signal;
	}

	Signal constant(size_t channels, size_t frames, float level)
	{
		return {channels, std::vector<float>(frames, level)};
	}

	Signal sine(size_t channels, size_t frames, double frequency, double amplitude)
	{
		Signal signal = silence(channels, frames);
		for (std::vector<float>& channel : signal)
			for (size_t i = 0; i < frames; ++i)
				channel[i] = static_cast<float>(
					amplitude * std::sin(2.0 * g_pi * frequency * static_cast<double>(i) / g_sampleRate));
		return signal;
	}

	// Uniform noise in [-level, level), different on each channel, the same on every run.
	Signal noise(size_t channels, size_t frames, double level, uint32_t seed = 1)
	{
		Signal signal = silence(channels, frames);
		dsp::Random random(seed);
		for (std::vector<float>& channel : signal)
			for (float& sample : channel)
				sample = static_cast<float>(level * ((2.0 * random.uniform()) - 1.0));
		return signal;
	}

	// The magnitude in dB of a response at a frequency: one bin of its discrete-time Fourier transform.
	double responseDb(const std::vector<float>& response, double frequency)
	{
		std::complex<double> sum = 0.0;
		const double w = 2.0 * g_pi * frequency / g_sampleRate;
		for (size_t n = 0; n < response.size(); ++n)
			sum += static_cast<double>(response[n]) * std::polar(1.0, -w * static_cast<double>(n));
		return 20.0 * std::log10(std::abs(sum));
	}

	// Log-spaced frequencies from 20 Hz to 20 kHz.
	std::vector<double> frequencies(int count = 41)
	{
		std::vector<double> result;
		result.reserve(static_cast<size_t>(count));
		for (int i = 0; i < count; ++i)
			result.push_back(20.0 * std::pow(1000.0, i / static_cast<double>(count - 1)));
		return result;
	}

	double rms(const std::vector<float>& samples, size_t begin = 0, size_t end = SIZE_MAX)
	{
		end = std::min(end, samples.size());
		double sum = 0.0;
		for (size_t i = begin; i < end; ++i)
			sum += static_cast<double>(samples[i]) * samples[i];
		return std::sqrt(sum / static_cast<double>(end - begin));
	}

	double peak(const std::vector<float>& samples, size_t begin = 0, size_t end = SIZE_MAX)
	{
		double result = 0.0;
		for (size_t i = begin; i < std::min(end, samples.size()); ++i)
			result = std::max(result, static_cast<double>(std::abs(samples[i])));
		return result;
	}

	// The analog prototypes of the cookbook filters at the prewarped frequency ratio w = tan(pi f / fs) /
	// tan(pi f0 / fs), as |H|^2. The bilinear transform maps the digital response onto them exactly.
	double warped(double frequency, double center)
	{
		return std::tan(g_pi * frequency / g_sampleRate) / std::tan(g_pi * center / g_sampleRate);
	}

	double bellDb(double f, double f0, double q, double gainDb)
	{
		const double a = std::pow(10.0, gainDb / 40.0);
		const double w = warped(f, f0);
		const double real = 1.0 - (w * w);
		const double numerator = (real * real) + std::pow(w * a / q, 2.0);
		const double denominator = (real * real) + std::pow(w / (a * q), 2.0);
		return 10.0 * std::log10(numerator / denominator);
	}

	double shelfDb(double f, double f0, double q, double gainDb, bool low)
	{
		const double a = std::pow(10.0, gainDb / 40.0);
		const double w = warped(f, f0);
		const double odd = std::pow(w * std::sqrt(a) / q, 2.0);
		const double lowPart = std::pow(a - (w * w), 2.0) + odd;        // |A - w^2 + j w sqrt(A)/Q|^2
		const double highPart = std::pow(1.0 - (a * w * w), 2.0) + odd; // |1 - A w^2 + j w sqrt(A)/Q|^2
		return 10.0 * std::log10(a * a * (low ? lowPart / highPart : highPart / lowPart));
	}

	double butterworthDb(double f, double f0, int order, bool highPass)
	{
		const double w = warped(f, f0);
		const double ratio = highPass ? 1.0 / w : w;
		return -10.0 * std::log10(1.0 + std::pow(ratio, 2.0 * order));
	}

	using dsp::Equalizer;

	// An EQ with every band but the given one turned off.
	std::vector<std::pair<uint32_t, double>> onlyBand(
		int band, dsp::FilterType type, double frequency, double gain, double q, int slope = 1)
	{
		std::vector<std::pair<uint32_t, double>> values;
		values.reserve(Equalizer::g_bands + 5);
		for (int b = 0; b < Equalizer::g_bands; ++b)
			values.emplace_back(Equalizer::parameter(b, Equalizer::On), b == band ? 1.0 : 0.0);
		values.emplace_back(Equalizer::parameter(band, Equalizer::Type), static_cast<double>(type));
		values.emplace_back(Equalizer::parameter(band, Equalizer::Frequency), frequency);
		values.emplace_back(Equalizer::parameter(band, Equalizer::Gain), gain);
		values.emplace_back(Equalizer::parameter(band, Equalizer::Q), q);
		values.emplace_back(Equalizer::parameter(band, Equalizer::Slope), slope);
		return values;
	}

	std::vector<float> impulseResponse(const std::vector<std::pair<uint32_t, double>>& values)
	{
		processors::Equalizer eq;
		Runner runner(eq);
		runner.set(values);
		return runner.run(impulse(1, 32768))[0];
	}

	// The time the energy decay curve (Schroeder's backward integral) takes from -5 to -35 dB, doubled: RT60.
	double reverberationTime(const std::vector<float>& response)
	{
		std::vector<double> energy(response.size() + 1, 0.0);
		for (size_t i = response.size(); i > 0; --i)
			energy[i - 1] = energy[i] + (static_cast<double>(response[i - 1]) * response[i - 1]);
		const auto timeAt = [&](double db)
		{
			const double level = energy[0] * std::pow(10.0, db / 10.0);
			size_t i = 0;
			while (i < response.size() && energy[i] > level)
				++i;
			return static_cast<double>(i) / g_sampleRate;
		};
		return 2.0 * (timeAt(-35.0) - timeAt(-5.0));
	}

	// The first frame that isn't silent.
	size_t onset(const std::vector<float>& samples)
	{
		for (size_t i = 0; i < samples.size(); ++i)
			if (samples[i] != 0.0f)
				return i;
		return samples.size();
	}
}

TEST_CASE("EQ bells and shelves follow the analytic cookbook curves", "[unit]")
{
	struct Case
	{
		dsp::FilterType type;
		double frequency;
		double gain;
		double q;
	};
	const Case cases[] = {
		{dsp::FilterType::Bell, 1000.0, 6.0, 1.0},
		{dsp::FilterType::Bell, 200.0, -12.0, 4.0},
		{dsp::FilterType::Bell, 9000.0, 18.0, 0.5},
		{dsp::FilterType::LowShelf, 100.0, 9.0, 0.71},
		{dsp::FilterType::LowShelf, 400.0, -6.0, 1.5},
		{dsp::FilterType::HighShelf, 8000.0, -6.0, 0.71},
		{dsp::FilterType::HighShelf, 3000.0, 12.0, 0.4},
	};
	for (const Case& c : cases)
	{
		const std::vector<float> response = impulseResponse(onlyBand(2, c.type, c.frequency, c.gain, c.q));
		for (const double f : frequencies())
		{
			const double expected = c.type == dsp::FilterType::Bell
				? bellDb(f, c.frequency, c.q, c.gain)
				: shelfDb(f, c.frequency, c.q, c.gain, c.type == dsp::FilterType::LowShelf);
			INFO("type " << static_cast<int>(c.type) << " at " << c.frequency << " Hz, measured at " << f << " Hz");
			CHECK_THAT(responseDb(response, f), WithinAbs(expected, 0.02));
		}
		if (c.type == dsp::FilterType::Bell)
			CHECK_THAT(responseDb(response, c.frequency), WithinAbs(c.gain, 0.02));
	}
}

TEST_CASE("EQ cuts are Butterworth filters with the selected slope", "[unit]")
{
	const int orders[] = {1, 2, 4, 6, 8}; // 6, 12, 24, 36, 48 dB/oct
	for (int slope = 0; slope < 5; ++slope)
		for (const bool low : {true, false})
		{
			const dsp::FilterType type = low ? dsp::FilterType::LowCut : dsp::FilterType::HighCut;
			const std::vector<float> response = impulseResponse(onlyBand(0, type, 1000.0, 0.0, 1.0, slope));
			INFO((low ? "low cut" : "high cut") << ", order " << orders[slope]);
			CHECK_THAT(responseDb(response, 1000.0), WithinAbs(-3.0103, 0.02));
			for (const double f : frequencies())
			{
				const double expected = butterworthDb(f, 1000.0, orders[slope], low);
				if (expected < -80.0)
					continue; // beyond what float samples resolve
				INFO("at " << f << " Hz");
				CHECK_THAT(responseDb(response, f), WithinAbs(expected, expected > -40.0 ? 0.02 : 0.5));
			}
		}
}

TEST_CASE("EQ bands add up, and a flat EQ leaves the signal untouched", "[unit]")
{
	SECTION("flat: bands at 0 dB and output at 0 dB change nothing, bit for bit")
	{
		processors::Equalizer eq;
		Runner runner(eq);
		const Signal input = noise(2, 4096, 0.5);
		CHECK(runner.run(input) == input);
	}

	SECTION("a bell and a high cut multiply, and the output gain applies to everything")
	{
		processors::Equalizer eq;
		Runner runner(eq);
		std::vector<std::pair<uint32_t, double>> values = onlyBand(2, dsp::FilterType::Bell, 500.0, 6.0, 2.0);
		values.emplace_back(Equalizer::parameter(5, Equalizer::On), 1.0);
		values.emplace_back(Equalizer::parameter(5, Equalizer::Type), static_cast<double>(dsp::FilterType::HighCut));
		values.emplace_back(Equalizer::parameter(5, Equalizer::Frequency), 5000.0);
		values.emplace_back(Equalizer::parameter(5, Equalizer::Slope), 2.0); // 24 dB/oct
		values.emplace_back(Equalizer::Output, -3.0);
		runner.set(values);
		const std::vector<float> response = runner.run(impulse(1, 32768))[0];
		for (const double f : frequencies())
		{
			const double expected = bellDb(f, 500.0, 2.0, 6.0) + butterworthDb(f, 5000.0, 4, false) - 3.0;
			if (expected < -80.0)
				continue;
			INFO("at " << f << " Hz");
			CHECK_THAT(responseDb(response, f), WithinAbs(expected, expected > -40.0 ? 0.02 : 0.5));
		}
	}

	SECTION("every channel is filtered the same way")
	{
		processors::Equalizer eq;
		Runner runner(eq);
		runner.set(onlyBand(1, dsp::FilterType::LowShelf, 300.0, -9.0, 0.71));
		Signal input = noise(1, 4096, 0.5);
		input.push_back(input[0]);
		input.push_back(input[0]);
		const Signal output = runner.run(input);
		CHECK(output[1] == output[0]);
		CHECK(output[2] == output[0]);
		CHECK(output[0] != input[0]);
	}
}

TEST_CASE("the compressor follows its static curve with peak and RMS detection", "[unit]")
{
	using dsp::Compressor;
	SECTION("the curve: below the knee unchanged, above it divided by the ratio, a quadratic in between")
	{
		CHECK(Compressor::curve(-30.0, -20.0, 4.0, 0.0) == -30.0);
		CHECK_THAT(Compressor::curve(-8.0, -20.0, 4.0, 0.0), WithinAbs(-17.0, 1e-12));
		CHECK_THAT(Compressor::curve(-20.0, -20.0, 4.0, 6.0), WithinAbs(-20.5625, 1e-12));
		CHECK(Compressor::curve(-23.0, -20.0, 4.0, 6.0) == -23.0);                       // the knee's lower end
		CHECK_THAT(Compressor::curve(-17.0, -20.0, 4.0, 6.0), WithinAbs(-19.25, 1e-12)); // its upper end
		CHECK(Compressor::curve(0.0, -20.0, 1.0, 6.0) == 0.0);                           // ratio 1: no compression
	}

	for (const double detection : {0.0, 1.0})
		for (const double knee : {0.0, 6.0, 12.0})
		{
			const double threshold = -20.0;
			const double ratio = 4.0;
			const double makeup = 3.0;
			for (int step = 0; step <= 16; ++step)
			{
				const double levelDb = -40.0 + (2.5 * step);
				processors::Compressor compressor;
				Runner runner(compressor);
				runner.set({{Compressor::Threshold, threshold}, {Compressor::Ratio, ratio}, {Compressor::Knee, knee},
					{Compressor::Attack, 1.0}, {Compressor::Release, 10.0}, {Compressor::Makeup, makeup},
					{Compressor::Detection, detection}});
				const auto level = static_cast<float>(std::pow(10.0, levelDb / 20.0));
				const Signal output = runner.run(constant(2, 24000, level));
				const double gainDb = 20.0 * std::log10(output[0].back() / level);
				INFO("detection " << detection << ", knee " << knee << ", level " << levelDb << " dB");
				CHECK_THAT(
					gainDb, WithinAbs(Compressor::curve(levelDb, threshold, ratio, knee) - levelDb + makeup, 0.01));
				CHECK(output[1].back() == output[0].back());
			}
		}

	SECTION("RMS detection measures a sine at its RMS level, peak detection at its peaks")
	{
		const double amplitude = 0.5; // -6 dB peak, -9 dB RMS
		const double rmsDb = 20.0 * std::log10(amplitude / std::sqrt(2.0));
		for (const double detection : {0.0, 1.0})
		{
			processors::Compressor compressor;
			Runner runner(compressor);
			runner.set({{Compressor::Threshold, -30.0}, {Compressor::Ratio, 3.0}, {Compressor::Knee, 0.0},
				{Compressor::Attack, 5.0}, {Compressor::Release, 500.0}, {Compressor::Detection, detection}});
			const Signal input = sine(1, 48000, 1000.0, amplitude);
			const Signal output = runner.run(input);
			const double gainDb = 20.0 * std::log10(rms(output[0], 24000) / rms(input[0], 24000));
			const double levelDb = detection == 1.0 ? rmsDb : 20.0 * std::log10(amplitude);
			INFO("detection " << detection);
			// RMS is exact; peak detection lets the gain recover a little between the peaks.
			CHECK_THAT(gainDb,
				WithinAbs(Compressor::curve(levelDb, -30.0, 3.0, 0.0) - levelDb, detection == 1.0 ? 0.05 : 0.5));
		}
	}

	SECTION("attack and release are the times of the gain reduction's exponential steps")
	{
		processors::Compressor compressor;
		Runner runner(compressor);
		runner.set({{Compressor::Threshold, -20.0}, {Compressor::Ratio, 20.0}, {Compressor::Knee, 0.0},
			{Compressor::Attack, 10.0}, {Compressor::Release, 100.0}});
		const double target = Compressor::curve(0.0, -20.0, 20.0, 0.0); // -19 dB at 0 dB in
		// 10 ms after a step to 0 dB, 1 - 1/e of the reduction is there.
		const Signal attack = runner.run(constant(1, 480, 1.0f));
		CHECK_THAT(20.0 * std::log10(attack[0].back()), WithinAbs(target * (1.0 - std::exp(-1.0)), 0.05));
		runner.run(constant(1, 48000, 1.0f));
		// 100 ms after the level fell below the threshold, 1/e of it is left.
		const Signal release = runner.run(constant(1, 4800, 0.01f));
		CHECK_THAT(20.0 * std::log10(release[0].back() / 0.01), WithinAbs(target * std::exp(-1.0), 0.05));
	}
}

TEST_CASE("the gate opens above its threshold, closes to its range, and holds", "[unit]")
{
	using dsp::Gate;
	SECTION("the static curve: 0 dB above the threshold, the range below")
	{
		for (const double range : {-80.0, -20.0, 0.0})
			for (int step = 0; step < 12; ++step)
			{
				const double levelDb = -57.5 + (5.0 * step); // never at the threshold itself
				processors::Gate gate;
				Runner runner(gate);
				runner.set({{Gate::Threshold, -30.0}, {Gate::Range, range}, {Gate::Attack, 0.1}, {Gate::Hold, 0.0},
					{Gate::Release, 10.0}});
				const auto level = static_cast<float>(std::pow(10.0, levelDb / 20.0));
				const Signal output = runner.run(constant(2, 24000, level));
				INFO("range " << range << ", level " << levelDb << " dB");
				CHECK_THAT(
					20.0 * std::log10(output[0].back() / level), WithinAbs(levelDb >= -30.0 ? 0.0 : range, 0.01));
				CHECK(output[1].back() == output[0].back());
			}
	}

	SECTION("after the level drops, the gate holds, then releases")
	{
		processors::Gate gate;
		Runner runner(gate);
		runner.set({{Gate::Threshold, -30.0}, {Gate::Range, -60.0}, {Gate::Attack, 0.1}, {Gate::Hold, 50.0},
			{Gate::Release, 20.0}});
		Signal input = constant(1, 48000, 0.001f); // -60 dB, below the threshold
		std::fill(input[0].begin(), input[0].begin() + 24000, 0.5f);
		const Signal output = runner.run(input);
		const size_t holdEnd = 24000 + 2400; // 50 ms
		CHECK(output[0][24000] == 0.001f);
		CHECK(output[0][holdEnd - 1] == 0.001f); // still open: exactly unity gain
		CHECK(output[0][holdEnd + 1] < 0.001f);
		// 20 ms into the release, 1/e of the way from open to the range is left.
		const double expected = 1e-3 + ((1.0 - 1e-3) * std::exp(-1.0));
		CHECK_THAT(output[0][holdEnd + 960] / 0.001, WithinRel(expected, 0.01));
		CHECK_THAT(20.0 * std::log10(output[0].back() / 0.001), WithinAbs(-60.0, 0.1));
	}

	SECTION("the attack opens the gate with an exponential step")
	{
		processors::Gate gate;
		Runner runner(gate);
		runner.set({{Gate::Threshold, -30.0}, {Gate::Range, -40.0}, {Gate::Attack, 5.0}});
		gate.reset();                                             // closed at the new range
		const Signal output = runner.run(constant(1, 240, 0.5f)); // 5 ms
		const double range = 0.01;
		CHECK_THAT(output[0].back() / 0.5, WithinRel(range + ((1.0 - range) * (1.0 - std::exp(-1.0))), 0.01));
	}
}

TEST_CASE("the limiter never exceeds its ceiling and reports its look-ahead as latency", "[unit]")
{
	using dsp::Limiter;
	const uint32_t lookahead = 48; // 1 ms at 48 kHz

	SECTION("latency")
	{
		processors::Limiter limiter;
		limiter.prepareFor(g_sampleRate, g_block);
		CHECK(limiter.latency() == lookahead);
		limiter.prepareFor(96000.0, g_block);
		CHECK(limiter.latency() == 96);
	}

	SECTION("below the ceiling the signal passes unchanged, delayed by the latency")
	{
		processors::Limiter limiter;
		Runner runner(limiter);
		runner.set({{Limiter::Ceiling, 0.0}});
		const Signal input = noise(2, 8192, 0.9);
		const Signal output = runner.run(input);
		for (size_t c = 0; c < 2; ++c)
		{
			for (size_t i = 0; i < lookahead; ++i)
				CHECK(output[c][i] == 0.0f);
			for (size_t i = lookahead; i < input[c].size(); ++i)
				CHECK(output[c][i] == input[c][i - lookahead]);
		}
	}

	SECTION("loud signals never exceed the ceiling, for every release and input gain")
	{
		for (const double release : {1.0, 50.0, 1000.0})
			for (const double input : {0.0, 12.0, 24.0})
				for (const double ceiling : {0.0, -0.3, -6.0, -30.0})
				{
					processors::Limiter limiter;
					Runner runner(limiter);
					runner.set({{Limiter::Input, input}, {Limiter::Ceiling, ceiling}, {Limiter::Release, release}});
					Signal signal = noise(2, 48000, 4.0, 7);
					const Signal tone = sine(2, 48000, 50.0, 2.0);
					for (size_t c = 0; c < 2; ++c)
						for (size_t i = 0; i < signal[c].size(); ++i)
							signal[c][i] = (i / 4000) % 2 == 0 ? signal[c][i] * 0.05f : signal[c][i] + tone[c][i];
					const Signal output = runner.run(signal);
					const auto limit = static_cast<float>(std::pow(10.0, ceiling / 20.0));
					INFO("release " << release << " ms, input " << input << " dB, ceiling " << ceiling << " dB");
					CHECK(peak(output[0]) <= limit);
					CHECK(peak(output[1]) <= limit);
					CHECK(peak(output[0]) > 0.9 * limit); // it limits rather than silences
				}
	}

	SECTION("a single peak is brought down to the ceiling exactly, without clipping its neighbours")
	{
		processors::Limiter limiter;
		Runner runner(limiter);
		runner.set({{Limiter::Ceiling, -6.0}, {Limiter::Release, 100.0}});
		Signal input = constant(1, 48000, 0.1f);
		input[0][1000] = 2.0f;
		const Signal output = runner.run(input);
		const double ceiling = std::pow(10.0, -6.0 / 20.0);
		CHECK_THAT(output[0][1000 + lookahead], WithinRel(ceiling, 1e-6));
		// The gain falls in equal steps over the look-ahead before the peak and recovers after it.
		CHECK(output[0][999] == 0.1f);
		const double step = 0.1 * (1.0 - (ceiling / 2.0)) / (lookahead + 1);
		for (uint32_t i = 0; i < lookahead; ++i)
			CHECK_THAT(output[0][1000 + i], WithinAbs(0.1 - (step * (i + 1)), 1e-6));
		CHECK(output[0][47999] > 0.0999f);
	}
}

TEST_CASE("the delay repeats its input after its time, synced to the tempo or as ping-pong echoes", "[unit]")
{
	using dsp::Delay;
	SECTION("echoes at the time in ms, each lowered by the feedback")
	{
		processors::Delay delay;
		Runner runner(delay);
		runner.set({{Delay::Time, 100.0}, {Delay::Feedback, 50.0}, {Delay::Mix, 100.0}});
		const Signal output = runner.run(impulse(2, 16000));
		for (size_t c = 0; c < 2; ++c)
		{
			CHECK(output[c][4800] == 1.0f);
			CHECK(output[c][9600] == 0.5f);
			CHECK(output[c][14400] == 0.25f);
			size_t echoes = 0;
			for (const float sample : output[c])
				echoes += sample != 0.0f ? 1 : 0;
			CHECK(echoes == 3);
		}
	}

	SECTION("mix: 0 is the dry signal, 50 half of each")
	{
		processors::Delay delay;
		Runner runner(delay);
		runner.set({{Delay::Time, 10.0}, {Delay::Feedback, 0.0}, {Delay::Mix, 50.0}});
		Signal output = runner.run(impulse(1, 1000));
		CHECK(output[0][0] == 0.5f);
		CHECK(output[0][480] == 0.5f);
		runner.set({{Delay::Mix, 0.0}});
		const Signal input = noise(1, 2000, 0.5);
		output = runner.run(input);
		CHECK(output == input);
	}

	SECTION("synced: a note value at the transport's tempo")
	{
		for (const double tempo : {120.0, 90.0, 140.0})
		{
			processors::Delay delay;
			Runner runner(delay, tempo);
			runner.set({{Delay::Sync, 1.0}, {Delay::Note, 7.0}, {Delay::Feedback, 0.0}, {Delay::Mix, 100.0}}); // 1/8
			const auto expected = static_cast<size_t>(std::lround(0.5 * 60.0 / tempo * g_sampleRate));
			INFO("tempo " << tempo);
			CHECK(delay.core().delayFrames() == expected);
			CHECK(onset(runner.run(impulse(1, 30000))[0]) == expected);
		}
		CHECK(Delay::noteValues()[7].name == std::string("1/8"));
		CHECK(Delay::noteValues()[Delay::g_defaultNote].quarters == 1.0);
	}

	SECTION("ping-pong: the echoes alternate between left and right")
	{
		processors::Delay delay;
		Runner runner(delay);
		runner.set({{Delay::Time, 10.0}, {Delay::Feedback, 50.0}, {Delay::Mix, 100.0}, {Delay::PingPong, 1.0}});
		Signal input = silence(2, 2000);
		input[0][0] = 1.0f; // left only
		const Signal output = runner.run(input);
		CHECK(output[0][480] == 0.5f); // the sum of both inputs, halved
		CHECK(output[1][480] == 0.0f);
		CHECK(output[1][960] == 0.25f);
		CHECK(output[0][960] == 0.0f);
		CHECK(output[0][1440] == 0.125f);
		CHECK(output[1][1440] == 0.0f);
	}

	SECTION("at full feedback the echoes repeat forever without growing")
	{
		processors::Delay delay;
		Runner runner(delay);
		runner.set({{Delay::Time, 1.0}, {Delay::Feedback, 100.0}, {Delay::Mix, 100.0}, {Delay::PingPong, 1.0}});
		const Signal output = runner.run(impulse(2, 48000));
		CHECK(peak(output[0]) == 1.0);
		CHECK(peak(output[1]) == 1.0);
		CHECK(peak(output[0], 47000) == 1.0); // still there after a second
		CHECK(delay.tail() == UINT32_MAX);
	}

	SECTION("the tail lasts until the echoes are 90 dB down")
	{
		processors::Delay delay;
		Runner runner(delay);
		runner.set({{Delay::Time, 100.0}, {Delay::Feedback, 50.0}});
		CHECK(delay.tail() == 4800 * 16); // 0.5^15 is below -90 dB
	}
}

TEST_CASE("the reverb decays at its decay time and stays stable at every setting", "[unit][slow]")
{
	using dsp::Reverb;
	SECTION("RT60 from the energy decay of the impulse response")
	{
		for (const double decay : {0.5, 1.0, 2.0})
			for (const double size : {20.0, 80.0})
			{
				processors::Reverb reverb;
				Runner runner(reverb);
				runner.set({{Reverb::Size, size}, {Reverb::Decay, decay}, {Reverb::Damping, 0.0},
					{Reverb::Predelay, 0.0}, {Reverb::Mix, 100.0}});
				const Signal output = runner.run(impulse(2, static_cast<size_t>(decay * 2.0 * g_sampleRate)));
				INFO("decay " << decay << " s, size " << size << "%");
				CHECK_THAT(reverberationTime(output[0]), WithinRel(decay, 0.08));
				CHECK_THAT(reverberationTime(output[1]), WithinRel(decay, 0.08));
				CHECK(output[0] != output[1]); // the sides are decorrelated
			}
	}

	SECTION("damping shortens the decay of high frequencies only")
	{
		std::vector<float> responses[2];
		for (const int damping : {0, 1})
		{
			processors::Reverb reverb;
			Runner runner(reverb);
			runner.set({{Reverb::Decay, 1.0}, {Reverb::Damping, damping * 100.0}, {Reverb::Predelay, 0.0},
				{Reverb::Mix, 100.0}});
			responses[damping] = runner.run(impulse(1, 96000))[0];
		}
		// High frequencies: the share of the first difference in the late tail's energy.
		const auto brightness = [](const std::vector<float>& response)
		{
			double energy = 0.0;
			double difference = 0.0;
			for (size_t i = 14400; i < 24000; ++i)
			{
				energy += static_cast<double>(response[i]) * response[i];
				difference += std::pow(static_cast<double>(response[i]) - response[i - 1], 2.0);
			}
			return difference / energy;
		};
		CHECK(brightness(responses[1]) < 0.1 * brightness(responses[0]));
		// Low frequencies: the decay of a 64-frame moving average stays at the decay time.
		for (const std::vector<float>& response : responses)
		{
			std::vector<float> low(response.size());
			double sum = 0.0;
			for (size_t i = 0; i < response.size(); ++i)
			{
				sum += response[i] - (i >= 64 ? response[i - 64] : 0.0f);
				low[i] = static_cast<float>(sum / 64.0);
			}
			CHECK_THAT(reverberationTime(low), WithinRel(1.0, 0.15));
		}
	}

	SECTION("predelay delays the reverb by its time")
	{
		size_t onsets[2] = {};
		for (int i = 0; i < 2; ++i)
		{
			processors::Reverb reverb;
			Runner runner(reverb);
			runner.set({{Reverb::Predelay, i * 50.0}, {Reverb::Mix, 100.0}});
			onsets[i] = onset(runner.run(impulse(1, 9600))[0]);
		}
		CHECK(onsets[1] - onsets[0] == 2400);
		CHECK(onsets[0] > 0);
	}

	SECTION("at the extremes, loud noise in and then silence: finite, bounded, and decaying")
	{
		for (const double damping : {0.0, 100.0})
			for (const double size : {0.0, 100.0})
			{
				processors::Reverb reverb;
				Runner runner(reverb);
				runner.set({{Reverb::Size, size}, {Reverb::Decay, 20.0}, {Reverb::Damping, damping},
					{Reverb::Predelay, 200.0}, {Reverb::Mix, 100.0}});
				Signal input = noise(2, size_t{5} * 48000, 1.0, 3);
				for (size_t c = 0; c < 2; ++c)
					std::fill(input[c].begin() + (int64_t{3} * 48000), input[c].end(), 0.0f);
				const Signal output = runner.run(input);
				INFO("damping " << damping << "%, size " << size << "%");
				for (const std::vector<float>& channel : output)
				{
					CHECK(std::all_of(channel.begin(), channel.end(), [](float x) { return std::isfinite(x); }));
					CHECK(peak(channel) < 8.0);
					CHECK(rms(channel, size_t{4} * 48000) < rms(channel, size_t{2} * 48000, size_t{3} * 48000));
				}
				CHECK_THAT(static_cast<double>(reverb.tail()), WithinAbs((0.2 + 30.0) * g_sampleRate, 1.0));
			}
	}
}

TEST_CASE("dither quantizes to the bit depth with TPDF noise, optionally shaped", "[unit]")
{
	using dsp::Dither;
	for (const int bits : {16, 24})
	{
		const double step = std::ldexp(1.0, 1 - bits);
		INFO(bits << " bits");

		SECTION("every sample is a whole number of steps, within [-1, 1)")
		{
			processors::Dither dither;
			Runner runner(dither);
			runner.set({{Dither::Bits, static_cast<double>(bits)}});
			const Signal output = runner.run(noise(2, 48000, 1.2));
			for (const std::vector<float>& channel : output)
				for (const float sample : channel)
				{
					const double steps = sample / step;
					REQUIRE(steps == std::round(steps));
					REQUIRE(sample >= -1.0f);
					REQUIRE(sample <= 1.0 - step);
				}
		}

		SECTION("the error is white noise of half a step RMS, independent of the signal")
		{
			for (const double level : {0.0, 0.1})
			{
				processors::Dither dither;
				Runner runner(dither);
				runner.set({{Dither::Bits, static_cast<double>(bits)}});
				const Signal input = sine(1, 192000, 997.0, level);
				const Signal output = runner.run(input);
				std::vector<double> error(input[0].size());
				double mean = 0.0;
				for (size_t i = 0; i < error.size(); ++i)
				{
					error[i] = static_cast<double>(output[0][i]) - input[0][i];
					mean += error[i];
				}
				mean /= static_cast<double>(error.size());
				double power = 0.0;
				double lag1 = 0.0;
				for (size_t i = 0; i < error.size(); ++i)
				{
					power += error[i] * error[i];
					if (i > 0)
						lag1 += error[i] * error[i - 1];
				}
				INFO("level " << level);
				CHECK(std::abs(mean) < 0.02 * step);
				CHECK_THAT(std::sqrt(power / static_cast<double>(error.size())), WithinRel(step / 2.0, 0.02));
				CHECK(std::abs(lag1 / power) < 0.02);
			}
		}
	}

	SECTION("noise shaping: the error passes through 1 - z^-1 or (1 - z^-1)^2")
	{
		const double step = std::ldexp(1.0, -15);
		// Variance gain of the noise transfer function, and its normalized autocorrelation at lags 1 and 2.
		struct Expected
		{
			double shaping;
			double gain;
			double lag1;
			double lag2;
		};
		for (const Expected expected : {Expected{1.0, 2.0, -0.5, 0.0}, Expected{2.0, 6.0, -4.0 / 6.0, 1.0 / 6.0}})
		{
			processors::Dither dither;
			Runner runner(dither);
			runner.set({{Dither::Bits, 16.0}, {Dither::NoiseShaping, expected.shaping}});
			const Signal input = sine(1, 192000, 440.0, 0.3);
			const Signal output = runner.run(input);
			std::vector<double> error(input[0].size());
			for (size_t i = 0; i < error.size(); ++i)
				error[i] = static_cast<double>(output[0][i]) - input[0][i];
			double power = 0.0;
			double lag1 = 0.0;
			double lag2 = 0.0;
			for (size_t i = 2; i < error.size(); ++i)
			{
				power += error[i] * error[i];
				lag1 += error[i] * error[i - 1];
				lag2 += error[i] * error[i - 2];
			}
			INFO("shaping " << expected.shaping);
			const double variance = power / static_cast<double>(error.size() - 2);
			CHECK_THAT(variance, WithinRel(expected.gain * step * step / 4.0, 0.04));
			CHECK_THAT(lag1 / power, WithinAbs(expected.lag1, 0.02));
			CHECK_THAT(lag2 / power, WithinAbs(expected.lag2, 0.02));
		}
	}

	SECTION("the noise is the same on every run and after a reset")
	{
		processors::Dither first;
		processors::Dither second;
		Runner a(first);
		Runner b(second);
		const Signal input = noise(2, 4096, 0.5);
		const Signal output = a.run(input);
		CHECK(b.run(input) == output);
		CHECK(output[0] != output[1]); // each channel has noise of its own
		first.reset();
		CHECK(a.run(input) == output);
	}
}

TEST_CASE("every built-in processor is deterministic, and its defaults are the ones it describes", "[unit]")
{
	using namespace gnubase::engine::processors;
	for (const BuiltinInfo& info : builtins())
	{
		INFO(info.id);
		const std::shared_ptr<Processor> fresh = createBuiltin(info.id);
		const std::shared_ptr<Processor> configured = createBuiltin(info.id);
		Runner freshRunner(*fresh);
		Runner configuredRunner(*configured);
		std::vector<std::pair<uint32_t, double>> defaults;
		defaults.reserve(info.parameters.size());
		for (const ParameterInfo& parameter : info.parameters)
			defaults.emplace_back(parameter.id, parameter.defaultValue);
		configuredRunner.set(defaults);
		const Signal input = noise(2, size_t{4} * g_block, 0.5);
		const Signal output = freshRunner.run(input);
		CHECK(configuredRunner.run(input) == output);

		for (const ParameterInfo& parameter : info.parameters)
		{
			CHECK(parameter.minimum <= parameter.defaultValue);
			CHECK(parameter.defaultValue <= parameter.maximum);
			if (!parameter.options.empty())
			{
				CHECK(parameter.stepped);
				CHECK(parameter.maximum - parameter.minimum + 1 == static_cast<double>(parameter.options.size()));
			}
		}
	}
}

TEST_CASE("every effect forgets its past on reset", "[unit]")
{
	using namespace gnubase::engine::processors;
	for (const char* id : {"compressor", "delay", "dither", "eq", "gate", "limiter", "reverb"})
	{
		INFO(id);
		const std::shared_ptr<Processor> processor = createBuiltin(id);
		Runner runner(*processor);
		const Signal input = noise(2, 8192, 0.8);
		const Signal output = runner.run(input);
		runner.run(noise(2, 8192, 1.0, 5));
		(*processor).reset();
		CHECK(runner.run(input) == output);
	}
}

TEST_CASE("the effect cores decay to exact silence without subnormal numbers", "[unit]")
{
	// The cores on their own, without the processors' denormal mode: their own flushing has to do it.
	const auto check = [](auto& core, const char* name)
	{
		INFO(name);
		core.prepare(g_sampleRate, g_block);
		Signal signal = noise(2, size_t{3} * 48000, 0.5);
		for (std::vector<float>& channel : signal)
			std::fill(channel.begin() + 4800, channel.end(), 0.0f);
		float* const channels[] = {signal[0].data(), signal[1].data()};
		const auto length = static_cast<uint32_t>(signal[0].size());
		for (uint32_t start = 0; start < length; start += g_block)
			core.process(channels, 2, start, std::min(start + g_block, length));
		size_t subnormal = 0;
		for (const std::vector<float>& channel : signal)
			for (const float sample : channel)
				subnormal += std::fpclassify(sample) == FP_SUBNORMAL ? 1 : 0;
		CHECK(subnormal == 0);
		CHECK(peak(signal[0], signal[0].size() - 4800) == 0.0);
		CHECK(peak(signal[1], signal[1].size() - 4800) == 0.0);
	};

	dsp::Equalizer eq;
	eq.setParameter(Equalizer::parameter(0, Equalizer::On), 1.0);
	eq.setParameter(Equalizer::parameter(0, Equalizer::Slope), 4.0);
	eq.setParameter(Equalizer::parameter(1, Equalizer::Gain), 12.0);
	eq.setParameter(Equalizer::parameter(2, Equalizer::Gain), -12.0);
	eq.setParameter(Equalizer::parameter(2, Equalizer::Q), 12.0);
	eq.setParameter(Equalizer::parameter(5, Equalizer::On), 1.0);
	check(eq, "eq");
	dsp::Gate gate;
	gate.setParameter(dsp::Gate::Range, 0.0);
	check(gate, "gate");
	dsp::Compressor compressor;
	compressor.setParameter(dsp::Compressor::Detection, 1.0);
	check(compressor, "compressor");
	dsp::Limiter limiter;
	check(limiter, "limiter");
	dsp::Delay delay;
	delay.setParameter(dsp::Delay::Time, 10.0);
	delay.setParameter(dsp::Delay::Feedback, 50.0);
	check(delay, "delay");
	dsp::Reverb reverb;
	reverb.setParameter(dsp::Reverb::Decay, 0.2);
	reverb.setParameter(dsp::Reverb::Predelay, 0.0);
	check(reverb, "reverb");
}
