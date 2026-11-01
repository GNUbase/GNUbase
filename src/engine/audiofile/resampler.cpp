// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/resampler.h"

#include "engine/audiofile/r8brain.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::engine
{
	struct Resampler::Channels
	{
		std::vector<std::unique_ptr<detail::ChannelResampler>> resamplers;
	};

	Resampler::Resampler(double fromRate, double toRate, int channels, size_t maxInputFrames)
		: m_fromRate(fromRate), m_toRate(toRate), m_channelCount(channels), m_maxInputFrames(maxInputFrames),
		  m_input(maxInputFrames)
	{
		if (fromRate == toRate)
			return;
		m_channels = std::make_unique<Channels>();
		for (int channel = 0; channel < channels; ++channel)
			m_channels->resamplers.push_back(
				detail::createChannelResampler(fromRate, toRate, static_cast<int>(maxInputFrames)));
	}

	Resampler::~Resampler() = default;

	size_t Resampler::run(const float* input, size_t frames, std::vector<float>& output)
	{
		const auto channels = static_cast<size_t>(m_channelCount);
		const size_t start = output.size();
		size_t produced = 0;
		for (size_t channel = 0; channel < channels; ++channel)
		{
			for (size_t i = 0; i < frames; ++i)
				m_input[i] = input != nullptr ? input[(i * channels) + channel] : 0.0;
			double* result = nullptr;
			produced = static_cast<size_t>(
				m_channels->resamplers[channel]->process(m_input.data(), static_cast<int>(frames), result));
			if (channel == 0)
				output.resize(start + (produced * channels));
			for (size_t i = 0; i < produced; ++i)
				output[start + (i * channels) + channel] = static_cast<float>(result[i]);
		}
		return produced;
	}

	void Resampler::process(const float* input, size_t frames, std::vector<float>& output)
	{
		m_inputFrames += static_cast<int64_t>(frames);
		if (!m_channels)
		{
			output.insert(output.end(), input, input + (frames * static_cast<size_t>(m_channelCount)));
			m_outputFrames += static_cast<int64_t>(frames);
			return;
		}
		m_outputFrames += static_cast<int64_t>(run(input, frames, output));
	}

	void Resampler::finish(std::vector<float>& output)
	{
		if (!m_channels)
			return;
		// Silence pushes out what is still inside the filters; then the output is cut to its length.
		const int64_t length = outputLength(m_inputFrames, m_fromRate, m_toRate);
		while (m_outputFrames < length)
			m_outputFrames += static_cast<int64_t>(run(nullptr, m_maxInputFrames, output));
		output.resize(
			output.size() - (static_cast<size_t>(m_outputFrames - length) * static_cast<size_t>(m_channelCount)));
		m_outputFrames = length;
	}

	void Resampler::reset()
	{
		if (m_channels)
			for (const auto& resampler : m_channels->resamplers)
				resampler->clear();
		m_inputFrames = 0;
		m_outputFrames = 0;
	}

	size_t Resampler::inputLatency() const
	{
		return m_channels ? static_cast<size_t>(m_channels->resamplers.front()->inputLatency()) : 0;
	}

	int64_t Resampler::outputLength(int64_t inputFrames, double fromRate, double toRate)
	{
		return std::llround(static_cast<double>(inputFrames) * toRate / fromRate);
	}

	AudioFileInfo convertSampleRate(
		const std::string& input, const std::vector<std::string>& outputs, double sampleRate)
	{
		const auto reader = AudioFileReader::open(input);
		const AudioFileInfo& source = reader->info();
		const auto channels = static_cast<size_t>(source.channels);
		const bool split = outputs.size() != 1;
		if (split && outputs.size() != channels)
			throw AudioFileError(
				input + " has " + std::to_string(channels) + " channels, not " + std::to_string(outputs.size()));
		WriteOptions options;
		options.sampleFormat = SampleFormat::Float32;
		options.sampleRate = sampleRate;
		options.channels = split ? 1 : source.channels;
		std::vector<std::unique_ptr<AudioFileWriter>> writers;
		writers.reserve(outputs.size());
		for (const std::string& output : outputs)
			writers.push_back(AudioFileWriter::create(output, options));

		constexpr size_t piece = 8192;
		Resampler resampler(source.sampleRate, sampleRate, source.channels, piece);
		std::vector<float> in(piece * channels);
		std::vector<float> out;
		std::vector<float> mono;
		AudioFileInfo written = source;
		written.format = AudioFormat::Wav;
		written.sampleFormat = SampleFormat::Float32;
		written.sampleRate = sampleRate;
		written.channels = options.channels;
		written.frames = 0;
		written.broadcast.reset();
		const auto flush = [&]
		{
			const size_t frames = out.size() / channels;
			if (!split)
				writers.front()->write(out.data(), frames);
			for (size_t channel = 0; split && channel < channels; ++channel)
			{
				mono.resize(frames);
				for (size_t frame = 0; frame < frames; ++frame)
					mono[frame] = out[(frame * channels) + channel];
				writers[channel]->write(mono.data(), frames);
			}
			written.frames += static_cast<time::Samples>(frames);
			out.clear();
		};
		while (const size_t frames = reader->read(in.data(), piece))
		{
			resampler.process(in.data(), frames, out);
			flush();
		}
		resampler.finish(out);
		flush();
		for (const auto& writer : writers)
			writer->close();
		return written;
	}
}
