// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/time/tempomap.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

// Reading and writing audio files. Opening, reading and writing do file I/O, so none of this runs on audio threads;
// the disk threads stream through it. Paths are UTF-8 on every platform.
namespace gnubase::engine
{
	enum class AudioFormat : uint8_t
	{
		Wav, // also RF64 and Wave64
		Aiff,
		Flac,
		OggVorbis,
		Mp3,
	};

	// How samples are stored. Lossy formats decode to Float32.
	enum class SampleFormat : uint8_t
	{
		Int16,
		Int24,
		Int32,
		Float32,
		Float64, // WAV only
	};

	// The bits a sample takes in the format.
	constexpr int bitsOf(SampleFormat format)
	{
		switch (format)
		{
		case SampleFormat::Int16:
			return 16;
		case SampleFormat::Int24:
			return 24;
		case SampleFormat::Float64:
			return 64;
		default:
			return 32;
		}
	}

	// The Broadcast Wave description chunk ("bext", EBU Tech 3285).
	struct BroadcastInfo
	{
		std::string description;         // up to 256 characters
		std::string originator;          // up to 32 characters
		std::string originatorReference; // up to 32 characters
		std::string originationDate;     // yyyy-mm-dd
		std::string originationTime;     // hh:mm:ss
		uint64_t timeReference = 0;      // the first sample's position, in samples since midnight
	};

	// The text that Wave and AIFF files carry (reference manual p. 1615): in a Wave file's LIST INFO chunk as ICMT,
	// IART and ISRC, in an AIFF file as its NAME, AUTH and ANNO chunks. Empty strings are left out.
	struct FileText
	{
		std::string description;
		std::string author;
		std::string reference;
	};

	struct AudioFileInfo
	{
		AudioFormat format = AudioFormat::Wav;
		SampleFormat sampleFormat = SampleFormat::Float32;
		double sampleRate = 48000.0;
		int channels = 2;
		time::Samples frames = 0;
		std::optional<BroadcastInfo> broadcast; // WAV files with a bext chunk
		uint32_t channelMask = 0; // WAVEFORMATEXTENSIBLE speaker bits, in ascending bit order; 0 when unspecified
		// Speaker bits in source order, e.g. Vorbis; a zero entry is unassigned. Absent when no layout is known.
		std::optional<std::array<uint32_t, 8>> channelSpeakers;
	};

	class AudioFileError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// A write that would make the file larger than its format can address: AIFF and, with Rf64::Never, WAV at 4 GB.
	// The file stays as it was before the write.
	class AudioFileFull : public AudioFileError
	{
	public:
		using AudioFileError::AudioFileError;
	};

	namespace detail
	{
		// For tests: the size in bytes that AIFF and WAV without RF64 may reach instead of 4 GB, or 0 for 4 GB.
		void setFileSizeLimit(uint64_t bytes);
	}

	// Reads an audio file as 32-bit float sample frames with the channels interleaved.
	class AudioFileReader
	{
	public:
		// Recognizes WAV, RF64, Wave64, AIFF, FLAC, Ogg Vorbis and MP3 by their content, whatever the file is called.
		// Throws AudioFileError.
		static std::unique_ptr<AudioFileReader> open(const std::string& path);

		virtual ~AudioFileReader() = default;
		AudioFileReader(const AudioFileReader&) = delete;
		AudioFileReader& operator=(const AudioFileReader&) = delete;

		const AudioFileInfo& info() const { return m_info; }

		// Reads up to frames sample frames into buffer (room for frames * channels floats) and returns how many it
		// read: fewer only at the end of the file.
		virtual size_t read(float* buffer, size_t frames) = 0;

		// Reading continues at frame, from 0 up to the number of frames. Throws AudioFileError.
		void seek(time::Samples frame);

	protected:
		AudioFileReader() = default;
		virtual bool seekTo(time::Samples frame) = 0;

		AudioFileInfo m_info;
	};

	// WAV: RF64 when the file outgrows the 4 GB that plain WAV can address, always, or never: then a write that would
	// pass 4 GB throws AudioFileFull.
	enum class Rf64 : uint8_t
	{
		Auto,
		Always,
		Never,
	};

	struct WriteOptions
	{
		AudioFormat format = AudioFormat::Wav;
		SampleFormat sampleFormat = SampleFormat::Int24;
		double sampleRate = 48000.0;
		int channels = 2;
		Rf64 rf64 = Rf64::Auto;                 // WAV only
		std::optional<BroadcastInfo> broadcast; // WAV only
		std::optional<FileText> text;           // WAV and AIFF
	};

	// Writes an audio file from 32-bit float sample frames with the channels interleaved. Integer formats round and
	// clip to their range.
	class AudioFileWriter
	{
	public:
		// WAV in every sample format, AIFF in the integer ones, FLAC in 16 and 24 bits. Throws AudioFileError.
		static std::unique_ptr<AudioFileWriter> create(const std::string& path, const WriteOptions& options);

		// Closes the file if close wasn't called, ignoring errors.
		virtual ~AudioFileWriter() = default;
		AudioFileWriter(const AudioFileWriter&) = delete;
		AudioFileWriter& operator=(const AudioFileWriter&) = delete;

		// Throws AudioFileError.
		virtual void write(const float* buffer, size_t frames) = 0;

		// Completes the file. Throws AudioFileError.
		virtual void close() = 0;

	protected:
		AudioFileWriter() = default;
	};
}
