// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
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

	// The object in list with the given id; fails the test if there is none.
	Json withId(const Json& list, ObjectId id)
	{
		const auto found = std::find_if(
			list.begin(), list.end(), [id](const Json& object) { return object.at("id").get<ObjectId>() == id; });
		REQUIRE(found != list.end());
		return *found;
	}

	Json track(Client& client, ObjectId id)
	{
		return withId(run(client, "project.get").at("tracks"), id);
	}

	Json channelOf(Client& client, ObjectId trackId)
	{
		const ObjectId channel = track(client, trackId).at("channel").get<ObjectId>();
		REQUIRE(channel != 0);
		return withId(run(client, "project.get").at("channels"), channel);
	}

	// The snapshot channel of a track.
	const RenderChannel& renderChannel(const RenderSnapshot& snapshot, ObjectId trackId)
	{
		const auto found = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
			[trackId](const RenderChannel& channel) { return channel.track == trackId; });
		REQUIRE(found != snapshot.channels.end());
		return *found;
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
	CHECK(renderChannel(snapshot, mono).format == ChannelFormat::Mono);
	CHECK(renderChannel(snapshot, mono).output.kind == RenderTarget::Kind::Bus);
	CHECK(renderChannel(snapshot, mono).output.index == 1);
	CHECK(renderChannel(snapshot, midi).output.kind == RenderTarget::Kind::None);
	CHECK(renderChannel(snapshot, group).type == TrackType::Group);
}

TEST_CASE("track.add routes the new channel as channel.set would: input, output and instrument", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId mic = run(client, "bus.add", {{"direction", "input"}, {"name", "Mic"}, {"channels", 1}})
							 .at("bus")
							 .at("id")
							 .get<ObjectId>();
	const ObjectId cue = run(client, "bus.add", {{"direction", "output"}, {"name", "Cue"}, {"channels", 2}})
							 .at("bus")
							 .at("id")
							 .get<ObjectId>();
	const Json group = channelOf(client, addTrack(client, {{"type", "group"}, {"output", cue}}));
	CHECK(group.at("output") == cue);

	const Json audio =
		run(client, "track.add", {{"type", "audio"}, {"input", mic}, {"output", group.at("id")}}).at("channel");
	CHECK(audio.at("input") == mic);
	CHECK(audio.at("output") == group.at("id"));
	CHECK(channelOf(client, audio.at("track").get<ObjectId>()) == audio);
	CHECK(run(client, "track.add", {{"type", "audio"}, {"input", 0}}).at("channel").at("input") == 0);

	const Json instrument =
		run(client, "track.add", {{"type", "instrument"}, {"instrument", "testSynth"}}).at("channel");
	CHECK(instrument.at("instrument") == "testSynth");
	const Json midi = run(client, "track.add", {{"type", "midi"}, {"output", instrument.at("id")}}).at("channel");
	CHECK(midi.at("output") == instrument.at("id"));
	const Json folder =
		run(client, "track.add", {{"type", "folder"}, {"groupChannel", true}, {"output", cue}}).at("channel");
	CHECK(folder.at("output") == cue);

	// What channel.set refuses, track.add refuses too, and adds nothing.
	const Json before = run(client, "project.get");
	CHECK_THAT(reject(client, "track.add", {{"type", "midi"}, {"input", mic}}), ContainsSubstring("input"));
	CHECK_THAT(reject(client, "track.add", {{"type", "audio"}, {"output", mic}}), ContainsSubstring("input bus"));
	CHECK_THAT(reject(client, "track.add", {{"type", "audio"}, {"output", audio.at("id")}}),
		ContainsSubstring("group channel"));
	CHECK_THAT(reject(client, "track.add", {{"type", "midi"}, {"output", group.at("id")}}),
		ContainsSubstring("instrument channel"));
	CHECK_THAT(
		reject(client, "track.add", {{"type", "audio"}, {"instrument", "testSynth"}}), ContainsSubstring("instrument"));
	CHECK_THAT(reject(client, "track.add", {{"type", "folder"}, {"output", cue}}), ContainsSubstring("no channel"));
	CHECK_THAT(reject(client, "track.add", {{"type", "marker"}, {"output", cue}}), ContainsSubstring("no channel"));
	CHECK(run(client, "project.get") == before);
}

TEST_CASE("ruler tracks have a time format and no channel", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json ruler = run(client, "track.add", {{"type", "ruler"}});
	CHECK(ruler.at("channel").is_null());
	CHECK(ruler.at("track").at("name") == "Ruler 01");
	CHECK(ruler.at("track").at("timeFormat") == "barsBeats");
	CHECK(ruler.at("track").at("channel") == 0);

	const ObjectId folder = addTrack(client, {{"type", "folder"}});
	const ObjectId timecode = addTrack(client, {{"type", "ruler"}, {"timeFormat", "timecode"}, {"parent", folder}});
	CHECK(track(client, timecode).at("timeFormat") == "timecode");
	CHECK(track(client, timecode).at("name") == "Ruler 02");
	run(client, "track.set", {{"track", timecode}, {"timeFormat", "seconds"}});
	CHECK(track(client, timecode).at("timeFormat") == "seconds");
	run(client, "history.undo"); // a change of the time format alone is an undo step
	CHECK(track(client, timecode).at("timeFormat") == "timecode");
	run(client, "history.redo");
	CHECK(track(client, timecode).at("timeFormat") == "seconds");
	CHECK_FALSE(track(client, folder).contains("timeFormat")); // only ruler tracks have one

	CHECK_THAT(reject(client, "track.add", {{"type", "audio"}, {"timeFormat", "seconds"}}), ContainsSubstring("ruler"));
	CHECK_THAT(reject(client, "track.set", {{"track", folder}, {"timeFormat", "seconds"}}), ContainsSubstring("ruler"));
	reject(client, "track.add", {{"type", "ruler"}, {"timeFormat", "minutes"}});
	CHECK_THAT(reject(client, "track.add", {{"type", "ruler"}, {"format", "mono"}}), ContainsSubstring("no audio"));
	CHECK(engine.renderSnapshots().acquire().channels.empty());
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
	CHECK(names == std::vector<std::string>{"track.added", "history.changed", "track.changed", "history.changed"});
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

TEST_CASE(
	"tracks moved into a folder with its group channel play through it, and moved out through the main mix", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId main = run(client, "project.get").at("busses")[1].at("id").get<ObjectId>();
	const ObjectId folder = addTrack(client, {{"type", "folder"}, {"groupChannel", true}});
	const ObjectId group = channelOf(client, folder).at("id").get<ObjectId>();
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	const ObjectId instrument = addTrack(client, {{"type", "instrument"}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}, {"output", channelOf(client, instrument).at("id")}});
	const ObjectId plain = addTrack(client, {{"type", "folder"}});
	const ObjectId inside = addTrack(client, {{"type", "audio"}, {"parent", plain}});
	const auto output = [&client](ObjectId track) { return channelOf(client, track).at("output").get<ObjectId>(); };

	// Into it (reference manual p. 222): its tracks' audio goes to the group channel, a MIDI track's stays with its
	// instrument, and so does the audio of tracks in a folder without a group channel inside it.
	run(client, "track.move", {{"track", audio}, {"parent", folder}, {"index", 0}});
	CHECK(output(audio) == group);
	run(client, "track.move", {{"track", midi}, {"parent", folder}, {"index", 1}});
	CHECK(output(midi) == channelOf(client, instrument).at("id").get<ObjectId>());
	run(client, "track.move", {{"track", plain}, {"parent", folder}, {"index", 2}});
	CHECK(output(inside) == group);
	// Moving within it changes nothing, and one undo step takes the move and the routing back.
	run(client, "track.move", {{"track", audio}, {"parent", folder}, {"index", 1}});
	CHECK(output(audio) == group);
	run(client, "history.undo");
	run(client, "history.undo");
	CHECK(track(client, plain).at("parent").get<ObjectId>() == 0);
	CHECK(output(inside) == main);
	run(client, "history.redo");
	CHECK(output(inside) == group);

	// Out of it, to the main mix; between places without a group channel, a track keeps its routing.
	run(client, "track.move", {{"track", audio}, {"parent", 0}, {"index", 0}});
	CHECK(output(audio) == main);
	const ObjectId cue = channelOf(client, addTrack(client, {{"type", "group"}})).at("id").get<ObjectId>();
	run(client, "channel.set", {{"channel", channelOf(client, audio).at("id")}, {"output", cue}});
	const ObjectId empty = addTrack(client, {{"type", "folder"}});
	run(client, "track.move", {{"track", audio}, {"parent", empty}, {"index", 0}});
	CHECK(output(audio) == cue);
	// A folder with a group channel inside another one plays through the outer one; its tracks stay with it.
	const ObjectId nested = addTrack(client, {{"type", "folder"}, {"groupChannel", true}});
	const ObjectId deep = addTrack(client, {{"type", "audio"}, {"parent", nested}});
	run(client, "track.move", {{"track", nested}, {"parent", folder}, {"index", 0}});
	CHECK(output(nested) == group);
	CHECK(output(deep) == channelOf(client, nested).at("id").get<ObjectId>());
	// A track added to a folder inside one with a group channel plays through that group channel.
	CHECK(output(addTrack(client, {{"type", "audio"}, {"parent", plain}})) == group);
}

TEST_CASE("a folder's group channel goes on and off, and the tracks inside follow it", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId main = run(client, "project.get").at("busses")[1].at("id").get<ObjectId>();
	const ObjectId folder = addTrack(client, {{"type", "folder"}, {"name", "Band"}});
	const ObjectId audio = addTrack(client, {{"type", "audio"}, {"parent", folder}});
	const ObjectId nested = addTrack(client, {{"type", "folder"}, {"parent", folder}, {"groupChannel", true}});
	const ObjectId deep = addTrack(client, {{"type", "audio"}, {"parent", nested}});
	const ObjectId outside = addTrack(client, {{"type", "audio"}});
	const auto output = [&client](ObjectId track) { return channelOf(client, track).at("output").get<ObjectId>(); };

	// On (reference manual p. 224): the folder gets a channel on the main mix, which its tracks play through.
	const Json on = run(client, "track.set", {{"track", folder}, {"groupChannel", true}});
	const ObjectId group = on.at("track").at("channel").get<ObjectId>();
	REQUIRE(group != 0);
	CHECK(output(folder) == main);
	CHECK(output(audio) == group);
	CHECK(output(nested) == group);
	CHECK(output(deep) == channelOf(client, nested).at("id").get<ObjectId>());
	CHECK(renderChannel(engine.renderSnapshots().acquire(), folder).track == folder);
	CHECK(run(client, "history.get").at("steps").size() == 6);

	// Off: the channel and its automation go, the tracks inside play through the main mix, and other channels
	// routed to it are disconnected.
	run(client, "channel.set", {{"channel", channelOf(client, outside).at("id")}, {"output", group}});
	run(client, "automationLane.add", {{"track", folder}, {"target", {{"type", "volume"}}}});
	run(client, "track.set", {{"track", folder}, {"groupChannel", false}});
	CHECK(track(client, folder).at("channel").get<ObjectId>() == 0);
	CHECK(track(client, folder).at("name") == "Band");
	CHECK(output(audio) == main);
	CHECK(output(nested) == main);
	CHECK(output(outside) == 0);
	CHECK(run(client, "project.get").at("automation").empty());
	CHECK(run(client, "project.get").at("channels").size() == 4);
	run(client, "history.undo");
	CHECK(track(client, folder).at("channel").get<ObjectId>() == group);
	CHECK(output(audio) == group);
	CHECK(run(client, "project.get").at("automation").size() == 1);

	CHECK_THAT(reject(client, "track.set", {{"track", audio}, {"groupChannel", false}}),
		ContainsSubstring("only folder tracks"));
}

TEST_CASE("track.add with tracks moves them into a new folder", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId first = addTrack(client, {{"type", "audio"}});
	const ObjectId second = addTrack(client, {{"type", "audio"}});
	const ObjectId third = addTrack(client, {{"type", "group"}});

	// Move Selected Tracks to New Folder with Group Channel (reference manual p. 223), where the first one was.
	const Json added = run(
		client, "track.add", {{"type", "folder"}, {"index", 1}, {"groupChannel", true}, {"tracks", {third, second}}});
	const ObjectId folder = added.at("track").at("id").get<ObjectId>();
	const ObjectId group = added.at("channel").at("id").get<ObjectId>();
	const Json tracks = run(client, "project.get").at("tracks");
	REQUIRE(tracks.size() == 4);
	CHECK(tracks[0].at("id").get<ObjectId>() == first);
	CHECK(tracks[1].at("id").get<ObjectId>() == folder);
	CHECK(tracks[2].at("id").get<ObjectId>() == third);
	CHECK(tracks[3].at("id").get<ObjectId>() == second);
	CHECK(channelOf(client, second).at("output").get<ObjectId>() == group);
	CHECK(channelOf(client, third).at("output").get<ObjectId>() == group);
	run(client, "history.undo"); // one step
	CHECK(run(client, "project.get").at("tracks").size() == 3);
	CHECK(track(client, second).at("parent").get<ObjectId>() == 0);

	CHECK_THAT(
		reject(client, "track.add", {{"type", "audio"}, {"tracks", {first}}}), ContainsSubstring("only folders"));
	const ObjectId outer = addTrack(client, {{"type", "folder"}});
	CHECK_THAT(reject(client, "track.add", {{"type", "folder"}, {"parent", outer}, {"tracks", {outer}}}),
		ContainsSubstring("into itself"));
	CHECK(run(client, "project.get").at("tracks").size() == 4);
}

TEST_CASE("with editing.applyFolderColor, folders give their color to the tracks inside them", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId folder = addTrack(client, {{"type", "folder"}});
	const ObjectId inside = addTrack(client, {{"type", "audio"}, {"parent", folder}, {"color", "#102030"}});
	const ObjectId outside = addTrack(client, {{"type", "audio"}});

	// Off, the default: a folder's color is its own.
	run(client, "track.set", {{"track", folder}, {"color", "#c08020"}});
	CHECK(track(client, inside).at("color") == "#102030");
	run(client, "track.move", {{"track", outside}, {"parent", folder}, {"index", 0}});
	CHECK(track(client, outside).at("color").is_null());
	run(client, "track.move", {{"track", outside}, {"parent", 0}, {"index", 0}});

	// On (reference manual p. 221): the tracks inside take a new color, and so do tracks moved or added there.
	run(client, "settings.set", {{"key", "editing.applyFolderColor"}, {"value", true}});
	run(client, "track.set", {{"track", folder}, {"color", "#2080c0"}});
	CHECK(track(client, inside).at("color") == "#2080c0");
	run(client, "track.move", {{"track", outside}, {"parent", folder}, {"index", 0}});
	CHECK(track(client, outside).at("color") == "#2080c0");
	CHECK(track(client, addTrack(client, {{"type", "midi"}, {"parent", folder}})).at("color") == "#2080c0");
	CHECK(track(client, addTrack(client, {{"type", "midi"}, {"parent", folder}, {"color", "#ffffff"}})).at("color") ==
		"#ffffff");
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

TEST_CASE("audio events have a volume in dB and can be muted, which the render snapshot applies", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId clip = addClip(client);
	const ObjectId audio = addTrack(client, {{"type", "audio"}, {"timeBase", "linear"}});
	const Json event = run(client, "audioEvent.add", {{"track", audio}, {"clip", clip}, {"position", 0}}).at("event");
	CHECK(event.at("volume") == 0.0);
	CHECK(event.at("mute") == false);
	const ObjectId id = event.at("id").get<ObjectId>();

	const Json changed = run(client, "audioEvent.set", {{"event", id}, {"volume", -6.0}, {"mute", true}}).at("event");
	CHECK(changed.at("volume") == -6.0);
	CHECK(changed.at("mute") == true);
	const RenderAudioEvent& compiled = renderChannel(engine.renderSnapshots().acquire(), audio).audioEvents[0];
	CHECK_THAT(compiled.gain, WithinAbs(std::pow(10.0, -6.0 / 20.0), 1e-6));
	CHECK(compiled.muted);

	run(client, "history.undo");
	CHECK(run(client, "project.get").at("audioEvents")[0].at("volume") == 0.0);
	CHECK_THAT(reject(client, "audioEvent.set", {{"event", id}, {"volume", 6.5}}), ContainsSubstring("volume"));
	CHECK_THAT(reject(client, "audioEvent.add", {{"track", audio}, {"clip", clip}, {"position", 0}, {"volume", -200}}),
		ContainsSubstring("volume"));
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

TEST_CASE("the snapshot has a MIDI track's parts by start, and each controller event knows its part", "[unit]")
{
	// For chasing limited to the parts under the position. The later part is added first.
	Engine engine;
	Client client(engine);
	const ObjectId track = addTrack(client, {{"type", "midi"}});
	ObjectId parts[2] = {};
	for (const int i : {1, 0})
	{
		parts[i] = run(client, "midiPart.add",
			{{"track", track}, {"position", 4 * g_ticksPerQuarter * i}, {"length", 4 * g_ticksPerQuarter}})
					   .at("part")
					   .at("id")
					   .get<ObjectId>();
		run(client, "controller.add",
			{{"part", parts[i]}, {"controllers", {{{"position", 0}, {"number", 7}, {"value", 0.5}}}}});
	}

	const RenderChannel& channel = renderChannel(engine.renderSnapshots().acquire(), track);
	REQUIRE(channel.midiParts.size() == 2);
	CHECK(channel.midiParts[0].start == 0);
	CHECK(channel.midiParts[0].end == 4 * g_quarterSamples);
	CHECK(channel.midiParts[1].start == 4 * g_quarterSamples);
	REQUIRE(channel.controllers.size() == 2);
	CHECK(channel.controllers[0].part == 0);
	CHECK(channel.controllers[1].part == 1);
}

TEST_CASE("the snapshot carries the project's tempo map, for processors that follow the tempo", "[unit]")
{
	Project project;
	project.tempo = gnubase::time::TempoMap({{0, 90.0, false}, {4 * g_ticksPerQuarter, 150.0, true}});
	const std::unique_ptr<const RenderSnapshot> snapshot = compile(project, 1);
	CHECK(snapshot->tempo == project.tempo);
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

TEST_CASE("muted channels are silent; soloed ones silence the others, except where they play through and what plays "
		  "into them",
	"[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId group = addTrack(client, {{"type", "group"}});
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	const ObjectId other = addTrack(client, {{"type", "audio"}});
	const ObjectId instrument = addTrack(client, {{"type", "instrument"}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	const auto set = [&client](ObjectId trackId, Json parameters)
	{
		parameters["channel"] = channelOf(client, trackId).at("id");
		return run(client, "channel.set", parameters).at("channel");
	};
	set(audio, {{"output", channelOf(client, group).at("id")}});
	set(midi, {{"output", channelOf(client, instrument).at("id")}});
	const ObjectId part = run(client, "midiPart.add", {{"track", midi}, {"position", 0}, {"length", g_ticksPerQuarter}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
	run(client, "note.add",
		{{"part", part}, {"notes", {{{"position", 0}, {"length", 100}, {"pitch", 60}, {"velocity", 0.5}}}}});
	const auto silentOnes = [&engine, ids = std::vector<ObjectId>{group, audio, other, instrument, midi}]
	{
		const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
		std::vector<ObjectId> result;
		for (const ObjectId id : ids)
			if (renderChannel(snapshot, id).silent)
				result.push_back(id);
		return result;
	};
	CHECK(channelOf(client, audio).at("mute") == false);
	CHECK(channelOf(client, audio).at("solo") == false);
	CHECK(silentOnes().empty());

	CHECK(set(other, {{"mute", true}}).at("mute") == true);
	CHECK(silentOnes() == std::vector<ObjectId>{other});
	set(other, {{"mute", false}});

	// A soloed group keeps the tracks that play into it, a soloed track the group it plays through, a soloed MIDI
	// track its instrument.
	CHECK(set(group, {{"solo", true}}).at("solo") == true);
	CHECK(silentOnes() == std::vector<ObjectId>{other, instrument, midi});
	set(group, {{"solo", false}});
	set(audio, {{"solo", true}});
	CHECK(silentOnes() == std::vector<ObjectId>{other, instrument, midi});
	set(audio, {{"solo", false}});
	set(midi, {{"solo", true}});
	CHECK(silentOnes() == std::vector<ObjectId>{group, audio, other});
	CHECK(renderChannel(engine.renderSnapshots().acquire(), midi).notes.size() == 1);
	// Soloing two tracks plays both; a muted one stays silent, though what it plays through is heard, and a silent
	// MIDI track's notes don't play.
	set(other, {{"solo", true}});
	CHECK(silentOnes() == std::vector<ObjectId>{group, audio});
	set(midi, {{"mute", true}});
	CHECK(silentOnes() == std::vector<ObjectId>{group, audio, midi});
	CHECK(renderChannel(engine.renderSnapshots().acquire(), midi).notes.empty());

	run(client, "history.undo", {{"steps", 2}});
	CHECK(channelOf(client, midi).at("mute") == false);
	CHECK(channelOf(client, other).at("solo") == false);
	CHECK(silentOnes() == std::vector<ObjectId>{group, audio, other});
}

TEST_CASE("disabled tracks play nothing; only audio, instrument and MIDI tracks can be disabled", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	run(client, "audioEvent.add", {{"track", audio}, {"clip", addClip(client)}, {"position", 0}});
	CHECK(track(client, audio).at("enabled") == true);
	CHECK(renderChannel(engine.renderSnapshots().acquire(), audio).audioEvents.size() == 1);

	CHECK(run(client, "track.set", {{"track", audio}, {"enabled", false}}).at("track").at("enabled") == false);
	{
		const RenderChannel& disabled = renderChannel(engine.renderSnapshots().acquire(), audio);
		CHECK(disabled.silent);
		CHECK(disabled.audioEvents.empty());
	}
	// A disabled track keeps its events and channel settings.
	CHECK(run(client, "project.get").at("audioEvents").size() == 1);

	for (const char* type : {"instrument", "midi"})
	{
		const ObjectId id = addTrack(client, {{"type", type}});
		run(client, "track.set", {{"track", id}, {"enabled", false}});
		CHECK(renderChannel(engine.renderSnapshots().acquire(), id).silent);
	}
	for (const char* type : {"group", "effect", "folder", "marker", "ruler"})
	{
		INFO(type);
		const ObjectId id = addTrack(client, {{"type", type}});
		CHECK_THAT(reject(client, "track.set", {{"track", id}, {"enabled", false}}), ContainsSubstring("disabled"));
		run(client, "track.remove", {{"track", id}});
	}
	run(client, "history.undo", {{"steps", 15}});
	CHECK(track(client, audio).at("enabled") == true);
	CHECK(renderChannel(engine.renderSnapshots().acquire(), audio).audioEvents.size() == 1);
}

TEST_CASE("track.duplicate copies a track below it with its channel, events, parts and automation", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId first = addTrack(client, {{"type", "audio"}});
	const ObjectId audio = addTrack(client, {{"type", "audio"}, {"name", "Guitar"}, {"color", "#c03040"}});
	const ObjectId last = addTrack(client, {{"type", "audio"}});
	run(client, "channel.set", {{"channel", channelOf(client, audio).at("id")}, {"volume", -6.0}, {"mute", true}});
	run(client, "audioEvent.add", {{"track", audio}, {"clip", addClip(client)}, {"position", 480}});
	const ObjectId lane = run(client, "automationLane.add", {{"track", audio}, {"target", {{"type", "pan"}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	run(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", 0.5}}}}});

	const Json copy = run(client, "track.duplicate", {{"track", audio}}).at("track");
	const auto copyId = copy.at("id").get<ObjectId>();
	Json project = run(client, "project.get");
	REQUIRE(project.at("tracks").size() == 4);
	CHECK(project.at("tracks")[0].at("id") == first);
	CHECK(project.at("tracks")[1].at("id") == audio);
	CHECK(project.at("tracks")[2].at("id") == copyId);
	CHECK(project.at("tracks")[3].at("id") == last);
	CHECK(copy.at("name") == "Guitar");
	CHECK(copy.at("color") == "#c03040");
	const Json channel = channelOf(client, copyId);
	CHECK(channel.at("id") != channelOf(client, audio).at("id"));
	CHECK(channel.at("track") == copyId);
	CHECK(channel.at("volume") == -6.0);
	CHECK(channel.at("mute") == true);
	CHECK(channel.at("input") == channelOf(client, audio).at("input"));
	REQUIRE(project.at("audioEvents").size() == 2);
	CHECK(project.at("audioEvents")[1].at("track") == copyId);
	CHECK(project.at("audioEvents")[1].at("position") == 480);
	CHECK(project.at("audioEvents")[1].at("clip") == project.at("audioEvents")[0].at("clip"));
	REQUIRE(project.at("automation").size() == 2);
	CHECK(project.at("automation")[1].at("track") == copyId);
	CHECK(project.at("automation")[1].at("points")[0].at("value") == 0.5);
	CHECK(project.at("automation")[1].at("points")[0].at("id") != project.at("automation")[0].at("points")[0].at("id"));

	// A MIDI part's notes get ids of their own.
	const ObjectId instrument = addTrack(client, {{"type", "instrument"}});
	const ObjectId part =
		run(client, "midiPart.add", {{"track", instrument}, {"position", 0}, {"length", g_ticksPerQuarter}})
			.at("part")
			.at("id")
			.get<ObjectId>();
	run(client, "note.add",
		{{"part", part}, {"notes", {{{"position", 0}, {"length", 100}, {"pitch", 60}, {"velocity", 0.5}}}}});
	run(client, "track.duplicate", {{"track", instrument}});
	project = run(client, "project.get");
	REQUIRE(project.at("midiParts").size() == 2);
	const Json& original = project.at("midiParts")[0];
	const Json& duplicate = project.at("midiParts")[1];
	CHECK(duplicate.at("id") != original.at("id"));
	CHECK(duplicate.at("track") != original.at("track"));
	REQUIRE(duplicate.at("notes").size() == 1);
	CHECK(duplicate.at("notes")[0].at("pitch") == 60);
	CHECK(duplicate.at("notes")[0].at("id") != original.at("notes")[0].at("id"));

	// A folder comes with its tracks, which play through the new folder's group channel; one undo removes it all.
	const ObjectId folder = addTrack(client, {{"type", "folder"}, {"groupChannel", true}, {"name", "Band"}});
	addTrack(client, {{"type", "audio"}, {"parent", folder}});
	const size_t before = run(client, "project.get").at("tracks").size();
	const auto folderCopy = run(client, "track.duplicate", {{"track", folder}}).at("track").at("id").get<ObjectId>();
	project = run(client, "project.get");
	REQUIRE(project.at("tracks").size() == before + 2);
	const Json& inside = project.at("tracks")[before + 1];
	CHECK(project.at("tracks")[before].at("id") == folderCopy);
	CHECK(inside.at("parent") == folderCopy);
	CHECK(channelOf(client, inside.at("id").get<ObjectId>()).at("output") == channelOf(client, folderCopy).at("id"));
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("tracks").size() == before);

	// Marker tracks stay within their limit.
	const ObjectId markers = addTrack(client, {{"type", "marker"}});
	run(client, "marker.add", {{"track", markers}, {"position", 0}});
	run(client, "track.duplicate", {{"track", markers}});
	CHECK(run(client, "project.get").at("markers").size() == 2);
	for (int i = 2; i < 10; ++i)
		addTrack(client, {{"type", "marker"}});
	CHECK_THAT(reject(client, "track.duplicate", {{"track", markers}}), ContainsSubstring("at most"));
}
