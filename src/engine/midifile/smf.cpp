// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midifile/smf.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace gnubase::engine::midifile
{
	namespace
	{
		constexpr uint32_t g_maxVariableLength = 0x0FFFFFFF; // four bytes of seven bits

		bool isChannelStatus(uint8_t status)
		{
			return status >= 0x80 && status < 0xF0;
		}

		class Output
		{
		public:
			void byte(uint8_t value) { m_bytes.push_back(value); }

			void bytes(const std::vector<uint8_t>& values)
			{
				m_bytes.insert(m_bytes.end(), values.begin(), values.end());
			}

			void tag(const char* text) { m_bytes.insert(m_bytes.end(), text, text + 4); }

			void u16(uint32_t value)
			{
				byte(static_cast<uint8_t>(value >> 8));
				byte(static_cast<uint8_t>(value));
			}

			void u32(uint32_t value)
			{
				u16(value >> 16);
				u16(value & 0xFFFFU);
			}

			// The value in groups of seven bits, the most significant first; every byte but the last has bit 7 set.
			void variableLength(uint64_t value)
			{
				if (value > g_maxVariableLength)
					throw std::invalid_argument("a delta time or length exceeds 0x0FFFFFFF");
				uint8_t groups[4];
				int count = 0;
				do
				{
					groups[count++] = static_cast<uint8_t>(value & 0x7FU);
					value >>= 7;
				} while (value != 0);
				while (count > 1)
					byte(static_cast<uint8_t>(groups[--count] | 0x80U));
				byte(groups[0]);
			}

			size_t size() const { return m_bytes.size(); }

			// Fills in a chunk length written as four placeholder bytes at offset.
			void patchLength(size_t offset)
			{
				const auto length = static_cast<uint32_t>(m_bytes.size() - offset - 4);
				for (int i = 0; i < 4; ++i)
					m_bytes[offset + static_cast<size_t>(i)] = static_cast<uint8_t>(length >> (24 - (8 * i)));
			}

			std::vector<uint8_t> take() { return std::move(m_bytes); }

		private:
			std::vector<uint8_t> m_bytes;
		};

		void writeTrack(Output& out, const Track& track)
		{
			out.tag("MTrk");
			const size_t lengthAt = out.size();
			out.u32(0);
			int64_t previous = 0;
			uint8_t running = 0;
			for (const Event& event : track.events)
			{
				if (event.tick < previous)
					throw std::invalid_argument("events must be in tick order");
				out.variableLength(static_cast<uint64_t>(event.tick - previous));
				previous = event.tick;
				if (isChannelStatus(event.status))
				{
					if (event.data1 > 0x7F || event.data2 > 0x7F)
						throw std::invalid_argument("MIDI data bytes must be 0 to 127");
					if (event.status != running)
						out.byte(event.status);
					running = event.status;
					out.byte(event.data1);
					if (dataBytes(event.status) == 2)
						out.byte(event.data2);
					continue;
				}
				running = 0; // SysEx and meta events cancel running status
				const bool sysEx = event.status == g_statusSysEx || event.status == g_statusSysExContinue;
				if (!sysEx && event.status != g_statusMeta)
					throw std::invalid_argument("unknown event status");
				out.byte(event.status);
				if (!sysEx)
					out.byte(event.meta);
				out.variableLength(event.data.size());
				out.bytes(event.data);
			}
			out.variableLength(static_cast<uint64_t>(std::max(track.end, previous) - previous));
			out.byte(g_statusMeta);
			out.byte(g_metaEndOfTrack);
			out.byte(0);
			out.patchLength(lengthAt);
		}

		// Reads bytes; running past the end throws Truncated, which the track reader takes as the track's end.
		struct Truncated
		{
		};

		class Input
		{
		public:
			Input(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

			bool atEnd() const { return m_position >= m_size; }
			size_t position() const { return m_position; }
			size_t remaining() const { return m_size - m_position; }

			uint8_t byte()
			{
				if (atEnd())
					throw Truncated();
				return m_data[m_position++];
			}

			uint8_t peek() const
			{
				if (atEnd())
					throw Truncated();
				return m_data[m_position];
			}

			uint32_t u16()
			{
				const uint32_t high = byte();
				return (high << 8) | byte();
			}

			uint32_t u32()
			{
				const uint32_t high = u16();
				return (high << 16) | u16();
			}

			uint32_t variableLength()
			{
				uint32_t value = 0;
				for (int i = 0; i < 4; ++i)
				{
					const uint8_t next = byte();
					value = (value << 7) | (next & 0x7FU);
					if ((next & 0x80U) == 0)
						return value;
				}
				throw std::runtime_error("invalid MIDI file: a variable-length number has more than four bytes");
			}

			std::vector<uint8_t> bytes(size_t count)
			{
				if (count > remaining())
					throw Truncated();
				std::vector<uint8_t> result(m_data + m_position, m_data + m_position + count);
				m_position += count;
				return result;
			}

			bool tagIs(const char* text) const
			{
				return remaining() >= 4 && std::memcmp(m_data + m_position, text, 4) == 0;
			}

			void skip(size_t count) { m_position += std::min(count, remaining()); }

		private:
			const uint8_t* m_data;
			size_t m_size;
			size_t m_position = 0;
		};

		// Reads the next event into the track; false at the end-of-track event.
		bool readEvent(Input& in, Track& track, int64_t& tick, uint8_t& running)
		{
			tick += in.variableLength();
			uint8_t status = in.peek();
			if (status < 0x80)
			{
				// Running status. Strictly, meta and SysEx events cancel it, but files that keep it after them are
				// common enough to accept.
				if (running == 0)
					throw std::runtime_error("invalid MIDI file: a data byte where an event should start");
				status = running;
			}
			else
			{
				in.byte();
			}
			if (isChannelStatus(status))
			{
				running = status;
				Event event = channelEvent(tick, status, in.byte());
				if (dataBytes(status) == 2)
					event.data2 = in.byte();
				if (event.data1 > 0x7F || event.data2 > 0x7F)
					throw std::runtime_error("invalid MIDI file: a status byte where a data byte should be");
				track.events.push_back(std::move(event));
				return true;
			}
			if (status == g_statusSysEx || status == g_statusSysExContinue)
			{
				Event event;
				event.tick = tick;
				event.status = status;
				event.data = in.bytes(in.variableLength());
				track.events.push_back(std::move(event));
				return true;
			}
			if (status != g_statusMeta)
				throw std::runtime_error("invalid MIDI file: unknown status byte");
			const uint8_t type = in.byte();
			std::vector<uint8_t> payload = in.bytes(in.variableLength());
			if (type == g_metaEndOfTrack)
				return false;
			track.events.push_back(metaEvent(tick, type, std::move(payload)));
			return true;
		}

		Track readTrack(const uint8_t* data, size_t size)
		{
			Input in(data, size);
			Track track;
			int64_t tick = 0;
			uint8_t running = 0;
			try
			{
				while (!in.atEnd() && readEvent(in, track, tick, running))
				{
				}
			}
			catch (const Truncated&)
			{
				track.end = tick; // a cut-off track ends where its data ends
				return track;
			}
			track.end = tick;
			return track;
		}

		File readMidi(Input& in)
		{
			if (!in.tagIs("MThd"))
				throw std::runtime_error("not a MIDI file");
			in.skip(4);
			File file;
			try
			{
				const uint32_t headerLength = in.u32();
				if (headerLength < 6)
					throw std::runtime_error("invalid MIDI file: the header is too short");
				file.format = static_cast<int>(in.u16());
				in.u16(); // the track count: the chunks found count
				const uint32_t division = in.u16();
				in.skip(headerLength - 6);
				if ((division & 0x8000U) != 0)
					throw std::runtime_error("MIDI files with SMPTE time division are not supported");
				if (division == 0)
					throw std::runtime_error("invalid MIDI file: the resolution is 0");
				file.ticksPerQuarter = static_cast<int>(division);
			}
			catch (const Truncated&)
			{
				throw std::runtime_error("invalid MIDI file: the header is cut short");
			}
			if (file.format > 2)
				throw std::runtime_error("unknown MIDI file type " + std::to_string(file.format));

			while (in.remaining() >= 8)
			{
				const bool isTrack = in.tagIs("MTrk");
				in.skip(4);
				const uint32_t declared = in.u32();
				const size_t length = std::min<size_t>(declared, in.remaining()); // the last chunk may be cut short
				const std::vector<uint8_t> chunk = in.bytes(length);
				if (isTrack) // chunks of other kinds are skipped
					file.tracks.push_back(readTrack(chunk.data(), chunk.size()));
			}
			return file;
		}
	}

	Event channelEvent(int64_t tick, uint8_t status, uint8_t data1, uint8_t data2)
	{
		Event event;
		event.tick = tick;
		event.status = status;
		event.data1 = data1;
		event.data2 = data2;
		return event;
	}

	Event metaEvent(int64_t tick, uint8_t type, std::vector<uint8_t> data)
	{
		Event event;
		event.tick = tick;
		event.status = g_statusMeta;
		event.meta = type;
		event.data = std::move(data);
		return event;
	}

	Event metaText(int64_t tick, uint8_t type, const std::string& text)
	{
		return metaEvent(tick, type, std::vector<uint8_t>(text.begin(), text.end()));
	}

	int dataBytes(uint8_t status)
	{
		const uint8_t kind = status & 0xF0U;
		return kind == 0xC0 || kind == 0xD0 ? 1 : 2;
	}

	std::vector<uint8_t> write(const File& file)
	{
		if (file.format < 0 || file.format > 2)
			throw std::invalid_argument("MIDI file type must be 0, 1 or 2");
		if (file.format == 0 && file.tracks.size() != 1)
			throw std::invalid_argument("a MIDI file of type 0 has exactly one track");
		if (file.ticksPerQuarter < 1 || file.ticksPerQuarter > 0x7FFF)
			throw std::invalid_argument("the resolution must be 1 to 32767 ticks per quarter note");
		if (file.tracks.size() > 0xFFFF)
			throw std::invalid_argument("a MIDI file has at most 65535 tracks");
		Output out;
		out.tag("MThd");
		out.u32(6);
		out.u16(static_cast<uint32_t>(file.format));
		out.u16(static_cast<uint32_t>(file.tracks.size()));
		out.u16(static_cast<uint32_t>(file.ticksPerQuarter));
		for (const Track& track : file.tracks)
			writeTrack(out, track);
		return out.take();
	}

	File read(const uint8_t* data, size_t size)
	{
		Input in(data, size);
		if (in.tagIs("RIFF"))
		{
			// RMID: a RIFF container whose "data" chunk holds the MIDI file.
			in.skip(8);
			if (!in.tagIs("RMID"))
				throw std::runtime_error("not a MIDI file");
			in.skip(4);
			while (in.remaining() >= 8)
			{
				const bool isData = in.tagIs("data");
				in.skip(4);
				uint32_t length = 0;
				for (int i = 0; i < 4; ++i)
					length |= static_cast<uint32_t>(in.byte()) << (8 * i); // RIFF is little-endian
				const size_t available = std::min<size_t>(length, in.remaining());
				if (isData)
				{
					Input inner(data + in.position(), available);
					return readMidi(inner);
				}
				in.skip(available + (length & 1U)); // chunks are padded to even sizes
			}
			throw std::runtime_error("not a MIDI file");
		}
		return readMidi(in);
	}

	File read(const std::vector<uint8_t>& bytes)
	{
		return read(bytes.data(), bytes.size());
	}

	void save(const File& file, const std::string& path)
	{
		const std::vector<uint8_t> bytes = write(file);
		std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		out.close();
		if (!out)
			throw std::runtime_error("can't write " + path);
	}

	File load(const std::string& path)
	{
		std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
		if (!in)
			throw std::runtime_error("can't open " + path);
		const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
		try
		{
			return read(bytes);
		}
		catch (const std::runtime_error& error)
		{
			throw std::runtime_error(path + ": " + error.what());
		}
	}
}
