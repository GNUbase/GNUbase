// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/file.h"

#include <FLAC/stream_encoder.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr uint64_t g_max32 = 0xffffffff; // the largest size a 32-bit chunk size field holds
		constexpr uint64_t g_ds64Size = 28;      // RF64's ds64 chunk without a table
		constexpr uint64_t g_bextSize = 602;     // the bext chunk without coding history

		int bitsOf(SampleFormat format)
		{
			switch (format)
			{
			case SampleFormat::Int16:
				return 16;
			case SampleFormat::Int24:
				return 24;
			default:
				return 32;
			}
		}

		// Scales to the integer range like the decoders that read it back (dividing by 2^(bits-1)), rounds and clips.
		int32_t toInteger(float value, int bits)
		{
			const double scale = std::ldexp(1.0, bits - 1);
			const double rounded = std::round(static_cast<double>(value) * scale);
			return static_cast<int32_t>(std::clamp(rounded, -scale, scale - 1.0));
		}

		// Byte buffers in the file's byte order.
		class Bytes
		{
		public:
			explicit Bytes(bool bigEndian) : m_bigEndian(bigEndian) {}

			void text(const char* id) { m_data.insert(m_data.end(), id, id + std::strlen(id)); }

			// A text field of fixed size, padded with null characters.
			void field(const std::string& value, size_t size)
			{
				m_data.insert(m_data.end(), value.begin(), value.end());
				m_data.insert(m_data.end(), size - value.size(), 0);
			}

			void integer(uint64_t value, int bytes)
			{
				for (int i = 0; i < bytes; ++i)
				{
					const int shift = 8 * (m_bigEndian ? bytes - 1 - i : i);
					m_data.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
				}
			}

			void sample(float value, SampleFormat format)
			{
				if (format == SampleFormat::Float32)
				{
					uint32_t bits = 0;
					std::memcpy(&bits, &value, sizeof bits);
					integer(bits, 4);
					return;
				}
				const int bits = bitsOf(format);
				integer(static_cast<uint32_t>(toInteger(value, bits)), bits / 8);
			}

			// An 80-bit IEEE 754 extended number, as AIFF stores the sample rate.
			void extended(double value)
			{
				int exponent = 0;
				const double fraction =
					std::frexp(value, &exponent); // value = fraction * 2^exponent, 0.5 <= fraction < 1
				integer(static_cast<uint64_t>(16383 + exponent - 1), 2);
				integer(static_cast<uint64_t>(std::ldexp(fraction, 64)), 8);
			}

			const std::vector<uint8_t>& data() const { return m_data; }
			void clear() { m_data.clear(); }

		private:
			bool m_bigEndian;
			std::vector<uint8_t> m_data;
		};

		// Writes bytes at the current position or at a given one, and reports failures with the path.
		class Output
		{
		public:
			Output(detail::File file, std::string path) : m_file(std::move(file)), m_path(std::move(path)) {}

			void write(const std::vector<uint8_t>& bytes)
			{
				if (std::fwrite(bytes.data(), 1, bytes.size(), m_file.get()) != bytes.size())
					fail();
				m_size += bytes.size();
			}

			void writeAt(uint64_t position, const std::vector<uint8_t>& bytes)
			{
				if (!detail::seekFile(m_file.get(), static_cast<int64_t>(position), SEEK_SET))
					fail();
				if (std::fwrite(bytes.data(), 1, bytes.size(), m_file.get()) != bytes.size())
					fail();
			}

			void finish()
			{
				if (std::fflush(m_file.get()) != 0)
					fail();
				m_file.reset();
			}

			uint64_t size() const { return m_size; }

		private:
			[[noreturn]] void fail() const
			{
				throw AudioFileError("can't write " + m_path + ": " + detail::lastError());
			}

			detail::File m_file;
			std::string m_path;
			uint64_t m_size = 0;
		};

		detail::File createFile(const std::string& path)
		{
			detail::File file = detail::openFile(path, "wb");
			if (!file)
				throw AudioFileError("can't create " + path + ": " + detail::lastError());
			return file;
		}

		// WAV with the room an RF64 header needs reserved as a JUNK chunk, as EBU Tech 3306 describes, so a file
		// that outgrows 4 GB becomes RF64 when it is closed. With Rf64::Always it is RF64 from the start.
		class WavWriter final : public AudioFileWriter
		{
		public:
			WavWriter(const std::string& path, const WriteOptions& options)
				: m_options(options), m_output(createFile(path), path)
			{
				Bytes header(false);
				header.text(options.rf64 == Rf64::Always ? "RF64" : "RIFF");
				header.integer(0, 4); // completed when closing
				header.text("WAVE");
				header.text(options.rf64 == Rf64::Always ? "ds64" : "JUNK");
				header.integer(g_ds64Size, 4);
				m_ds64 = header.data().size();
				header.field({}, g_ds64Size);
				if (options.broadcast)
					broadcastChunk(header, *options.broadcast);
				formatChunk(header);
				if (options.sampleFormat == SampleFormat::Float32)
				{
					header.text("fact");
					header.integer(4, 4);
					m_fact = header.data().size();
					header.integer(0, 4);
				}
				header.text("data");
				m_dataSize = header.data().size();
				header.integer(0, 4);
				m_output.write(header.data());
			}

			~WavWriter() override
			{
				try
				{
					close();
				}
				catch (const AudioFileError&) // NOLINT(bugprone-empty-catch): a destructor can't report it
				{
				}
			}

			WavWriter(const WavWriter&) = delete;
			WavWriter& operator=(const WavWriter&) = delete;

			void write(const float* buffer, size_t frames) override
			{
				const size_t samples = frames * static_cast<size_t>(m_options.channels);
				m_bytes.clear();
				for (size_t i = 0; i < samples; ++i)
					m_bytes.sample(buffer[i], m_options.sampleFormat);
				m_output.write(m_bytes.data());
				m_frames += frames;
			}

			void close() override
			{
				if (m_closed)
					return;
				m_closed = true;
				const uint64_t dataBytes = m_output.size() - m_dataSize - 4;
				if (dataBytes % 2 != 0)
					m_output.write({0}); // chunks are padded to an even size
				const uint64_t riffSize = m_output.size() - 8;
				const bool rf64 = m_options.rf64 == Rf64::Always || riffSize > g_max32;

				Bytes bytes(false);
				bytes.text(rf64 ? "RF64" : "RIFF");
				bytes.integer(rf64 ? g_max32 : riffSize, 4);
				m_output.writeAt(0, bytes.data());
				if (rf64)
				{
					bytes.clear();
					bytes.text("ds64");
					bytes.integer(g_ds64Size, 4);
					bytes.integer(riffSize, 8);
					bytes.integer(dataBytes, 8);
					bytes.integer(m_frames, 8);
					bytes.integer(0, 4); // no table of other chunk sizes
					m_output.writeAt(m_ds64 - 8, bytes.data());
				}
				if (m_fact != 0)
				{
					bytes.clear();
					bytes.integer(std::min<uint64_t>(m_frames, g_max32), 4);
					m_output.writeAt(m_fact, bytes.data());
				}
				bytes.clear();
				bytes.integer(rf64 ? g_max32 : dataBytes, 4);
				m_output.writeAt(m_dataSize, bytes.data());
				m_output.finish();
			}

		private:
			void formatChunk(Bytes& header) const
			{
				const bool isFloat = m_options.sampleFormat == SampleFormat::Float32;
				const int bits = bitsOf(m_options.sampleFormat);
				const auto channels = static_cast<uint64_t>(m_options.channels);
				const auto rate = static_cast<uint64_t>(std::lround(m_options.sampleRate));
				const uint64_t blockAlign = channels * static_cast<uint64_t>(bits / 8);
				// More than two channels need WAVE_FORMAT_EXTENSIBLE.
				const bool extensible = m_options.channels > 2;
				header.text("fmt ");
				header.integer(extensible ? 40 : 16, 4);
				uint64_t formatTag = isFloat ? 3 : 1; // WAVE_FORMAT_IEEE_FLOAT or WAVE_FORMAT_PCM
				if (extensible)
					formatTag = 0xfffe; // WAVE_FORMAT_EXTENSIBLE
				header.integer(formatTag, 2);
				header.integer(channels, 2);
				header.integer(rate, 4);
				header.integer(rate * blockAlign, 4);
				header.integer(blockAlign, 2);
				header.integer(static_cast<uint64_t>(bits), 2);
				if (extensible)
				{
					header.integer(22, 2);                          // size of the extension
					header.integer(static_cast<uint64_t>(bits), 2); // valid bits per sample
					header.integer(0, 4);                           // speaker positions not assigned
					// KSDATAFORMAT_SUBTYPE_PCM or _IEEE_FLOAT: {0000000x-0000-0010-8000-00aa00389b71}
					header.integer(isFloat ? 3 : 1, 4);
					const std::array<uint64_t, 12> guid{
						0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
					for (const uint64_t byte : guid)
						header.integer(byte, 1);
				}
			}

			static void broadcastChunk(Bytes& header, const BroadcastInfo& info)
			{
				header.text("bext");
				header.integer(g_bextSize, 4);
				header.field(info.description, 256);
				header.field(info.originator, 32);
				header.field(info.originatorReference, 32);
				header.field(info.originationDate, 10);
				header.field(info.originationTime, 8);
				header.integer(info.timeReference & 0xffffffff, 4);
				header.integer(info.timeReference >> 32, 4);
				header.integer(1, 2);       // version 1: with the UMID field, without loudness values
				header.field({}, 64 + 190); // UMID, then the loudness fields of version 2 and the reserved bytes
			}

			WriteOptions m_options;
			Output m_output;
			Bytes m_bytes{false};
			size_t m_ds64 = 0;     // where the ds64 or JUNK chunk's content starts
			size_t m_fact = 0;     // where the fact chunk's frame count is, or 0
			size_t m_dataSize = 0; // where the data chunk's size is
			uint64_t m_frames = 0;
			bool m_closed = false;
		};

		class AiffWriter final : public AudioFileWriter
		{
		public:
			AiffWriter(const std::string& path, const WriteOptions& options)
				: m_options(options), m_output(createFile(path), path)
			{
				Bytes header(true);
				header.text("FORM");
				header.integer(0, 4); // completed when closing
				header.text("AIFF");
				header.text("COMM");
				header.integer(18, 4);
				header.integer(static_cast<uint64_t>(options.channels), 2);
				m_frameCount = header.data().size();
				header.integer(0, 4);
				header.integer(static_cast<uint64_t>(bitsOf(options.sampleFormat)), 2);
				header.extended(options.sampleRate);
				header.text("SSND");
				m_soundSize = header.data().size();
				header.integer(0, 4);
				header.integer(0, 4); // offset
				header.integer(0, 4); // block size
				m_output.write(header.data());
			}

			~AiffWriter() override
			{
				try
				{
					close();
				}
				catch (const AudioFileError&) // NOLINT(bugprone-empty-catch): a destructor can't report it
				{
				}
			}

			AiffWriter(const AiffWriter&) = delete;
			AiffWriter& operator=(const AiffWriter&) = delete;

			void write(const float* buffer, size_t frames) override
			{
				const size_t samples = frames * static_cast<size_t>(m_options.channels);
				m_bytes.clear();
				for (size_t i = 0; i < samples; ++i)
					m_bytes.sample(buffer[i], m_options.sampleFormat);
				if (m_output.size() + m_bytes.data().size() > g_max32)
					throw AudioFileError("AIFF files can't be larger than 4 GB");
				m_output.write(m_bytes.data());
				m_frames += frames;
			}

			void close() override
			{
				if (m_closed)
					return;
				m_closed = true;
				const uint64_t soundBytes = m_output.size() - m_soundSize - 4;
				if (soundBytes % 2 != 0)
					m_output.write({0});
				Bytes bytes(true);
				bytes.integer(m_output.size() - 8, 4);
				m_output.writeAt(4, bytes.data());
				bytes.clear();
				bytes.integer(m_frames, 4);
				m_output.writeAt(m_frameCount, bytes.data());
				bytes.clear();
				bytes.integer(soundBytes, 4);
				m_output.writeAt(m_soundSize, bytes.data());
				m_output.finish();
			}

		private:
			WriteOptions m_options;
			Output m_output;
			Bytes m_bytes{true};
			size_t m_frameCount = 0; // where COMM's frame count is
			size_t m_soundSize = 0;  // where the SSND chunk's size is
			uint64_t m_frames = 0;
			bool m_closed = false;
		};

		class FlacWriter final : public AudioFileWriter
		{
		public:
			FlacWriter(const std::string& path, const WriteOptions& options)
				: m_options(options), m_path(path), m_bits(bitsOf(options.sampleFormat)),
				  m_encoder(FLAC__stream_encoder_new())
			{
				if (m_encoder == nullptr)
					throw AudioFileError("can't create a FLAC encoder");
				FLAC__stream_encoder_set_channels(m_encoder, static_cast<uint32_t>(options.channels));
				FLAC__stream_encoder_set_bits_per_sample(m_encoder, static_cast<uint32_t>(m_bits));
				FLAC__stream_encoder_set_sample_rate(m_encoder, static_cast<uint32_t>(std::lround(options.sampleRate)));
				FLAC__stream_encoder_set_compression_level(m_encoder, 5);
				// From here on the encoder owns the file and closes it when it finishes.
				std::FILE* file = createFile(path).release();
				if (FLAC__stream_encoder_init_FILE(m_encoder, file, nullptr, nullptr) !=
					FLAC__STREAM_ENCODER_INIT_STATUS_OK)
				{
					std::fclose(file);
					FLAC__stream_encoder_delete(m_encoder);
					throw AudioFileError("can't write " + path + ": the FLAC encoder rejected the settings");
				}
			}

			~FlacWriter() override
			{
				try
				{
					close();
				}
				catch (const AudioFileError&) // NOLINT(bugprone-empty-catch): a destructor can't report it
				{
				}
				FLAC__stream_encoder_delete(m_encoder);
			}

			FlacWriter(const FlacWriter&) = delete;
			FlacWriter& operator=(const FlacWriter&) = delete;

			void write(const float* buffer, size_t frames) override
			{
				const size_t samples = frames * static_cast<size_t>(m_options.channels);
				m_samples.resize(samples);
				for (size_t i = 0; i < samples; ++i)
					m_samples[i] = toInteger(buffer[i], m_bits);
				if (FLAC__stream_encoder_process_interleaved(
						m_encoder, m_samples.data(), static_cast<uint32_t>(frames)) == 0)
					throw AudioFileError("can't write " + m_path + ": " +
						FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(m_encoder)]);
			}

			void close() override
			{
				if (m_closed)
					return;
				m_closed = true;
				if (FLAC__stream_encoder_finish(m_encoder) == 0)
					throw AudioFileError("can't complete " + m_path);
			}

		private:
			WriteOptions m_options;
			std::string m_path;
			int m_bits;
			FLAC__StreamEncoder* m_encoder;
			std::vector<FLAC__int32> m_samples;
			bool m_closed = false;
		};

		void check(const WriteOptions& options)
		{
			if (options.channels < 1 || options.channels > 64)
				throw AudioFileError("channels must be 1 to 64");
			if (!(options.sampleRate > 0.0))
				throw AudioFileError("the sample rate must be positive");
			const auto rejected = [](const char* what) { throw AudioFileError(std::string("can't write ") + what); };
			switch (options.format)
			{
			case AudioFormat::Wav:
				break;
			case AudioFormat::Aiff:
				if (options.sampleFormat == SampleFormat::Float32)
					rejected("32-bit float AIFF files; AIFF stores integers");
				break;
			case AudioFormat::Flac:
				if (options.sampleFormat != SampleFormat::Int16 && options.sampleFormat != SampleFormat::Int24)
					rejected("FLAC files with other than 16 or 24 bits");
				if (options.channels > 8)
					rejected("FLAC files with more than 8 channels");
				break;
			default:
				rejected("lossy formats");
			}
			if (options.broadcast && options.format != AudioFormat::Wav)
				rejected("broadcast information outside WAV files");
			if (options.broadcast)
			{
				const BroadcastInfo& info = *options.broadcast;
				if (info.description.size() > 256 || info.originator.size() > 32 ||
					info.originatorReference.size() > 32 || info.originationDate.size() > 10 ||
					info.originationTime.size() > 8)
					rejected("broadcast information with fields longer than their chunk allows");
			}
		}
	}

	std::unique_ptr<AudioFileWriter> AudioFileWriter::create(const std::string& path, const WriteOptions& options)
	{
		check(options);
		if (options.format == AudioFormat::Aiff)
			return std::make_unique<AiffWriter>(path, options);
		if (options.format == AudioFormat::Flac)
			return std::make_unique<FlacWriter>(path, options);
		return std::make_unique<WavWriter>(path, options);
	}
}
