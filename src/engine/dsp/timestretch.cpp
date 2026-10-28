// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dsp/timestretch.h"

#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <rubberband/RubberBandStretcher.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <utility>

namespace gnubase::engine::dsp
{
	namespace
	{
		using RubberBand::RubberBandStretcher;

		constexpr std::array<std::pair<StretchMode, std::string_view>, 2> g_modeNames{{
			{StretchMode::RealTime, "realtime"},
			{StretchMode::Offline, "offline"},
		}};

		constexpr std::array<std::pair<StretchPreset, std::string_view>, 8> g_presetNames{{
			{StretchPreset::Quality, "quality"},
			{StretchPreset::Efficient, "efficient"},
			{StretchPreset::Drums, "drums"},
			{StretchPreset::Plucked, "plucked"},
			{StretchPreset::Pads, "pads"},
			{StretchPreset::Vocals, "vocals"},
			{StretchPreset::Mix, "mix"},
			{StretchPreset::Solo, "solo"},
		}};

		template <typename Value, size_t Count>
		std::string_view nameOf(const std::array<std::pair<Value, std::string_view>, Count>& names, Value value)
		{
			for (const auto& [entry, name] : names)
				if (entry == value)
					return name;
			return {};
		}

		template <typename Value, size_t Count> std::optional<Value> valueOf(
			const std::array<std::pair<Value, std::string_view>, Count>& names, std::string_view name)
		{
			for (const auto& [value, entry] : names)
				if (entry == name)
					return value;
			return std::nullopt;
		}

		template <typename Value, size_t Count>
		std::vector<std::string> namesOf(const std::array<std::pair<Value, std::string_view>, Count>& names)
		{
			std::vector<std::string> result;
			result.reserve(names.size());
			for (const auto& entry : names)
				result.emplace_back(entry.second);
			return result;
		}

		// Rubber Band's messages go to GNUbase's log, which is real-time safe.
		class Logger final : public RubberBandStretcher::Logger
		{
		public:
			void log(const char* message) override { logging::warning("time stretch: {}", message); }
			void log(const char* message, double a) override { logging::warning("time stretch: {} {}", message, a); }
			void log(const char* message, double a, double b) override
			{
				logging::warning("time stretch: {} {} {}", message, a, b);
			}
		};

		// The engine and options of each preset; docs/time-stretch.md describes them.
		RubberBandStretcher::Options presetOptions(StretchPreset preset)
		{
			switch (preset)
			{
			case StretchPreset::Quality:
				return RubberBandStretcher::OptionEngineFiner | RubberBandStretcher::OptionWindowStandard |
					RubberBandStretcher::OptionChannelsTogether;
			case StretchPreset::Efficient:
				return RubberBandStretcher::OptionEngineFiner | RubberBandStretcher::OptionWindowShort |
					RubberBandStretcher::OptionChannelsTogether;
			case StretchPreset::Drums:
				return RubberBandStretcher::OptionEngineFaster | RubberBandStretcher::OptionWindowShort |
					RubberBandStretcher::OptionPhaseIndependent | RubberBandStretcher::OptionTransientsCrisp |
					RubberBandStretcher::OptionDetectorPercussive;
			case StretchPreset::Plucked:
				return RubberBandStretcher::OptionEngineFaster | RubberBandStretcher::OptionTransientsCrisp |
					RubberBandStretcher::OptionDetectorCompound;
			case StretchPreset::Pads:
				return RubberBandStretcher::OptionEngineFaster | RubberBandStretcher::OptionTransientsSmooth |
					RubberBandStretcher::OptionWindowLong;
			case StretchPreset::Vocals:
				return RubberBandStretcher::OptionEngineFaster | RubberBandStretcher::OptionTransientsMixed |
					RubberBandStretcher::OptionDetectorSoft;
			case StretchPreset::Mix:
				return RubberBandStretcher::OptionEngineFaster | RubberBandStretcher::OptionTransientsMixed |
					RubberBandStretcher::OptionDetectorCompound | RubberBandStretcher::OptionChannelsTogether;
			case StretchPreset::Solo:
				return RubberBandStretcher::OptionEngineFaster | RubberBandStretcher::OptionTransientsCrisp |
					RubberBandStretcher::OptionDetectorSoft;
			}
			return RubberBandStretcher::DefaultOptions;
		}

		RubberBandStretcher::Options options(const StretchSettings& settings)
		{
			RubberBandStretcher::Options result = presetOptions(settings.preset) |
				RubberBandStretcher::OptionThreadingNever |
				(settings.preserveFormants ? RubberBandStretcher::OptionFormantPreserved
										   : RubberBandStretcher::OptionFormantShifted);
			// In real time the pitch may change while playing, without clicks when it crosses no shift.
			if (settings.mode == StretchMode::RealTime)
				result |= RubberBandStretcher::OptionProcessRealTime | RubberBandStretcher::OptionPitchHighConsistency;
			return result;
		}

		struct Limits
		{
			double minRatio;
			double maxRatio;
			double maxSemitones;
		};

		Limits limits(StretchMode mode)
		{
			if (mode == StretchMode::RealTime)
				return {g_minRealtimeRatio, g_maxRealtimeRatio, g_maxRealtimePitchSemitones};
			return {g_minStretchRatio, g_maxStretchRatio, g_maxPitchSemitones};
		}

		double clampRatio(double ratio, StretchMode mode)
		{
			return std::clamp(ratio, limits(mode).minRatio, limits(mode).maxRatio);
		}

		double clampedPitchScale(double semitones, double cents, StretchMode mode)
		{
			const double limit = pitchScale(limits(mode).maxSemitones, 0.0);
			return std::clamp(pitchScale(semitones, cents), 1.0 / limit, limit);
		}

		void check(bool condition, const char* message)
		{
			if (!condition)
				throw std::invalid_argument(message);
		}
	}

	std::string_view toString(StretchMode mode)
	{
		return nameOf(g_modeNames, mode);
	}

	std::string_view toString(StretchPreset preset)
	{
		return nameOf(g_presetNames, preset);
	}

	std::optional<StretchMode> stretchModeFromString(std::string_view name)
	{
		return valueOf(g_modeNames, name);
	}

	std::optional<StretchPreset> stretchPresetFromString(std::string_view name)
	{
		return valueOf(g_presetNames, name);
	}

	std::vector<std::string> stretchModeNames()
	{
		return namesOf(g_modeNames);
	}

	std::vector<std::string> stretchPresetNames()
	{
		return namesOf(g_presetNames);
	}

	double pitchScale(double semitones, double cents)
	{
		return std::pow(2.0, (semitones + (cents / 100.0)) / 12.0);
	}

	int64_t stretchedLength(int64_t inputFrames, double ratio)
	{
		return std::llround(static_cast<double>(inputFrames) * ratio);
	}

	struct TimeStretcher::Impl
	{
		std::unique_ptr<RubberBandStretcher> stretcher;
		bool started = false; // offline mode: study or process was called, so the settings are fixed
	};

	TimeStretcher::TimeStretcher(const StretchSettings& settings)
		: m_settings(settings), m_impl(std::make_unique<Impl>())
	{
		check(settings.channels > 0, "a time stretcher needs at least one channel");
		check(settings.sampleRate > 0.0, "the sample rate must be positive");
		check(settings.maxBlockFrames > 0, "the block size must be positive");
		const Limits range = limits(settings.mode);
		check(settings.ratio >= range.minRatio && settings.ratio <= range.maxRatio,
			settings.mode == StretchMode::RealTime ? "the stretch ratio must be between 0.25 and 4 in real time"
												   : "the stretch ratio must be between 0.1 and 10");
		check(std::abs(settings.semitones + (settings.cents / 100.0)) <= range.maxSemitones,
			settings.mode == StretchMode::RealTime ? "the pitch shift must be within 24 semitones in real time"
												   : "the pitch shift must be within 48 semitones");
		const double scale = clampedPitchScale(settings.semitones, settings.cents, settings.mode);
		m_impl->stretcher = std::make_unique<RubberBandStretcher>(static_cast<size_t>(std::lround(settings.sampleRate)),
			static_cast<size_t>(settings.channels), std::make_shared<Logger>(), options(settings), settings.ratio,
			scale);
		RubberBandStretcher& stretcher = *m_impl->stretcher;
		stretcher.setMaxProcessSize(settings.maxBlockFrames);
		if (settings.mode == StretchMode::RealTime)
		{
			// The faster engine sizes its buffers, windows and FFTs for the current ratio and pitch, and grows them
			// when those change, which would allocate while playing. Going through the whole range once allocates them
			// all now.
			constexpr int steps = 12;
			const double maxScale = pitchScale(range.maxSemitones, 0.0);
			for (int r = 0; r <= steps; ++r)
				for (int p = 0; p <= steps; ++p)
				{
					stretcher.setTimeRatio(
						range.minRatio * std::pow(range.maxRatio / range.minRatio, r / double{steps}));
					stretcher.setPitchScale(std::pow(maxScale, ((2.0 * p) / steps) - 1.0));
				}
			stretcher.setTimeRatio(settings.ratio);
			stretcher.setPitchScale(scale);
		}
	}

	TimeStretcher::~TimeStretcher() = default;

	void TimeStretcher::setRatio(double ratio) noexcept
	{
		if (m_impl->started)
			return;
		m_settings.ratio = clampRatio(ratio, m_settings.mode);
		m_impl->stretcher->setTimeRatio(m_settings.ratio);
	}

	void TimeStretcher::setPitch(double semitones, double cents) noexcept
	{
		if (m_impl->started)
			return;
		m_settings.semitones = semitones;
		m_settings.cents = cents;
		m_impl->stretcher->setPitchScale(clampedPitchScale(semitones, cents, m_settings.mode));
	}

	void TimeStretcher::setPreserveFormants(bool preserve) noexcept
	{
		if (m_impl->started || preserve == m_settings.preserveFormants)
			return;
		m_settings.preserveFormants = preserve;
		m_impl->stretcher->setFormantOption(
			preserve ? RubberBandStretcher::OptionFormantPreserved : RubberBandStretcher::OptionFormantShifted);
	}

	size_t TimeStretcher::startPad() const noexcept
	{
		return m_impl->stretcher->getPreferredStartPad();
	}

	size_t TimeStretcher::latency() const noexcept
	{
		return m_impl->stretcher->getStartDelay();
	}

	void TimeStretcher::setInputLength(size_t frames)
	{
		rt::assertNotRealtime("TimeStretcher::setInputLength");
		m_impl->stretcher->setExpectedInputDuration(frames);
	}

	void TimeStretcher::study(const float* const* input, size_t frames, bool final)
	{
		rt::assertNotRealtime("TimeStretcher::study");
		GNUBASE_ASSERT(m_settings.mode == StretchMode::Offline);
		m_impl->started = true;
		m_impl->stretcher->study(input, frames, final);
	}

	size_t TimeStretcher::inputRequired() const noexcept
	{
		return std::min(m_impl->stretcher->getSamplesRequired(), m_settings.maxBlockFrames);
	}

	void TimeStretcher::process(const float* const* input, size_t frames, bool final) noexcept
	{
		GNUBASE_ASSERT(frames <= m_settings.maxBlockFrames);
		m_impl->started = m_settings.mode == StretchMode::Offline;
		m_impl->stretcher->process(input, std::min(frames, m_settings.maxBlockFrames), final);
	}

	size_t TimeStretcher::available() const noexcept
	{
		return static_cast<size_t>(std::max(m_impl->stretcher->available(), 0));
	}

	bool TimeStretcher::finished() const noexcept
	{
		return m_impl->stretcher->available() < 0;
	}

	size_t TimeStretcher::retrieve(float* const* output, size_t frames) noexcept
	{
		return m_impl->stretcher->retrieve(output, frames);
	}

	void TimeStretcher::reset()
	{
		rt::assertNotRealtime("TimeStretcher::reset");
		RubberBandStretcher& stretcher = *m_impl->stretcher;
		stretcher.reset();
		m_impl->started = false;
	}

	namespace
	{
		// Fills one buffer per channel with the next frames of the input and returns how many it read; 0 at the end.
		using Source = std::function<size_t(float* const* channels, size_t frames)>;
		// Takes the next frames of the output, one buffer per channel.
		using Sink = std::function<void(const float* const* channels, size_t frames)>;

		// Buffers of one block per channel and pointers into them.
		struct Block
		{
			Block(int channels, size_t frames)
				: data(static_cast<size_t>(channels), std::vector<float>(frames)), pointers(data.size()),
				  shifted(data.size())
			{
				for (size_t channel = 0; channel < data.size(); ++channel)
					pointers[channel] = data[channel].data();
			}

			size_t frames() const { return data.front().size(); }

			// Pointers to frame offset of every channel, valid until the next call.
			float* const* at(size_t offset)
			{
				for (size_t channel = 0; channel < data.size(); ++channel)
					shifted[channel] = data[channel].data() + offset;
				return shifted.data();
			}

			// Silence from frame begin to frame end.
			void silence(size_t begin, size_t end)
			{
				for (std::vector<float>& channel : data)
					std::fill(channel.begin() + static_cast<std::ptrdiff_t>(begin),
						channel.begin() + static_cast<std::ptrdiff_t>(end), 0.0f);
			}

			std::vector<std::vector<float>> data;
			std::vector<float*> pointers;
			std::vector<float*> shifted;
		};

		// Retrieves what the stretcher has and passes it on to sink, after dropping the first skip frames of the whole
		// output and up to length frames in all. Counts the frames dropped in skipped and passed on in written.
		void drain(TimeStretcher& stretcher, Block& output, size_t skip, size_t length, size_t& skipped,
			size_t& written, const Sink& sink)
		{
			while (const size_t available = stretcher.available())
			{
				const size_t got = stretcher.retrieve(output.pointers.data(), std::min(available, output.frames()));
				const size_t dropped = std::min(got, skip - skipped);
				skipped += dropped;
				const size_t count = std::min(got - dropped, length - written);
				if (count > 0)
					sink(output.at(dropped), count);
				written += count;
			}
		}

		// Offline mode: a study pass over the input, then a process pass. open returns the input from its start.
		void runOffline(
			const StretchSettings& settings, int64_t inputFrames, const std::function<Source()>& open, const Sink& sink)
		{
			TimeStretcher stretcher(settings);
			stretcher.setInputLength(static_cast<size_t>(inputFrames));
			const size_t blockFrames = settings.maxBlockFrames;
			Block input(settings.channels, blockFrames);
			Block output(settings.channels, blockFrames);

			Source source = open();
			int64_t studied = 0;
			for (;;)
			{
				const size_t frames = source(input.pointers.data(), blockFrames);
				studied += static_cast<int64_t>(frames);
				const bool final = frames < blockFrames;
				stretcher.study(input.pointers.data(), frames, final);
				if (final)
					break;
			}

			const auto length = static_cast<size_t>(stretchedLength(studied, settings.ratio));
			size_t written = 0;
			size_t skipped = 0;
			source = open();
			bool final = false;
			while (!final)
			{
				const size_t frames = source(input.pointers.data(), blockFrames);
				final = frames < blockFrames;
				stretcher.process(input.pointers.data(), frames, final);
				drain(stretcher, output, 0, length, skipped, written, sink);
			}
			// Everything is out once the final block is processed; the loop only guards against a stuck stretcher.
			for (int round = 0; round < 64 && !stretcher.finished() && written < length; ++round)
			{
				stretcher.process(input.pointers.data(), 0, true);
				drain(stretcher, output, 0, length, skipped, written, sink);
			}
			// The stretcher aims at the exact length; make it exact.
			output.silence(0, output.frames());
			while (written < length)
			{
				const size_t frames = std::min(blockFrames, length - written);
				sink(output.pointers.data(), frames);
				written += frames;
			}
		}

		// Real-time mode, as a player runs it: the start pad first, then the input, then silence until the output
		// that the latency held back is out too.
		void runRealtime(const StretchSettings& settings, int64_t inputFrames, const Source& source, const Sink& sink)
		{
			TimeStretcher stretcher(settings);
			const size_t blockFrames = settings.maxBlockFrames;
			Block input(settings.channels, blockFrames);
			Block output(settings.channels, blockFrames);
			const auto length = static_cast<size_t>(stretchedLength(inputFrames, settings.ratio));
			const size_t skip = stretcher.latency();
			size_t pad = stretcher.startPad();
			bool ended = false;
			size_t written = 0;
			size_t skipped = 0;
			while (written < length)
			{
				const size_t frames = std::clamp<size_t>(stretcher.inputRequired(), 1, blockFrames);
				size_t filled = std::min(pad, frames);
				input.silence(0, filled);
				pad -= filled;
				if (filled < frames && !ended)
				{
					const size_t got = source(input.at(filled), frames - filled);
					ended = got < frames - filled;
					filled += got;
				}
				input.silence(filled, frames);
				stretcher.process(input.pointers.data(), frames, false);
				drain(stretcher, output, skip, length, skipped, written, sink);
			}
		}

		void run(
			const StretchSettings& settings, int64_t inputFrames, const std::function<Source()>& open, const Sink& sink)
		{
			if (settings.mode == StretchMode::Offline)
				runOffline(settings, inputFrames, open, sink);
			else
				runRealtime(settings, inputFrames, open(), sink);
		}
	}

	std::vector<std::vector<float>> stretch(const std::vector<std::vector<float>>& input, StretchSettings settings)
	{
		rt::assertNotRealtime("dsp::stretch");
		settings.channels = static_cast<int>(input.size());
		const size_t frames = input.empty() ? 0 : input.front().size();
		std::vector<std::vector<float>> output(input.size());
		const auto length = static_cast<size_t>(stretchedLength(static_cast<int64_t>(frames), settings.ratio));
		for (std::vector<float>& channel : output)
			channel.reserve(length);
		const auto open = [&input, frames]
		{
			return Source(
				[&input, frames, position = size_t{0}](float* const* channels, size_t count) mutable
				{
					count = std::min(count, frames - position);
					for (size_t channel = 0; channel < input.size(); ++channel)
						std::copy_n(
							input[channel].begin() + static_cast<std::ptrdiff_t>(position), count, channels[channel]);
					position += count;
					return count;
				});
		};
		run(settings, static_cast<int64_t>(frames), open,
			[&output](const float* const* channels, size_t count)
			{
				for (size_t channel = 0; channel < output.size(); ++channel)
					output[channel].insert(output[channel].end(), channels[channel], channels[channel] + count);
			});
		return output;
	}

	AudioFileInfo stretchFile(const std::string& input, const std::string& output, StretchSettings settings)
	{
		rt::assertNotRealtime("dsp::stretchFile");
		const AudioFileInfo source = AudioFileReader::open(input)->info();
		settings.channels = source.channels;
		settings.sampleRate = source.sampleRate;
		// Checks the settings before the output file is created.
		{
			const TimeStretcher check(settings);
		}
		WriteOptions options;
		options.sampleFormat = SampleFormat::Float32;
		options.sampleRate = source.sampleRate;
		options.channels = source.channels;
		const auto writer = AudioFileWriter::create(output, options);
		const auto channels = static_cast<size_t>(source.channels);

		std::vector<float> interleaved;
		std::unique_ptr<AudioFileReader> reader;
		const auto open = [&]
		{
			reader = AudioFileReader::open(input);
			return Source(
				[&](float* const* buffers, size_t frames)
				{
					interleaved.resize(frames * channels);
					const size_t got = reader->read(interleaved.data(), frames);
					for (size_t i = 0; i < got; ++i)
						for (size_t channel = 0; channel < channels; ++channel)
							buffers[channel][i] = interleaved[(i * channels) + channel];
					return got;
				});
		};
		AudioFileInfo written = source;
		written.format = AudioFormat::Wav;
		written.sampleFormat = SampleFormat::Float32;
		written.frames = 0;
		written.broadcast.reset();
		std::vector<float> out;
		run(settings, source.frames, open,
			[&](const float* const* buffers, size_t frames)
			{
				out.resize(frames * channels);
				for (size_t i = 0; i < frames; ++i)
					for (size_t channel = 0; channel < channels; ++channel)
						out[(i * channels) + channel] = buffers[channel][i];
				writer->write(out.data(), frames);
				written.frames += static_cast<time::Samples>(frames);
			});
		writer->close();
		return written;
	}
}
