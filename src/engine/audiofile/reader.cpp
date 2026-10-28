// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/file.h"

#include <dr_flac.h>
#include <dr_mp3.h>
#include <dr_wav.h>
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c> // NOLINT(bugprone-suspicious-include): stb_vorbis is one .c file; only its declarations

#include <array>
#include <cstring>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		// File callbacks for the dr_libs decoders. Their seek origins are 0, 1 and 2 like SEEK_SET, SEEK_CUR and
		// SEEK_END, but each library has its own enum type.
		std::FILE* fileOf(void* userData)
		{
			return static_cast<std::FILE*>(userData);
		}

		size_t readFile(void* userData, void* buffer, size_t bytes)
		{
			return std::fread(buffer, 1, bytes, fileOf(userData));
		}

		int whence(int origin)
		{
			if (origin == 0)
				return SEEK_SET;
			return origin == 1 ? SEEK_CUR : SEEK_END;
		}

		template <typename Origin> uint32_t seekFile(void* userData, int offset, Origin origin)
		{
			return detail::seekFile(fileOf(userData), offset, whence(static_cast<int>(origin))) ? 1 : 0;
		}

		template <typename Position> uint32_t tellFile(void* userData, Position* position)
		{
			*position = detail::tellFile(fileOf(userData));
			return *position >= 0 ? 1 : 0;
		}

		SampleFormat integerFormat(int bits)
		{
			if (bits <= 16)
				return SampleFormat::Int16;
			return bits <= 24 ? SampleFormat::Int24 : SampleFormat::Int32;
		}

		// A fixed-size bext text field, which need not end with a null character.
		std::string field(const char* text, size_t size)
		{
			return {text, strnlen(text, size)};
		}

		std::string text(const char* value)
		{
			return value != nullptr ? std::string(value) : std::string();
		}

		class WavReader final : public AudioFileReader
		{
		public:
			WavReader(detail::File file, const std::string& path) : m_file(std::move(file))
			{
				if (drwav_init_with_metadata(&m_wav, readFile, seekFile<drwav_seek_origin>, tellFile<drwav_int64>,
						m_file.get(), 0, nullptr) == DRWAV_FALSE)
					throw AudioFileError("can't read " + path + ": not a valid WAV or AIFF file");
				m_open = true;
				m_info.format = m_wav.container == drwav_container_aiff ? AudioFormat::Aiff : AudioFormat::Wav;
				m_info.sampleFormat = m_wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT
					? SampleFormat::Float32
					: integerFormat(m_wav.bitsPerSample);
				m_info.sampleRate = m_wav.sampleRate;
				m_info.channels = m_wav.channels;
				m_info.frames = static_cast<time::Samples>(m_wav.totalPCMFrameCount);
				for (drwav_uint32 i = 0; i < m_wav.metadataCount; ++i)
					if (m_wav.pMetadata[i].type == drwav_metadata_type_bext)
					{
						const drwav_bext& bext = m_wav.pMetadata[i].data.bext;
						m_info.broadcast = BroadcastInfo{text(bext.pDescription), text(bext.pOriginatorName),
							text(bext.pOriginatorReference), field(bext.pOriginationDate, sizeof bext.pOriginationDate),
							field(bext.pOriginationTime, sizeof bext.pOriginationTime), bext.timeReference};
					}
			}

			~WavReader() override
			{
				if (m_open)
					drwav_uninit(&m_wav);
			}

			WavReader(const WavReader&) = delete;
			WavReader& operator=(const WavReader&) = delete;

			size_t read(float* buffer, size_t frames) override
			{
				return static_cast<size_t>(drwav_read_pcm_frames_f32(&m_wav, frames, buffer));
			}

		protected:
			bool seekTo(time::Samples frame) override
			{
				return drwav_seek_to_pcm_frame(&m_wav, static_cast<drwav_uint64>(frame)) == DRWAV_TRUE;
			}

		private:
			detail::File m_file;
			drwav m_wav{};
			bool m_open = false;
		};

		class FlacReader final : public AudioFileReader
		{
		public:
			FlacReader(detail::File file, const std::string& path) : m_file(std::move(file))
			{
				m_flac =
					drflac_open(readFile, seekFile<drflac_seek_origin>, tellFile<drflac_int64>, m_file.get(), nullptr);
				if (m_flac == nullptr)
					throw AudioFileError("can't read " + path + ": not a valid FLAC file");
				m_info.format = AudioFormat::Flac;
				m_info.sampleFormat = integerFormat(m_flac->bitsPerSample);
				m_info.sampleRate = m_flac->sampleRate;
				m_info.channels = m_flac->channels;
				m_info.frames = static_cast<time::Samples>(m_flac->totalPCMFrameCount);
			}

			~FlacReader() override { drflac_close(m_flac); }

			FlacReader(const FlacReader&) = delete;
			FlacReader& operator=(const FlacReader&) = delete;

			size_t read(float* buffer, size_t frames) override
			{
				return static_cast<size_t>(drflac_read_pcm_frames_f32(m_flac, frames, buffer));
			}

		protected:
			bool seekTo(time::Samples frame) override
			{
				return drflac_seek_to_pcm_frame(m_flac, static_cast<drflac_uint64>(frame)) == DRFLAC_TRUE;
			}

		private:
			detail::File m_file;
			drflac* m_flac = nullptr;
		};

		class Mp3Reader final : public AudioFileReader
		{
		public:
			Mp3Reader(detail::File file, const std::string& path) : m_file(std::move(file))
			{
				if (drmp3_init(&m_mp3, readFile, seekFile<drmp3_seek_origin>, tellFile<drmp3_int64>, nullptr,
						m_file.get(), nullptr) == DRMP3_FALSE)
					throw AudioFileError("can't read " + path + ": not a valid MP3 file");
				m_open = true;
				m_info.format = AudioFormat::Mp3;
				m_info.sampleRate = m_mp3.sampleRate;
				m_info.channels = static_cast<int>(m_mp3.channels);
				// Counting decodes the whole file once; then reading starts again from the beginning.
				m_info.frames = static_cast<time::Samples>(drmp3_get_pcm_frame_count(&m_mp3));
				if (drmp3_seek_to_pcm_frame(&m_mp3, 0) == DRMP3_FALSE)
					throw AudioFileError("can't read " + path + ": not a valid MP3 file");
			}

			~Mp3Reader() override
			{
				if (m_open)
					drmp3_uninit(&m_mp3);
			}

			Mp3Reader(const Mp3Reader&) = delete;
			Mp3Reader& operator=(const Mp3Reader&) = delete;

			size_t read(float* buffer, size_t frames) override
			{
				return static_cast<size_t>(drmp3_read_pcm_frames_f32(&m_mp3, frames, buffer));
			}

		protected:
			bool seekTo(time::Samples frame) override
			{
				return drmp3_seek_to_pcm_frame(&m_mp3, static_cast<drmp3_uint64>(frame)) == DRMP3_TRUE;
			}

		private:
			detail::File m_file;
			drmp3 m_mp3{};
			bool m_open = false;
		};

		class VorbisReader final : public AudioFileReader
		{
		public:
			VorbisReader(detail::File file, const std::string& path) : m_file(std::move(file))
			{
				int error = 0;
				m_vorbis = stb_vorbis_open_file(m_file.get(), 0, &error, nullptr);
				if (m_vorbis == nullptr)
					throw AudioFileError("can't read " + path + ": not a valid Ogg Vorbis file");
				const stb_vorbis_info info = stb_vorbis_get_info(m_vorbis);
				m_info.format = AudioFormat::OggVorbis;
				m_info.sampleRate = info.sample_rate;
				m_info.channels = info.channels;
				m_info.frames = stb_vorbis_stream_length_in_samples(m_vorbis);
			}

			~VorbisReader() override { stb_vorbis_close(m_vorbis); }

			VorbisReader(const VorbisReader&) = delete;
			VorbisReader& operator=(const VorbisReader&) = delete;

			size_t read(float* buffer, size_t frames) override
			{
				size_t done = 0;
				const auto channels = static_cast<size_t>(m_info.channels);
				while (!m_atEnd && done < frames)
				{
					const int got = stb_vorbis_get_samples_float_interleaved(m_vorbis, m_info.channels,
						buffer + (done * channels), static_cast<int>((frames - done) * channels));
					if (got <= 0)
						break;
					done += static_cast<size_t>(got);
				}
				return done;
			}

		protected:
			bool seekTo(time::Samples frame) override
			{
				// stb_vorbis can't seek to the very end, so that position is kept here.
				m_atEnd = frame == m_info.frames;
				return m_atEnd || stb_vorbis_seek(m_vorbis, static_cast<unsigned int>(frame)) != 0;
			}

		private:
			detail::File m_file;
			stb_vorbis* m_vorbis = nullptr;
			bool m_atEnd = false;
		};
	}

	std::unique_ptr<AudioFileReader> AudioFileReader::open(const std::string& path)
	{
		detail::File file = detail::openFile(path, "rb");
		if (!file)
			throw AudioFileError("can't open " + path + ": " + detail::lastError());
		std::array<unsigned char, 4> magic{};
		const size_t size = std::fread(magic.data(), 1, magic.size(), file.get());
		if (!detail::seekFile(file.get(), 0, SEEK_SET))
			throw AudioFileError("can't read " + path + ": " + detail::lastError());
		const auto is = [&magic, size](const char* id) { return size == 4 && std::memcmp(magic.data(), id, 4) == 0; };

		if (is("RIFF") || is("RF64") || is("BW64") || is("riff") || is("FORM"))
			return std::make_unique<WavReader>(std::move(file), path);
		if (is("fLaC"))
			return std::make_unique<FlacReader>(std::move(file), path);
		if (is("OggS"))
			return std::make_unique<VorbisReader>(std::move(file), path);
		// MP3: an ID3 tag, or straight away an MPEG audio frame header, which starts with 11 set bits.
		if ((size >= 3 && std::memcmp(magic.data(), "ID3", 3) == 0) ||
			(size >= 2 && magic[0] == 0xff && (magic[1] & 0xe0) == 0xe0))
			return std::make_unique<Mp3Reader>(std::move(file), path);
		throw AudioFileError("can't read " + path + ": not a supported audio file");
	}

	void AudioFileReader::seek(time::Samples frame)
	{
		if (frame < 0 || frame > m_info.frames)
			throw AudioFileError(
				"can't seek to frame " + std::to_string(frame) + " of " + std::to_string(m_info.frames));
		if (!seekTo(frame))
			throw AudioFileError("seeking to frame " + std::to_string(frame) + " failed");
	}
}
