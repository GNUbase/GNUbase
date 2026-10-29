// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/diskstream.h"

#include "engine/audiofile/resampler.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace gnubase::engine
{
	DiskStream::DiskStream(
		std::unique_ptr<AudioFileReader> reader, double sampleRate, const DiskStreamSettings& settings)
		: m_reader(std::move(reader)), m_sampleRate(sampleRate), m_channels(m_reader->info().channels),
		  m_frames(Resampler::outputLength(m_reader->info().frames, m_reader->info().sampleRate, sampleRate)),
		  m_settings(settings), m_blocks(settings.blocks), m_filled(settings.blocks), m_empty(settings.blocks),
		  m_source(settings.blockFrames * static_cast<size_t>(m_channels))
	{
		for (Block& block : m_blocks)
		{
			block.samples.resize(settings.blockFrames * static_cast<size_t>(m_channels));
			m_spare.push_back(&block);
		}
		m_spareCount.store(m_spare.size(), std::memory_order_release);
		if (m_reader->info().sampleRate != sampleRate)
			m_resampler =
				std::make_unique<Resampler>(m_reader->info().sampleRate, sampleRate, m_channels, settings.blockFrames);
	}

	DiskStream::~DiskStream() = default;

	void DiskStream::seek(time::Samples frame)
	{
		m_seekTarget.store(std::clamp<time::Samples>(frame, 0, m_frames), std::memory_order_release);
		m_generation.fetch_add(1, std::memory_order_acq_rel);
	}

	bool DiskStream::ready() const
	{
		return m_readyGeneration.load(std::memory_order_acquire) == m_generation.load(std::memory_order_acquire);
	}

	bool DiskStream::full() const
	{
		const uint64_t generation = m_generation.load(std::memory_order_acquire);
		return m_readyGeneration.load(std::memory_order_acquire) == generation &&
			(m_returned.load(std::memory_order_acquire) == 0 ||
				m_endGeneration.load(std::memory_order_acquire) == generation);
	}

	double DiskStream::load() const
	{
		const uint64_t generation = m_generation.load(std::memory_order_acquire);
		if (m_endGeneration.load(std::memory_order_acquire) == generation)
			return 0.0;
		// Until the disk thread takes a seek's old blocks back, they all wait to be filled again.
		if (m_takenGeneration.load(std::memory_order_acquire) != generation)
			return 1.0;
		const size_t empty = m_returned.load(std::memory_order_acquire) + m_spareCount.load(std::memory_order_acquire);
		return std::min(1.0, static_cast<double>(empty) / static_cast<double>(std::max<size_t>(m_blocks.size(), 1)));
	}

	void DiskStream::giveBack(Block* block)
	{
		if (m_empty.tryPush(block))
			m_returned.fetch_add(1, std::memory_order_release);
	}

	size_t DiskStream::read(float* buffer, size_t frames)
	{
		const auto channels = static_cast<size_t>(m_channels);
		const uint64_t generation = m_generation.load(std::memory_order_acquire);
		if (generation != m_readGeneration)
		{
			if (m_current != nullptr)
				giveBack(m_current);
			m_current = nullptr;
			m_readGeneration = generation;
			m_located = false;
			m_ended = false;
		}

		size_t done = 0;
		while (done < frames && !m_ended && (m_current != nullptr || nextBlock()))
		{
			Block& block = *m_current;
			const size_t count = std::min(block.frames - m_offset, frames - done);
			std::copy_n(block.samples.data() + (m_offset * channels), count * channels, buffer + (done * channels));
			m_offset += count;
			done += count;
			m_next += static_cast<time::Samples>(count);
			if (m_offset == block.frames)
			{
				m_ended = block.last;
				giveBack(m_current);
				m_current = nullptr;
			}
		}
		std::fill(buffer + (done * channels), buffer + (frames * channels), 0.0f);

		// Missing frames before the end are dropouts. Skipping them keeps the later frames at their time.
		if (done < frames && m_located && !m_ended && m_next < m_frames)
		{
			const auto missing = std::min(static_cast<time::Samples>(frames - done), m_frames - m_next);
			m_dropouts.fetch_add(static_cast<uint64_t>(missing), std::memory_order_relaxed);
			m_next += missing;
		}
		return done;
	}

	bool DiskStream::nextBlock()
	{
		Block* block = nullptr;
		while (m_filled.tryPop(block))
		{
			// A seek after this read began: follow it rather than lose the new position's first block.
			if (block->generation > m_readGeneration)
			{
				m_readGeneration = block->generation;
				m_located = false;
			}
			if (block->generation < m_readGeneration)
			{
				giveBack(block);
				continue;
			}
			// A generation's first block starts where the seek went.
			if (!m_located)
			{
				m_next = block->start;
				m_located = true;
			}
			const time::Samples end = block->start + static_cast<time::Samples>(block->frames);
			if (end <= m_next) // passed during a dropout
			{
				m_ended = block->last;
				giveBack(block);
				if (m_ended)
					return false;
				continue;
			}
			m_current = block;
			m_offset = static_cast<size_t>(m_next - block->start);
			return true;
		}
		return false;
	}

	bool DiskStream::fill()
	{
		const uint64_t generation = m_generation.load(std::memory_order_acquire);
		if (generation != m_fillGeneration)
		{
			m_fillGeneration = generation;
			// Blocks the audio thread hasn't taken yet all belong to older generations: take them back.
			Block* block = nullptr;
			while (m_filled.tryPop(block))
				m_spare.push_back(block);
			m_spareCount.store(m_spare.size(), std::memory_order_release);
			m_takenGeneration.store(generation, std::memory_order_release);
			try
			{
				reposition(m_seekTarget.load(std::memory_order_acquire));
			}
			catch (const AudioFileError&)
			{
				m_failed.store(true, std::memory_order_relaxed);
				m_fillEnded = true;
			}
		}
		if (m_fillEnded)
		{
			m_endGeneration.store(generation, std::memory_order_release);
			m_readyGeneration.store(generation, std::memory_order_release);
			return false;
		}

		Block* block = nullptr;
		bool returned = false; // counted in m_returned or m_spareCount until it is filled again
		if (!m_spare.empty())
		{
			block = m_spare.back();
			m_spare.pop_back();
		}
		else if (m_empty.tryPop(block))
		{
			returned = true;
		}
		else
		{
			m_readyGeneration.store(generation, std::memory_order_release); // the buffer is full
			return false;
		}
		size_t frames = 0;
		try
		{
			frames = decode(block->samples.data(), m_settings.blockFrames);
		}
		catch (const AudioFileError&)
		{
			m_failed.store(true, std::memory_order_relaxed);
		}
		block->generation = generation;
		block->start = m_fillPosition;
		block->frames = frames;
		m_fillPosition += static_cast<time::Samples>(frames);
		block->last = failed() || frames < m_settings.blockFrames || m_fillPosition >= m_frames;
		m_fillEnded = block->last;
		m_filled.tryPush(block);
		if (returned)
			m_returned.fetch_sub(1, std::memory_order_acq_rel);
		else
			m_spareCount.store(m_spare.size(), std::memory_order_release);
		if (m_fillEnded)
		{
			m_endGeneration.store(generation, std::memory_order_release);
			m_readyGeneration.store(generation, std::memory_order_release);
		}
		return true;
	}

	void DiskStream::reposition(time::Samples frame)
	{
		m_fillPosition = frame;
		m_fillEnded = frame >= m_frames;
		m_decoded.clear();
		m_decodedOffset = 0;
		m_discard = 0;
		m_sourceEnded = false;
		if (!m_resampler)
		{
			m_reader->seek(frame);
			return;
		}

		// Read from early enough for the filters to have the history they would have when playing through, and
		// from a file frame whose time is also a whole output frame, so the output lands on the same grid.
		const double from = m_reader->info().sampleRate;
		int64_t stepIn = 1;
		int64_t stepOut = 1;
		const auto fromRate = std::llround(from);
		const auto toRate = std::llround(m_sampleRate);
		if (static_cast<double>(fromRate) == from && static_cast<double>(toRate) == m_sampleRate)
		{
			const int64_t divisor = std::gcd(fromRate, toRate);
			stepIn = fromRate / divisor;
			stepOut = toRate / divisor;
		}
		const auto sourceFrame = static_cast<int64_t>(std::floor(static_cast<double>(frame) * from / m_sampleRate));
		int64_t start = std::max<int64_t>(0, sourceFrame - (2 * static_cast<int64_t>(m_resampler->inputLatency())));
		start -= start % stepIn;
		const int64_t startOutput =
			stepIn == 1 && stepOut == 1 ? Resampler::outputLength(start, from, m_sampleRate) : start / stepIn * stepOut;
		m_discard = std::max<int64_t>(0, frame - startOutput);
		m_resampler->reset();
		m_reader->seek(start);
	}

	size_t DiskStream::decode(float* destination, size_t frames)
	{
		if (!m_resampler)
			return m_reader->read(destination, frames);
		const auto channels = static_cast<size_t>(m_channels);
		size_t done = 0;
		while (done < frames)
		{
			if (m_decodedOffset == m_decoded.size())
			{
				if (m_sourceEnded)
					break;
				m_decoded.clear();
				const size_t got = m_reader->read(m_source.data(), m_settings.blockFrames);
				if (got > 0)
					m_resampler->process(m_source.data(), got, m_decoded);
				if (got < m_settings.blockFrames)
				{
					m_resampler->finish(m_decoded);
					m_sourceEnded = true;
				}
				const auto drop = std::min(m_decoded.size() / channels, static_cast<size_t>(m_discard));
				m_decodedOffset = drop * channels;
				m_discard -= static_cast<time::Samples>(drop);
				continue;
			}
			const size_t count = std::min(frames - done, (m_decoded.size() - m_decodedOffset) / channels);
			std::copy_n(m_decoded.data() + m_decodedOffset, count * channels, destination + (done * channels));
			m_decodedOffset += count * channels;
			done += count;
		}
		return done;
	}

	DiskStreamer::DiskStreamer(std::chrono::microseconds idle) : m_idle(idle), m_thread([this] { run(); })
	{
	}

	DiskStreamer::~DiskStreamer()
	{
		m_stopping.store(true);
		m_thread.join();
	}

	std::shared_ptr<DiskStream> DiskStreamer::open(
		std::unique_ptr<AudioFileReader> reader, double sampleRate, const DiskStreamSettings& settings)
	{
		auto stream = std::make_shared<DiskStream>(std::move(reader), sampleRate, settings);
		const std::scoped_lock lock(m_mutex);
		m_streams.push_back(stream);
		return stream;
	}

	void DiskStreamer::close(const std::shared_ptr<DiskStream>& stream)
	{
		const std::scoped_lock lock(m_mutex);
		m_streams.erase(std::remove(m_streams.begin(), m_streams.end(), stream), m_streams.end());
	}

	void DiskStreamer::run()
	{
		std::vector<std::shared_ptr<DiskStream>> streams;
		while (!m_stopping.load())
		{
			{
				const std::scoped_lock lock(m_mutex);
				streams = m_streams;
			}
			bool worked = false;
			for (const auto& stream : streams)
				for (size_t i = 0; i < stream->m_settings.blocks && stream->fill(); ++i)
					worked = true;
			if (!worked)
				std::this_thread::sleep_for(m_idle);
		}
	}
}
