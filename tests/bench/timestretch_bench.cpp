// SPDX-License-Identifier: GPL-3.0-or-later
// Time-stretch library benchmark (#142): measures Rubber Band Library through GNUbase's wrapper and, when the build
// is given its headers, Signalsmith Stretch, on test signals. Prints the tables of docs/time-stretch.md. Build it
// with -DGNUBASE_BENCHMARKS=ON (and -DGNUBASE_BENCH_SIGNALSMITH_DIRS=...); run it in Release for CPU numbers and in an
// MSVC Debug build for allocation counts that include malloc.
#include "engine/dsp/timestretch.h"
#include "platform/rt/realtime.h"

#ifdef GNUBASE_BENCH_SIGNALSMITH
#include <signalsmith-stretch.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{
	using Channels = std::vector<std::vector<float>>;
	namespace dsp = gnubase::engine::dsp;
	using Clock = std::chrono::steady_clock;

	constexpr double g_pi = 3.14159265358979323846;
	constexpr double g_rate = 48000.0;
	constexpr size_t g_block = 512; // output frames per real-time block

	// Test signals

	std::vector<float> sine(double frequency, size_t frames)
	{
		std::vector<float> samples(frames);
		for (size_t i = 0; i < frames; ++i)
			samples[i] = static_cast<float>(0.5 * std::sin(2.0 * g_pi * frequency * static_cast<double>(i) / g_rate));
		return samples;
	}

	// 20 harmonics of 110 Hz in cosine phase: a pulse-like wave with a high crest factor that phase coherence between
	// the partials shapes.
	std::vector<float> pulseTone(size_t frames)
	{
		std::vector<float> samples(frames);
		for (size_t i = 0; i < frames; ++i)
		{
			double value = 0.0;
			for (int harmonic = 1; harmonic <= 20; ++harmonic)
				value += std::cos(2.0 * g_pi * 110.0 * harmonic * static_cast<double>(i) / g_rate);
			samples[i] = static_cast<float>(0.03 * value);
		}
		return samples;
	}

	// A noise burst with a sharp attack and a 20 ms decay every 250 ms.
	std::vector<float> hits(size_t frames)
	{
		std::vector<float> samples(frames);
		uint32_t state = 12345;
		const auto period = static_cast<size_t>(0.25 * g_rate);
		for (size_t i = 0; i < frames; ++i)
		{
			state = (state * 1664525u) + 1013904223u;
			const double noise = (static_cast<double>(state >> 8) / 8388608.0) - 1.0;
			const double t = static_cast<double>(i % period) / g_rate;
			samples[i] = static_cast<float>(0.8 * noise * std::exp(-t / 0.02));
		}
		return samples;
	}

	// Measurements

	double rms(const std::vector<float>& x, size_t begin, size_t end)
	{
		double sum = 0.0;
		for (size_t i = begin; i < end; ++i)
			sum += static_cast<double>(x[i]) * x[i];
		return std::sqrt(sum / static_cast<double>(std::max<size_t>(end - begin, 1)));
	}

	double db(double ratio)
	{
		return 20.0 * std::log10(std::max(ratio, 1e-12));
	}

	double frequency(const std::vector<float>& x, size_t begin, size_t end)
	{
		double first = -1.0;
		double last = -1.0;
		int crossings = 0;
		for (size_t i = begin + 1; i < end; ++i)
			if (x[i - 1] < 0.0f && x[i] >= 0.0f)
			{
				const double at = static_cast<double>(i - 1) + (-x[i - 1] / (x[i] - x[i - 1]));
				first = crossings == 0 ? at : first;
				last = at;
				++crossings;
			}
		return crossings > 2 ? (crossings - 1) / (last - first) * g_rate : 0.0;
	}

	double crestFactor(const std::vector<float>& x, size_t begin, size_t end)
	{
		float peak = 0.0f;
		for (size_t i = begin; i < end; ++i)
			peak = std::max(peak, std::abs(x[i]));
		return db(peak / rms(x, begin, end));
	}

	// The spread (standard deviation) of the RMS levels of 100 ms windows in dB: amplitude modulation of the steady
	// pulse tone, whose period fits a window exactly 11 times, so the input itself has none.
	double ripple(const std::vector<float>& x, size_t begin, size_t end)
	{
		const auto window = static_cast<size_t>(0.1 * g_rate);
		std::vector<double> levels;
		for (size_t at = begin; at + window <= end; at += window)
			levels.push_back(db(rms(x, at, at + window)));
		double mean = 0.0;
		for (const double level : levels)
			mean += level;
		mean /= static_cast<double>(levels.size());
		double variance = 0.0;
		for (const double level : levels)
			variance += (level - mean) * (level - mean);
		return std::sqrt(variance / static_cast<double>(levels.size()));
	}

	// Residual of a null test against the input at the best lag within +-64 frames, in dB below the input.
	double nullResidual(const std::vector<float>& output, const std::vector<float>& input)
	{
		const size_t edge = 9600;
		double best = 0.0;
		for (int lag = -64; lag <= 64; ++lag)
		{
			double residual = 0.0;
			double signal = 0.0;
			for (size_t i = edge; i + edge < input.size(); ++i)
			{
				const auto j = static_cast<size_t>(static_cast<int64_t>(i) + lag);
				const double difference = static_cast<double>(output[j]) - input[i];
				residual += difference * difference;
				signal += static_cast<double>(input[i]) * input[i];
			}
			const double level = 10.0 * std::log10(residual / signal);
			best = lag == -64 ? level : std::min(best, level);
		}
		return best;
	}

	struct Smear
	{
		double preEcho = 0.0; // energy 3-25 ms before the hit's peak over the 22 ms after it, dB
		double rise = 0.0;    // 10 % to 90 % of the peak of the 1 ms envelope, ms
	};

	Smear transientSmear(const std::vector<float>& x, double ratio, size_t hitCount)
	{
		const auto ms = [](double value) { return static_cast<size_t>(value * g_rate / 1000.0); };
		// 1 ms peak envelope
		std::vector<float> envelope(x.size());
		const size_t half = ms(0.5);
		for (size_t i = 0; i < x.size(); ++i)
		{
			float peak = 0.0f;
			for (size_t j = i > half ? i - half : 0; j < std::min(x.size(), i + half); ++j)
				peak = std::max(peak, std::abs(x[j]));
			envelope[i] = peak;
		}
		Smear sum;
		int counted = 0;
		for (size_t hit = 1; hit + 1 < hitCount; ++hit)
		{
			const auto expected = static_cast<size_t>(std::llround(static_cast<double>(hit) * 0.25 * g_rate * ratio));
			size_t peakAt = expected;
			for (size_t i = expected - ms(20); i < expected + ms(20); ++i)
				if (envelope[i] > envelope[peakAt])
					peakAt = i;
			double before = 0.0;
			double after = 0.0;
			for (size_t i = peakAt - ms(25); i < peakAt - ms(3); ++i)
				before += static_cast<double>(x[i]) * x[i];
			for (size_t i = peakAt; i < peakAt + ms(22); ++i)
				after += static_cast<double>(x[i]) * x[i];
			size_t low = peakAt;
			while (low > peakAt - ms(30) && envelope[low] > 0.1f * envelope[peakAt])
				--low;
			size_t high = low;
			while (high < peakAt && envelope[high] < 0.9f * envelope[peakAt])
				++high;
			sum.preEcho += 10.0 * std::log10(std::max(before, 1e-20) / after);
			sum.rise += static_cast<double>(high - low) * 1000.0 / g_rate;
			++counted;
		}
		sum.preEcho /= counted;
		sum.rise /= counted;
		return sum;
	}

	// Allocations in real-time code, through the RT checker linked into the benchmark.
	std::atomic<int> g_allocations{0};

	void countAllocation(const char* /*what*/) noexcept
	{
		++g_allocations;
	}

	// Candidates

	struct Rendered
	{
		Channels output;
		double seconds = 0.0;    // processing time
		double worstBlock = 0.0; // seconds, real time only
		size_t latency = 0;      // output frames, real time only
		int allocations = 0;     // in real-time processing
	};

	struct Candidate
	{
		std::string name;
		std::function<Rendered(const Channels&, double ratio, double semitones, bool realtime)> render;
	};

	Rendered renderWrapper(
		dsp::StretchPreset preset, const Channels& input, double ratio, double semitones, bool realtime)
	{
		dsp::StretchSettings settings;
		settings.channels = static_cast<int>(input.size());
		settings.sampleRate = g_rate;
		settings.preset = preset;
		settings.ratio = ratio;
		settings.semitones = semitones;
		Rendered result;
		if (!realtime)
		{
			settings.mode = dsp::StretchMode::Offline;
			const auto start = Clock::now();
			result.output = dsp::stretch(input, settings);
			result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
			return result;
		}
		settings.mode = dsp::StretchMode::RealTime;
		settings.maxBlockFrames = 2048;
		dsp::TimeStretcher stretcher(settings);
		const size_t channels = input.size();
		const size_t frames = input.front().size();
		const auto length = static_cast<size_t>(dsp::stretchedLength(static_cast<int64_t>(frames), ratio));
		result.latency = stretcher.latency();
		const size_t pad = stretcher.startPad();
		Channels output(channels, std::vector<float>(result.latency + length + g_block));
		Channels scratch(channels, std::vector<float>(settings.maxBlockFrames));
		std::vector<const float*> in(channels);
		std::vector<float*> out(channels);
		size_t fed = 0;
		size_t produced = 0;
		const auto previous = gnubase::rt::setViolationHandler(&countAllocation);
		g_allocations = 0;
		while (produced < result.latency + length)
		{
			const auto start = Clock::now();
			{
				const gnubase::rt::ScopedRealtimeContext realtimeContext;
				while (stretcher.available() < g_block)
				{
					const size_t count = std::clamp<size_t>(stretcher.inputRequired(), 1, settings.maxBlockFrames);
					for (size_t c = 0; c < channels; ++c)
					{
						for (size_t i = 0; i < count; ++i)
						{
							const size_t at = fed + i;
							scratch[c][i] = at >= pad && at - pad < frames ? input[c][at - pad] : 0.0f;
						}
						in[c] = scratch[c].data();
					}
					stretcher.process(in.data(), count, false);
					fed += count;
				}
				for (size_t c = 0; c < channels; ++c)
					out[c] = output[c].data() + produced;
				produced += stretcher.retrieve(out.data(), g_block);
			}
			const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
			result.seconds += seconds;
			result.worstBlock = std::max(result.worstBlock, seconds);
		}
		gnubase::rt::setViolationHandler(previous);
		result.allocations = g_allocations;
		for (std::vector<float>& channel : output)
			channel = std::vector<float>(channel.begin() + static_cast<std::ptrdiff_t>(result.latency),
				channel.begin() + static_cast<std::ptrdiff_t>(result.latency + length));
		result.output = std::move(output);
		return result;
	}

#ifdef GNUBASE_BENCH_SIGNALSMITH
	Rendered renderSignalsmith(bool cheaper, const Channels& input, double ratio, double semitones, bool realtime)
	{
		signalsmith::stretch::SignalsmithStretch<float> stretch(42); // seeded: deterministic
		const int channels = static_cast<int>(input.size());
		if (cheaper)
			stretch.presetCheaper(channels, static_cast<float>(g_rate));
		else
			stretch.presetDefault(channels, static_cast<float>(g_rate));
		stretch.setTransposeSemitones(static_cast<float>(semitones));
		const size_t frames = input.front().size();
		const auto length = static_cast<size_t>(dsp::stretchedLength(static_cast<int64_t>(frames), ratio));
		Rendered result;
		Channels output(input.size(), std::vector<float>(length));
		if (!realtime)
		{
			std::vector<const float*> in;
			std::vector<float*> out;
			for (size_t c = 0; c < input.size(); ++c)
			{
				in.push_back(input[c].data());
				out.push_back(output[c].data());
			}
			const auto start = Clock::now();
			stretch.exact(in.data(), static_cast<int>(frames), out.data(), static_cast<int>(length));
			result.seconds = std::chrono::duration<double>(Clock::now() - start).count();
			result.output = std::move(output);
			return result;
		}
		// The internal position lags the input by inputLatency and leads the output by outputLatency.
		result.latency = static_cast<size_t>(std::lround(stretch.outputLatency() + (stretch.inputLatency() * ratio)));
		const size_t total = result.latency + length;
		Channels raw(input.size(), std::vector<float>(total + g_block));
		Channels scratch(input.size(), std::vector<float>(static_cast<size_t>(g_block / 0.2) + 2));
		std::vector<const float*> in(input.size());
		std::vector<float*> out(input.size());
		size_t fed = 0;
		const auto previous = gnubase::rt::setViolationHandler(&countAllocation);
		g_allocations = 0;
		for (size_t produced = 0; produced < total; produced += g_block)
		{
			const auto target = static_cast<size_t>(std::llround(static_cast<double>(produced + g_block) / ratio));
			const size_t count = target - fed;
			const auto start = Clock::now();
			{
				const gnubase::rt::ScopedRealtimeContext realtimeContext;
				for (size_t c = 0; c < input.size(); ++c)
				{
					for (size_t i = 0; i < count; ++i)
						scratch[c][i] = fed + i < frames ? input[c][fed + i] : 0.0f;
					in[c] = scratch[c].data();
					out[c] = raw[c].data() + produced;
				}
				stretch.process(in.data(), static_cast<int>(count), out.data(), static_cast<int>(g_block));
			}
			fed += count;
			const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
			result.seconds += seconds;
			result.worstBlock = std::max(result.worstBlock, seconds);
		}
		gnubase::rt::setViolationHandler(previous);
		result.allocations = g_allocations;
		for (size_t c = 0; c < input.size(); ++c)
			std::copy_n(raw[c].begin() + static_cast<std::ptrdiff_t>(result.latency), length, output[c].begin());
		result.output = std::move(output);
		return result;
	}
#endif

	std::vector<Candidate> candidates()
	{
		std::vector<Candidate> list;
		for (const auto& [name, preset] : {std::pair{"Rubber Band R3 (quality)", dsp::StretchPreset::Quality},
				 std::pair{"Rubber Band R3 short (efficient)", dsp::StretchPreset::Efficient},
				 std::pair{"Rubber Band R2 (mix)", dsp::StretchPreset::Mix},
				 std::pair{"Rubber Band R2 (drums)", dsp::StretchPreset::Drums}})
			list.push_back({name, [preset = preset](const Channels& input, double ratio, double semitones, bool rt)
				{ return renderWrapper(preset, input, ratio, semitones, rt); }});
#ifdef GNUBASE_BENCH_SIGNALSMITH
		list.push_back(
			{"Signalsmith Stretch (default)", [](const Channels& input, double ratio, double semitones, bool rt)
				{ return renderSignalsmith(false, input, ratio, semitones, rt); }});
		list.push_back(
			{"Signalsmith Stretch (cheaper)", [](const Channels& input, double ratio, double semitones, bool rt)
				{ return renderSignalsmith(true, input, ratio, semitones, rt); }});
#endif
		return list;
	}
}

namespace
{
	void runBenchmark()
	{
		const size_t seconds2 = 96000;
		const std::vector<float> sine440 = sine(440.0, seconds2);
		const std::vector<float> pulse = pulseTone(seconds2);
		const std::vector<float> drums = hits(seconds2);
		// 10 s of stereo for CPU: the tone and the hits, a little apart.
		std::vector<float> left(480000);
		std::vector<float> right(480000);
		{
			const std::vector<float> tone = pulseTone(480000);
			const std::vector<float> beat = hits(480000);
			for (size_t i = 0; i < left.size(); ++i)
			{
				left[i] = (0.7f * tone[i]) + (0.5f * beat[i]);
				right[i] = (0.5f * tone[i]) + (0.7f * beat[i]);
			}
		}
		const Channels mix{left, right};
		const double pulseCrest = crestFactor(pulse, 9600, seconds2 - 9600);

		struct Shift
		{
			double semitones;
			const char* label;
		};
		const Shift shifts[] = {{7.0, "+7"}, {-5.0, "-5"}, {12.0, "+12"}, {0.5, "+0.5"}, {-12.3, "-12.3"}};

		for (const bool realtime : {false, true})
		{
			std::printf("\n## %s mode\n\n", realtime ? "Real-time" : "Offline");
			std::printf("| Candidate | Pitch error, max (cents) | Pre-echo (dB) | Attack (ms) | Crest loss (dB) | "
						"Ripple (dB) | "
						"Unity null (dB) | CPU (x real time) | Worst block (%% of %zu frames) | Latency (frames) | "
						"Allocations |\n",
				g_block);
			std::printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
			for (const Candidate& candidate : candidates())
			{
				double pitchError = 0.0;
				for (const Shift& shift : shifts)
				{
					const Rendered shifted = candidate.render({sine440}, 1.0, shift.semitones, realtime);
					const double expected = 440.0 * dsp::pitchScale(shift.semitones, 0.0);
					const double measured = frequency(shifted.output.front(), 9600, seconds2 - 9600);
					pitchError = std::max(pitchError, std::abs(1200.0 * std::log2(measured / expected)));
				}
				const Rendered smeared = candidate.render({drums}, 1.5, 0.0, realtime);
				const Smear smear = transientSmear(smeared.output.front(), 1.5, 8);
				const Rendered stretched = candidate.render({pulse}, 1.5, 0.0, realtime);
				const size_t stretchedEnd = stretched.output.front().size() - 9600;
				const double crestLoss = pulseCrest - crestFactor(stretched.output.front(), 9600, stretchedEnd);
				const double rippleDb = ripple(stretched.output.front(), 9600, stretchedEnd);
				const Rendered unity = candidate.render({pulse}, 1.0, 0.0, realtime);
				const double null = nullResidual(unity.output.front(), pulse);
				const Rendered cpu = candidate.render(mix, 1.25, 3.0, realtime);
				const double speed = 10.0 / cpu.seconds;
				const double worst = 100.0 * cpu.worstBlock / (static_cast<double>(g_block) / g_rate);
				std::printf("| %s | %.1f | %.1f | %.2f | %.1f | %.2f | %.1f | %.0f | %s | %s | %s |\n",
					candidate.name.c_str(), pitchError, smear.preEcho, smear.rise, crestLoss, rippleDb, null, speed,
					realtime ? std::to_string(static_cast<int>(std::lround(worst))).c_str() : "-",
					realtime ? std::to_string(cpu.latency).c_str() : "0",
					realtime ? std::to_string(cpu.allocations).c_str() : "-");
				std::fflush(stdout);
			}
		}
		std::printf(
			"\nInput: crest factor %.1f dB, ripple %.2f dB; pre-echo and attack of the unstretched hits: %.1f dB, "
			"%.2f ms\n",
			pulseCrest, ripple(pulse, 9600, seconds2 - 9600), transientSmear(drums, 1.0, 8).preEcho,
			transientSmear(drums, 1.0, 8).rise);
	}
}

int main()
{
	try
	{
		runBenchmark();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "benchmark failed: %s\n", error.what());
		return 1;
	}
}
