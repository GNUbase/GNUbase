// SPDX-License-Identifier: GPL-3.0-or-later
// track.removeEmpty (#630, docs/features/track-list.md): which tracks count as empty, routing and folders that keep
// tracks, and the search that repeats until nothing more goes, in one undo step.
#include "engine/document/document.h"
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;

namespace
{
	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	ObjectId addTrack(Client& client, Json parameters)
	{
		return run(client, "track.add", std::move(parameters)).at("track").at("id").get<ObjectId>();
	}

	// The project's tracks, from top to bottom.
	std::vector<ObjectId> trackIds(Client& client)
	{
		std::vector<ObjectId> ids;
		const Json project = run(client, "project.get");
		for (const Json& track : project.at("tracks"))
			ids.push_back(track.at("id").get<ObjectId>());
		return ids;
	}

	Json withId(const Json& list, ObjectId id)
	{
		const auto found = std::find_if(
			list.begin(), list.end(), [id](const Json& object) { return object.at("id").get<ObjectId>() == id; });
		REQUIRE(found != list.end());
		return *found;
	}

	ObjectId channelOf(Client& client, ObjectId track)
	{
		return withId(run(client, "project.get").at("tracks"), track).at("channel").get<ObjectId>();
	}

	ObjectId outputOf(Client& client, ObjectId track)
	{
		const ObjectId channel = channelOf(client, track);
		return withId(run(client, "project.get").at("channels"), channel).at("output").get<ObjectId>();
	}

	// The ids track.removeEmpty returns.
	std::vector<ObjectId> removeEmpty(Client& client)
	{
		return run(client, "track.removeEmpty").at("tracks").get<std::vector<ObjectId>>();
	}

	// The marker tracks marker.trackActivated announced to a client since it last looked, in order.
	std::vector<ObjectId> activatedMarkerTracks(Client& client)
	{
		std::vector<ObjectId> tracks;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				if (event->name == "marker.trackActivated")
					tracks.push_back(event->data.at("track").get<ObjectId>());
		return tracks;
	}

	void addPart(Client& client, ObjectId track)
	{
		run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 1000}});
	}

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

TEST_CASE("track.removeEmpty removes the tracks that hold nothing, in one undo step", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId event = addTrack(client, {{"type", "audio"}});
	run(client, "audioEvent.add", {{"track", event}, {"clip", addClip(client)}, {"position", 0}});
	const ObjectId audioPart = addTrack(client, {{"type", "audio"}});
	run(client, "audioPart.add", {{"track", audioPart}, {"position", 0}, {"length", 1000}});
	const ObjectId midiPart = addTrack(client, {{"type", "instrument"}});
	addPart(client, midiPart);
	const ObjectId markers = addTrack(client, {{"type", "marker"}});
	run(client, "marker.add", {{"track", markers}, {"position", 0}});
	const ObjectId automated = addTrack(client, {{"type", "audio"}});
	run(client, "automationLane.add", {{"track", automated}, {"target", {{"type", "volume"}}}});
	// Tracks that never hold anything, or that others play through, stay.
	const ObjectId group = addTrack(client, {{"type", "group"}});
	const ObjectId effect = addTrack(client, {{"type", "effect"}});
	const ObjectId ruler = addTrack(client, {{"type", "ruler"}});
	const ObjectId tempo = addTrack(client, {{"type", "tempo"}});
	const ObjectId signature = addTrack(client, {{"type", "signature"}});
	const ObjectId vca = addTrack(client, {{"type", "vca"}});
	// Empty: these go.
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	const ObjectId instrument = addTrack(client, {{"type", "instrument"}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	const ObjectId marker = addTrack(client, {{"type", "marker"}});
	const ObjectId folder = addTrack(client, {{"type", "folder"}});
	const std::vector<ObjectId> before = trackIds(client);
	const int64_t steps = run(client, "history.get").at("position").get<int64_t>();

	CHECK(removeEmpty(client) == std::vector<ObjectId>{audio, instrument, midi, marker, folder});
	CHECK(trackIds(client) ==
		std::vector<ObjectId>{
			event, audioPart, midiPart, markers, automated, group, effect, ruler, tempo, signature, vca});
	const Json history = run(client, "history.get");
	CHECK(history.at("position") == steps + 1);
	CHECK(history.at("steps").back().at("action") == "Remove empty tracks");

	run(client, "history.undo");
	CHECK(trackIds(client) == before);
	run(client, "history.redo");
	CHECK(trackIds(client) ==
		std::vector<ObjectId>{
			event, audioPart, midiPart, markers, automated, group, effect, ruler, tempo, signature, vca});

	// Without empty tracks it changes nothing and records no undo step.
	CHECK(removeEmpty(client).empty());
	CHECK(run(client, "history.get").at("position") == steps + 1);
}

TEST_CASE(
	"track.removeEmpty removes empty tracks with plugins or an instrument, and one undo brings them back", "[unit]")
{
	Engine engine;
	Client client(engine);
	// Plugins in a channel's slots and an instrument track's instrument don't keep a track.
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	const ObjectId slot =
		run(client, "plugin.insert", {{"channel", channelOf(client, audio)}, {"plugin", "builtin:eq"}})
			.at("slot")
			.at("id")
			.get<ObjectId>();
	const ObjectId instrument = addTrack(client, {{"type", "instrument"}, {"instrument", "testSynth"}});
	const Json before = run(client, "project.get");
	REQUIRE(withId(before.at("slots"), slot).at("channel") == channelOf(client, audio));
	REQUIRE(withId(before.at("channels"), channelOf(client, instrument)).at("instrument") == "testSynth");
	const int64_t steps = run(client, "history.get").at("position").get<int64_t>();

	CHECK(removeEmpty(client) == std::vector<ObjectId>{audio, instrument});
	Json project = run(client, "project.get");
	CHECK(project.at("tracks").empty());
	CHECK(project.at("channels").empty());
	CHECK(project.at("slots").empty());

	run(client, "history.undo");
	CHECK(run(client, "history.get").at("position") == steps);
	project = run(client, "project.get");
	CHECK(project.at("tracks") == before.at("tracks"));
	CHECK(project.at("channels") == before.at("channels"));
	CHECK(project.at("slots") == before.at("slots"));
}

TEST_CASE("track.removeEmpty keeps tracks that others play or send into, until those go too", "[unit]")
{
	Engine engine;
	Client client(engine);
	// An instrument track without parts that a MIDI track with a part plays stays.
	const ObjectId played = addTrack(client, {{"type", "instrument"}});
	const ObjectId player = addTrack(client, {{"type", "midi"}, {"output", channelOf(client, played)}});
	addPart(client, player);
	// One that only an empty MIDI track plays goes with it.
	const ObjectId unplayed = addTrack(client, {{"type", "instrument"}});
	const ObjectId emptyPlayer = addTrack(client, {{"type", "midi"}, {"output", channelOf(client, unplayed)}});
	// A folder's group channel that a track outside it plays into stays, with its empty track.
	const ObjectId outputFolder = addTrack(client, {{"type", "folder"}, {"groupChannel", true}});
	const ObjectId inFolder = addTrack(client, {{"type", "audio"}, {"parent", outputFolder}}); // empty, goes
	const ObjectId outside = addTrack(client, {{"type", "instrument"}, {"output", channelOf(client, outputFolder)}});
	addPart(client, outside);
	// So does one that a track sends to, whether the send is on or not.
	const ObjectId sendFolder = addTrack(client, {{"type", "folder"}, {"groupChannel", true}});
	const ObjectId sender = addTrack(client, {{"type", "audio"}});
	run(client, "audioPart.add", {{"track", sender}, {"position", 0}, {"length", 1000}});
	run(client, "channel.set",
		{{"channel", channelOf(client, sender)},
			{"sends", {{{"send", 2}, {"destination", channelOf(client, sendFolder)}, {"on", false}}}}});
	// A group track stays even if only an empty track played through it.
	const ObjectId group = addTrack(client, {{"type", "group"}});
	const ObjectId throughGroup = addTrack(client, {{"type", "audio"}, {"output", channelOf(client, group)}}); // goes

	CHECK(removeEmpty(client) == std::vector<ObjectId>{unplayed, emptyPlayer, inFolder, throughGroup});
	CHECK(trackIds(client) == std::vector<ObjectId>{played, player, outputFolder, outside, sendFolder, sender, group});
	CHECK(outputOf(client, player) == channelOf(client, played));
	CHECK(outputOf(client, outside) == channelOf(client, outputFolder));
	const Json sends = withId(run(client, "project.get").at("channels"), channelOf(client, sender)).at("sends");
	CHECK(sends.at(1).at("destination") == channelOf(client, sendFolder));
}

TEST_CASE("track.removeEmpty leaves a marker track with markers active and a link group its other channels", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	const ObjectId first = addTrack(client, {{"type", "marker"}}); // empty and made active, goes
	addTrack(client, {{"type", "marker"}});                        // empty, goes
	const ObjectId markers = addTrack(client, {{"type", "marker"}});
	run(client, "marker.add", {{"track", markers}, {"position", 0}});
	run(client, "marker.activateTrack", {{"track", first}});
	REQUIRE(run(client, "project.get").at("activeMarkerTrack") == first);
	// An empty track in a link group that a VCA fader controls leaves the group; the others and the VCA stay.
	const ObjectId vca = addTrack(client, {{"type", "vca"}});
	const std::vector<ObjectId> linked{addTrack(client, {{"type", "audio"}}), addTrack(client, {{"type", "audio"}}),
		addTrack(client, {{"type", "audio"}})};
	run(client, "audioPart.add", {{"track", linked[0]}, {"position", 0}, {"length", 1000}});
	run(client, "audioPart.add", {{"track", linked[1]}, {"position", 0}, {"length", 1000}});
	const std::vector<ObjectId> channels{
		channelOf(client, linked[0]), channelOf(client, linked[1]), channelOf(client, linked[2])};
	const ObjectId group = run(client, "linkGroup.add", {{"channels", channels}, {"vca", channelOf(client, vca)}})
							   .at("group")
							   .at("id")
							   .get<ObjectId>();

	activatedMarkerTracks(listener); // drops what adding the tracks announced

	CHECK(removeEmpty(client).size() == 3);
	Json project = run(client, "project.get");
	CHECK(project.at("activeMarkerTrack") == markers);
	// Only the marker track that stays is announced as active, never one the command removes next.
	CHECK(activatedMarkerTracks(listener) == std::vector<ObjectId>{markers});
	Json linkGroup = withId(project.at("linkGroups"), group);
	CHECK(linkGroup.at("channels") == Json{channels[0], channels[1]});
	CHECK(linkGroup.at("vca") == channelOf(client, vca));

	run(client, "history.undo");
	project = run(client, "project.get");
	CHECK(project.at("activeMarkerTrack") == first);
	CHECK(activatedMarkerTracks(listener) == std::vector<ObjectId>{first});
	linkGroup = withId(project.at("linkGroups"), group);
	CHECK(linkGroup.at("channels") == Json(channels));
}

TEST_CASE("track.removeEmpty removing every marker track announces once that none is active", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	const ObjectId first = addTrack(client, {{"type", "marker"}});
	addTrack(client, {{"type", "marker"}});
	run(client, "marker.activateTrack", {{"track", first}});
	activatedMarkerTracks(listener); // drops what adding the tracks announced

	CHECK(removeEmpty(client).size() == 2);
	CHECK(run(client, "project.get").at("activeMarkerTrack") == 0);
	CHECK(activatedMarkerTracks(listener) == std::vector<ObjectId>{0});
}

TEST_CASE("track.removeEmpty removes nested group channels at once, and one undo restores their routing", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId outer = addTrack(client, {{"type", "folder"}, {"groupChannel", true}});
	const ObjectId inner = addTrack(client, {{"type", "folder"}, {"groupChannel", true}, {"parent", outer}});
	const ObjectId audio = addTrack(client, {{"type", "audio"}, {"parent", inner}});
	const ObjectId outerChannel = channelOf(client, outer);
	const ObjectId innerChannel = channelOf(client, inner);
	REQUIRE(outputOf(client, inner) == outerChannel); // the inner channel plays into the outer one
	REQUIRE(outputOf(client, audio) == innerChannel);

	CHECK(removeEmpty(client) == std::vector<ObjectId>{outer, inner, audio});
	CHECK(trackIds(client).empty());
	CHECK(run(client, "project.get").at("channels").empty());

	run(client, "history.undo");
	CHECK(trackIds(client) == std::vector<ObjectId>{outer, inner, audio});
	CHECK(outputOf(client, inner) == outerChannel);
	CHECK(outputOf(client, audio) == innerChannel);
}

TEST_CASE("track.removeEmpty keeps folders, nested or not, while a track inside holds something", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId outer = addTrack(client, {{"type", "folder"}});
	const ObjectId outerFull = addTrack(client, {{"type", "instrument"}, {"parent", outer}});
	addPart(client, outerFull);
	const ObjectId outerEmpty = addTrack(client, {{"type", "audio"}, {"parent", outer}});
	const ObjectId inner = addTrack(client, {{"type", "folder"}, {"groupChannel", true}, {"parent", outer}});
	const ObjectId innerFull = addTrack(client, {{"type", "midi"}, {"parent", inner}});
	addPart(client, innerFull);
	const ObjectId innerEmpty = addTrack(client, {{"type", "audio"}, {"parent", inner}});

	CHECK(removeEmpty(client) == std::vector<ObjectId>{outerEmpty, innerEmpty});
	CHECK(trackIds(client) == std::vector<ObjectId>{outer, outerFull, inner, innerFull});
	CHECK(withId(run(client, "project.get").at("tracks"), innerFull).at("parent") == inner);
	CHECK(withId(run(client, "project.get").at("tracks"), inner).at("parent") == outer);
}

TEST_CASE("track.removeEmpty with no tracks changes nothing", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK(removeEmpty(client).empty());
	CHECK(run(client, "history.get").at("steps").empty());
}
