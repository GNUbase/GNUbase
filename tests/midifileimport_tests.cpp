// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI file import options of midiFile.import (#478, reference manual pp. 364-365): position, dissolving type 0
// files, single parts, extracting the first patch and volume/pan, and controllers as automation.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/midifile/smf.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
namespace midifile = gnubase::engine::midifile;
namespace test = gnubase::test;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;
using gnubase::time::Ticks;

namespace
{
	constexpr Ticks g_quarter = g_ticksPerQuarter;

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

	midifile::Event note(int64_t tick, uint8_t channel, uint8_t pitch, bool on)
	{
		return midifile::channelEvent(tick, static_cast<uint8_t>((on ? 0x90U : 0x80U) | channel), pitch, 0x40);
	}

	midifile::Event control(int64_t tick, uint8_t channel, uint8_t number, uint8_t value)
	{
		return midifile::channelEvent(tick, static_cast<uint8_t>(0xB0U | channel), number, value);
	}

	midifile::Event program(int64_t tick, uint8_t channel, uint8_t number)
	{
		return midifile::channelEvent(tick, static_cast<uint8_t>(0xC0U | channel), number);
	}

	midifile::Track track(const std::string& name, std::vector<midifile::Event> events)
	{
		midifile::Track result;
		result.events.push_back(midifile::metaText(0, midifile::g_metaTrackName, name));
		for (midifile::Event& event : events)
			result.events.push_back(std::move(event));
		result.end = result.events.back().tick;
		return result;
	}

	std::string save(const test::TempDir& dir, const std::string& name, const midifile::File& file)
	{
		const std::string path = (dir.path() / name).u8string();
		midifile::save(file, path);
		return path;
	}

	const Json& find(const Json& list, const char* key, const Json& value)
	{
		const auto found =
			std::find_if(list.begin(), list.end(), [key, &value](const Json& item) { return item.at(key) == value; });
		INFO("no item with " << key << " " << value.dump());
		REQUIRE(found != list.end());
		return *found;
	}
}

TEST_CASE("midiFile.import places the file at a position", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempo", 100.0}});

	// 60 BPM and 3/4 from the file's start, a marker and a note a quarter note in.
	midifile::File file;
	file.tracks.push_back(track("Tempo",
		{midifile::metaEvent(0, midifile::g_metaTempo, {0x0F, 0x42, 0x40}),
			midifile::metaEvent(0, midifile::g_metaTimeSignature, {3, 2, 24, 8}),
			midifile::metaText(480, midifile::g_metaMarker, "Verse")}));
	file.tracks.push_back(track("Piano", {note(480, 0, 60, true), note(960, 0, 60, false)}));
	const std::string path = save(dir, "song.mid", file);

	// A bar of 4/4 at 100 BPM is 2.4 seconds.
	const Json imported = run(client, "midiFile.import", {{"path", path}, {"position", 115200}});
	Json project = run(client, "project.get");
	const Json& part = find(project.at("midiParts"), "id", imported.at("parts")[0]);
	CHECK(part.at("position") == 4 * g_quarter);
	CHECK(part.at("notes")[0].at("position") == g_quarter);
	// The project's tempo stays before the position, the file's follows; its signature starts at the bar there.
	REQUIRE(project.at("tempo").size() == 2);
	CHECK(project.at("tempo")[0].at("bpm") == 100.0);
	CHECK(project.at("tempo")[1].at("position") == 4 * g_quarter);
	CHECK(project.at("tempo")[1].at("bpm") == 60.0);
	REQUIRE(project.at("signatures").size() == 2);
	CHECK(project.at("signatures")[1].at("bar") == 1);
	CHECK(project.at("signatures")[1].at("numerator") == 3);
	REQUIRE(project.at("markers").size() == 1);
	CHECK(project.at("markers")[0].at("position") == 5 * g_quarter);

	// Without tempo, the position is in the project's tempo and the project's tempo stays.
	run(client, "history.undo");
	run(client, "midiFile.import", {{"path", path}, {"position", 115200}, {"tempo", false}, {"markers", false}});
	project = run(client, "project.get");
	CHECK(project.at("tempo").size() == 1);
	CHECK(project.at("midiParts")[0].at("position") == 4 * g_quarter);
	CHECK(project.at("markers").empty());

	CHECK_THAT(reject(client, "midiFile.import", {{"path", path}, {"position", -1}}),
		ContainsSubstring("position must not be negative"));
}

namespace
{
	gnubase::time::TempoMap tempoOf(const Json& project)
	{
		std::vector<gnubase::time::TempoPoint> points;
		for (const Json& point : project.at("tempo"))
			points.push_back(
				{point.at("position").get<Ticks>(), point.at("bpm").get<double>(), point.at("ramp").get<bool>()});
		return gnubase::time::TempoMap(points);
	}
}

TEST_CASE("midiFile.import inside a tempo ramp keeps the ramp before the position", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	run(client, "tempo.add", {{"position", 0}, {"bpm", 100.0}, {"ramp", false}});
	run(client, "tempo.add", {{"position", 8 * g_quarter}, {"bpm", 140.0}, {"ramp", true}});
	Json project = run(client, "project.get");
	const gnubase::time::TempoMap before = tempoOf(project);
	const double sampleRate = project.at("sampleRate").get<double>();

	// A file without tempo, a quarter note long, dropped in the middle of the ramp.
	midifile::File file;
	file.tracks.push_back(track("Piano", {note(0, 0, 60, true), note(480, 0, 60, false)}));
	const std::string path = save(dir, "song.mid", file);
	const int64_t position = before.toSamples(4 * g_quarter, sampleRate);
	run(client, "midiFile.import", {{"path", path}, {"position", position}});
	project = run(client, "project.get");
	const gnubase::time::TempoMap after = tempoOf(project);

	// The ramp plays on up to the position, so the file starts where it was dropped, at its own 120 BPM.
	const Ticks start = before.toTicks(position, sampleRate);
	const Json& part = project.at("midiParts")[0];
	CHECK(part.at("position").get<Ticks>() + part.at("notes")[0].at("position").get<Ticks>() == start);
	CHECK(std::abs(after.toSamples(2 * g_quarter, sampleRate) - before.toSamples(2 * g_quarter, sampleRate)) <= 1);
	CHECK(after.bpmAt(3 * g_quarter) == Catch::Approx(before.bpmAt(3 * g_quarter)));
	CHECK(std::abs(after.toSamples(start, sampleRate) - position) <= 1);
	CHECK(after.bpmAt(start) == 120.0);
	// After the file's end, a quarter note after its start wherever the position lands, the project's ramp goes on as
	// it was.
	const Ticks end = start + g_quarter;
	CHECK(after.bpmAt(end - 1) == 120.0);
	CHECK(after.bpmAt(end) == Catch::Approx(before.bpmAt(end)));
	CHECK(after.bpmAt(end + (g_quarter / 2)) == Catch::Approx(before.bpmAt(end + (g_quarter / 2))));
	CHECK(after.bpmAt(7 * g_quarter) == Catch::Approx(before.bpmAt(7 * g_quarter)));
	CHECK(after.bpmAt(8 * g_quarter) == 140.0);
}

TEST_CASE("midiFile.import keeps the project's tempo and signatures after the file's end", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	run(client, "tempo.add", {{"position", 0}, {"bpm", 100.0}, {"ramp", false}});
	run(client, "tempo.add", {{"position", 16 * g_quarter}, {"bpm", 90.0}, {"ramp", false}});
	run(client, "signature.add", {{"bar", 4}, {"numerator", 3}, {"denominator", 4}});

	// 60 BPM in 2/4 for two bars, dropped at bar 2 (a bar of 4/4 at 100 BPM is 2.4 seconds).
	midifile::File file;
	file.tracks.push_back(track("Tempo",
		{midifile::metaEvent(0, midifile::g_metaTempo, {0x0F, 0x42, 0x40}),
			midifile::metaEvent(0, midifile::g_metaTimeSignature, {2, 2, 24, 8})}));
	file.tracks.push_back(track("Piano", {note(0, 0, 60, true), note(1920, 0, 60, false)}));
	const std::string path = save(dir, "song.mid", file);
	run(client, "midiFile.import", {{"path", path}, {"position", 115200}});
	const Json project = run(client, "project.get");

	// The file's tempo and signature over its two bars, then the project's again from where it ends.
	const Json& tempo = project.at("tempo");
	REQUIRE(tempo.size() == 4);
	CHECK(tempo[1].at("position") == 4 * g_quarter);
	CHECK(tempo[1].at("bpm") == 60.0);
	CHECK(tempo[2].at("position") == 8 * g_quarter);
	CHECK(tempo[2].at("bpm") == 100.0);
	CHECK(tempo[3].at("position") == 16 * g_quarter);
	CHECK(tempo[3].at("bpm") == 90.0);
	const Json& signatures = project.at("signatures");
	REQUIRE(signatures.size() == 4);
	CHECK(signatures[1].at("bar") == 1);
	CHECK(signatures[1].at("numerator") == 2);
	CHECK(signatures[2].at("bar") == 3); // at 8 quarter notes, after two bars of 2/4
	CHECK(signatures[2].at("numerator") == 4);
	CHECK(signatures[3].at("bar") == 5); // at 16 quarter notes, where the project's 3/4 was
	CHECK(signatures[3].at("numerator") == 3);
}

TEST_CASE(
	"midiFile.import keeps a constant project tempo and signature for the parts after the file's end", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempo", 100.0}});
	const Ticks bar = 4 * g_quarter;
	const auto keys =
		run(client, "track.add", {{"type", "midi"}, {"name", "Keys"}}).at("track").at("id").get<ObjectId>();
	run(client, "midiPart.add", {{"track", keys}, {"position", 19 * bar}, {"length", bar}});
	Json project = run(client, "project.get");
	const double sampleRate = project.at("sampleRate").get<double>();

	// A bar of 3/4 without tempo, at 120 BPM, dropped at bar 10.
	midifile::File file;
	file.tracks.push_back(track("Tempo", {midifile::metaEvent(0, midifile::g_metaTimeSignature, {3, 2, 24, 8})}));
	file.tracks.push_back(track("Piano", {note(0, 0, 60, true), note(1440, 0, 60, false)}));
	const std::string path = save(dir, "song.mid", file);
	const Ticks start = 9 * bar;
	run(client, "midiFile.import",
		{{"path", path}, {"position", gnubase::time::TempoMap(100.0).toSamples(start, sampleRate)}});
	project = run(client, "project.get");

	// The file's 120 BPM and 3/4 for its bar, then the project's 100 BPM and 4/4 again.
	const Json& tempo = project.at("tempo");
	REQUIRE(tempo.size() == 3);
	CHECK(tempo[1].at("position") == start);
	CHECK(tempo[1].at("bpm") == 120.0);
	CHECK(tempo[2].at("position") == start + (3 * g_quarter));
	CHECK(tempo[2].at("bpm") == 100.0);
	const Json& signatures = project.at("signatures");
	REQUIRE(signatures.size() == 3);
	CHECK(signatures[1].at("bar") == 9);
	CHECK(signatures[1].at("numerator") == 3);
	CHECK(signatures[2].at("bar") == 10);
	CHECK(signatures[2].at("numerator") == 4);
	CHECK(signatures[2].at("denominator") == 4);
	CHECK(project.at("tempoTrackActive") == true);

	// Undo gives back the project's tempo, signature and tempo mode, redo the merged ones again.
	const auto tempoState = [&client]
	{
		const Json tempo = run(client, "project.get");
		return Json{{"tempo", tempo.at("tempo")}, {"signatures", tempo.at("signatures")},
			{"tempoTrackActive", tempo.at("tempoTrackActive")}};
	};
	const Json merged = tempoState();
	run(client, "history.undo");
	const Json undone = tempoState();
	REQUIRE(undone.at("tempo").size() == 1);
	CHECK(undone.at("tempo")[0].at("bpm") == 100.0);
	CHECK(undone.at("signatures").size() == 1);
	CHECK(undone.at("tempoTrackActive") == false);
	CHECK(run(client, "project.get").at("midiParts").size() == 1);
	run(client, "history.redo");
	CHECK(tempoState() == merged);
	CHECK(run(client, "project.get").at("midiParts").size() == 2);
}

TEST_CASE("midiFile.import keeps a constant project tempo and signature for any content after the file's end",
	"[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempo", 100.0}});
	const double sampleRate = run(client, "project.get").at("sampleRate").get<double>();
	const Ticks bar = 4 * g_quarter;
	// The file goes at bar 10 and is a bar of 3/4 at 120 BPM long, a second and a half; in the project's 100 BPM that
	// bar would end later. Bar 20, in ticks and in samples, is after the file's end.
	const Ticks start = 9 * bar;
	const int64_t startSamples = gnubase::time::TempoMap(100.0).toSamples(start, sampleRate);
	const int64_t endSamples = startSamples + std::llround(1.5 * sampleRate);
	const Ticks later = 19 * bar;
	const int64_t laterSamples = gnubase::time::TempoMap(100.0).toSamples(later, sampleRate);
	const auto addTrack = [&client](const char* type, const char* timeBase)
	{ return run(client, "track.add", {{"type", type}, {"timeBase", timeBase}}).at("track").at("id").get<ObjectId>(); };
	bool after = true;
	bool slow = false; // the file at 60 BPM, slower than the project

	// An audio event of length samples (at the project's 48 kHz) on a new audio track; position is in the track's time
	// base. Returns the track.
	const auto addAudioEvent = [&client, &dir, &addTrack](
								   int64_t position, const char* timeBase = "linear", int64_t length = 4000)
	{
		test::Audio audio;
		audio.samples.assign(static_cast<size_t>(length), 0.25F);
		const std::filesystem::path wav = dir.path() / ("tone" + std::to_string(length) + ".wav");
		test::writeWav(wav, audio);
		const Json media = run(client, "media.add", {{"path", wav.u8string()}});
		const Json clip = run(client, "clip.add", {{"media", media.at("media").at("id")}}).at("clip").at("id");
		const ObjectId audioTrack = addTrack("audio", timeBase);
		run(client, "audioEvent.add",
			{{"track", audioTrack}, {"clip", clip}, {"position", position}, {"offset", 0}, {"length", length}});
		return audioTrack;
	};

	SECTION("An audio event on a linear track")
	{
		addAudioEvent(laterSamples);
	}
	SECTION("An audio event that starts before the file's end and ends after it, in the file's tempo")
	{
		// In the project's tempo it would end before the file's end.
		addAudioEvent(endSamples - 2000);
	}
	SECTION("An audio event on a musical track after the end of a file slower than the project")
	{
		// It keeps its ticks: a quarter note after the file's end. Its end in samples in the project's tempo is before
		// the file's end in the file's 60 BPM, which must not matter.
		slow = true;
		addAudioEvent(start + (4 * g_quarter), "musical");
	}
	// An audio event on a musical track keeps its start in ticks and its length in samples: it counts by where it ends
	// in the file's tempo. The file at 120 BPM ends 72000 samples after its start, at 60 BPM 144000.
	SECTION("An audio event on a musical track that ends after the end of a faster file only in the file's tempo")
	{
		// It starts 60000 samples after the file's start and ends at 73000. In the project's 100 BPM it would end
		// 2.95 quarter notes after the start, before the file's 3.
		addAudioEvent(start + ((5 * g_quarter) / 2), "musical", 13000);
	}
	SECTION("An audio event on a musical track that ends before the end of a slower file only in the file's tempo")
	{
		// It starts 120000 samples after the file's start and ends at 140000. In the project's 100 BPM it would end
		// 3.19 quarter notes after the start, after the file's 3.
		slow = true;
		after = false;
		addAudioEvent(start + ((5 * g_quarter) / 2), "musical", 20000);
	}
	SECTION("An audio event on a musical track that starts before the file and ends after it in the file's tempo")
	{
		// A bar before the file's start in the project's 100 BPM, 115200 samples, so it ends 84800 samples after the
		// file's start: 2.94 quarter notes in the project's tempo.
		addAudioEvent(start - bar, "musical", 200000);
	}
	SECTION("A MIDI part on another version of its track")
	{
		const ObjectId keys = addTrack("midi", "musical");
		run(client, "midiPart.add", {{"track", keys}, {"position", later}, {"length", bar}});
		run(client, "trackVersion.add", {{"tracks", {keys}}});
		REQUIRE(run(client, "project.get").at("midiParts").empty());
	}
	SECTION("An audio event on another version of its track")
	{
		run(client, "trackVersion.add", {{"tracks", {addAudioEvent(laterSamples)}}});
		REQUIRE(run(client, "project.get").at("audioEvents").empty());
	}
	SECTION("An audio part on another version of its track")
	{
		const ObjectId audio = addTrack("audio", "musical");
		run(client, "audioPart.add", {{"track", audio}, {"position", later}, {"length", bar}});
		run(client, "trackVersion.add", {{"tracks", {audio}}});
		REQUIRE(run(client, "project.get").at("audioParts").empty());
	}
	SECTION("An audio part on a linear track")
	{
		run(client, "audioPart.add",
			{{"track", addTrack("audio", "linear")}, {"position", laterSamples}, {"length", 48000}});
	}
	SECTION("A MIDI part on a linear track")
	{
		run(client, "midiPart.add",
			{{"track", addTrack("midi", "linear")}, {"position", laterSamples}, {"length", 48000}});
	}
	SECTION("A marker on a linear marker track")
	{
		run(client, "marker.add", {{"track", addTrack("marker", "linear")}, {"position", laterSamples}});
	}
	SECTION("An arranger event")
	{
		addTrack("arranger", "musical");
		run(client, "arranger.add", {{"position", later}, {"end", later + bar}});
	}
	SECTION("A chord event")
	{
		addTrack("chord", "musical");
		run(client, "chord.add", {{"position", later}, {"chord", "Dm7"}});
	}
	SECTION("A scale event")
	{
		addTrack("chord", "musical");
		run(client, "chord.add", {{"position", later}, {"scale", "D dorian"}});
	}
	SECTION("A chord event right at the file's end, which it plays from")
	{
		addTrack("chord", "musical");
		run(client, "chord.add", {{"position", start + (3 * g_quarter)}, {"chord", "Dm7"}});
	}
	SECTION("A scale event right at the file's end")
	{
		addTrack("chord", "musical");
		run(client, "chord.add", {{"position", start + (3 * g_quarter)}, {"scale", "D dorian"}});
	}
	SECTION("A transpose event right at the file's end, in the file's tempo, on a linear transpose track")
	{
		addTrack("transpose", "linear");
		run(client, "transpose.add", {{"position", endSamples}, {"value", 2}});
	}
	SECTION("An automation point right at the file's end, in the file's tempo, on a linear track")
	{
		const ObjectId audio = addTrack("audio", "linear");
		const Json lane = run(client, "automationLane.add", {{"track", audio}, {"target", {{"type", "volume"}}}});
		run(client, "automationPoint.add",
			{{"lane", lane.at("lane").at("id")}, {"points", {{{"position", endSamples}, {"value", -6.0}}}}});
	}
	SECTION("A transpose event on a linear transpose track")
	{
		addTrack("transpose", "linear");
		run(client, "transpose.add", {{"position", laterSamples}, {"value", 2}});
	}
	SECTION("An automation point on a linear track")
	{
		const ObjectId audio = addTrack("audio", "linear");
		const Json lane = run(client, "automationLane.add", {{"track", audio}, {"target", {{"type", "volume"}}}});
		run(client, "automationPoint.add",
			{{"lane", lane.at("lane").at("id")}, {"points", {{{"position", laterSamples}, {"value", -6.0}}}}});
	}
	SECTION("Content that ends before the file's end in the file's tempo counts for nothing")
	{
		after = false;
		// A linear event that ends before the file's end, and one on a musical track that ends before it in the
		// file's tempo, though after it in the project's.
		addAudioEvent(endSamples - 6000);
		addAudioEvent(start + ((5 * g_quarter) / 2), "musical");
		addTrack("chord", "musical");
		run(client, "chord.add", {{"position", bar}, {"chord", "Dm7"}});
		addTrack("arranger", "musical");
		run(client, "arranger.add", {{"position", 0}, {"end", 2 * bar}});
	}

	// A bar of 3/4, without tempo at 120 BPM or at 60 BPM if slow, dropped at bar 10.
	midifile::File file;
	std::vector<midifile::Event> meta = {midifile::metaEvent(0, midifile::g_metaTimeSignature, {3, 2, 24, 8})};
	if (slow)
		meta.push_back(midifile::metaEvent(0, midifile::g_metaTempo, {0x0F, 0x42, 0x40}));
	file.tracks.push_back(track("Tempo", std::move(meta)));
	file.tracks.push_back(track("Piano", {note(0, 0, 60, true), note(1440, 0, 60, false)}));
	const std::string path = save(dir, "song.mid", file);
	run(client, "midiFile.import", {{"path", path}, {"position", startSamples}});
	const Json project = run(client, "project.get");

	// The file's tempo and 3/4 for its bar, then the project's 100 BPM and 4/4 again if anything follows the file;
	// otherwise the file's go on.
	const Json& tempo = project.at("tempo");
	REQUIRE(tempo.size() == (after ? 3U : 2U));
	CHECK(tempo[1].at("position") == start);
	CHECK(tempo[1].at("bpm") == (slow ? 60.0 : 120.0));
	const Json& signatures = project.at("signatures");
	REQUIRE(signatures.size() == (after ? 3U : 2U));
	CHECK(signatures[1].at("bar") == 9);
	CHECK(signatures[1].at("numerator") == 3);
	if (after)
	{
		CHECK(tempo[2].at("position") == start + (3 * g_quarter));
		CHECK(tempo[2].at("bpm") == 100.0);
		CHECK(signatures[2].at("bar") == 10);
		CHECK(signatures[2].at("numerator") == 4);
		CHECK(signatures[2].at("denominator") == 4);
	}
}

TEST_CASE("midiFile.import keeps a type 0 file in one track unless it dissolves it", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	midifile::File file;
	file.format = 0;
	file.tracks.push_back(
		track("Band", {note(0, 9, 36, true), note(0, 1, 48, true), note(480, 9, 36, false), note(480, 1, 48, false)}));
	const std::string path = save(dir, "band.mid", file);

	run(client, "midiFile.import", {{"path", path}, {"dissolve", false}});
	const Json project = run(client, "project.get");
	REQUIRE(project.at("tracks").size() == 1);
	CHECK(project.at("tracks")[0].at("name") == "Band");
	REQUIRE(project.at("midiParts").size() == 1);
	CHECK(project.at("midiParts")[0].at("notes").size() == 2);
}

TEST_CASE("midiFile.import puts the whole file into one part", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	midifile::File file;
	file.tracks.push_back(track("Piano", {note(0, 0, 60, true), note(480, 0, 60, false)}));
	file.tracks.push_back(track("Bass", {note(1920, 1, 36, true), note(2400, 1, 36, false)}));
	const std::string path = save(dir, "song.mid", file);

	// On a new track named after the file.
	const Json imported = run(client, "midiFile.import", {{"path", path}, {"singlePart", true}});
	CHECK(imported.at("tracks").size() == 1);
	Json project = run(client, "project.get");
	REQUIRE(project.at("tracks").size() == 1);
	CHECK(project.at("tracks")[0].at("name") == "song");
	REQUIRE(project.at("midiParts").size() == 1);
	CHECK(project.at("midiParts")[0].at("position") == 0);
	CHECK(project.at("midiParts")[0].at("length") == 8 * g_quarter);
	CHECK(project.at("midiParts")[0].at("notes").size() == 2);

	// On a track of the project.
	const auto target =
		run(client, "track.add", {{"type", "instrument"}, {"name", "Keys"}}).at("track").at("id").get<ObjectId>();
	const Json onTrack = run(client, "midiFile.import",
		{{"path", path}, {"singlePart", true}, {"track", target}, {"position", 96000}, {"tempo", false}});
	CHECK(onTrack.at("tracks").empty());
	project = run(client, "project.get");
	CHECK(project.at("tracks").size() == 2);
	const Json& part = find(project.at("midiParts"), "id", onTrack.at("parts")[0]);
	CHECK(part.at("track") == target);
	CHECK(part.at("position") == 4 * g_quarter);

	// On a linear track, the part's start and length are in samples: 120 BPM at 48 kHz, its notes still in ticks.
	const auto linear =
		run(client, "track.add", {{"type", "midi"}, {"timeBase", "linear"}}).at("track").at("id").get<ObjectId>();
	const Json onLinear = run(client, "midiFile.import",
		{{"path", path}, {"singlePart", true}, {"track", linear}, {"position", 96000}, {"tempo", false}});
	project = run(client, "project.get");
	const Json& linearPart = find(project.at("midiParts"), "id", onLinear.at("parts")[0]);
	CHECK(linearPart.at("position") == 96000);
	CHECK(linearPart.at("length") == 192000);
	REQUIRE(linearPart.at("notes").size() == 2);
	CHECK(linearPart.at("notes")[1].at("position") == 4 * g_quarter);

	const auto audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	CHECK_THAT(reject(client, "midiFile.import", {{"path", path}, {"singlePart", true}, {"track", audio}}),
		ContainsSubstring("MIDI or instrument track"));
	CHECK_THAT(reject(client, "midiFile.import", {{"path", path}, {"track", target}}), ContainsSubstring("singlePart"));
}

TEST_CASE("midiFile.import extracts the first patch and volume and pan into the track's settings", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	// Channel 1: bank 2, program 5, volume 100 and pan 30, then more of each; channel 2's stay in their part.
	midifile::File file;
	file.tracks.push_back(track("Piano",
		{control(0, 0, 0, 0), control(0, 0, 32, 2), program(0, 0, 5), control(0, 0, 7, 100), control(0, 0, 10, 30),
			note(0, 0, 60, true), control(240, 0, 7, 90), program(240, 0, 6), note(480, 0, 60, false)}));
	file.tracks.push_back(
		track("Strings", {program(0, 1, 48), control(0, 1, 7, 80), note(0, 1, 60, true), note(480, 1, 60, false)}));
	const std::string path = save(dir, "song.mid", file);

	const Json imported =
		run(client, "midiFile.import", {{"path", path}, {"extractPatch", true}, {"extractVolumePan", true}});
	const Json project = run(client, "project.get");
	const auto channelOf = [&project](const Json& track)
	{ return find(project.at("channels"), "id", find(project.at("tracks"), "id", track).at("channel")); };
	const Json& piano = channelOf(imported.at("tracks")[0]);
	CHECK(piano.at("program") == 5);
	CHECK(piano.at("bank") == 2);
	CHECK(piano.at("midiVolume") == 100);
	CHECK(piano.at("midiPan") == 30);
	const Json& pianoPart = find(project.at("midiParts"), "id", imported.at("parts")[0]);
	REQUIRE(pianoPart.at("controllers").size() == 2); // the second volume and program change
	for (const Json& controller : pianoPart.at("controllers"))
		CHECK(controller.at("position") == g_quarter / 2);
	const Json& strings = channelOf(imported.at("tracks")[1]);
	CHECK(strings.at("program").is_null());
	CHECK(strings.at("midiVolume").is_null());
	CHECK(find(project.at("midiParts"), "id", imported.at("parts")[1]).at("controllers").size() == 2);

	// Instrument tracks have no such settings: the events stay (the bank selects in the program change).
	run(client, "history.undo");
	const Json instrument = run(client, "midiFile.import",
		{{"path", path}, {"extractPatch", true}, {"extractVolumePan", true}, {"trackType", "instrument"}});
	CHECK(find(run(client, "project.get").at("midiParts"), "id", instrument.at("parts")[0]).at("controllers").size() ==
		5);
}

TEST_CASE("midiFile.import keeps a track that the extraction leaves empty, without a part", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	midifile::File file;
	file.tracks.push_back(track("Setup", {program(0, 0, 5), control(0, 0, 7, 100), control(0, 0, 10, 30)}));
	file.tracks.push_back(track("Piano", {note(0, 0, 60, true), note(480, 0, 60, false)}));
	const std::string path = save(dir, "song.mid", file);

	const Json imported =
		run(client, "midiFile.import", {{"path", path}, {"extractPatch", true}, {"extractVolumePan", true}});
	const Json project = run(client, "project.get");
	REQUIRE(imported.at("tracks").size() == 2);
	REQUIRE(imported.at("parts").size() == 1);
	const Json& setup = find(project.at("tracks"), "id", imported.at("tracks")[0]);
	CHECK(setup.at("name") == "Setup");
	const Json& channel = find(project.at("channels"), "id", setup.at("channel"));
	CHECK(channel.at("program") == 5);
	CHECK(channel.at("midiVolume") == 100);
	CHECK(channel.at("midiPan") == 30);
	REQUIRE(project.at("midiParts").size() == 1);
	CHECK(project.at("midiParts")[0].at("track") == imported.at("tracks")[1]);
}

TEST_CASE("midiFile.import moves controllers into automation lanes", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	midifile::File file;
	file.tracks.push_back(track("Piano",
		{control(0, 0, 1, 0), control(0, 0, 64, 127), note(0, 0, 60, true), control(240, 0, 1, 127),
			control(480, 0, 64, 0), note(480, 0, 60, false)}));
	const std::string path = save(dir, "song.mid", file);

	const Json imported = run(client, "midiFile.import", {{"path", path}, {"automation", true}});
	const Json project = run(client, "project.get");
	REQUIRE(project.at("automation").size() == 1);
	const Json& lane = project.at("automation")[0];
	CHECK(lane.at("track") == imported.at("tracks")[0]);
	CHECK(lane.at("target").at("number") == 1);
	CHECK(lane.at("points").size() == 2);
	// The sustain pedal is a switch: it stays in the part, as with MIDI > Functions > Extract MIDI Automation.
	CHECK(project.at("midiParts")[0].at("controllers").size() == 2);

	// One undo step.
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Import MIDI file");
	run(client, "history.undo");
	const Json undone = run(client, "project.get");
	CHECK(undone.at("tracks").empty());
	CHECK(undone.at("automation").empty());
}
