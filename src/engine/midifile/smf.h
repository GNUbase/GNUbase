// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Standard MIDI files (SMF) as they are stored: tracks of MIDI 1.0 channel messages, SysEx and meta events at ticks.
// write() and read() convert between this and the bytes of a file of type 0, 1 or 2; midiconvert.h converts between
// this and the project. Only the command thread reads and writes files.
namespace gnubase::engine::midifile
{
	// Meta event types.
	inline constexpr uint8_t g_metaText = 0x01;
	inline constexpr uint8_t g_metaTrackName = 0x03;
	inline constexpr uint8_t g_metaMarker = 0x06;
	inline constexpr uint8_t g_metaEndOfTrack = 0x2F;
	inline constexpr uint8_t g_metaTempo = 0x51;
	inline constexpr uint8_t g_metaTimeSignature = 0x58;

	// Status bytes besides channel messages.
	inline constexpr uint8_t g_statusSysEx = 0xF0;
	inline constexpr uint8_t g_statusSysExContinue = 0xF7; // a continued or escaped SysEx packet
	inline constexpr uint8_t g_statusMeta = 0xFF;

	struct Event
	{
		int64_t tick = 0;          // from the start of the file
		uint8_t status = 0;        // a channel message's status byte (0x80-0xEF, with the channel), 0xF0, 0xF7 or 0xFF
		uint8_t data1 = 0;         // channel messages: the first data byte
		uint8_t data2 = 0;         // channel messages with two data bytes: the second one
		uint8_t meta = 0;          // meta events: the type
		std::vector<uint8_t> data; // SysEx: the bytes after the status byte (a whole message ends with 0xF7); meta
	};

	Event channelEvent(int64_t tick, uint8_t status, uint8_t data1, uint8_t data2 = 0);
	Event metaEvent(int64_t tick, uint8_t type, std::vector<uint8_t> data);
	Event metaText(int64_t tick, uint8_t type, const std::string& text);

	// A channel message's data byte count: 1 for program changes and channel pressure, otherwise 2.
	int dataBytes(uint8_t status);

	struct Track
	{
		std::vector<Event> events; // by tick; without the end-of-track event
		int64_t end = 0;           // the end-of-track event's tick: at least the last event's
	};

	struct File
	{
		int format = 1;            // 0: one track with every channel, 1: tracks played together, 2: independent
		int ticksPerQuarter = 480; // the resolution (PPQ)
		std::vector<Track> tracks;
	};

	// The file's bytes. Channel messages use running status; every track ends with an end-of-track event. Throws
	// std::invalid_argument for a file that can't be written: format 0 without exactly one track, a resolution
	// outside 1-32767, events out of order or data bytes above 127.
	std::vector<uint8_t> write(const File& file);

	// Reads a file's bytes, also inside a RIFF (RMID) container. Tolerates what common files get wrong: running
	// status across meta events, unknown chunks, a missing end-of-track event and a last chunk that is cut short.
	// Throws std::runtime_error for what isn't a readable MIDI file, such as SMPTE time division.
	File read(const uint8_t* data, size_t size);
	File read(const std::vector<uint8_t>& bytes);

	// write() and read() with a file at a UTF-8 path. Throw std::runtime_error.
	void save(const File& file, const std::string& path);
	File load(const std::string& path);
}
