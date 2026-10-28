// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audiofile/audiofile.h"
#include "platform/rt/mpmcqueue.h"
#include "platform/rt/spscqueue.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gnubase::engine
{
	class Resampler;

	struct DiskStreamSettings
	{
		size_t blockFrames = 4096; // what the disk thread reads at a time
		size_t blocks = 32;        // the buffer holds blocks * blockFrames frames: about 2.7 s at 48 kHz
	};

#ifdef _MSC_VER
// The queues keep their producer and consumer data on separate cache lines; the padding that adds here is intended.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
	// Plays an audio file to the audio thread from a buffer that the disk thread keeps filled ahead of it, converted
	// to the output sample rate. Blocks of frames, allocated once, pass between the two threads through lock-free
	// queues, so read() never waits, allocates or touches the file.
	//
	// A seek starts a new generation. The disk thread takes the older generation's blocks back and reads from the
	// new position; the audio thread ignores blocks of older generations. With a different sample rate, reading
	// starts early enough before the position, and on the same grid of frames as from the start, for the output
	// after a seek to be the same as when playing through.
	class DiskStream
	{
	public:
		DiskStream(std::unique_ptr<AudioFileReader> reader, double sampleRate, const DiskStreamSettings& settings);
		~DiskStream();
		DiskStream(const DiskStream&) = delete;
		DiskStream& operator=(const DiskStream&) = delete;

		int channels() const { return m_channels; }
		double sampleRate() const { return m_sampleRate; } // the output sample rate
		time::Samples frames() const { return m_frames; }  // the length at the output sample rate

		// Audio thread: fills buffer (frames * channels floats) with the next frames and returns how many came from
		// the file. The rest is silence: after the end, or where the disk thread was late, which counts as dropouts.
		// Later frames stay at their time either way.
		size_t read(float* buffer, size_t frames);

		// Frames the audio thread had to replace with silence because the disk thread was late.
		uint64_t dropouts() const { return m_dropouts.load(std::memory_order_relaxed); }

		// Any thread but the audio thread: playback continues at frame.
		void seek(time::Samples frame);

		// True once the disk thread has filled the buffer since the last seek, or read up to the end.
		bool ready() const;

		// True while the buffer holds everything the disk thread can read ahead: it is ready and has refilled every
		// block the audio thread gave back since. Offline rendering waits for this, so it never drops out.
		bool full() const;

		// True if reading the file failed; the stream then plays silence.
		bool failed() const { return m_failed.load(std::memory_order_relaxed); }

	private:
		friend class DiskStreamer;

		struct Block
		{
			uint64_t generation = 0;
			time::Samples start = 0;
			size_t frames = 0;
			bool last = false; // the file ends with this block
			std::vector<float> samples;
		};

		// Disk thread: fills one block. False when there was nothing to do.
		bool fill();
		void reposition(time::Samples frame);
		// Disk thread: the next frames at the output sample rate; fewer only at the end.
		size_t decode(float* destination, size_t frames);
		// Audio thread: takes the next block of the current generation, dropping older ones.
		bool nextBlock();
		// Audio thread: hands a block back to the disk thread.
		void giveBack(Block* block);

		std::unique_ptr<AudioFileReader> m_reader;
		double m_sampleRate;
		int m_channels;
		time::Samples m_frames;
		DiskStreamSettings m_settings;
		std::vector<Block> m_blocks;
		rt::MpmcQueue<Block*> m_filled; // from the disk thread to the audio thread, which both take from it
		rt::SpscQueue<Block*> m_empty;  // from the audio thread back to the disk thread

		std::atomic<uint64_t> m_generation{1};
		std::atomic<time::Samples> m_seekTarget{0}; // stored before the generation changes
		std::atomic<uint64_t> m_readyGeneration{0};
		std::atomic<uint64_t> m_endGeneration{0}; // the last generation the disk thread read to the end
		std::atomic<size_t> m_returned{0};        // blocks in m_empty
		std::atomic<uint64_t> m_dropouts{0};
		std::atomic<bool> m_failed{false};

		// Audio thread only.
		Block* m_current = nullptr;
		size_t m_offset = 0; // frames of m_current already read
		uint64_t m_readGeneration = 0;
		time::Samples m_next = 0; // the frame the next read starts at, once m_located
		bool m_located = false;   // m_next is known: the generation's first block arrived
		bool m_ended = false;

		// Disk thread only.
		std::vector<Block*> m_spare; // blocks taken back after a seek
		uint64_t m_fillGeneration = 0;
		time::Samples m_fillPosition = 0;
		bool m_fillEnded = false;
		std::unique_ptr<Resampler> m_resampler; // null at equal sample rates
		std::vector<float> m_source;            // frames read from the file
		std::vector<float> m_decoded;           // resampled frames not yet in a block
		size_t m_decodedOffset = 0;
		time::Samples m_discard = 0; // resampled frames before the seek position, still to drop
		bool m_sourceEnded = false;
	};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

	// The disk thread: keeps every open stream's buffer filled, and sleeps briefly when all are full.
	class DiskStreamer
	{
	public:
		explicit DiskStreamer(std::chrono::microseconds idle = std::chrono::microseconds(1000));
		~DiskStreamer();
		DiskStreamer(const DiskStreamer&) = delete;
		DiskStreamer& operator=(const DiskStreamer&) = delete;

		// Streams reader's file at sampleRate. Not on audio threads: it allocates the buffer.
		std::shared_ptr<DiskStream> open(
			std::unique_ptr<AudioFileReader> reader, double sampleRate, const DiskStreamSettings& settings = {});

		// Stops filling the stream; it lives on while someone holds it.
		void close(const std::shared_ptr<DiskStream>& stream);

	private:
		void run();

		std::mutex m_mutex; // guards m_streams
		std::vector<std::shared_ptr<DiskStream>> m_streams;
		std::atomic<bool> m_stopping{false};
		std::chrono::microseconds m_idle;
		std::thread m_thread;
	};
}
