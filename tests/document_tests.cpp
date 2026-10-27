// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;

namespace
{
	// At the default 120 BPM and 48 kHz, a quarter note lasts 24000 samples.
	constexpr int64_t g_quarterSamples = 24000;

	// Runs a command that must succeed and returns its result.
	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	// Runs a command that must fail and returns why it failed.
	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	ObjectId addTrack(Client& client, Json parameters)
	{
		return run(client, "track.add", std::move(parameters)).at("track").at("id").get<ObjectId>();
	}

	Json track(Client& client, ObjectId id)
	{
		const Json project = run(client, "project.get");
		for (const Json& candidate : project.at("tracks"))
			if (candidate.at("id").get<ObjectId>() == id)
				return candidate;
		FAIL("no track " << id);
		return nullptr;
	}

	Json channelOf(Client& client, ObjectId trackId)
	{
		const ObjectId channel = track(client, trackId).at("channel").get<ObjectId>();
		const Json project = run(client, "project.get");
		for (const Json& candidate : project.at("channels"))
			if (candidate.at("id").get<ObjectId>() == channel)
				return candidate;
		FAIL("track " << trackId << " has no channel");
		return nullptr;
	}

	// The snapshot channel of a track.
	const RenderChannel& renderChannel(const RenderSnapshot& snapshot, ObjectId trackId)
	{
		for (const RenderChannel& channel : snapshot.channels)
			if (channel.track == trackId)
				return channel;
		FAIL("no snapshot channel for track " << trackId);
		return snapshot.channels.front();
	}

	// A clip of a 4-second stereo file at 48 kHz.
	ObjectId addClip(Client& client)
	{
		const ObjectId media = run(client, "media.add",
			{{"path", "/audio/loop.wav"}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 192000}})
								   .at("media")
								   .at("id")
								   .get<ObjectId>();
		return run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	}
}

TEST_CASE("a new project has a stereo input and a stereo output bus", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json project = run(client, "project.get");
	CHECK(project.at("sampleRate").get<double>() == 48000.0);
	REQUIRE(project.at("busses").size() == 2);
	CHECK(project.at("busses")[0].at("direction") == "input");
	CHECK(project.at("busses")[1].at("direction") == "output");
	CHECK(project.at("busses")[1].at("channels") == 2);
	CHECK(project.at("tracks").empty());

	const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
	CHECK(snapshot.sampleRate == 48000.0);
	REQUIRE(snapshot.busses.size() == 2);
	CHECK(snapshot.busses[1].direction == BusDirection::Output);
	CHECK(snapshot.channels.empty());
}

TEST_CASE("tracks get ids, default names and channels routed to the main output", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId output = run(client, "project.get").at("busses")[1].at("id").get<ObjectId>();

	const Json added = run(client, "track.add", {{"type", "audio"}});
	const Json& audio = added.at("track");
	CHECK(audio.at("name") == "Audio 01");
	CHECK(audio.at("timeBase") == "musical");
	CHECK(added.at("channel").at("output").get<ObjectId>() == output);
	CHECK(added.at("channel").at("channels") == 2);

	const ObjectId mono = addTrack(client, {{"type", "audio"}, {"name", "Vocals"}, {"channels", 1}});
	const ObjectId instrument = addTrack(client, {{"type", "instrument"}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	const ObjectId group = addTrack(client, {{"type", "group"}});
	const ObjectId effect = addTrack(client, {{"type", "effect"}, {"color", "#ff8000"}});
	CHECK(track(client, mono).at("name") == "Vocals");
	CHECK(track(client, instrument).at("name") == "Instrument 01");
	CHECK(track(client, effect).at("color") == "#ff8000");
	CHECK(channelOf(client, midi).at("output").get<ObjectId>() == 0);

	const std::vector<ObjectId> ids{audio.at("id").get<ObjectId>(), mono, instrument, midi, group, effect};
	const Json tracks = run(client, "project.get").at("tracks");
	REQUIRE(tracks.size() == ids.size());
	for (size_t i = 0; i < ids.size(); ++i)
		CHECK(tracks[i].at("id").get<ObjectId>() == ids[i]);

	const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
	REQUIRE(snapshot.channels.size() == ids.size());
	CHECK(renderChannel(snapshot, mono).channels == 1);
	CHECK(renderChannel(snapshot, mono).output.kind == RenderTarget::Kind::Bus);
	CHECK(renderChannel(snapshot, mono).output.index == 1);
	CHECK(renderChannel(snapshot, midi).output.kind == RenderTarget::Kind::None);
	CHECK(renderChannel(snapshot, group).type == TrackType::Group);
}

TEST_CASE("every change is announced to every client", "[unit]")
{
	Engine engine;
	Client sender(engine);
	Client listener(engine);
	const ObjectId id = addTrack(sender, {{"type", "audio"}});
	run(sender, "track.set", {{"track", id}, {"name", "Drums"}});

	std::vector<std::string> names;
	while (const auto message = listener.poll())
		if (const auto* event = std::get_if<Event>(&*message))
		{
			names.push_back(event->name);
			if (event->name == "track.changed")
				CHECK(event->data.at("track").at("name") == "Drums");
		}
	CHECK(names == std::vector<std::string>{"track.added", "track.changed"});
}

TEST_CASE("a folder's group channel sums the tracks added to it", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId plain = addTrack(client, {{"type", "folder"}});
	CHECK(track(client, plain).at("channel").get<ObjectId>() == 0);

	const ObjectId folder = addTrack(client, {{"type", "folder"}, {"groupChannel", true}});
	const ObjectId inside = addTrack(client, {{"type", "audio"}, {"parent", folder}});
	CHECK(track(client, inside).at("parent").get<ObjectId>() == folder);
	CHECK(channelOf(client, inside).at("output") == channelOf(client, folder).at("id"));

	const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
	REQUIRE(snapshot.channels.size() == 2); // the plain folder has no channel
	CHECK(snapshot.channels[0].track == folder);
	CHECK(snapshot.channels[1].output.kind == RenderTarget::Kind::Channel);
	CHECK(snapshot.channels[1].output.index == 0);
}

TEST_CASE("channels can't be routed in a loop", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json a = channelOf(client, addTrack(client, {{"type", "group"}}));
	const Json b = channelOf(client, addTrack(client, {{"type", "group"}}));
	run(client, "channel.set", {{"channel", a.at("id")}, {"output", b.at("id")}});
	const uint64_t revision = engine.renderSnapshots().acquire().revision;

	CHECK_THAT(
		reject(client, "channel.set", {{"channel", b.at("id")}, {"output", a.at("id")}}), ContainsSubstring("loop"));
	CHECK_THAT(
		reject(client, "channel.set", {{"channel", a.at("id")}, {"output", a.at("id")}}), ContainsSubstring("loop"));
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	CHECK_THAT(reject(client, "channel.set", {{"channel", a.at("id")}, {"output", channelOf(client, audio).at("id")}}),
		ContainsSubstring("group"));
	CHECK(engine.renderSnapshots().acquire().revision == revision + 1); // only track.add changed something
}

TEST_CASE("tracks move between folders, and removing a folder removes what it contains", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId first = addTrack(client, {{"type", "audio"}});
	const ObjectId folder = addTrack(client, {{"type", "folder"}, {"groupChannel", true}});
	const ObjectId inside = addTrack(client, {{"type", "midi"}, {"parent", folder}});
	const ObjectId group = addTrack(client, {{"type", "group"}});
	run(client, "channel.set",
		{{"channel", channelOf(client, first).at("id")}, {"output", channelOf(client, folder).at("id")}});

	run(client, "track.move", {{"track", group}, {"parent", folder}, {"index", 0}});
	Json tracks = run(client, "project.get").at("tracks");
	REQUIRE(tracks.size() == 4);
	CHECK(tracks[2].at("id").get<ObjectId>() == group);
	CHECK(tracks[3].at("id").get<ObjectId>() == inside);
	CHECK_THAT(reject(client, "track.move", {{"track", folder}, {"parent", folder}, {"index", 0}}),
		ContainsSubstring("into itself"));

	run(client, "track.remove", {{"track", folder}});
	tracks = run(client, "project.get").at("tracks");
	REQUIRE(tracks.size() == 1);
	CHECK(tracks[0].at("id").get<ObjectId>() == first);
	CHECK(channelOf(client, first).at("output").get<ObjectId>() == 0); // its output went away
	CHECK(run(client, "project.get").at("channels").size() == 1);
	CHECK(engine.renderSnapshots().acquire().channels.size() == 1);
}

TEST_CASE("audio events play sections of clips at sample positions", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId clip = addClip(client);
	const ObjectId musical = addTrack(client, {{"type", "audio"}});
	const ObjectId linear = addTrack(client, {{"type", "audio"}, {"timeBase", "linear"}});

	const Json event = run(client, "audioEvent.add",
		{{"track", musical}, {"clip", clip}, {"position", 4 * g_ticksPerQuarter}, {"offset", 1000}, {"length", 48000}})
						   .at("event");
	CHECK(event.at("track").get<ObjectId>() == musical);
	run(client, "audioEvent.add", {{"track", linear}, {"clip", clip}, {"position", 12345}});

	const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
	REQUIRE(snapshot.media.size() == 1);
	CHECK(snapshot.media[0].path == "/audio/loop.wav");
	const RenderChannel& first = renderChannel(snapshot, musical);
	REQUIRE(first.audioEvents.size() == 1);
	CHECK(first.audioEvents[0].start == 4 * g_quarterSamples);
	CHECK(first.audioEvents[0].offset == 1000);
	CHECK(first.audioEvents[0].length == 48000);
	CHECK(first.audioEvents[0].media == 0);
	const RenderChannel& second = renderChannel(snapshot, linear);
	REQUIRE(second.audioEvents.size() == 1);
	CHECK(second.audioEvents[0].start == 12345);
	CHECK(second.audioEvents[0].length == 192000); // the whole clip by default
}

TEST_CASE("audio events stay within their clip and on audio tracks", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId clip = addClip(client);
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	CHECK_THAT(
		reject(client, "audioEvent.add", {{"track", audio}, {"clip", clip}, {"position", 0}, {"offset", 192000}}),
		ContainsSubstring("clip"));
	CHECK_THAT(reject(client, "audioEvent.add",
				   {{"track", audio}, {"clip", clip}, {"position", 0}, {"offset", 100}, {"length", 192000}}),
		ContainsSubstring("clip"));
	CHECK_THAT(reject(client, "audioEvent.add", {{"track", audio}, {"clip", clip}, {"position", -1}}),
		ContainsSubstring("position"));
	CHECK_THAT(reject(client, "audioEvent.add", {{"track", midi}, {"clip", clip}, {"position", 0}}),
		ContainsSubstring("audio track"));
	CHECK_THAT(reject(client, "audioEvent.add", {{"track", audio}, {"clip", 999}, {"position", 0}}),
		ContainsSubstring("no clip 999"));
}

TEST_CASE("MIDI parts hold notes and controllers, compiled to samples in time order", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId instrument = addTrack(client, {{"type", "instrument"}});
	const ObjectId part = run(client, "midiPart.add",
		{{"track", instrument}, {"position", 4 * g_ticksPerQuarter}, {"length", 4 * g_ticksPerQuarter}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
	const Json notes = run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", g_ticksPerQuarter}, {"length", g_ticksPerQuarter}, {"pitch", 64}, {"velocity", 0.5}},
					{{"position", 0}, {"length", g_ticksPerQuarter / 2}, {"pitch", 60}, {"velocity", 1.0},
						{"channel", 9}},
					{{"position", 3 * g_ticksPerQuarter}, {"length", 2 * g_ticksPerQuarter}, {"pitch", 67},
						{"velocity", 0.25}},
					{{"position", 4 * g_ticksPerQuarter}, {"length", g_ticksPerQuarter}, {"pitch", 72},
						{"velocity", 0.25}}}}})
						   .at("notes");
	REQUIRE(notes.size() == 4);
	CHECK(notes[0].at("id").get<ObjectId>() != notes[1].at("id").get<ObjectId>());
	run(client, "controller.add",
		{{"part", part},
			{"controllers",
				{{{"position", g_ticksPerQuarter}, {"number", 7}, {"value", 1.0}},
					{{"position", 0}, {"number", 1}, {"value", 0.0}}}}});

	const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
	const RenderChannel& channel = renderChannel(snapshot, instrument);
	REQUIRE(channel.notes.size() == 3); // the note at the part's end is not played
	CHECK(channel.notes[0].pitch == 60);
	CHECK(channel.notes[0].start == 4 * g_quarterSamples);
	CHECK(channel.notes[0].end == (4 * g_quarterSamples) + (g_quarterSamples / 2));
	CHECK(channel.notes[0].velocity == 65535);
	CHECK(channel.notes[0].channel == 9);
	CHECK(channel.notes[1].pitch == 64);
	CHECK(channel.notes[1].velocity == 32768);
	CHECK(channel.notes[2].pitch == 67);
	CHECK(channel.notes[2].end == 8 * g_quarterSamples); // cut at the part's end
	REQUIRE(channel.controllers.size() == 2);
	CHECK(channel.controllers[0].number == 1);
	CHECK(channel.controllers[0].value == 0);
	CHECK(channel.controllers[1].position == 5 * g_quarterSamples);
	CHECK(channel.controllers[1].value == 0xffffffffU);

	run(client, "note.remove", {{"part", part}, {"notes", {notes[1].at("id")}}});
	CHECK(engine.renderSnapshots().acquire().channels[0].notes.size() == 2);
	CHECK_THAT(reject(client, "note.add",
				   {{"part", part}, {"notes", {{{"position", 0}, {"length", 1}, {"pitch", 128}, {"velocity", 1.0}}}}}),
		ContainsSubstring("pitch"));
	CHECK_THAT(reject(client, "midiPart.add",
				   {{"track", addTrack(client, {{"type", "audio"}})}, {"position", 0}, {"length", 1}}),
		ContainsSubstring("MIDI or instrument track"));
}

TEST_CASE("the snapshot follows the sample rate and the track's time base", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId clip = addClip(client);
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	const ObjectId event =
		run(client, "audioEvent.add", {{"track", audio}, {"clip", clip}, {"position", 2 * g_ticksPerQuarter}})
			.at("event")
			.at("id")
			.get<ObjectId>();

	run(client, "project.set", {{"sampleRate", 44100.0}});
	CHECK(renderChannel(engine.renderSnapshots().acquire(), audio).audioEvents[0].start == 44100);

	run(client, "track.set", {{"track", audio}, {"timeBase", "linear"}});
	const Json events = run(client, "project.get").at("audioEvents");
	REQUIRE(events.size() == 1);
	CHECK(events[0].at("id").get<ObjectId>() == event);
	CHECK(events[0].at("position") == 44100); // now in samples, at the same time
	run(client, "project.set", {{"sampleRate", 48000.0}});
	CHECK(run(client, "project.get").at("audioEvents")[0].at("position") == 48000); // still one second
	CHECK(renderChannel(engine.renderSnapshots().acquire(), audio).audioEvents[0].start == 48000);
}

TEST_CASE("failed commands leave the project and its snapshot unchanged", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	const Json before = run(client, "project.get");
	const uint64_t revision = engine.renderSnapshots().acquire().revision;

	CHECK_THAT(reject(client, "track.add", {{"type", "audio"}, {"parent", audio}}), ContainsSubstring("folder"));
	CHECK_THAT(reject(client, "track.add", {{"type", "audio"}, {"color", "orange"}}), ContainsSubstring("color"));
	CHECK_THAT(reject(client, "track.add", {{"type", "audio"}, {"channels", 0}}), ContainsSubstring("channels"));
	CHECK_THAT(reject(client, "track.set", {{"track", 999}, {"name", "x"}}), ContainsSubstring("no track 999"));
	CHECK_THAT(reject(client, "project.set", {{"sampleRate", 0.0}}), ContainsSubstring("sampleRate"));
	CHECK_THAT(reject(client, "bus.add", {{"direction", "output"}, {"name", "Out"}, {"channels", 99}}),
		ContainsSubstring("channels"));
	reject(client, "track.add", {{"type", "piano"}});

	CHECK(run(client, "project.get") == before);
	CHECK(engine.renderSnapshots().acquire().revision == revision);
}

TEST_CASE("ids are never reused, and objects in use can't be removed", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId removed = addTrack(client, {{"type", "audio"}});
	run(client, "track.remove", {{"track", removed}});
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	CHECK(audio > removed);

	const ObjectId clip = addClip(client);
	const ObjectId media = run(client, "project.get").at("clips")[0].at("media").get<ObjectId>();
	const ObjectId event = run(client, "audioEvent.add", {{"track", audio}, {"clip", clip}, {"position", 0}})
							   .at("event")
							   .at("id")
							   .get<ObjectId>();
	CHECK_THAT(reject(client, "clip.remove", {{"clip", clip}}), ContainsSubstring("used"));
	CHECK_THAT(reject(client, "media.remove", {{"media", media}}), ContainsSubstring("used"));
	run(client, "audioEvent.remove", {{"event", event}});
	run(client, "clip.remove", {{"clip", clip}});
	run(client, "media.remove", {{"media", media}});

	const ObjectId bus = run(client, "bus.add", {{"direction", "output"}, {"name", "Cue"}, {"channels", 2}})
							 .at("bus")
							 .at("id")
							 .get<ObjectId>();
	run(client, "channel.set", {{"channel", channelOf(client, audio).at("id")}, {"output", bus}});
	run(client, "bus.remove", {{"bus", bus}});
	CHECK(channelOf(client, audio).at("output").get<ObjectId>() == 0);
}

TEST_CASE("project.new starts over with an empty project", "[unit]")
{
	Engine engine;
	Client client(engine);
	addTrack(client, {{"type", "audio"}});
	run(client, "project.set", {{"name", "Song"}, {"sampleRate", 96000.0}});
	run(client, "project.new");
	const Json project = run(client, "project.get");
	CHECK(project.at("tracks").empty());
	CHECK(project.at("sampleRate").get<double>() == 48000.0);
	CHECK(engine.renderSnapshots().acquire().channels.empty());
}
