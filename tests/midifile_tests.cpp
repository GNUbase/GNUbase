// SPDX-License-Identifier: GPL-3.0-or-later
// Standard MIDI files (#137): the writer and reader byte for byte, the conversion from and to the project, round
// trips, and the midiFile.export and midiFile.import commands.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/midi/midivalues.h"
#include "engine/midifile/midiconvert.h"
#include "engine/midifile/smf.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace gnubase::engine;
namespace midifile = gnubase::engine::midifile;
namespace test = gnubase::test;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using gnubase::time::g_ticksPerQuarter;
using gnubase::time::SignatureMap;
using gnubase::time::TempoMap;
using gnubase::time::Ticks;
using Bytes = std::vector<uint8_t>;

namespace
{
	constexpr Ticks g_quarter = g_ticksPerQuarter;

	// Hand-built file contents.

	Bytes text(const std::string& value)
	{
		return {value.begin(), value.end()};
	}

	Bytes join(std::initializer_list<Bytes> parts)
	{
		Bytes result;
		for (const Bytes& part : parts)
			result.insert(result.end(), part.begin(), part.end());
		return result;
	}

	Bytes chunk(const char* tag, const Bytes& body)
	{
		const auto size = static_cast<uint32_t>(body.size());
		return join({text(tag),
			{static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16), static_cast<uint8_t>(size >> 8),
				static_cast<uint8_t>(size)},
			body});
	}

	Bytes header(int format, int tracks, int ticksPerQuarter)
	{
		return chunk("MThd",
			{0, static_cast<uint8_t>(format), 0, static_cast<uint8_t>(tracks),
				static_cast<uint8_t>(ticksPerQuarter >> 8), static_cast<uint8_t>(ticksPerQuarter)});
	}

	Bytes endOfTrack()
	{
		return {0xFF, 0x2F, 0x00};
	}

	std::string hex(const Bytes& bytes)
	{
		static const char* digits = "0123456789ABCDEF";
		std::string result;
		for (const uint8_t byte : bytes)
		{
			result += digits[byte >> 4];
			result += digits[byte & 0x0F];
			result += ' ';
		}
		return result;
	}

	void writeBytes(const std::filesystem::path& path, const Bytes& bytes)
	{
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	Bytes readBytes(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
	}

	// Events as comparable tuples.
	using EventKey = std::tuple<int64_t, int, int, int, int, Bytes>;

	EventKey key(const midifile::Event& event)
	{
		return {event.tick, event.status, event.data1, event.data2, event.meta, event.data};
	}

	std::vector<EventKey> keys(const midifile::Track& track)
	{
		std::vector<EventKey> result;
		result.reserve(track.events.size());
		for (const midifile::Event& event : track.events)
			result.push_back(key(event));
		return result;
	}

	// A project with a MIDI track whose part has notes and controllers of every kind; see the expected bytes below.
	Project exampleProject()
	{
		Project project = newProject();
		project.name = "Song";
		project.tempo = TempoMap({{0, 120.0, false}, {4 * g_quarter, 90.0, false}});
		project.signatures = SignatureMap({{0, {4, 4}}, {2, {3, 4}}});
		project.markers[1] = Marker{1, 4 * g_quarter, "Verse", ""};

		Track track;
		track.id = 2;
		track.type = TrackType::Midi;
		track.name = "Piano";
		track.channel = 3;
		project.trackById[track.id] = track;
		project.tracks.push_back(track.id);
		project.channels[3] = Channel{3, 2, 0, 0, 0, {}};

		MidiPart part;
		part.id = 4;
		part.track = 2;
		part.name = "Piano";
		part.position = {gnubase::time::TimeBase::Musical, 4 * g_quarter};
		part.length = 4 * g_quarter;
		const auto note = [&part](ObjectId id, Ticks position, Ticks length, uint8_t pitch, uint16_t velocity,
							  uint16_t release, uint8_t channel)
		{ part.notes[id] = Note{id, position, length, pitch, velocity, channel, release, {}}; };
		note(10, 0, g_quarter, 60, midi::velocityFromMidi1(100), midi::velocityFromMidi1(30), 0);
		note(11, g_quarter / 2, g_quarter / 2, 64, midi::velocityFromMidi1(80), g_defaultReleaseVelocity, 1);
		// MIDI 2.0 velocity 0x0100 is 0 in MIDI 1.0, which a note-on can't have; the note is cut at the part's end.
		note(12, 3 * g_quarter, 2 * g_quarter, 67, 0x0100, g_defaultReleaseVelocity, 0);
		const auto controller = [&part](ObjectId id, Ticks position, ControllerType type, uint8_t number,
									uint32_t value, uint8_t channel, Bytes data = {})
		{ part.controllers[id] = Controller{id, position, type, number, value, channel, std::move(data)}; };
		controller(20, 0, ControllerType::Program, 5, 130, 0);
		controller(21, 0, ControllerType::Control, 7, midi::controllerFromMidi1(90), 0);
		controller(22, g_quarter, ControllerType::PitchBend, 0, midi::pitchBendFromMidi1(0x2100), 1);
		controller(23, g_quarter, ControllerType::ChannelPressure, 0, 0xC0000000U, 0); // 96 in MIDI 1.0
		controller(24, 2 * g_quarter, ControllerType::PolyPressure, 60, midi::controllerFromMidi1(50), 0);
		controller(25, 2 * g_quarter, ControllerType::SysEx, 0, 0, 0, {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7});
		controller(26, 5 * g_quarter, ControllerType::Control, 1, 0, 0); // after the part's end: not played
		project.midiParts[part.id] = part;
		project.nextId = 30;
		return project;
	}

	// A project with one empty MIDI track, id 2.
	Project midiProject()
	{
		Project project = newProject();
		Track track;
		track.id = 2;
		track.type = TrackType::Midi;
		track.name = "Keys";
		project.trackById[track.id] = track;
		project.tracks.push_back(track.id);
		project.nextId = 100;
		return project;
	}

	MidiPart& addPart(Project& project, ObjectId id, Ticks position, Ticks length)
	{
		MidiPart part;
		part.id = id;
		part.track = 2;
		part.position = {gnubase::time::TimeBase::Musical, position};
		part.length = length;
		return project.midiParts[id] = part;
	}

	// exampleProject() at 96 ticks per quarter note: the tempo track, then the piano track.
	Bytes exampleTempoTrack()
	{
		return join({
			{0x00, 0xFF, 0x03, 0x04},
			text("Song"),
			{0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20},       // 120 BPM: 500000 microseconds per quarter note
			{0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}, // 4/4
			{0x83, 0x00, 0xFF, 0x51, 0x03, 0x0A, 0x2C, 0x2B}, // tick 384: 90 BPM
			{0x00, 0xFF, 0x06, 0x05},
			text("Verse"),                                          // the marker
			{0x83, 0x00, 0xFF, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08}, // tick 768: 3/4
			{0x00},
			endOfTrack(),
		});
	}

	Bytes examplePianoTrack()
	{
		return join({
			{0x00, 0xFF, 0x03, 0x05},
			text("Piano"),
			{0x83, 0x00, 0xB0, 0x00, 0x01}, // tick 384: bank select 130 = 1 * 128 + 2
			{0x00, 0x20, 0x02},             // running status
			{0x00, 0xC0, 0x05},             // program 5
			{0x00, 0xB0, 0x07, 0x5A},       // volume 90
			{0x00, 0x90, 0x3C, 0x64},       // note-ons after controllers
			{0x30, 0x91, 0x40, 0x50},       // tick 432
			{0x30, 0x80, 0x3C, 0x1E},       // tick 480: note-offs first, with the release velocity
			{0x00, 0x81, 0x40, 0x40},
			{0x00, 0xE1, 0x00, 0x42}, // pitch bend 0x2100
			{0x00, 0xD0, 0x60},       // channel pressure 96
			{0x60, 0xA0, 0x3C, 0x32}, // tick 576: poly pressure
			{0x00, 0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7},
			{0x60, 0x90, 0x43, 0x01}, // tick 672: SysEx cancelled running status; velocity at least 1
			{0x60, 0x80, 0x43, 0x40}, // tick 768: cut at the part's end
			{0x00},
			endOfTrack(),
		});
	}

	// Engine helpers.

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
		REQUIRE(!result.ok());
		return result.error;
	}

	std::string utf8(const std::filesystem::path& path)
	{
		return path.u8string();
	}

	// A note with its part's position added, comparable across projects.
	using NoteKey = std::tuple<Ticks, Ticks, int, int, int, int>;
	using ControllerKey = std::tuple<Ticks, ControllerType, int, uint32_t, int, Bytes>;

	std::vector<NoteKey> absoluteNotes(const Project& project, ObjectId track)
	{
		std::vector<NoteKey> notes;
		for (const auto& [id, part] : project.midiParts)
			if (part.track == track)
				for (const auto& [noteId, note] : part.notes)
					notes.emplace_back(part.position.value + note.position, note.length, note.pitch, note.velocity,
						note.releaseVelocity, note.channel);
		std::sort(notes.begin(), notes.end());
		return notes;
	}

	std::vector<NoteKey> absoluteNotes(const midifile::ImportedTrack& track)
	{
		std::vector<NoteKey> notes;
		notes.reserve(track.notes.size());
		for (const Note& note : track.notes)
			notes.emplace_back(
				note.position, note.length, note.pitch, note.velocity, note.releaseVelocity, note.channel);
		std::sort(notes.begin(), notes.end());
		return notes;
	}

	std::vector<ControllerKey> absoluteControllers(const midifile::ImportedTrack& track)
	{
		std::vector<ControllerKey> controllers;
		controllers.reserve(track.controllers.size());
		for (const Controller& c : track.controllers)
			controllers.emplace_back(c.position, c.type, c.number, c.value, c.channel, c.data);
		std::sort(controllers.begin(), controllers.end());
		return controllers;
	}
}

TEST_CASE("the MIDI file writer writes variable-length deltas, running status and track ends", "[unit]")
{
	midifile::File file;
	file.format = 1;
	file.ticksPerQuarter = 96;
	midifile::Track names;
	names.events.push_back(midifile::metaText(0, midifile::g_metaTrackName, "A"));
	names.end = 96;
	midifile::Track notes;
	notes.events = {midifile::channelEvent(0, 0x90, 60, 100), midifile::channelEvent(0, 0x90, 64, 100),
		midifile::channelEvent(96, 0x80, 60, 64), midifile::channelEvent(96, 0x80, 64, 64),
		midifile::channelEvent(224, 0xB0, 7, 127), midifile::channelEvent(224 + 0x3FFF, 0xC0, 5)};
	midifile::Event sysEx;
	sysEx.tick = 224 + 0x3FFF + 0x200000;
	sysEx.status = midifile::g_statusSysEx;
	sysEx.data = {0x7E, 0x7F, 0x09, 0x01, 0xF7};
	notes.events.push_back(sysEx);
	notes.events.push_back(midifile::channelEvent(sysEx.tick, 0xB0, 7, 0));
	file.tracks = {names, notes};

	const Bytes expected =
		join({header(1, 2, 96), chunk("MTrk", join({{0x00, 0xFF, 0x03, 0x01, 'A'}, {0x60}, endOfTrack()})),
			chunk("MTrk",
				join({{0x00, 0x90, 0x3C, 0x64}, {0x00, 0x40, 0x64}, {0x60, 0x80, 0x3C, 0x40}, {0x00, 0x40, 0x40},
					{0x81, 0x00, 0xB0, 0x07, 0x7F}, {0xFF, 0x7F, 0xC0, 0x05},
					{0x81, 0x80, 0x80, 0x00, 0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7}, {0x00, 0xB0, 0x07, 0x00},
					{0x00}, endOfTrack()}))});
	const Bytes written = midifile::write(file);
	CHECK(hex(written) == hex(expected));

	// And back.
	const midifile::File read = midifile::read(written);
	CHECK(read.format == 1);
	CHECK(read.ticksPerQuarter == 96);
	REQUIRE(read.tracks.size() == 2);
	CHECK(keys(read.tracks[0]) == keys(names));
	CHECK(read.tracks[0].end == 96);
	CHECK(keys(read.tracks[1]) == keys(notes));
	CHECK(read.tracks[1].end == sysEx.tick);
}

TEST_CASE("the MIDI file writer rejects what a file can't hold", "[unit]")
{
	midifile::File file;
	file.format = 0;
	file.tracks.resize(2);
	CHECK_THROWS_AS(midifile::write(file), std::invalid_argument);
	file.tracks.resize(1);
	file.tracks[0].events = {midifile::channelEvent(10, 0x90, 60, 100), midifile::channelEvent(5, 0x80, 60, 0)};
	CHECK_THROWS_AS(midifile::write(file), std::invalid_argument);
	file.tracks[0].events = {midifile::channelEvent(0, 0x90, 128, 100)};
	CHECK_THROWS_AS(midifile::write(file), std::invalid_argument);
	file.tracks[0].events.clear();
	file.ticksPerQuarter = 0;
	CHECK_THROWS_AS(midifile::write(file), std::invalid_argument);
}

TEST_CASE("the MIDI file reader follows running status, also across meta events", "[unit]")
{
	const Bytes bytes = join({header(0, 1, 480),
		chunk("MTrk",
			join({{0x00, 0x91, 0x3C, 0x40},         // note-on, channel 1
				{0x10, 0x3E, 0x41},                 // running status
				{0x10, 0x3C, 0x00},                 // velocity 0: a note-off
				{0x00, 0xFF, 0x01, 0x02, 'h', 'i'}, // a text event
				{0x10, 0x3E, 0x00},                 // running status kept across it
				{0x00}, endOfTrack()}))});
	const midifile::File file = midifile::read(bytes);
	CHECK(file.format == 0);
	REQUIRE(file.tracks.size() == 1);
	const std::vector<EventKey> expected = {key(midifile::channelEvent(0, 0x91, 60, 64)),
		key(midifile::channelEvent(16, 0x91, 62, 65)), key(midifile::channelEvent(32, 0x91, 60, 0)),
		key(midifile::metaText(32, midifile::g_metaText, "hi")), key(midifile::channelEvent(48, 0x91, 62, 0))};
	CHECK(keys(file.tracks[0]) == expected);
	CHECK(file.tracks[0].end == 48);

	const midifile::Import imported = midifile::importFile(file);
	REQUIRE(imported.tracks.size() == 1);
	CHECK(imported.tracks[0].name == "Channel 2");
	const Ticks tick = g_quarter / 480;
	const uint16_t release64 = midi::velocityFromMidi1(64);
	CHECK(absoluteNotes(imported.tracks[0]) ==
		std::vector<NoteKey>{{0, 32 * tick, 60, midi::velocityFromMidi1(64), release64, 1},
			{16 * tick, 32 * tick, 62, midi::velocityFromMidi1(65), release64, 1}});
}

TEST_CASE("the MIDI file reader copes with odd files", "[unit]")
{
	const Bytes notes = join({{0x00, 0x90, 0x3C, 0x64}, {0x60, 0x80, 0x3C, 0x40}});

	SECTION("a longer header, unknown chunks, a missing end of track and a cut-off last chunk")
	{
		const Bytes longHeader = chunk("MThd", {0, 1, 0, 2, 0, 96, 0xAA, 0xBB});
		Bytes truncated = chunk("MTrk", join({notes, {0x60, 0x90, 0x40}}));
		truncated[7] = 0x40; // claims more bytes than there are
		const midifile::File file =
			midifile::read(join({longHeader, chunk("XFIH", {1, 2, 3}), chunk("MTrk", notes), truncated}));
		CHECK(file.ticksPerQuarter == 96);
		REQUIRE(file.tracks.size() == 2);
		CHECK(file.tracks[0].events.size() == 2);
		CHECK(file.tracks[0].end == 96);
		CHECK(file.tracks[1].events.size() == 2); // the cut-off third event is left out
	}

	SECTION("a RIFF container")
	{
		const Bytes smf = join({header(0, 1, 96), chunk("MTrk", join({notes, {0x00}, endOfTrack()}))});
		const auto size = static_cast<uint32_t>(smf.size());
		const Bytes riff = join({text("RIFF"), {0, 0, 0, 0}, text("RMID"), text("data"),
			{static_cast<uint8_t>(size), static_cast<uint8_t>(size >> 8), 0, 0}, smf});
		const midifile::File file = midifile::read(riff);
		REQUIRE(file.tracks.size() == 1);
		CHECK(file.tracks[0].events.size() == 2);
	}

	SECTION("notes without a note-off, note-offs without a note and the same pitch overlapping")
	{
		const midifile::File file = midifile::read(join({header(1, 1, 96),
			chunk("MTrk",
				join({{0x00, 0x80, 0x30, 0x40},                   // no note to end
					{0x00, 0x90, 0x3C, 0x64}, {0x60, 0x3C, 0x50}, // the same pitch twice
					{0x60, 0x80, 0x3C, 0x10},                     // ends the first one
					{0x60, 0x3C, 0x20},                           // and the second
					{0x00, 0x90, 0x40, 0x64},                     // never ends
					{0x60}, endOfTrack()}))}));
		const midifile::Import imported = midifile::importFile(file);
		REQUIRE(imported.tracks.size() == 1);
		const Ticks tick = g_quarter / 96;
		CHECK(absoluteNotes(imported.tracks[0]) ==
			std::vector<NoteKey>{{0, 192 * tick, 60, midi::velocityFromMidi1(100), midi::velocityFromMidi1(16), 0},
				{96 * tick, 192 * tick, 60, midi::velocityFromMidi1(80), midi::velocityFromMidi1(32), 0},
				{288 * tick, 96 * tick, 64, midi::velocityFromMidi1(100), g_defaultReleaseVelocity, 0}});
	}

	SECTION("SysEx sent in packets")
	{
		const midifile::File file = midifile::read(join({header(1, 1, 96),
			chunk("MTrk",
				join({{0x00, 0xF0, 0x02, 0x43, 0x10}, {0x10, 0xF7, 0x02, 0x4C, 0xF7}, // one message in two packets
					{0x00, 0xF7, 0x01, 0xF8},                                         // a raw escape: left out
					{0x00}, endOfTrack()}))}));
		const midifile::Import imported = midifile::importFile(file);
		REQUIRE(imported.tracks.size() == 1);
		REQUIRE(imported.tracks[0].controllers.size() == 1);
		const Controller& sysEx = imported.tracks[0].controllers[0];
		CHECK(sysEx.type == ControllerType::SysEx);
		CHECK(sysEx.position == 0);
		CHECK(sysEx.data == Bytes{0xF0, 0x43, 0x10, 0x4C, 0xF7});
	}

	SECTION("what isn't a readable MIDI file")
	{
		CHECK_THROWS_WITH(
			midifile::read(join({text("RIFF"), {0, 0, 0, 0}, text("WAVE")})), ContainsSubstring("not a MIDI file"));
		CHECK_THROWS_WITH(midifile::read(text("hello")), ContainsSubstring("not a MIDI file"));
		CHECK_THROWS_WITH(midifile::read(chunk("MThd", {0, 1, 0, 1, 0xE7, 0x28})), ContainsSubstring("SMPTE"));
		CHECK_THROWS_WITH(midifile::read(chunk("MThd", {0, 1})), ContainsSubstring("header"));
		CHECK_THROWS_WITH(midifile::read(join({header(1, 1, 96), chunk("MTrk", {0x00, 0x3C, 0x40})})),
			ContainsSubstring("data byte"));
		CHECK_THROWS_WITH(midifile::read(join({header(1, 1, 96), chunk("MTrk", {0x81, 0x81, 0x81, 0x81, 0x01})})),
			ContainsSubstring("variable-length"));
	}
}

TEST_CASE("MIDI file export writes tempo, signatures, markers, names and MIDI 1.0 events byte for byte", "[unit]")
{
	const Project project = exampleProject();
	midifile::ExportOptions options;
	options.ticksPerQuarter = 96;

	SECTION("type 1")
	{
		const Bytes expected =
			join({header(1, 2, 96), chunk("MTrk", exampleTempoTrack()), chunk("MTrk", examplePianoTrack())});
		CHECK(hex(midifile::write(midifile::exportProject(project, options))) == hex(expected));
	}

	SECTION("type 0")
	{
		options.format = 0;
		const Bytes track = join({
			{0x00, 0xFF, 0x03, 0x04},
			text("Song"),
			{0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20},
			{0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08},
			{0x83, 0x00, 0xFF, 0x51, 0x03, 0x0A, 0x2C, 0x2B},
			{0x00, 0xFF, 0x06, 0x05},
			text("Verse"),
			{0x00, 0xB0, 0x00, 0x01}, // meta events cancel running status
			{0x00, 0x20, 0x02},
			{0x00, 0xC0, 0x05},
			{0x00, 0xB0, 0x07, 0x5A},
			{0x00, 0x90, 0x3C, 0x64},
			{0x30, 0x91, 0x40, 0x50},
			{0x30, 0x80, 0x3C, 0x1E},
			{0x00, 0x81, 0x40, 0x40},
			{0x00, 0xE1, 0x00, 0x42},
			{0x00, 0xD0, 0x60},
			{0x60, 0xA0, 0x3C, 0x32},
			{0x00, 0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7},
			{0x60, 0x90, 0x43, 0x01},
			{0x60, 0xFF, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08}, // meta events before note-offs
			{0x00, 0x80, 0x43, 0x40},
			{0x00},
			endOfTrack(),
		});
		CHECK(hex(midifile::write(midifile::exportProject(project, options))) ==
			hex(join({header(0, 1, 96), chunk("MTrk", track)})));
	}

	SECTION("a range, without markers")
	{
		options.start = 5 * g_quarter; // the second beat of the part
		options.end = 7 * g_quarter;
		options.markers = false;
		const Bytes tempo = join({{0x00, 0xFF, 0x03, 0x04}, text("Song"), {0x00, 0xFF, 0x51, 0x03, 0x0A, 0x2C, 0x2B},
			{0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08}, {0x81, 0x40}, endOfTrack()});
		const Bytes piano = join({{0x00, 0xFF, 0x03, 0x05}, text("Piano"), {0x00, 0xE1, 0x00, 0x42}, {0x00, 0xD0, 0x60},
			{0x60, 0xA0, 0x3C, 0x32}, {0x00, 0xF0, 0x05, 0x7E, 0x7F, 0x09, 0x01, 0xF7}, {0x60}, endOfTrack()});
		CHECK(hex(midifile::write(midifile::exportProject(project, options))) ==
			hex(join({header(1, 2, 96), chunk("MTrk", tempo), chunk("MTrk", piano)})));
	}

	SECTION("only MIDI and instrument tracks")
	{
		Project withAudio = project;
		withAudio.trackById[40] = Track{40, TrackType::Audio, "Vocals", "", {}, 0, 0, {}, {}};
		withAudio.tracks.push_back(40);
		CHECK(midifile::exportProject(withAudio, options).tracks.size() == 2);
		options.tracks = {40};
		CHECK_THROWS_AS(midifile::exportProject(withAudio, options), std::invalid_argument);
	}
}

TEST_CASE("MIDI file export and import keep notes, controllers, tempo, signatures and markers", "[unit]")
{
	Project project = exampleProject();
	// Only values MIDI 1.0 can hold, and notes inside the part, so that everything comes back.
	MidiPart& part = project.midiParts.at(4);
	part.notes.at(12).velocity = midi::velocityFromMidi1(1);
	part.notes.at(12).length = g_quarter;
	part.controllers.erase(26);
	part.controllers.at(23).value = midi::controllerFromMidi1(96);
	project.tempo = TempoMap({{0, 120.0, false}, {4 * g_quarter, 90.0, false}, {6 * g_quarter, 140.5, false}});

	std::vector<NoteKey> notes = absoluteNotes(project, 2);
	std::vector<ControllerKey> controllers;
	controllers.reserve(part.controllers.size());
	for (const auto& [id, c] : part.controllers)
		controllers.emplace_back(part.position.value + c.position, c.type, c.number, c.value, c.channel, c.data);
	std::sort(controllers.begin(), controllers.end());

	for (const int format : {0, 1})
	{
		CAPTURE(format);
		midifile::ExportOptions options;
		options.format = format;
		const midifile::Import imported =
			midifile::importFile(midifile::read(midifile::write(midifile::exportProject(project, options))));
		REQUIRE(imported.tracks.size() == (format == 0 ? 2 : 1)); // type 0 splits by channel
		midifile::ImportedTrack all;
		for (const midifile::ImportedTrack& track : imported.tracks)
		{
			all.notes.insert(all.notes.end(), track.notes.begin(), track.notes.end());
			all.controllers.insert(all.controllers.end(), track.controllers.begin(), track.controllers.end());
		}
		CHECK(absoluteNotes(all) == notes);
		CHECK(absoluteControllers(all) == controllers);
		CHECK(imported.tracks[0].name == (format == 0 ? "Song (Channel 1)" : "Piano"));
		// 140.5 BPM is 427046 microseconds per quarter note.
		CHECK(imported.tempo.points().size() == 3);
		CHECK(imported.tempo.points()[1].position == 4 * g_quarter);
		CHECK(imported.tempo.points()[1].bpm == 60000000.0 / 666667.0);
		CHECK_THAT(imported.tempo.points()[2].bpm, WithinAbs(140.5, 1e-3));
		CHECK(imported.signatures == project.signatures);
		REQUIRE(imported.markers.size() == 1);
		CHECK(imported.markers[0].position == 4 * g_quarter);
		CHECK(imported.markers[0].name == "Verse");
	}
}

TEST_CASE("MIDI file export turns tempo ramps into steps that take as long", "[unit]")
{
	Project project = newProject();
	project.tempo = TempoMap({{0, 120.0, false}, {8 * g_quarter, 60.0, true}, {12 * g_quarter, 60.0, false}});
	const midifile::Import imported =
		midifile::importFile(midifile::read(midifile::write(midifile::exportProject(project, {}))));
	CHECK(imported.tempo.points().size() == (8 * 4) + 1);
	for (const Ticks at : {2 * g_quarter, 8 * g_quarter, 12 * g_quarter})
		CHECK_THAT(imported.tempo.toSeconds(at), WithinAbs(project.tempo.toSeconds(at), 1e-5));
}

TEST_CASE("MIDI file import places signatures at bars and keeps tempo in range", "[unit]")
{
	const Bytes track = join({{0x00, 0xFF, 0x58, 0x04, 0x06, 0x03, 0x24, 0x08}, // 6/8
		{0x00, 0xFF, 0x51, 0x03, 0x00, 0x00, 0x01},                             // far too fast
		{0x81, 0x40, 0xFF, 0x58, 0x04, 0x02, 0x02, 0x18, 0x08},                 // 2/4 after a quarter: next bar
		{0x00, 0xFF, 0x58, 0x04, 0x05, 0x05, 0x18, 0x08},                       // 5/32: left out
		{0x00}, endOfTrack()});
	const midifile::Import imported =
		midifile::importFile(midifile::read(join({header(1, 1, 192), chunk("MTrk", track)})));
	CHECK(imported.signatures == SignatureMap({{0, {6, 8}}, {1, {2, 4}}}));
	CHECK(imported.tempo.points().size() == 1);
	CHECK(imported.tempo.points()[0].bpm == gnubase::time::g_maxBpm);
	CHECK(imported.tracks.empty());
}

TEST_CASE("MIDI file import reads many signature changes", "[unit]")
{
	// A change at every bar, 3/4 and 4/4 in turn, at one tick per quarter note.
	constexpr int count = 20000;
	Bytes track;
	track.reserve((count * 8) + 32);
	std::vector<gnubase::time::SignatureChange> expected;
	expected.reserve(count + 1);
	for (int i = 0; i < count; ++i)
	{
		const auto numerator = static_cast<uint8_t>(i % 2 == 0 ? 3 : 4);
		const auto delta = static_cast<uint8_t>(i == 0 ? 0 : 7 - numerator); // the previous bar's length
		const Bytes event = {delta, 0xFF, 0x58, 0x04, numerator, 0x02, 0x18, 0x08};
		track.insert(track.end(), event.begin(), event.end());
		expected.push_back({i, {numerator, 4}});
	}
	// 5/8 a tick into the last bar moves to the next bar start, and 7/8 a tick later, still before it, replaces it.
	const Bytes rest = join({{0x01, 0xFF, 0x58, 0x04, 0x05, 0x03, 0x0C, 0x08},
		{0x01, 0xFF, 0x58, 0x04, 0x07, 0x03, 0x0C, 0x08}, {0x00}, endOfTrack()});
	track.insert(track.end(), rest.begin(), rest.end());
	expected.push_back({count, {7, 8}});

	const midifile::Import imported =
		midifile::importFile(midifile::read(join({header(1, 1, 1), chunk("MTrk", track)})));
	CHECK(imported.signatures == SignatureMap(expected));
}

TEST_CASE("MIDI file import keeps UTF-8 text and reads other text as Windows-1252", "[unit]")
{
	const auto named = [](const Bytes& name, uint8_t pitch)
	{
		const Bytes start = {0x00, 0xFF, 0x03, static_cast<uint8_t>(name.size())};
		return chunk(
			"MTrk", join({start, name, {0x00, 0x90, pitch, 0x64}, {0x60, 0x80, pitch, 0x40}, {0x00}, endOfTrack()}));
	};
	const Bytes marker = join({{0x00, 0xFF, 0x06, 0x06}, {'B', 'r', 0xFC, 'c', 'k', 'e'}, {0x00}, endOfTrack()});
	const midifile::File file =
		midifile::read(join({header(1, 6, 96), chunk("MTrk", marker), named({'B', 0xE4, 's', 's', 'e'}, 40), // Latin-1
			named({'B', 0xC3, 0xA4, 's', 's', 'e'}, 41),                                                     // UTF-8
			named({0x93, 'H', 'i', 0x94, 0x81}, 42), // quotes, and a byte Windows-1252 has no character for
			named({'a', 0xC0, 0xAF}, 43),            // an overlong UTF-8 sequence
			named({0xE2, 0x82}, 44)}));              // a UTF-8 sequence cut short
	const midifile::Import imported = midifile::importFile(file);
	REQUIRE(imported.markers.size() == 1);
	CHECK(imported.markers[0].name ==
		"Br\xC3\xBC"
		"cke");
	REQUIRE(imported.tracks.size() == 5);
	CHECK(imported.tracks[0].name == "B\xC3\xA4sse");
	CHECK(imported.tracks[1].name == "B\xC3\xA4sse");
	CHECK(imported.tracks[2].name == "\xE2\x80\x9CHi\xE2\x80\x9D\xEF\xBF\xBD");
	CHECK(imported.tracks[3].name == "a\xC3\x80\xC2\xAF");
	CHECK(imported.tracks[4].name == "\xC3\xA2\xE2\x80\x9A");
}

TEST_CASE("the MIDI file reader ends a track before tick 2^32", "[unit]")
{
	// A note-on after each of 17 maximal delta times: the 17th would be past 2^32.
	constexpr int64_t maxDelta = 0x0FFFFFFF;
	Bytes track = {0x00, 0x90, 0x3C, 0x64};
	for (int i = 0; i < 17; ++i)
	{
		const Bytes event = {0xFF, 0xFF, 0xFF, 0x7F, 0x3C, 0x64};
		track.insert(track.end(), event.begin(), event.end());
	}
	const Bytes rest = join({{0x00}, endOfTrack()});
	track.insert(track.end(), rest.begin(), rest.end());
	const midifile::File file = midifile::read(join({header(1, 1, 1), chunk("MTrk", track)}));
	REQUIRE(file.tracks.size() == 1);
	CHECK(file.tracks[0].events.size() == 17);
	CHECK(file.tracks[0].events.back().tick == 16 * maxDelta);
	CHECK(file.tracks[0].end == 16 * maxDelta);
	CHECK(file.tracks[0].end < midifile::g_maxTick);

	// At one tick per quarter note, project ticks are far larger, and still exact.
	const midifile::Import imported = midifile::importFile(file);
	REQUIRE(imported.tracks.size() == 1);
	REQUIRE(imported.tracks[0].notes.size() == 17);
	CHECK(imported.tracks[0].notes.back().position == 16 * maxDelta * g_quarter);
}

TEST_CASE("MIDI file export rescales positions far into the project exactly", "[unit]")
{
	Project project = midiProject();
	constexpr Ticks far = Ticks{1} << 34; // quarter notes: far * g_quarter * 480 doesn't fit 64 bits
	MidiPart& part = addPart(project, 3, far * g_quarter, g_quarter);
	part.notes[4] = Note{4, 0, g_quarter / 2, 60, midi::velocityFromMidi1(100), 0, g_defaultReleaseVelocity, {}};
	const midifile::File file = midifile::exportProject(project, {});
	REQUIRE(file.tracks.size() == 2);
	std::vector<int64_t> noteTicks;
	for (const midifile::Event& event : file.tracks[1].events)
		if ((event.status & 0xE0) == 0x80)
			noteTicks.push_back(event.tick);
	CHECK(noteTicks == std::vector<int64_t>{far * 480, (far * 480) + 240});
	CHECK(file.tracks[1].end == (far + 1) * 480);
}

TEST_CASE("MIDI file export ends a note where the next one of its pitch and channel starts", "[unit]")
{
	Project project = midiProject();
	const uint16_t velocity = midi::velocityFromMidi1(100);
	const auto note = [velocity](
						  MidiPart& part, ObjectId id, Ticks position, Ticks length, uint8_t pitch, uint8_t channel)
	{ part.notes[id] = Note{id, position, length, pitch, velocity, channel, g_defaultReleaseVelocity, {}}; };
	MidiPart& first = addPart(project, 3, 0, 8 * g_quarter);
	note(first, 10, 0, 2 * g_quarter, 60, 0);                     // B inside it: ends where B starts
	note(first, 11, g_quarter / 2, g_quarter / 2, 60, 0);         // B
	note(first, 12, 0, 2 * g_quarter, 60, 1);                     // another channel: as it is
	note(first, 13, 3 * g_quarter, g_quarter, 62, 0);             // overlaps the next: ends where it starts
	note(first, 14, 7 * g_quarter / 2, 3 * g_quarter / 2, 62, 0); //
	note(first, 15, 11 * g_quarter / 2, 2 * g_quarter, 64, 0);    // a note in the next part starts inside it
	note(addPart(project, 4, 6 * g_quarter, 2 * g_quarter), 16, 0, g_quarter, 64, 0);

	const auto key = [velocity](Ticks position, Ticks length, int pitch, int channel)
	{ return NoteKey{position, length, pitch, velocity, g_defaultReleaseVelocity, channel}; };
	std::vector<NoteKey> expected = {key(0, g_quarter / 2, 60, 0), key(g_quarter / 2, g_quarter / 2, 60, 0),
		key(0, 2 * g_quarter, 60, 1), key(3 * g_quarter, g_quarter / 2, 62, 0),
		key(7 * g_quarter / 2, 3 * g_quarter / 2, 62, 0), key(11 * g_quarter / 2, g_quarter / 2, 64, 0),
		key(6 * g_quarter, g_quarter, 64, 0)};
	std::sort(expected.begin(), expected.end());

	for (const int format : {0, 1})
	{
		CAPTURE(format);
		midifile::ExportOptions options;
		options.format = format;
		const midifile::Import imported =
			midifile::importFile(midifile::read(midifile::write(midifile::exportProject(project, options))));
		midifile::ImportedTrack all;
		for (const midifile::ImportedTrack& track : imported.tracks)
			all.notes.insert(all.notes.end(), track.notes.begin(), track.notes.end());
		CHECK(absoluteNotes(all) == expected);
	}
}

TEST_CASE("MIDI file import lays a type 2 file's sequences out one after another", "[unit]")
{
	const Bytes first = join({{0x00, 0xFF, 0x51, 0x03, 0x09, 0x27, 0xC0},           // 100 BPM
		{0x00, 0xFF, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08},                           // 3/4
		{0x00, 0x90, 0x3C, 0x64}, {0x60, 0x80, 0x3C, 0x40}, {0x60}, endOfTrack()}); // two quarters
	const Bytes second =
		join({{0x00, 0x90, 0x3E, 0x64}, {0x60, 0x80, 0x3E, 0x40}, {0x82, 0x20}, endOfTrack()}); // a bar
	const Bytes third = join({{0x00, 0xFF, 0x51, 0x03, 0x06, 0x1A, 0x80},                       // 150 BPM
		{0x00, 0xFF, 0x58, 0x04, 0x06, 0x03, 0x0C, 0x08},                                       // 6/8
		{0x00, 0xFF, 0x06, 0x01, 'C'}, {0x30, 0x90, 0x40, 0x64}, {0x30, 0x80, 0x40, 0x40}, {0x00}, endOfTrack()});
	const midifile::Import imported = midifile::importFile(
		midifile::read(join({header(2, 3, 96), chunk("MTrk", first), chunk("MTrk", second), chunk("MTrk", third)})));

	// Each sequence starts at the bar start after the previous one's end, at 120 BPM in 4/4 unless it sets its own.
	CHECK(imported.signatures == SignatureMap({{0, {3, 4}}, {1, {4, 4}}, {2, {6, 8}}}));
	const std::vector<gnubase::time::TempoPoint>& tempo = imported.tempo.points();
	REQUIRE(tempo.size() == 3);
	CHECK(std::make_tuple(tempo[0].position, tempo[0].bpm) == std::make_tuple(Ticks{0}, 100.0));
	CHECK(std::make_tuple(tempo[1].position, tempo[1].bpm) == std::make_tuple(3 * g_quarter, 120.0));
	CHECK(std::make_tuple(tempo[2].position, tempo[2].bpm) == std::make_tuple(7 * g_quarter, 150.0));
	REQUIRE(imported.markers.size() == 1);
	CHECK(imported.markers[0].position == 7 * g_quarter);
	REQUIRE(imported.tracks.size() == 3);
	CHECK(imported.tracks[0].notes.at(0).position == 0);
	CHECK(imported.tracks[1].notes.at(0).position == 3 * g_quarter);
	CHECK(imported.tracks[2].notes.at(0).position == 15 * g_quarter / 2);
	CHECK(imported.tracks[2].notes.at(0).length == g_quarter / 2);
}

TEST_CASE("midiFile.import and midiFile.export round-trip a file through the engine", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);

	// exampleProject()'s file, in a new project whose name is the default.
	const Bytes tempoTrack = exampleTempoTrack();
	const Bytes original = join({header(1, 2, 96),
		chunk("MTrk",
			join({{0x00, 0xFF, 0x03, 0x08}, text("Untitled"), Bytes(tempoTrack.begin() + 8, tempoTrack.end())})),
		chunk("MTrk", examplePianoTrack())});
	const std::filesystem::path source = dir.path() / "song.mid";
	writeBytes(source, original);

	const Json imported = run(client, "midiFile.import", {{"path", utf8(source)}});
	REQUIRE(imported.at("tracks").size() == 1);
	REQUIRE(imported.at("parts").size() == 1);
	const Json project = run(client, "project.get");
	REQUIRE(project.at("tracks").size() == 1);
	CHECK(project.at("tracks")[0].at("name") == "Piano");
	CHECK(project.at("tracks")[0].at("type") == "midi");
	const Json& part = project.at("midiParts")[0];
	CHECK(part.at("position") == 4 * g_quarter); // the bar of the first event
	CHECK(part.at("length") == 4 * g_quarter);   // to the end of the bar of the last
	CHECK(part.at("notes").size() == 3);
	CHECK(part.at("controllers").size() == 6);
	CHECK(project.at("tempo").size() == 2);
	CHECK(project.at("signatures").size() == 2);
	REQUIRE(project.at("markers").size() == 1);
	CHECK(project.at("markers")[0].at("name") == "Verse");

	const std::filesystem::path copy = dir.path() / "copy.mid";
	CHECK(run(client, "midiFile.export", {{"path", utf8(copy)}, {"ppq", 96}}).at("tracks") == 2);
	CHECK(hex(readBytes(copy)) == hex(original));

	// One undo step takes it all back.
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Import MIDI file");
	run(client, "history.undo");
	const Json undone = run(client, "project.get");
	CHECK(undone.at("tracks").empty());
	CHECK(undone.at("markers").empty());
	CHECK(undone.at("tempo").size() == 1);

	// Without tempo and markers, into instrument tracks.
	run(client, "midiFile.import",
		{{"path", utf8(source)}, {"tempo", false}, {"markers", false}, {"trackType", "instrument"}});
	const Json second = run(client, "project.get");
	CHECK(second.at("tracks")[0].at("type") == "instrument");
	CHECK(second.at("tempo").size() == 1);
	CHECK(second.at("markers").empty());
}

TEST_CASE("midiFile.import splits a type 0 file by channel", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	const std::filesystem::path path = dir.path() / "type0.mid";
	writeBytes(path,
		join({header(0, 1, 480),
			chunk("MTrk",
				join({{0x00, 0xFF, 0x03, 0x04}, text("Band"), {0x00, 0x99, 0x24, 0x64}, {0x00, 0x91, 0x30, 0x50},
					{0x83, 0x60, 0x89, 0x24, 0x40}, {0x00, 0x81, 0x30, 0x40}, {0x00}, endOfTrack()}))}));
	run(client, "midiFile.import", {{"path", utf8(path)}});
	const Json project = run(client, "project.get");
	REQUIRE(project.at("tracks").size() == 2);
	CHECK(project.at("tracks")[0].at("name") == "Band (Channel 2)");
	CHECK(project.at("tracks")[1].at("name") == "Band (Channel 10)");
	for (const Json& part : project.at("midiParts"))
	{
		REQUIRE(part.at("notes").size() == 1);
		const bool drums = part.at("track") == project.at("tracks")[1].at("id");
		CHECK(part.at("notes")[0].at("channel") == (drums ? 9 : 1));
		CHECK(part.at("notes")[0].at("length") == g_quarter);
	}

	// Exported as type 0 again, both channels are in one track.
	const std::filesystem::path copy = dir.path() / "copy.mid";
	CHECK(run(client, "midiFile.export", {{"path", utf8(copy)}, {"type", 0}}).at("tracks") == 1);
	const midifile::File file = midifile::load(utf8(copy));
	CHECK(file.format == 0);
	REQUIRE(file.tracks.size() == 1);
	CHECK(std::count_if(file.tracks[0].events.begin(), file.tracks[0].events.end(),
			  [](const midifile::Event& event) { return (event.status & 0xF0) == 0x90; }) == 2);
}

TEST_CASE("midiFile.import reads names that aren't UTF-8 and the project still saves", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	const std::filesystem::path path = dir.path() / "latin1.mid";
	writeBytes(path,
		join({header(1, 1, 96),
			chunk("MTrk",
				join({{0x00, 0xFF, 0x03, 0x05}, {'B', 0xE4, 's', 's', 'e'}, {0x00, 0xFF, 0x06, 0x06},
					{'B', 'r', 0xFC, 'c', 'k', 'e'}, {0x00, 0x90, 0x3C, 0x64}, {0x60, 0x80, 0x3C, 0x40}, {0x00},
					endOfTrack()}))}));
	run(client, "midiFile.import", {{"path", utf8(path)}});
	const Json project = run(client, "project.get");
	REQUIRE(project.at("tracks").size() == 1);
	CHECK(project.at("tracks")[0].at("name") == "B\xC3\xA4sse");
	REQUIRE(project.at("markers").size() == 1);
	CHECK(project.at("markers")[0].at("name") ==
		"Br\xC3\xBC"
		"cke");

	const std::filesystem::path saved = dir.path() / "song.dawproject";
	run(client, "project.save", {{"path", utf8(saved)}});
	CHECK(std::filesystem::exists(saved));
}

TEST_CASE("midiFile commands reject what they can't do", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	const std::string path = utf8(dir.path() / "out.mid");
	CHECK_THAT(reject(client, "midiFile.export", {{"path", path}, {"ppq", 10}}), ContainsSubstring("ppq"));
	CHECK_THAT(reject(client, "midiFile.export", {{"path", path}, {"range", {{"start", 10}, {"end", 10}}}}),
		ContainsSubstring("range.end"));
	const ObjectId audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	CHECK_THAT(reject(client, "midiFile.export", {{"path", path}, {"tracks", {audio}}}),
		ContainsSubstring("not a MIDI or instrument track"));
	CHECK_THAT(reject(client, "midiFile.export", {{"path", utf8(dir.path() / "missing" / "out.mid")}}),
		ContainsSubstring("can't write"));

	CHECK_THAT(
		reject(client, "midiFile.import", {{"path", utf8(dir.path() / "none.mid")}}), ContainsSubstring("can't open"));
	const std::filesystem::path notMidi = dir.path() / "text.mid";
	writeBytes(notMidi, {'h', 'e', 'l', 'l', 'o'});
	CHECK_THAT(reject(client, "midiFile.import", {{"path", utf8(notMidi)}}), ContainsSubstring("not a MIDI file"));
	CHECK(run(client, "project.get").at("tracks").size() == 1); // nothing imported
}
