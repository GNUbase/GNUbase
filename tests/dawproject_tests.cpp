// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
namespace test = gnubase::test;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;

namespace
{
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

	ObjectId idOf(const Json& value, const char* key)
	{
		return value.at(key).at("id").get<ObjectId>();
	}

	std::string utf8(const std::filesystem::path& path)
	{
		return path.u8string();
	}

	void writeWav(const std::string& path, double sampleRate, int channels, size_t frames)
	{
		WriteOptions options;
		options.sampleFormat = SampleFormat::Int16;
		options.sampleRate = sampleRate;
		options.channels = channels;
		const auto writer = AudioFileWriter::create(path, options);
		std::vector<float> samples(frames * static_cast<size_t>(channels));
		for (size_t i = 0; i < samples.size(); ++i)
			samples[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.01) * 0.5);
		writer->write(samples.data(), frames);
		writer->close();
	}

	std::map<std::string, std::string> readZip(const std::string& path)
	{
		mz_zip_archive zip{};
		REQUIRE(mz_zip_reader_init_file(&zip, path.c_str(), 0) != MZ_FALSE);
		std::map<std::string, std::string> entries;
		for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i)
		{
			char name[512];
			mz_zip_reader_get_filename(&zip, i, name, sizeof name);
			size_t size = 0;
			void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
			entries[name] = std::string(static_cast<const char*>(data), size);
			mz_free(data);
		}
		mz_zip_reader_end(&zip);
		return entries;
	}

	void writeZip(const std::string& path, const std::map<std::string, std::string>& entries)
	{
		mz_zip_archive zip{};
		REQUIRE(mz_zip_writer_init_file(&zip, path.c_str(), 0) != MZ_FALSE);
		for (const auto& [name, data] : entries)
			REQUIRE(mz_zip_writer_add_mem(&zip, name.c_str(), data.data(), data.size(), MZ_DEFAULT_LEVEL) != MZ_FALSE);
		REQUIRE(mz_zip_writer_finalize_archive(&zip) != MZ_FALSE);
		mz_zip_writer_end(&zip);
	}

	std::string fileText(const std::string& path)
	{
		return test::readText(std::filesystem::u8path(path));
	}

	// A project with every kind of object GNUbase has, and gaps in its ids.
	void buildProject(Client& client, const std::string& audioFile)
	{
		run(client, "project.set", {{"name", "Song"}, {"sampleRate", 44100.0}});
		const ObjectId mic =
			idOf(run(client, "bus.add", {{"direction", "input"}, {"name", "Mic"}, {"channels", 1}}), "bus");
		const ObjectId cue =
			idOf(run(client, "bus.add", {{"direction", "output"}, {"name", "Cue"}, {"channels", 2}}), "bus");
		const ObjectId folder =
			idOf(run(client, "track.add", {{"type", "folder"}, {"name", "Band"}, {"groupChannel", true}}), "track");
		const Json vocals = run(client, "track.add",
			{{"type", "audio"}, {"name", "Vocals"}, {"channels", 1}, {"parent", folder}, {"color", "#c03040"},
				{"timeBase", "linear"}});
		run(client, "channel.set", {{"channel", vocals.at("channel").at("id")}, {"input", mic}});
		run(client, "track.remove", {{"track", idOf(run(client, "track.add", {{"type", "audio"}}), "track")}}); // a gap
		const Json piano = run(client, "track.add", {{"type", "instrument"}, {"name", "Piano"}});
		const Json midi = run(client, "track.add", {{"type", "midi"}, {"name", "Strings"}});
		run(client, "channel.set",
			{{"channel", midi.at("channel").at("id")}, {"output", piano.at("channel").at("id")}});
		const Json reverb = run(client, "track.add", {{"type", "effect"}, {"name", "Reverb"}});
		run(client, "channel.set", {{"channel", reverb.at("channel").at("id")}, {"output", cue}});
		run(client, "track.add", {{"type", "group"}, {"name", "Sub"}, {"parent", folder}});
		run(client, "track.add", {{"type", "folder"}, {"name", "Empty"}});

		const ObjectId media = idOf(run(client, "media.add", {{"path", audioFile}}), "media");
		const ObjectId clip = idOf(run(client, "clip.add", {{"media", media}, {"name", "Take 1"}}), "clip");
		run(client, "audioEvent.add",
			{{"track", idOf(vocals, "track")}, {"clip", clip}, {"position", 12345}, {"offset", 777}, {"length", 20001},
				{"volume", -3.5}, {"mute", true}});
		const ObjectId part = idOf(run(client, "midiPart.add",
									   {{"track", idOf(piano, "track")}, {"position", 4 * g_ticksPerQuarter},
										   {"length", 8 * g_ticksPerQuarter}, {"name", "Chords"}}),
			"part");
		run(client, "note.add",
			{{"part", part},
				{"notes",
					{{{"position", 0}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 0.7}},
						{{"position", g_ticksPerQuarter / 3}, {"length", 12345}, {"pitch", 64}, {"velocity", 0.123456},
							{"channel", 3}}}}});
		run(client, "controller.add",
			{{"part", part},
				{"controllers",
					{{{"position", 0}, {"number", 7}, {"value", 0.9}},
						{{"position", g_ticksPerQuarter}, {"number", 1}, {"value", 0.333333}, {"channel", 2}},
						{{"position", 10}, {"type", "pitchBend"}, {"value", 0.75}, {"channel", 1}},
						{{"position", 20}, {"type", "channelPressure"}, {"value", 0.5}},
						{{"position", 30}, {"type", "polyPressure"}, {"number", 61}, {"value", 0.25}},
						{{"position", 40}, {"type", "program"}, {"number", 5}, {"bank", 129}},
						{{"position", 50}, {"type", "sysEx"}, {"data", {0xF0, 0x43, 0x10, 0x4C, 0xF7}}}}}});
		run(client, "midiPart.add", {{"track", idOf(midi, "track")}, {"position", 0}, {"length", g_ticksPerQuarter}});
	}

	// The README's example project, exported from Bitwig Studio 5.0 (https://github.com/bitwig/dawproject, MIT).
	const char* const g_bitwigExample = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Project version="1.0">
  <Application name="Bitwig Studio" version="5.0"/>
  <Transport>
    <Tempo max="666.000000" min="20.000000" unit="bpm" value="149.000000" id="id0" name="Tempo"/>
    <TimeSignature denominator="4" numerator="4" id="id1"/>
  </Transport>
  <Structure>
    <Track contentType="notes" loaded="true" id="id2" name="Bass" color="#a2eabf">
      <Channel audioChannels="2" destination="id15" role="regular" solo="false" id="id3">
        <Devices>
          <ClapPlugin deviceID="org.surge-synth-team.surge-xt" deviceName="Surge XT" deviceRole="instrument" loaded="true" id="id7" name="Surge XT">
            <Parameters/>
            <Enabled value="true" id="id8" name="On/Off"/>
            <State path="plugins/d19b1f6e-bbb6-42fe-a6c9-54b41d97a05d.clap-preset"/>
          </ClapPlugin>
        </Devices>
        <Mute value="false" id="id6" name="Mute"/>
        <Pan max="1.000000" min="0.000000" unit="normalized" value="0.500000" id="id5" name="Pan"/>
        <Volume max="2.000000" min="0.000000" unit="linear" value="0.659140" id="id4" name="Volume"/>
      </Channel>
    </Track>
    <Track contentType="audio" loaded="true" id="id9" name="Drumloop" color="#b53bba">
      <Channel audioChannels="2" destination="id15" role="regular" solo="false" id="id10">
        <Mute value="false" id="id13" name="Mute"/>
        <Pan max="1.000000" min="0.000000" unit="normalized" value="0.500000" id="id12" name="Pan"/>
        <Volume max="2.000000" min="0.000000" unit="linear" value="0.177125" id="id11" name="Volume"/>
      </Channel>
    </Track>
    <Track contentType="audio notes" loaded="true" id="id14" name="Master">
      <Channel audioChannels="2" role="master" solo="false" id="id15">
        <Mute value="false" id="id18" name="Mute"/>
        <Pan max="1.000000" min="0.000000" unit="normalized" value="0.500000" id="id17" name="Pan"/>
        <Volume max="2.000000" min="0.000000" unit="linear" value="1.000000" id="id16" name="Volume"/>
      </Channel>
    </Track>
  </Structure>
  <Arrangement id="id19">
    <Lanes timeUnit="beats" id="id20">
      <Lanes track="id2" id="id21">
        <Clips id="id22">
          <Clip time="0.0" duration="8.0" playStart="0.0">
            <Notes id="id23">
              <Note time="0.000000" duration="0.250000" channel="0" key="65" vel="0.787402" rel="0.787402"/>
              <Note time="1.000000" duration="0.250000" channel="0" key="65" vel="0.787402" rel="0.787402"/>
              <Note time="4.000000" duration="0.250000" channel="0" key="65" vel="0.787402" rel="0.787402"/>
              <Note time="5.000000" duration="0.250000" channel="0" key="65" vel="0.787402" rel="0.787402"/>
              <Note time="0.500000" duration="0.250000" channel="0" key="64" vel="0.787402" rel="0.787402"/>
              <Note time="4.500000" duration="0.250000" channel="0" key="64" vel="0.787402" rel="0.787402"/>
              <Note time="1.500000" duration="2.500000" channel="0" key="53" vel="0.787402" rel="0.787402"/>
              <Note time="5.500000" duration="0.250000" channel="0" key="53" vel="0.787402" rel="0.787402"/>
              <Note time="6.000000" duration="2.000000" channel="0" key="53" vel="0.787402" rel="0.787402"/>
            </Notes>
          </Clip>
        </Clips>
      </Lanes>
      <Lanes track="id9" id="id24">
        <Clips id="id25">
          <Clip time="0.0" duration="8.00003433227539" playStart="0.0" loopStart="0.0" loopEnd="8.00003433227539" fadeTimeUnit="beats" fadeInTime="0.0" fadeOutTime="0.0" name="Drumfunk3 170bpm">
            <Clips id="id26">
              <Clip time="0.0" duration="8.00003433227539" contentTimeUnit="beats" playStart="0.0" fadeTimeUnit="beats" fadeInTime="0.0" fadeOutTime="0.0">
                <Warps contentTimeUnit="seconds" timeUnit="beats" id="id28">
                  <Audio algorithm="stretch" channels="2" duration="2.823541666666667" sampleRate="48000" id="id27">
                    <File path="audio/Drumfunk3 170bpm.wav"/>
                  </Audio>
                  <Warp time="0.0" contentTime="0.0"/>
                  <Warp time="8.00003433227539" contentTime="2.823541666666667"/>
                </Warps>
              </Clip>
            </Clips>
          </Clip>
        </Clips>
      </Lanes>
      <Lanes track="id14" id="id29">
        <Clips id="id30"/>
      </Lanes>
    </Lanes>
  </Arrangement>
  <Scenes/>
</Project>
)";

	// Folders, a group, an effect channel, nested clips, an external file, markers and tempo and signature changes.
	const char* const g_richExample = R"(<?xml version="1.0" encoding="UTF-8"?>
<Project version="1.0">
  <Application name="Test" version="1"/>
  <Transport>
    <Tempo unit="bpm" value="100" id="tempo"/>
    <TimeSignature numerator="3" denominator="4" id="signature"/>
  </Transport>
  <Structure>
    <Track id="drums" name="Drums" contentType="tracks">
      <Channel id="drumsBus" role="submix" audioChannels="2" destination="master"/>
      <Track id="kick" name="Kick" contentType="audio">
        <Channel id="kickChannel" role="regular" audioChannels="1" destination="drumsBus"/>
      </Track>
    </Track>
    <Track id="loop" name="Loop" contentType="audio">
      <Channel id="loopChannel" role="regular" audioChannels="2" destination="master"/>
    </Track>
    <Track id="fx" name="Reverb" contentType="audio">
      <Channel id="fxChannel" role="effect" audioChannels="2" destination="master"/>
    </Track>
    <Track id="keys" name="Keys" contentType="notes">
      <Channel id="keysChannel" role="regular" audioChannels="2" destination="master"/>
    </Track>
    <Track id="main" name="Main" contentType="audio notes">
      <Channel id="master" role="master" audioChannels="2"/>
    </Track>
  </Structure>
  <Arrangement id="arrangement">
    <Lanes timeUnit="beats" id="lanes">
      <Lanes track="kick" id="kickLanes">
        <Clips id="kickClips">
          <Clip time="4" duration="2" playStart="0" name="Outer">
            <Clips id="inner">
              <Clip time="1" duration="1" playStart="0.5" contentTimeUnit="seconds">
                <Audio channels="1" sampleRate="48000" duration="2" id="kickAudio"><File path="audio/kick.wav"/></Audio>
              </Clip>
            </Clips>
          </Clip>
        </Clips>
      </Lanes>
      <Lanes track="loop" id="loopLanes" timeUnit="seconds">
        <Clips id="loopClips">
          <Clip time="1.5" duration="0.5" playStart="0" contentTimeUnit="seconds">
            <Audio channels="2" sampleRate="44100" duration="1" id="loopAudio"><File path="loop.wav" external="true"/></Audio>
          </Clip>
        </Clips>
      </Lanes>
      <Lanes track="keys" id="keysLanes">
        <Clips id="keysClips">
          <Clip time="8" duration="4" playStart="1" name="Riff">
            <Lanes id="riff">
              <Notes id="notes">
                <Note time="1" duration="0.5" key="60" vel="0.5" channel="0"/>
                <Note time="2.5" duration="1" key="64" vel="1" channel="0"/>
              </Notes>
              <Points id="modulation" unit="normalized">
                <Target expression="channelController" channel="0" controller="1"/>
                <RealPoint time="1" value="0.25"/>
              </Points>
            </Lanes>
          </Clip>
        </Clips>
      </Lanes>
    </Lanes>
    <Markers id="markers">
      <Marker time="0" name="Intro" color="#ff0000"/>
      <Marker time="12" name="Verse"/>
    </Markers>
    <TempoAutomation id="tempoAutomation" unit="bpm">
      <Target parameter="tempo"/>
      <RealPoint time="0" value="100" interpolation="linear"/>
      <RealPoint time="8" value="120" interpolation="hold"/>
      <RealPoint time="16" value="90"/>
    </TempoAutomation>
    <TimeSignatureAutomation id="signatureAutomation">
      <Target parameter="signature"/>
      <TimeSignaturePoint time="0" numerator="3" denominator="4"/>
      <TimeSignaturePoint time="12" numerator="4" denominator="4"/>
    </TimeSignatureAutomation>
  </Arrangement>
</Project>
)";

	// The highest object id anywhere in project.get.
	ObjectId highestId(const Json& json)
	{
		ObjectId highest = 0;
		if (json.is_object() && json.contains("id") && json.at("id").is_number_unsigned())
			highest = json.at("id").get<ObjectId>();
		if (json.is_structured())
			for (const Json& item : json)
				highest = std::max(highest, highestId(item));
		return highest;
	}

	Json withId(const Json& list, const std::string& key, const Json& value)
	{
		const auto found =
			std::find_if(list.begin(), list.end(), [&](const Json& item) { return item.value(key, Json()) == value; });
		REQUIRE(found != list.end());
		return *found;
	}
}

TEST_CASE("a GNUbase project comes back exactly from its DAWproject file", "[integration]")
{
	for (const bool embed : {true, false})
	{
		INFO("embedMedia " << embed);
		const test::TempDir dir;
		const std::string audio = utf8(dir.path() / "take.wav");
		writeWav(audio, 44100.0, 1, 44100);
		Engine engine;
		Client client(engine);
		buildProject(client, audio);
		const Json before = run(client, "project.get");
		const std::string file = utf8(dir.path() / "song.dawproject");
		run(client, "project.save", {{"path", file}, {"embedMedia", embed}});

		run(client, "project.new");
		run(client, "project.load", {{"path", file}, {"mediaFolder", utf8(dir.path() / "media")}});
		Json after = run(client, "project.get");
		const std::string expectedPath = embed ? utf8(dir.path() / "media" / "audio" / "take.wav") : audio;
		CHECK(after.at("media")[0].at("path") == expectedPath);
		after["media"][0]["path"] = audio;
		CHECK(after == before);

		// Ids continue where they would have without saving and loading.
		const ObjectId next = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
		CHECK(next == highestId(before) + 1);
	}
}

TEST_CASE("without GNUbase's entry, a project is read from project.xml", "[integration]")
{
	const test::TempDir dir;
	const std::string audio = utf8(dir.path() / "take.wav");
	writeWav(audio, 44100.0, 1, 44100);
	Engine engine;
	Client client(engine);
	buildProject(client, audio);
	const Json before = run(client, "project.get");
	const std::string file = utf8(dir.path() / "song.dawproject");
	run(client, "project.save", {{"path", file}});
	std::map<std::string, std::string> entries = readZip(file);
	CHECK(entries.count("project.xml") == 1);
	CHECK(entries.count("metadata.xml") == 1);
	CHECK(entries.count("audio/take.wav") == 1);
	REQUIRE(entries.erase("gnubase.json") == 1);
	const std::string other = utf8(dir.path() / "other.dawproject");
	writeZip(other, entries);

	run(client, "project.load", {{"path", other}});
	const Json after = run(client, "project.get");
	// Tracks, their order, types, folders and colors.
	const Json& tracks = after.at("tracks");
	REQUIRE(tracks.size() == before.at("tracks").size());
	for (size_t i = 0; i < tracks.size(); ++i)
	{
		INFO(before.at("tracks")[i].at("name"));
		CHECK(tracks[i].at("name") == before.at("tracks")[i].at("name"));
		CHECK(tracks[i].at("type") == before.at("tracks")[i].at("type"));
		CHECK(tracks[i].at("color") == before.at("tracks")[i].at("color"));
		CHECK((tracks[i].at("parent") == 0) == (before.at("tracks")[i].at("parent") == 0));
	}
	// Routing: Vocals into the folder's group channel, Reverb into the Cue bus.
	const auto channelOf = [&after](const std::string& name)
	{ return withId(after.at("channels"), "id", withId(after.at("tracks"), "name", name).at("channel")); };
	CHECK(channelOf("Vocals").at("output") == channelOf("Band").at("id"));
	CHECK(channelOf("Vocals").at("channels") == 1);
	CHECK(channelOf("Reverb").at("output") == withId(after.at("busses"), "name", "Cue").at("id"));
	// The audio event, to the sample frame, and the notes and controllers.
	REQUIRE(after.at("audioEvents").size() == 1);
	const Json& event = after.at("audioEvents")[0];
	CHECK(event.at("offset") == 777);
	CHECK(std::abs(event.at("length").get<int64_t>() - 20001) <= 1);
	CHECK(event.at("mute") == true); // DAWproject's enable; it has no event volume
	CHECK(event.at("volume") == 0.0);
	const Json& part = withId(after.at("midiParts"), "name", "Chords");
	CHECK(part.at("position") == 4 * g_ticksPerQuarter);
	CHECK(part.at("length") == 8 * g_ticksPerQuarter);
	REQUIRE(part.at("notes").size() == 2);
	const Json& second = withId(part.at("notes"), "pitch", 64);
	CHECK(second.at("position") == g_ticksPerQuarter / 3);
	CHECK(second.at("channel") == 3);
	CHECK(std::abs(second.at("velocity").get<double>() - 0.123456) < 1e-4);
	// Pitch bend and pressure too; DAWproject has no program changes with banks or SysEx.
	REQUIRE(part.at("controllers").size() == 5);
	CHECK(std::abs(withId(part.at("controllers"), "number", 1).at("value").get<double>() - 0.333333) < 1e-6);
	const Json bend = withId(part.at("controllers"), "type", "pitchBend");
	CHECK(bend.at("position") == 10);
	CHECK(bend.at("channel") == 1);
	CHECK(std::abs(bend.at("value").get<double>() - 0.75) < 1e-9);
	CHECK(std::abs(withId(part.at("controllers"), "type", "channelPressure").at("value").get<double>() - 0.5) < 1e-9);
	CHECK(withId(part.at("controllers"), "type", "polyPressure").at("number") == 61);
}

TEST_CASE("a project exported from Bitwig Studio loads", "[integration]")
{
	const test::TempDir dir;
	const std::string loop = utf8(dir.path() / "loop.wav");
	writeWav(loop, 48000.0, 2, 135530); // 2.8235 s, as the example says
	const std::string file = utf8(dir.path() / "bitwig.dawproject");
	writeZip(file, {{"project.xml", g_bitwigExample}, {"audio/Drumfunk3 170bpm.wav", fileText(loop)}});

	Engine engine;
	Client client(engine);
	run(client, "project.load", {{"path", file}});
	const Json project = run(client, "project.get");
	CHECK(project.at("tempo")[0].at("bpm").get<double>() == 149.0);
	CHECK(project.at("signatures")[0].at("numerator") == 4);
	CHECK(withId(project.at("busses"), "direction", "output").at("name") == "Master");
	REQUIRE(project.at("tracks").size() == 2);
	const Json& bass = project.at("tracks")[0];
	CHECK(bass.at("name") == "Bass");
	CHECK(bass.at("type") == "instrument");
	CHECK(bass.at("color") == "#a2eabf");
	CHECK(project.at("tracks")[1].at("type") == "audio");

	REQUIRE(project.at("midiParts").size() == 1);
	const Json& part = project.at("midiParts")[0];
	CHECK(part.at("length") == 8 * g_ticksPerQuarter);
	REQUIRE(part.at("notes").size() == 9);
	const Json& longest = withId(part.at("notes"), "length", 10 * g_ticksPerQuarter / 4);
	CHECK(longest.at("pitch") == 53);
	CHECK(longest.at("position") == 3 * g_ticksPerQuarter / 2);
	CHECK(std::abs(longest.at("velocity").get<double>() - 0.787402) < 1e-4);

	REQUIRE(project.at("audioEvents").size() == 1);
	const Json& event = project.at("audioEvents")[0];
	CHECK(event.at("position") == 0);
	CHECK(event.at("offset") == 0);
	CHECK(event.at("length") == 135530); // eight beats at 149 BPM are longer than the file
	CHECK(project.at("clips")[0].at("name") == "Drumfunk3 170bpm");
	CHECK(project.at("media")[0].at("sampleRate").get<double>() == 48000.0);
	CHECK(std::filesystem::exists(std::filesystem::u8path(project.at("media")[0].at("path").get<std::string>())));

	// What was imported survives GNUbase's own save and load.
	const std::string saved = utf8(dir.path() / "again.dawproject");
	run(client, "project.save", {{"path", saved}, {"embedMedia", false}});
	run(client, "project.load", {{"path", saved}});
	CHECK(run(client, "project.get") == project);
}

TEST_CASE("folders, groups, nested clips, markers and tempo changes load from project.xml", "[integration]")
{
	const test::TempDir dir;
	const std::string kick = utf8(dir.path() / "kick.wav");
	writeWav(kick, 48000.0, 1, 96000);
	writeWav(utf8(dir.path() / "loop.wav"), 44100.0, 2, 44100);
	const std::string file = utf8(dir.path() / "rich.dawproject");
	writeZip(file, {{"project.xml", g_richExample}, {"audio/kick.wav", fileText(kick)}});

	Engine engine;
	Client client(engine);
	run(client, "project.load", {{"path", file}, {"mediaFolder", utf8(dir.path() / "media")}});
	const Json project = run(client, "project.get");

	CHECK(project.at("tempo") ==
		Json::parse(
			R"([{"position": 0, "bpm": 100.0, "ramp": false}, {"position": 13762560, "bpm": 120.0, "ramp": true},
			{"position": 27525120, "bpm": 90.0, "ramp": false}])"));
	CHECK(project.at("signatures") ==
		Json::parse(R"([{"bar": 0, "numerator": 3, "denominator": 4}, {"bar": 4, "numerator": 4, "denominator": 4}])"));
	REQUIRE(project.at("markers").size() == 2);
	CHECK(project.at("markers")[0].at("name") == "Intro");
	CHECK(project.at("markers")[0].at("color") == "#ff0000");
	CHECK(project.at("markers")[1].at("position") == 12 * g_ticksPerQuarter);

	const auto track = [&project](const std::string& name) { return withId(project.at("tracks"), "name", name); };
	const auto channel = [&project, &track](const std::string& name)
	{ return withId(project.at("channels"), "id", track(name).at("channel")); };
	CHECK(track("Drums").at("type") == "folder");
	CHECK(track("Kick").at("parent") == track("Drums").at("id"));
	CHECK(channel("Kick").at("output") == channel("Drums").at("id"));
	CHECK(channel("Kick").at("channels") == 1);
	CHECK(track("Reverb").at("type") == "effect");
	CHECK(track("Keys").at("type") == "instrument");
	CHECK(withId(project.at("busses"), "direction", "output").at("name") == "Main");

	// The kick's inner clip starts a beat into the outer one, half a second into the file.
	const Json kickEvent = withId(project.at("audioEvents"), "track", track("Kick").at("id"));
	CHECK(kickEvent.at("position") == 5 * g_ticksPerQuarter);
	CHECK(kickEvent.at("offset") == 24000);
	const gnubase::time::TempoMap tempo(
		{{0, 100.0, false}, {8 * g_ticksPerQuarter, 120.0, true}, {16 * g_ticksPerQuarter, 90.0, false}});
	const double seconds = tempo.toSeconds(6 * g_ticksPerQuarter) - tempo.toSeconds(5 * g_ticksPerQuarter);
	CHECK(std::abs(kickEvent.at("length").get<double>() - (seconds * 48000.0)) <= 1.0);
	CHECK(std::filesystem::exists(dir.path() / "media" / "audio" / "kick.wav"));

	// The external file stays where it is; its lane counts in seconds.
	const Json loopEvent = withId(project.at("audioEvents"), "track", track("Loop").at("id"));
	CHECK(loopEvent.at("position") == tempo.toTicks(1.5));
	CHECK(loopEvent.at("length") == 22050);
	CHECK(withId(project.at("media"), "sampleRate", 44100.0).at("path") == utf8(dir.path() / "loop.wav"));

	// Notes and controllers count from the clip's playStart, one beat in.
	const Json part = withId(project.at("midiParts"), "track", track("Keys").at("id"));
	CHECK(part.at("name") == "Riff");
	CHECK(part.at("position") == 8 * g_ticksPerQuarter);
	CHECK(withId(part.at("notes"), "pitch", 60).at("position") == 0);
	CHECK(withId(part.at("notes"), "pitch", 64).at("position") == 3 * g_ticksPerQuarter / 2);
	CHECK(withId(part.at("notes"), "pitch", 64).at("velocity").get<double>() == 1.0);
	REQUIRE(part.at("controllers").size() == 1);
	CHECK(part.at("controllers")[0].at("position") == 0);
	CHECK(std::abs(part.at("controllers")[0].at("value").get<double>() - 0.25) < 1e-9);
}

TEST_CASE("a project.xml changed by another application wins over GNUbase's entry", "[integration]")
{
	const test::TempDir dir;
	const std::string audio = utf8(dir.path() / "take.wav");
	writeWav(audio, 44100.0, 1, 44100);
	Engine engine;
	Client client(engine);
	buildProject(client, audio);
	const std::string file = utf8(dir.path() / "song.dawproject");
	run(client, "project.save", {{"path", file}});
	std::map<std::string, std::string> entries = readZip(file);
	std::string& xml = entries.at("project.xml");
	const size_t name = xml.find("name=\"Vocals\"");
	REQUIRE(name != std::string::npos);
	xml.replace(name, 13, "name=\"Lead\"");
	writeZip(file, entries);

	run(client, "project.load", {{"path", file}});
	CHECK(withId(run(client, "project.get").at("tracks"), "name", "Lead").at("type") == "audio");
}

TEST_CASE("unsafe and broken DAWproject files are refused", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);

	const std::string escaping = utf8(dir.path() / "escaping.dawproject");
	std::string xml = g_bitwigExample;
	const std::string inside = "audio/Drumfunk3 170bpm.wav";
	xml.replace(xml.find(inside), inside.size(), "../escaped.wav");
	writeZip(escaping, {{"project.xml", xml}, {"../escaped.wav", "data"}});
	CHECK_THAT(reject(client, "project.load", {{"path", escaping}, {"mediaFolder", utf8(dir.path() / "media")}}),
		ContainsSubstring("unsafe"));
	CHECK_FALSE(std::filesystem::exists(dir.path() / "escaped.wav"));

	const std::string text = utf8(dir.path() / "text.dawproject");
	writeZip(utf8(dir.path() / "empty.dawproject"), {{"readme.txt", "nothing"}});
	{
		const auto writer = AudioFileWriter::create(text, {});
		writer->close();
	}
	CHECK_THAT(reject(client, "project.load", {{"path", text}}), ContainsSubstring("not a DAWproject file"));
	CHECK_THAT(reject(client, "project.load", {{"path", utf8(dir.path() / "empty.dawproject")}}),
		ContainsSubstring("project.xml"));
	CHECK_THAT(reject(client, "project.load", {{"path", utf8(dir.path() / "missing.dawproject")}}),
		ContainsSubstring("missing.dawproject"));
}

TEST_CASE("saving keeps the history, loading starts a new one", "[integration]")
{
	const test::TempDir dir;
	Engine engine;
	Client client(engine);
	run(client, "track.add", {{"type", "audio"}});
	const std::string file = utf8(dir.path() / "song.dawproject");
	run(client, "project.save", {{"path", file}});
	CHECK(run(client, "history.get").at("position") == 1);
	run(client, "project.load", {{"path", file}});
	CHECK(run(client, "history.get").at("steps").empty());
	CHECK(run(client, "project.get").at("tracks").size() == 1);
}
