// SPDX-License-Identifier: GPL-3.0-or-later
// Track presets and track import and export (#164, docs/features/track-presets.md): saving the sound of one or more
// tracks as a preset and applying it to tracks or adding tracks from it, and exporting tracks as a track archive and
// importing tracks from archives and projects, round trip.
#include "engine/dawproject/dawproject.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
namespace test = gnubase::test;
namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

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
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	struct Added
	{
		ObjectId track = 0;
		ObjectId channel = 0;
	};

	Added add(Client& client, Json parameters)
	{
		const Json added = run(client, "track.add", std::move(parameters));
		return {added.at("track").at("id").get<ObjectId>(),
			added.at("channel").is_null() ? 0 : added.at("channel").at("id").get<ObjectId>()};
	}

	Json find(const Json& list, ObjectId id)
	{
		for (const Json& item : list)
			if (item.at("id") == id)
				return item;
		FAIL("no " << id);
	}

	Json channelJson(Client& client, ObjectId id)
	{
		return find(run(client, "project.get").at("channels"), id);
	}

	// The tracks from top to bottom as name, type and the name of their folder.
	std::vector<std::string> outline(const Json& project)
	{
		std::vector<std::string> result;
		for (const Json& track : project.at("tracks"))
		{
			std::string parent;
			if (track.at("parent") != 0)
				parent =
					find(project.at("tracks"), track.at("parent").get<ObjectId>()).at("name").get<std::string>() + "/";
			result.push_back(parent + track.at("name").get<std::string>() + ":" + track.at("type").get<std::string>());
		}
		return result;
	}

	// The track of that name.
	Json trackNamed(const Json& project, const std::string& name)
	{
		for (const Json& track : project.at("tracks"))
			if (track.at("name") == name)
				return track;
		FAIL("no track " << name);
	}

	Json channelOf(const Json& project, const std::string& name)
	{
		return find(project.at("channels"), trackNamed(project, name).at("channel").get<ObjectId>());
	}

	std::vector<std::string> slotPlugins(const Json& project, const Json& channel)
	{
		std::vector<std::string> result;
		for (const Json& slot : channel.at("slots"))
			result.push_back(find(project.at("slots"), slot.get<ObjectId>()).at("plugin").get<std::string>());
		return result;
	}

	// A sound's settings from a channel's JSON: what track presets keep.
	Json sound(const Json& channel)
	{
		return {{"volume", channel.at("volume")}, {"pan", channel.at("pan")}, {"pre", channel.at("pre")},
			{"eq", channel.at("eq")}, {"strip", channel.at("strip")}};
	}

	bool shown(test::UiApp& app, const std::string& selector)
	{
		const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
		return found.size() == 1 && found[0].at("visible").get<bool>();
	}

	void click(test::UiApp& app, const std::string& selector)
	{
		REQUIRE(app.waitUntil([&app, &selector] { return shown(app, selector); }));
		app.ui().click(std::nullopt, {selector, 0, std::nullopt}, gnubase::ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	void select(test::UiApp& app, const std::vector<ObjectId>& tracks)
	{
		// The track list shows new tracks once their events came.
		REQUIRE(app.waitUntil(
			[&app, &tracks]
			{
				app.appUi().trackList()->select(tracks);
				return app.appUi().trackList()->selection() == tracks;
			}));
	}

	void answer(test::UiApp& app, const fs::path& file)
	{
		REQUIRE(app.waitUntil([&app] { return app.shell().fileDialogs().size() == 1; }));
		CHECK(app.shell().answerFileDialog(app.shell().fileDialogs().begin()->first, {file}));
	}

	size_t count(const Json& project, const std::string& kind, ObjectId track)
	{
		size_t result = 0;
		for (const Json& item : project.at(kind))
			result += item.at("track") == track ? 1 : 0;
		return result;
	}

	// The ids of a track's versions, in their order.
	std::vector<int> idsOf(const Json& track)
	{
		std::vector<int> result;
		for (const Json& version : track.at("versions"))
			result.push_back(version.at("id").get<int>());
		return result;
	}

	// The positions of the MIDI parts that play on a track, in their order.
	std::vector<int64_t> playing(const Json& project, ObjectId track)
	{
		std::vector<int64_t> result;
		for (const Json& part : project.at("midiParts"))
			if (part.at("track") == track)
				result.push_back(part.at("position").get<int64_t>());
		return result;
	}

	// What one of a track's versions that isn't active holds.
	Json held(const Json& project, ObjectId track, const Json& version)
	{
		for (const Json& contents : project.at("versionContents"))
			if (contents.at("track") == track && contents.at("version") == version)
				return contents;
		FAIL("no contents of version " << version.dump() << " of track " << track);
	}
}

TEST_CASE(
	"track.savePreset and track.loadPreset give tracks of the preset's types its sound, in one undo step", "[unit]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added vocals = add(client, {{"type", "audio"}, {"name", "Vocals"}});
	const Added reverb = add(client, {{"type", "effect"}, {"name", "Reverb"}});
	run(client, "channel.set",
		{{"channel", vocals.channel}, {"volume", -7.5}, {"pan", 0.25}, {"pre", {{"gain", 4.0}, {"phase", true}}},
			{"eq", {{"bands", {{{"band", 2}, {"on", true}, {"gain", -3.0}}}}}}, {"mute", true},
			{"sends", {{{"send", 1}, {"destination", reverb.channel}, {"on", true}}}}});
	run(client, "plugin.insert", {{"channel", vocals.channel}, {"plugin", "builtin:eq"}});
	const Json saved = run(client, "track.savePreset", {{"tracks", {vocals.track}}, {"name", "Lead Vox"}});
	CHECK(saved.at("file") == (dir.path() / "settings" / "Track Presets" / "Lead Vox.dawproject").u8string());
	CHECK_THAT(
		reject(client, "track.savePreset", {{"tracks", {vocals.track}}, {"name", "a/b"}}), ContainsSubstring("name"));
	const Added folder = add(client, {{"type", "folder"}});
	CHECK_THAT(reject(client, "track.savePreset", {{"tracks", {folder.track}}, {"name", "Folder"}}),
		ContainsSubstring("folder"));

	const Added piano = add(client, {{"type", "instrument"}, {"name", "Piano"}, {"color", "#336699"}});
	const Added strings = add(client, {{"type", "midi"}, {"name", "Strings"}});
	run(client, "channel.set", {{"channel", strings.channel}, {"program", 48}, {"midiVolume", 90}, {"midiChannel", 5}});
	run(client, "track.savePreset", {{"tracks", {vocals.track, strings.track}}, {"name", "Band"}});

	const Json presets = run(client, "track.presets").at("presets");
	REQUIRE(presets.size() == 2);
	CHECK(presets[0].at("name") == "Band");
	CHECK(presets[0].at("tracks") == Json{"audio", "midi"});
	CHECK(presets[1].at("name") == "Lead Vox");
	CHECK(presets[1].at("tracks") == Json{"audio"});

	const Added other = add(client, {{"type", "audio"}, {"name", "Other"}});
	const Json before = channelJson(client, other.channel);
	const size_t steps = run(client, "history.get").at("steps").size();
	run(client, "track.loadPreset", {{"tracks", {other.track}}, {"name", "Lead Vox"}});
	Json project = run(client, "project.get");
	const Json loaded = channelOf(project, "Other");
	CHECK(sound(loaded) == sound(channelOf(project, "Vocals")));
	CHECK(slotPlugins(project, loaded) == std::vector<std::string>{"builtin:eq"});
	CHECK(loaded.at("mute") == false);               // not the buttons
	CHECK(loaded.at("sends") == before.at("sends")); // nor the routing
	CHECK(trackNamed(project, "Other").at("name") == "Other");
	CHECK(run(client, "history.get").at("steps").size() == steps + 1);
	run(client, "history.undo");
	CHECK(channelJson(client, other.channel) == before);

	CHECK_THAT(reject(client, "track.loadPreset", {{"tracks", {piano.track}}, {"name", "Lead Vox"}}),
		ContainsSubstring("type"));
	CHECK_THAT(reject(client, "track.loadPreset", {{"tracks", {other.track, vocals.track}}, {"name", "Lead Vox"}}),
		ContainsSubstring("1 track"));
	CHECK_THAT(reject(client, "track.loadPreset", {{"tracks", {other.track}}, {"name", "Nothing"}}),
		ContainsSubstring("Nothing"));
	// A multi-track preset goes to tracks of the same types in the same order.
	const Added keys = add(client, {{"type", "midi"}, {"name", "Keys"}});
	CHECK_THAT(reject(client, "track.loadPreset", {{"tracks", {keys.track, other.track}}, {"name", "Band"}}),
		ContainsSubstring("type"));
	run(client, "track.loadPreset", {{"tracks", {other.track, keys.track}}, {"name", "Band"}});
	project = run(client, "project.get");
	CHECK(channelOf(project, "Keys").at("program") == 48);
	CHECK(channelOf(project, "Keys").at("midiVolume") == 90);
	CHECK(channelOf(project, "Keys").at("midiChannel") == 5);
	CHECK(sound(channelOf(project, "Other")) == sound(channelOf(project, "Vocals")));
}

TEST_CASE("track.addFromPreset adds the preset's tracks below a track, with their sound", "[unit]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added reverb = add(client, {{"type", "effect"}, {"name", "Reverb"}});
	const Added drums = add(client, {{"type", "audio"}, {"name", "Kick"}});
	const Added snare = add(client, {{"type", "audio"}, {"name", "Snare"}, {"channels", 1}});
	run(client, "channel.set",
		{{"channel", snare.channel}, {"volume", -3.0},
			{"sends", {{{"send", 2}, {"destination", reverb.channel}, {"on", true}}}}});
	run(client, "track.savePreset", {{"tracks", {drums.track, snare.track}}, {"name", "Drums"}});

	run(client, "project.new");
	const Added top = add(client, {{"type", "audio"}, {"name", "Top"}});
	add(client, {{"type", "audio"}, {"name", "Bottom"}});
	const Json added = run(client, "track.addFromPreset", {{"name", "Drums"}, {"track", top.track}});
	REQUIRE(added.at("tracks").size() == 2);
	const Json project = run(client, "project.get");
	CHECK(outline(project) == std::vector<std::string>{"Top:audio", "Kick:audio", "Snare:audio", "Bottom:audio"});
	CHECK(channelOf(project, "Snare").at("volume") == -3.0);
	CHECK(channelOf(project, "Snare").at("channels") == 1);
	CHECK(channelOf(project, "Snare").at("output") == project.at("busses")[1].at("id"));
	// Presets keep no routing to other tracks: the send has no destination.
	CHECK(channelOf(project, "Snare").at("sends")[1].at("destination") == 0);

	// Without track, the tracks go to the bottom.
	run(client, "track.addFromPreset", {{"name", "Drums"}});
	CHECK(outline(run(client, "project.get")).back() == "Snare:audio");
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("tracks").size() == 4);
}

TEST_CASE("track.export and track.import round-trip tracks with their events, settings and automation", "[integration]")
{
	const test::TempDir dir;
	const fs::path wav = dir.path() / "take.wav";
	test::writeWav(wav, {std::vector<float>(4800, 0.25f), 1, 48000});
	const fs::path archive = dir.path() / "Stems.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);

	const Added folder = add(client, {{"type", "folder"}, {"name", "Band"}, {"groupChannel", true}});
	const Added vocals = add(client, {{"type", "audio"}, {"name", "Vocals"}, {"parent", folder.track}});
	const Added piano = add(client, {{"type", "instrument"}, {"name", "Piano"}, {"instrument", "testSynth"}});
	const Added strings = add(client, {{"type", "midi"}, {"name", "Strings"}});
	const Added reverb = add(client, {{"type", "effect"}, {"name", "Reverb"}});
	add(client, {{"type", "audio"}, {"name", "Left Out"}});
	run(client, "channel.set", {{"channel", strings.channel}, {"output", piano.channel}});
	run(client, "channel.set",
		{{"channel", vocals.channel}, {"volume", -6.0}, {"solo", true},
			{"sends", {{{"send", 1}, {"destination", reverb.channel}, {"on", true}, {"level", -10.0}}}}});
	run(client, "plugin.insert", {{"channel", vocals.channel}, {"plugin", "builtin:eq"}});
	const ObjectId media = run(client, "media.add", {{"path", wav.u8string()}}).at("media").at("id").get<ObjectId>();
	const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	run(client, "audioEvent.add",
		{{"track", vocals.track}, {"clip", clip}, {"position", 960}, {"offset", 0}, {"length", 4800}});
	const ObjectId part = run(client, "midiPart.add", {{"track", strings.track}, {"position", 0}, {"length", 3840}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
	run(client, "note.add",
		{{"part", part}, {"notes", {{{"position", 0}, {"length", 480}, {"pitch", 60}, {"velocity", 0.5}}}}});
	const ObjectId lane =
		run(client, "automationLane.add", {{"track", vocals.track}, {"target", {{"type", "volume"}}}, {"read", false}})
			.at("lane")
			.at("id")
			.get<ObjectId>();
	run(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", -12.0}}}}});

	CHECK(run(client, "track.export",
			  {{"tracks", {folder.track, piano.track, strings.track, reverb.track}}, {"path", archive.u8string()},
				  {"copyMedia", true}})
			  .at("file") == archive.u8string());
	CHECK_THAT(reject(client, "track.export", {{"tracks", Json::array()}, {"path", archive.u8string()}}),
		ContainsSubstring("track"));

	run(client, "project.new");
	const Json sources = run(client, "track.importSources", {{"path", archive.u8string()}}).at("tracks");
	REQUIRE(sources.size() == 5); // the folder with the track inside it
	CHECK(sources[0].at("name") == "Band");
	CHECK(sources[1].at("name") == "Vocals");
	CHECK(sources[1].at("parent") == sources[0].at("id"));
	CHECK(sources[1].at("type") == "audio");
	CHECK(!fs::exists(dir.path() / "Stems media")); // listing extracts no media files

	const Json imported = run(client, "track.import", {{"path", archive.u8string()}});
	CHECK(imported.at("tracks").size() == 4); // the top-level ones
	Json project = run(client, "project.get");
	CHECK(outline(project) ==
		std::vector<std::string>{
			"Band:folder", "Band/Vocals:audio", "Piano:instrument", "Strings:midi", "Reverb:effect"});
	const Json vocalsChannel = channelOf(project, "Vocals");
	CHECK(vocalsChannel.at("volume") == -6.0);
	CHECK(vocalsChannel.at("solo") == false); // the buttons aren't imported
	CHECK(vocalsChannel.at("output") == channelOf(project, "Band").at("id"));
	CHECK(vocalsChannel.at("sends")[0].at("destination") == channelOf(project, "Reverb").at("id"));
	CHECK(vocalsChannel.at("sends")[0].at("level") == -10.0);
	CHECK(slotPlugins(project, vocalsChannel) == std::vector<std::string>{"builtin:eq"});
	CHECK(channelOf(project, "Strings").at("output") == channelOf(project, "Piano").at("id"));
	CHECK(channelOf(project, "Piano").at("instrument") == "testSynth");
	const ObjectId vocalsTrack = trackNamed(project, "Vocals").at("id").get<ObjectId>();
	REQUIRE(count(project, "audioEvents", vocalsTrack) == 1);
	const Json event = project.at("audioEvents")[0];
	CHECK(event.at("position") == 960);
	const Json importedClip = find(project.at("clips"), event.at("clip").get<ObjectId>());
	CHECK(fs::exists(
		fs::u8path(find(project.at("media"), importedClip.at("media").get<ObjectId>()).at("path").get<std::string>())));
	const ObjectId stringsTrack = trackNamed(project, "Strings").at("id").get<ObjectId>();
	REQUIRE(count(project, "midiParts", stringsTrack) == 1);
	CHECK(project.at("midiParts")[0].at("notes").size() == 1);
	REQUIRE(count(project, "automation", vocalsTrack) == 1);
	CHECK(project.at("automation")[0].at("points").size() == 1);
	CHECK(project.at("automation")[0].at("read") == true); // the read state isn't imported (p. 200)

	// One undo step takes it all back.
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("tracks").empty());

	// Only some of the tracks, without their events or settings, at the bottom: the send finds the effect track of
	// that name in the project.
	const Added mine = add(client, {{"type", "effect"}, {"name", "Reverb"}});
	run(client, "track.import",
		{{"path", archive.u8string()}, {"tracks", {sources[1].at("id")}}, {"events", false}, {"settings", false}});
	project = run(client, "project.get");
	CHECK(outline(project) == std::vector<std::string>{"Reverb:effect", "Vocals:audio"});
	const ObjectId plain = trackNamed(project, "Vocals").at("id").get<ObjectId>();
	CHECK(count(project, "audioEvents", plain) == 0);
	CHECK(count(project, "automation", plain) == 1); // automation is imported unless it is switched off
	CHECK(channelOf(project, "Vocals").at("volume") == 0.0);
	CHECK(channelOf(project, "Vocals").at("slots").empty());
	run(client, "history.undo");
	run(client, "track.import",
		{{"path", archive.u8string()}, {"tracks", {sources[1].at("id")}}, {"automation", false}});
	project = run(client, "project.get");
	CHECK(count(project, "automation", trackNamed(project, "Vocals").at("id").get<ObjectId>()) == 0);
	CHECK(channelOf(project, "Vocals").at("sends")[0].at("destination") == mine.channel);
	CHECK(channelOf(project, "Vocals").at("output") == project.at("busses")[1].at("id"));

	CHECK_THAT(reject(client, "track.import", {{"path", (dir.path() / "none.dawproject").u8string()}}),
		ContainsSubstring("none.dawproject"));
	CHECK_THAT(reject(client, "track.import", {{"path", archive.u8string()}, {"tracks", {123456}}}),
		ContainsSubstring("123456"));
}

TEST_CASE("track.import takes tracks from a project file", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	add(client, {{"type", "marker"}, {"name", "Cues"}});
	const Added bass = add(client, {{"type", "instrument"}, {"name", "Bass"}, {"color", "#123456"}});
	run(client, "midiPart.add", {{"track", bass.track}, {"position", 0}, {"length", 1920}});
	run(client, "project.save", {{"path", song.u8string()}});

	run(client, "project.new");
	const Added first = add(client, {{"type", "audio"}, {"name", "First"}});
	add(client, {{"type", "audio"}, {"name", "Last"}});
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	REQUIRE(sources.size() == 2);
	run(client, "track.import", {{"path", song.u8string()}, {"tracks", {sources[1].at("id")}}, {"track", first.track}});
	const Json project = run(client, "project.get");
	CHECK(outline(project) == std::vector<std::string>{"First:audio", "Bass:instrument", "Last:audio"});
	CHECK(trackNamed(project, "Bass").at("color") == "#123456");
	CHECK(project.at("midiParts").size() == 1);
	CHECK(run(client, "project.get").at("tracks").size() == 3);
	// Marker tracks stay at the top level.
	run(client, "track.import", {{"path", song.u8string()}, {"tracks", {sources[0].at("id")}}});
	CHECK(outline(run(client, "project.get")).back() == "Cues:marker");
}

TEST_CASE("folders go along without the tracks inside them that can't be imported or exported yet", "[integration]")
{
	const test::TempDir dir;
	const fs::path archive = dir.path() / "Band.dawproject";
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added folder = add(client, {{"type", "folder"}, {"name", "Band"}});
	add(client, {{"type", "audio"}, {"name", "Vocals"}, {"parent", folder.track}});
	const Added vca = add(client, {{"type", "vca"}, {"name", "Faders"}, {"parent", folder.track}});
	const Json exported = run(client, "track.export", {{"tracks", {folder.track}}, {"path", archive.u8string()}});
	REQUIRE(exported.at("skipped").size() == 1);
	CHECK(exported.at("skipped")[0].at("id") == vca.track);
	CHECK(exported.at("skipped")[0].at("name") == "Faders");
	CHECK(exported.at("skipped")[0].at("type") == "vca");
	CHECK_THAT(reject(client, "track.export", {{"tracks", {vca.track}}, {"path", archive.u8string()}}),
		ContainsSubstring("vca"));
	// Listed, also inside a listed folder, they are refused.
	CHECK_THAT(reject(client, "track.export", {{"tracks", {folder.track, vca.track}}, {"path", archive.u8string()}}),
		ContainsSubstring("vca"));
	add(client, {{"type", "vca"}, {"name", "Outer"}});
	run(client, "project.save", {{"path", song.u8string()}});

	run(client, "project.new");
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}});
	REQUIRE(sources.at("tracks").size() == 4);
	CHECK(sources.at("tracks")[1].at("importable") == true);
	CHECK(sources.at("tracks")[2].at("name") == "Faders");
	CHECK(sources.at("tracks")[2].at("importable") == false);
	CHECK(sources.at("project") ==
		Json{{"name", "Untitled"}, {"sampleRate", 48000.0}, {"frameRate", "25"}, {"startTime", 0.0}});
	CHECK_THAT(reject(client, "track.import",
				   {{"path", song.u8string()},
					   {"tracks", {sources.at("tracks")[0].at("id"), sources.at("tracks")[2].at("id")}}}),
		ContainsSubstring("vca"));
	const Json imported = run(client, "track.import", {{"path", song.u8string()}});
	CHECK(outline(run(client, "project.get")) == std::vector<std::string>{"Band:folder", "Band/Vocals:audio"});
	// From top to bottom: the one inside the folder first.
	REQUIRE(imported.at("skipped").size() == 2);
	CHECK(imported.at("skipped")[0].at("name") == "Faders");
	CHECK(imported.at("skipped")[1].at("name") == "Outer");
	CHECK(run(client, "track.import", {{"path", archive.u8string()}}).at("skipped").empty());
}

TEST_CASE("track.import routes by name only to the tracks the project had before", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added first = add(client, {{"type", "group"}, {"name", "Group"}});
	const Added second = add(client, {{"type", "group"}, {"name", "Group"}});
	const Added source = add(client, {{"type", "audio"}, {"name", "Source"}});
	run(client, "channel.set", {{"channel", source.channel}, {"output", first.channel}});
	run(client, "project.save", {{"path", song.u8string()}});

	// The first group stays behind: the source doesn't go to the copy of the second, which has its name.
	run(client, "project.new");
	run(client, "track.import", {{"path", song.u8string()}, {"tracks", {second.track, source.track}}});
	Json project = run(client, "project.get");
	CHECK(channelOf(project, "Source").at("output") == project.at("busses")[1].at("id"));
	// A group of that name that the project had is found.
	run(client, "history.undo");
	const Added mine = add(client, {{"type", "group"}, {"name", "Group"}});
	run(client, "track.import", {{"path", song.u8string()}, {"tracks", {source.track}}});
	project = run(client, "project.get");
	CHECK(channelOf(project, "Source").at("output") == mine.channel);
}

TEST_CASE("track.import keeps the time of linear positions from a file at another sample rate", "[integration]")
{
	const test::TempDir dir;
	const fs::path wav = dir.path() / "take.wav";
	test::writeWav(wav, {std::vector<float>(4410, 0.25f), 1, 44100});
	const fs::path archive = dir.path() / "Linear.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	run(client, "project.set", {{"sampleRate", 44100.0}});
	const Added take = add(client, {{"type", "audio"}, {"name", "Take"}, {"timeBase", "linear"}});
	const ObjectId media = run(client, "media.add", {{"path", wav.u8string()}}).at("media").at("id").get<ObjectId>();
	const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	run(client, "audioEvent.add",
		{{"track", take.track}, {"clip", clip}, {"position", 44100}, {"offset", 0}, {"length", 4410}});
	const ObjectId lane = run(client, "automationLane.add", {{"track", take.track}, {"target", {{"type", "volume"}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	run(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 88200}, {"value", -12.0}}}}});
	const Added cues = add(client, {{"type", "marker"}, {"name", "Cues"}, {"timeBase", "linear"}});
	run(client, "marker.add", {{"track", cues.track}, {"position", 22050}, {"name", "Hit"}});
	run(client, "track.export", {{"tracks", {take.track, cues.track}}, {"path", archive.u8string()}});

	run(client, "project.new"); // at 48 kHz
	run(client, "track.import", {{"path", archive.u8string()}});
	const Json project = run(client, "project.get");
	REQUIRE(project.at("audioEvents").size() == 1);
	CHECK(project.at("audioEvents")[0].at("position") == 48000);
	REQUIRE(project.at("automation").size() == 1);
	CHECK(project.at("automation")[0].at("points")[0].at("position") == 96000);
	REQUIRE(project.at("markers").size() == 1);
	CHECK(project.at("markers")[0].at("position") == 24000);
}

TEST_CASE("track.import imports tracks onto the project's tracks of their type", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added reverb = add(client, {{"type", "effect"}, {"name", "Reverb"}});
	run(client, "channel.set", {{"channel", reverb.channel}, {"volume", -3.0}});
	const Added keys = add(client, {{"type", "instrument"}, {"name", "Keys"}});
	run(client, "channel.set",
		{{"channel", keys.channel}, {"volume", -5.0},
			{"sends", {{{"send", 1}, {"destination", reverb.channel}, {"on", true}}}}});
	run(client, "midiPart.add", {{"track", keys.track}, {"position", 1920}, {"length", 960}});
	const Added band = add(client, {{"type", "folder"}, {"name", "Band"}});
	add(client, {{"type", "audio"}, {"name", "Guitar"}, {"parent", band.track}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	const auto source = [&sources](const std::string& name)
	{ return trackNamed({{"tracks", sources}}, name).at("id"); };

	run(client, "project.new");
	const Added mine = add(client, {{"type", "effect"}, {"name", "Reverb"}});
	const Added dry = add(client, {{"type", "audio"}, {"name", "Dry"}});
	run(client, "channel.set",
		{{"channel", dry.channel}, {"sends", {{{"send", 1}, {"destination", mine.channel}, {"on", true}}}}});
	const Added piano = add(client, {{"type", "instrument"}, {"name", "Piano"}});
	run(client, "channel.set", {{"channel", piano.channel}, {"mute", true}});
	run(client, "midiPart.add", {{"track", piano.track}, {"position", 0}, {"length", 960}});
	const Added folder = add(client, {{"type", "folder"}, {"name", "Band"}});
	add(client, {{"type", "audio"}, {"name", "Old"}, {"parent", folder.track}});
	const std::vector<std::string> before = outline(run(client, "project.get"));
	const auto import = [&](const std::string& existing)
	{
		return run(client, "track.import",
			{{"path", song.u8string()}, {"tracks", {source("Reverb"), source("Keys"), source("Band")}},
				{"existing", existing},
				{"destinations",
					{{{"source", source("Reverb")}, {"track", mine.track}},
						{{"source", source("Keys")}, {"track", piano.track}},
						{{"source", source("Band")}, {"track", folder.track}}}}});
	};
	const size_t steps = run(client, "history.get").at("steps").size();
	CHECK(import("version").at("tracks").size() == 2); // the effect track and the folder that replace the project's
	CHECK(run(client, "history.get").at("steps").size() == steps + 1);
	Json project = run(client, "project.get");
	// The effect track and the folder were replaced, in their places; the dry track sends to the effect track
	// imported.
	CHECK(outline(project) ==
		std::vector<std::string>{"Reverb:effect", "Dry:audio", "Piano:instrument", "Band:folder", "Band/Guitar:audio"});
	CHECK(trackNamed(project, "Reverb").at("id") != mine.track);
	const ObjectId imported = channelOf(project, "Reverb").at("id").get<ObjectId>();
	CHECK(channelOf(project, "Reverb").at("volume") == -3.0);
	CHECK(channelOf(project, "Dry").at("sends")[0].at("destination") == imported);
	// The instrument track keeps its name and buttons and gets the settings, with the send to the imported effect
	// track, and a new version with the part.
	CHECK(trackNamed(project, "Piano").at("id") == piano.track);
	CHECK(channelOf(project, "Piano").at("id") == piano.channel);
	CHECK(channelOf(project, "Piano").at("volume") == -5.0);
	CHECK(channelOf(project, "Piano").at("mute") == true);
	CHECK(channelOf(project, "Piano").at("sends")[0].at("destination") == imported);
	CHECK(trackNamed(project, "Piano").at("versions").size() == 2);
	REQUIRE(count(project, "midiParts", piano.track) == 1);
	CHECK(project.at("midiParts")[0].at("position") == 1920);
	run(client, "history.undo");
	CHECK(outline(run(client, "project.get")) == before);

	// Replacing the track's parts, or adding to them.
	import("replace");
	project = run(client, "project.get");
	CHECK(count(project, "midiParts", piano.track) == 1);
	CHECK(trackNamed(project, "Piano").at("versions").size() == 1);
	run(client, "history.undo");
	import("add");
	CHECK(count(run(client, "project.get"), "midiParts", piano.track) == 2);
	run(client, "history.undo");

	CHECK_THAT(reject(client, "track.import",
				   {{"path", song.u8string()}, {"tracks", {source("Keys")}},
					   {"destinations", {{{"source", source("Keys")}, {"track", dry.track}}}}}),
		ContainsSubstring("instrument"));
	CHECK_THAT(reject(client, "track.import",
				   {{"path", song.u8string()}, {"tracks", {source("Reverb")}},
					   {"destinations", {{{"source", source("Keys")}, {"track", piano.track}}}}}),
		ContainsSubstring("among the tracks imported"));
	CHECK(outline(run(client, "project.get")) == before);
}

TEST_CASE("track.import places events at their absolute or relative position or at the cursor, within a range",
	"[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	run(client, "project.set", {{"startTime", 10.0}});
	const Added keys = add(client, {{"type", "midi"}, {"name", "Keys"}, {"timeBase", "linear"}});
	for (const int position : {48000, 480000})
		run(client, "midiPart.add", {{"track", keys.track}, {"position", position}, {"length", 4800}});
	run(client, "project.save", {{"path", song.u8string()}});

	run(client, "project.new");
	run(client, "project.set", {{"startTime", 5.0}});
	const auto import = [&client, &song](Json parameters)
	{
		parameters["path"] = song.u8string();
		run(client, "track.import", parameters);
		std::vector<int64_t> positions;
		const Json project = run(client, "project.get");
		for (const Json& part : project.at("midiParts"))
			positions.push_back(part.at("position").get<int64_t>());
		std::sort(positions.begin(), positions.end());
		run(client, "history.undo");
		return positions;
	};
	// At the timecode they had: 5 seconds later here.
	CHECK(import(Json::object()) == std::vector<int64_t>{288000, 720000});
	CHECK(import({{"position", "relative"}}) == std::vector<int64_t>{48000, 480000});
	run(client, "transport.locate", {{"position", 96000}});
	CHECK(import({{"position", "cursor"}}) == std::vector<int64_t>{144000, 576000});
	CHECK(import({{"position", "relative"}, {"range", {{"start", 0}, {"end", 96000}}}}) == std::vector<int64_t>{48000});
	CHECK_THAT(reject(client, "track.import", {{"path", song.u8string()}, {"range", {{"start", 10}, {"end", 10}}}}),
		ContainsSubstring("range"));
	run(client, "project.set", {{"startTime", 20.0}});
	CHECK_THAT(reject(client, "track.import", {{"path", song.u8string()}}), ContainsSubstring("project start"));
}

TEST_CASE("track.import places only what it imports", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added early = add(client, {{"type", "instrument"}, {"name", "Early"}, {"timeBase", "linear"}});
	run(client, "midiPart.add", {{"track", early.track}, {"position", 0}, {"length", 4800}});
	const ObjectId lane = run(client, "automationLane.add", {{"track", early.track}, {"target", {{"type", "volume"}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	run(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", -6.0}}}}});
	const Added late = add(client, {{"type", "midi"}, {"name", "Late"}, {"timeBase", "linear"}});
	run(client, "midiPart.add", {{"track", late.track}, {"position", 60 * 48000}, {"length", 4800}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	const auto source = [&sources](const std::string& name)
	{ return trackNamed({{"tracks", sources}}, name).at("id"); };

	// The project starts 30 seconds later: the early track's part and point would start before it, but they aren't
	// imported.
	run(client, "project.new");
	run(client, "project.set", {{"startTime", 30.0}});
	run(client, "track.import", {{"path", song.u8string()}, {"tracks", {source("Late")}}});
	Json project = run(client, "project.get");
	REQUIRE(project.at("midiParts").size() == 1);
	CHECK(project.at("midiParts")[0].at("position") == 30 * 48000);
	run(client, "history.undo");
	run(client, "track.import",
		{{"path", song.u8string()}, {"tracks", {source("Early")}}, {"events", false}, {"automation", false}});
	project = run(client, "project.get");
	CHECK(project.at("midiParts").empty());
	CHECK(project.at("automation").empty());
	run(client, "history.undo");
	CHECK_THAT(reject(client, "track.import", {{"path", song.u8string()}, {"tracks", {source("Early")}}}),
		ContainsSubstring("project start"));
}

TEST_CASE("track.import refuses a source onto two tracks or inside an imported folder, and merges in the track's time "
		  "base",
	"[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added lead = add(client, {{"type", "midi"}, {"name", "Lead"}, {"timeBase", "linear"}});
	run(client, "midiPart.add", {{"track", lead.track}, {"position", 48000}, {"length", 48000}});
	const Added band = add(client, {{"type", "folder"}, {"name", "Band"}});
	add(client, {{"type", "midi"}, {"name", "Bass"}, {"parent", band.track}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	const auto source = [&sources](const std::string& name)
	{ return trackNamed({{"tracks", sources}}, name).at("id"); };

	run(client, "project.new");
	const Added a = add(client, {{"type", "midi"}, {"name", "A"}}); // musical
	const Added b = add(client, {{"type", "midi"}, {"name", "B"}});
	const std::vector<std::string> before = outline(run(client, "project.get"));
	CHECK_THAT(reject(client, "track.import",
				   {{"path", song.u8string()}, {"tracks", {source("Lead")}},
					   {"destinations",
						   {{{"source", source("Lead")}, {"track", a.track}},
							   {{"source", source("Lead")}, {"track", b.track}}}}}),
		ContainsSubstring("onto two tracks"));
	CHECK_THAT(reject(client, "track.import",
				   {{"path", song.u8string()}, {"tracks", {source("Band"), source("Bass")}},
					   {"destinations", {{{"source", source("Bass")}, {"track", a.track}}}}}),
		ContainsSubstring("inside a folder that is imported"));
	CHECK(outline(run(client, "project.get")) == before);

	// The linear part merged onto the musical track is in ticks there: one second in, a second long, at 120 BPM.
	run(client, "track.import",
		{{"path", song.u8string()}, {"tracks", {source("Lead")}}, {"existing", "replace"},
			{"destinations", {{{"source", source("Lead")}, {"track", a.track}}}}});
	const Json project = run(client, "project.get");
	REQUIRE(count(project, "midiParts", a.track) == 1);
	CHECK(project.at("midiParts")[0].at("position") == 2 * gnubase::time::g_ticksPerQuarter);
	CHECK(project.at("midiParts")[0].at("length") == 2 * gnubase::time::g_ticksPerQuarter);
}

TEST_CASE("track.import refuses a destination inside a folder it replaces and puts new tracks below that folder",
	"[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added band = add(client, {{"type", "folder"}, {"name", "Band"}});
	add(client, {{"type", "midi"}, {"name", "Bass"}, {"parent", band.track}});
	add(client, {{"type", "midi"}, {"name", "Lead"}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	const auto source = [&sources](const std::string& name)
	{ return trackNamed({{"tracks", sources}}, name).at("id"); };

	run(client, "project.new");
	const Added folder = add(client, {{"type", "folder"}, {"name", "Old"}});
	const Added inner = add(client, {{"type", "midi"}, {"name", "Inner"}, {"parent", folder.track}});
	const std::vector<std::string> before = outline(run(client, "project.get"));
	// The destination inside the folder is refused in either order, before anything changes.
	const Json lead = {{"source", source("Lead")}, {"track", inner.track}};
	const Json old = {{"source", source("Band")}, {"track", folder.track}};
	for (const Json& destinations : {Json::array({lead, old}), Json::array({old, lead})})
	{
		CHECK_THAT(reject(client, "track.import",
					   {{"path", song.u8string()}, {"tracks", {source("Band"), source("Lead")}},
						   {"destinations", destinations}}),
			ContainsSubstring("track " + std::to_string(inner.track) +
				" (Inner) is inside a track that another one replaces: track " + std::to_string(folder.track) +
				" (Old)"));
		CHECK(outline(run(client, "project.get")) == before);
	}

	// New tracks below a track inside the folder go below the folder that replaces it, and stay.
	const Json result = run(client, "track.import",
		{{"path", song.u8string()}, {"tracks", {source("Band"), source("Lead")}}, {"track", inner.track},
			{"destinations", Json::array({old})}});
	const Json project = run(client, "project.get");
	CHECK(outline(project) == std::vector<std::string>{"Band:folder", "Band/Bass:midi", "Lead:midi"});
	REQUIRE(result.at("tracks").size() == 2);
	for (const Json& id : result.at("tracks"))
		CHECK(find(project.at("tracks"), id.get<ObjectId>()).at("parent") == 0);
}

TEST_CASE(
	"track.import puts new tracks below a track inside a replaced folder into the folder around it", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added band = add(client, {{"type", "folder"}, {"name", "Band"}});
	add(client, {{"type", "midi"}, {"name", "Bass"}, {"parent", band.track}});
	add(client, {{"type", "midi"}, {"name", "Lead"}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	const auto source = [&sources](const std::string& name)
	{ return trackNamed({{"tracks", sources}}, name).at("id"); };

	run(client, "project.new");
	const Added outer = add(client, {{"type", "folder"}, {"name", "Outer"}});
	const Added folder = add(client, {{"type", "folder"}, {"name", "Old"}, {"parent", outer.track}});
	const Added inner = add(client, {{"type", "midi"}, {"name", "Inner"}, {"parent", folder.track}});
	add(client, {{"type", "midi"}, {"name", "After"}, {"parent", outer.track}});
	run(client, "track.import",
		{{"path", song.u8string()}, {"tracks", {source("Band"), source("Lead")}}, {"track", inner.track},
			{"destinations", {{{"source", source("Band")}, {"track", folder.track}}}}});
	// The copy of the folder takes its place in the outer folder, and the new track comes right after it there.
	CHECK(outline(run(client, "project.get")) ==
		std::vector<std::string>{
			"Outer:folder", "Outer/Band:folder", "Band/Bass:midi", "Outer/Lead:midi", "Outer/After:midi"});
}

TEST_CASE("track.import replaces a marker track in a project with the most marker tracks", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added cues = add(client, {{"type", "marker"}, {"name", "Cues"}});
	run(client, "marker.add", {{"track", cues.track}, {"position", 1920}, {"name", "Verse"}});
	add(client, {{"type", "marker"}, {"name", "More"}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	const auto source = [&sources](const std::string& name)
	{ return trackNamed({{"tracks", sources}}, name).at("id"); };

	run(client, "project.new");
	std::vector<ObjectId> markerTracks;
	markerTracks.reserve(g_maxMarkerTracks);
	for (int i = 0; i < g_maxMarkerTracks; ++i)
		markerTracks.push_back(add(client, {{"type", "marker"}, {"name", "Markers " + std::to_string(i + 1)}}).track);
	run(client, "marker.activateTrack", {{"track", markerTracks[3]}});
	// As a new track, it is refused, also next to one that replaces a track; onto one of the project's, it replaces
	// it (reference manual pp. 200-201).
	CHECK_THAT(reject(client, "track.import", {{"path", song.u8string()}, {"tracks", {source("Cues")}}}),
		ContainsSubstring("marker tracks"));
	CHECK_THAT(reject(client, "track.import",
				   {{"path", song.u8string()}, {"tracks", {source("Cues"), source("More")}},
					   {"destinations", {{{"source", source("Cues")}, {"track", markerTracks[3]}}}}}),
		ContainsSubstring("marker tracks"));
	const Json imported = run(client, "track.import",
		{{"path", song.u8string()}, {"tracks", {source("Cues")}},
			{"destinations", {{{"source", source("Cues")}, {"track", markerTracks[3]}}}}});
	REQUIRE(imported.at("tracks").size() == 1);
	const Json project = run(client, "project.get");
	REQUIRE(outline(project).size() == static_cast<size_t>(g_maxMarkerTracks));
	CHECK(outline(project)[3] == "Cues:marker");
	// The copy is the active marker track, as the one it replaced was, though it isn't the topmost.
	CHECK(project.at("activeMarkerTrack") == imported.at("tracks")[0]);
	REQUIRE(project.at("markers").size() == 1);
	CHECK(project.at("markers")[0].at("name") == "Verse");
}

TEST_CASE("track.import keeps the version IDs that tracks share and gives versions names of their own", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added a = add(client, {{"type", "midi"}, {"name", "A"}});
	const Added b = add(client, {{"type", "midi"}, {"name", "B"}});
	for (const ObjectId track : {a.track, b.track})
		run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 960}});
	run(client, "trackVersion.add", {{"tracks", {a.track, b.track}}}); // v2, with an ID they share
	for (const ObjectId track : {a.track, b.track})
		run(client, "midiPart.add", {{"track", track}, {"position", 1920}, {"length", 960}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	const auto source = [&sources](const std::string& name)
	{ return trackNamed({{"tracks", sources}}, name).at("id"); };

	run(client, "project.new");
	const Added x = add(client, {{"type", "midi"}, {"name", "X"}});
	const Added y = add(client, {{"type", "midi"}, {"name", "Y"}});
	run(client, "track.import",
		{{"path", song.u8string()}, {"tracks", {source("A"), source("B")}},
			{"destinations",
				{{{"source", source("A")}, {"track", x.track}}, {{"source", source("B")}, {"track", y.track}}}}});
	const Json project = run(client, "project.get");
	const Json first = trackNamed(project, "X");
	const Json second = trackNamed(project, "Y");
	const auto names = [](const Json& track)
	{
		std::vector<std::string> result;
		for (const Json& version : track.at("versions"))
			result.push_back(version.at("name").get<std::string>());
		return result;
	};
	// The track's v1, the new version with what the file's tracks played, and their v1 under a name of its own.
	CHECK(names(first) == std::vector<std::string>{"v1", "v2", "v3"});
	CHECK(names(second) == std::vector<std::string>{"v1", "v2", "v3"});
	REQUIRE(first.at("versions").size() == 3);
	REQUIRE(second.at("versions").size() == 3);
	// The new versions share an ID, as versions made together do, and so do the file's v1, which shared one there.
	CHECK(first.at("activeVersion") == second.at("activeVersion"));
	CHECK(first.at("versions")[1].at("id") == second.at("versions")[1].at("id"));
	CHECK(first.at("versions")[2].at("id") == second.at("versions")[2].at("id"));
	CHECK(first.at("versions")[2].at("id") != first.at("versions")[1].at("id"));
	CHECK(first.at("versions")[2].at("id") != first.at("versions")[0].at("id"));
	// The project file keeps them, with their ids.
	const std::vector<int> kept = idsOf(first);
	const fs::path saved = dir.path() / "Saved.dawproject";
	run(client, "project.save", {{"path", saved.u8string()}});
	run(client, "project.load", {{"path", saved.u8string()}});
	const Json loaded = run(client, "project.get");
	CHECK(names(trackNamed(loaded, "X")) == std::vector<std::string>{"v1", "v2", "v3"});
	CHECK(idsOf(trackNamed(loaded, "X")) == kept);
	CHECK(idsOf(trackNamed(loaded, "Y")) == idsOf(second));
	CHECK(trackNamed(loaded, "X").at("activeVersion") == first.at("activeVersion"));
}

TEST_CASE("track.import keeps the version IDs of the versions the file's tracks don't play", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added a = add(client, {{"type", "midi"}, {"name", "A"}});
	const Added b = add(client, {{"type", "midi"}, {"name", "B"}});
	const Added c = add(client, {{"type", "midi"}, {"name", "C"}});
	for (const ObjectId track : {a.track, b.track, c.track})
		run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 960}});
	run(client, "trackVersion.add", {{"tracks", {a.track, b.track, c.track}}}); // v2, with an ID they share
	for (const ObjectId track : {a.track, b.track, c.track})
		run(client, "midiPart.add", {{"track", track}, {"position", 1920}, {"length", 960}});
	// Only A plays the version they share: on B and C its ID is one of a version that isn't active.
	run(client, "trackVersion.activate", {{"tracks", {b.track, c.track}}, {"version", 1}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	const auto source = [&sources](const std::string& name)
	{ return trackNamed({{"tracks", sources}}, name).at("id"); };

	run(client, "project.new");
	const Added x = add(client, {{"type", "midi"}, {"name", "X"}});
	const Added y = add(client, {{"type", "midi"}, {"name", "Y"}});
	const Added z = add(client, {{"type", "midi"}, {"name", "Z"}});
	run(client, "track.import",
		{{"path", song.u8string()}, {"tracks", {source("A"), source("B"), source("C")}},
			{"destinations",
				{{{"source", source("A")}, {"track", x.track}}, {{"source", source("B")}, {"track", y.track}},
					{{"source", source("C")}, {"track", z.track}}}}});
	Json project = run(client, "project.get");
	const Json first = trackNamed(project, "X");
	const Json second = trackNamed(project, "Y");
	const Json third = trackNamed(project, "Z");
	REQUIRE(idsOf(first).size() == 3);
	REQUIRE(idsOf(second).size() == 3);
	// What A played goes into X's new version, which shares its ID with the copies of B's and C's v2; what B and C
	// played goes into their new versions, which share their ID with the copy of A's v1.
	CHECK(first.at("activeVersion") == second.at("versions")[2].at("id"));
	CHECK(first.at("activeVersion") == third.at("versions")[2].at("id"));
	CHECK(second.at("activeVersion") == third.at("activeVersion"));
	CHECK(second.at("activeVersion") == first.at("versions")[2].at("id"));
	CHECK(first.at("activeVersion") != second.at("activeVersion"));
	// Each with the parts of the version of the file it holds.
	CHECK(playing(project, x.track) == std::vector<int64_t>{1920});
	CHECK(playing(project, y.track) == std::vector<int64_t>{0});
	REQUIRE(held(project, x.track, first.at("versions")[2].at("id")).at("midiParts").size() == 1);
	CHECK(held(project, x.track, first.at("versions")[2].at("id")).at("midiParts")[0].at("position") == 0);
	REQUIRE(held(project, y.track, second.at("versions")[2].at("id")).at("midiParts").size() == 1);
	CHECK(held(project, y.track, second.at("versions")[2].at("id")).at("midiParts")[0].at("position") == 1920);
	// The project file keeps the IDs.
	const std::vector<int> kept = idsOf(first);
	const fs::path saved = dir.path() / "Saved.dawproject";
	run(client, "project.save", {{"path", saved.u8string()}});
	run(client, "project.load", {{"path", saved.u8string()}});
	project = run(client, "project.get");
	CHECK(idsOf(trackNamed(project, "X")) == kept);
	CHECK(idsOf(trackNamed(project, "Y")) == idsOf(second));
	CHECK(trackNamed(project, "X").at("activeVersion") == first.at("activeVersion"));
}

TEST_CASE("track.import gives the versions of new tracks IDs of their own, shared as in the file", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added a = add(client, {{"type", "midi"}, {"name", "A"}});
	const Added b = add(client, {{"type", "midi"}, {"name", "B"}});
	const Added c = add(client, {{"type", "midi"}, {"name", "C"}});
	for (const ObjectId track : {a.track, b.track, c.track})
		run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 960}});
	run(client, "trackVersion.add", {{"tracks", {a.track, b.track, c.track}}}); // v2, with an ID they share
	for (const ObjectId track : {a.track, b.track, c.track})
		run(client, "midiPart.add", {{"track", track}, {"position", 1920}, {"length", 960}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json source = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks")[2].at("id");

	run(client, "project.new");
	// The project has a version group of its own, which the file's IDs must not fall into.
	const Added x = add(client, {{"type", "midi"}, {"name", "X"}});
	const Added y = add(client, {{"type", "midi"}, {"name", "Y"}});
	run(client, "trackVersion.add", {{"tracks", {x.track, y.track}}});
	REQUIRE(trackNamed(run(client, "project.get"), "X").at("activeVersion") == 2);
	// Two of the file's tracks come as new ones, the third is imported onto a track of the project.
	const Added z = add(client, {{"type", "midi"}, {"name", "Z"}});
	const Json imported = run(client, "track.import",
		{{"path", song.u8string()}, {"destinations", {{{"source", source}, {"track", z.track}}}}});
	REQUIRE(imported.at("tracks").size() == 2);
	const Json project = run(client, "project.get");
	const ObjectId upper = imported.at("tracks")[0].get<ObjectId>();
	const Json first = find(project.at("tracks"), upper);
	const Json second = find(project.at("tracks"), imported.at("tracks")[1].get<ObjectId>());
	const Json onto = trackNamed(project, "Z");
	REQUIRE(idsOf(first).size() == 2);
	REQUIRE(idsOf(onto).size() == 3);
	// The copies' versions share the IDs they shared in the file, and none of them is one the project has.
	CHECK(idsOf(second) == idsOf(first));
	CHECK(first.at("activeVersion") == second.at("activeVersion"));
	for (const int id : idsOf(first))
	{
		CHECK(id != 1);
		CHECK(id != 2);
	}
	// The track imported onto shares them: its new version plays with the copies' v2, and its copy of the file's v1
	// with their v1.
	CHECK(onto.at("activeVersion") == first.at("versions")[1].at("id"));
	CHECK(onto.at("versions")[2].at("id") == first.at("versions")[0].at("id"));
	// What the file's tracks played plays here, and their first version keeps its part.
	CHECK(playing(project, upper) == std::vector<int64_t>{1920});
	const Json kept = held(project, upper, first.at("versions")[0].at("id"));
	REQUIRE(kept.at("midiParts").size() == 1);
	CHECK(kept.at("midiParts")[0].at("position") == 0);
}

TEST_CASE("track.import without automation resets the automation of a track that it imports events or settings onto",
	"[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added lead = add(client, {{"type", "instrument"}, {"name", "Lead"}});
	run(client, "midiPart.add", {{"track", lead.track}, {"position", 0}, {"length", 960}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json source = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks")[0].at("id");

	run(client, "project.new");
	const Added mine = add(client, {{"type", "instrument"}, {"name", "Mine"}});
	const ObjectId lane = run(client, "automationLane.add", {{"track", mine.track}, {"target", {{"type", "volume"}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	run(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", -6.0}}}}});
	const Json parameters = {{"path", song.u8string()}, {"tracks", {source}}, {"automation", false},
		{"destinations", {{{"source", source}, {"track", mine.track}}}}};
	const auto lanes = [&](bool events, bool settings)
	{
		Json import = parameters;
		import["events"] = events;
		import["settings"] = settings;
		run(client, "track.import", import);
		const size_t result = count(run(client, "project.get"), "automation", mine.track);
		run(client, "history.undo");
		REQUIRE(count(run(client, "project.get"), "automation", mine.track) == 1);
		return result;
	};
	CHECK(lanes(true, false) == 0);
	CHECK(lanes(false, true) == 0);
	CHECK(lanes(true, true) == 0);
	// Importing neither events nor settings changes nothing: the track keeps its automation, and the import records no
	// undo step, so the next undo takes the fixture's point back.
	Json nothing = parameters;
	nothing["events"] = false;
	nothing["settings"] = false;
	run(client, "track.import", nothing);
	CHECK(count(run(client, "project.get"), "automation", mine.track) == 1);
	run(client, "history.undo");
	const Json after = run(client, "project.get");
	REQUIRE(count(after, "automation", mine.track) == 1);
	CHECK(after.at("automation")[0].at("points").empty());
}

TEST_CASE("track.import moves the project end after what it imports", "[integration]")
{
	constexpr int64_t second = 48000; // the project's samples
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	const fs::path take = dir.path() / "take.wav";
	test::writeWav(take, {std::vector<float>(44100, 0.25f), 1, 44100}); // a second at another rate than the project's
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added late = add(client, {{"type", "midi"}, {"name", "Late"}, {"timeBase", "linear"}});
	run(client, "midiPart.add", {{"track", late.track}, {"position", 700 * second}, {"length", 2 * second}});
	const Added cues = add(client, {{"type", "marker"}, {"name", "Cues"}, {"timeBase", "linear"}});
	run(client, "marker.add",
		{{"track", cues.track}, {"type", "cycle"}, {"position", 650 * second}, {"end", 705 * second},
			{"name", "Outro"}});
	// A musical part, from 710 to 712 seconds (1420 to 1424 quarters at 120 BPM).
	const Added musical = add(client, {{"type", "instrument"}, {"name", "Musical"}});
	run(client, "midiPart.add",
		{{"track", musical.track}, {"position", 1420 * gnubase::time::g_ticksPerQuarter},
			{"length", 4 * gnubase::time::g_ticksPerQuarter}});
	// An event of the media file at the other rate, from 700 up to 701 seconds.
	const Added singing = add(client, {{"type", "audio"}, {"name", "Singing"}, {"timeBase", "linear"}});
	const ObjectId media = run(client, "media.add", {{"path", take.u8string()}}).at("media").at("id").get<ObjectId>();
	const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	run(client, "audioEvent.add",
		{{"track", singing.track}, {"clip", clip}, {"position", 700 * second}, {"offset", 0}, {"length", 44100}});
	// An empty audio part, from 700 up to 706 seconds.
	const Added room = add(client, {{"type", "audio"}, {"name", "Room"}, {"timeBase", "linear"}});
	run(client, "audioPart.add", {{"track", room.track}, {"position", 700 * second}, {"length", 6 * second}});
	// An automation point, at 703 seconds.
	const Added fader = add(client, {{"type", "audio"}, {"name", "Fader"}, {"timeBase", "linear"}});
	const ObjectId lane = run(client, "automationLane.add", {{"track", fader.track}, {"target", {{"type", "volume"}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	run(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 703 * second}, {"value", -6.0}}}}});
	// A part past the setup's 24 hours.
	const Added far = add(client, {{"type", "midi"}, {"name", "Far"}, {"timeBase", "linear"}});
	run(client, "midiPart.add", {{"track", far.track}, {"position", int64_t{90000} * second}, {"length", second}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json sources = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	const auto source = [&sources](const std::string& name)
	{ return trackNamed({{"tracks", sources}}, name).at("id"); };

	run(client, "project.new"); // 10 minutes long
	REQUIRE(run(client, "project.get").at("length") == 600.0);
	const auto imported = [&client, &song, &source](const std::string& name)
	{
		run(client, "track.import", {{"path", song.u8string()}, {"tracks", {source(name)}}});
		const double result = run(client, "project.get").at("length").get<double>();
		run(client, "history.undo");
		REQUIRE(run(client, "project.get").at("length") == 600.0);
		return result;
	};
	CHECK(imported("Late") == 702.0);
	// A cycle marker's end counts too, and a musical part's end in the project's tempo.
	CHECK(imported("Cues") == 705.0);
	CHECK(imported("Musical") == 712.0);
	// An audio event ends where its media does at the project's rate, and an audio part and an automation point count.
	CHECK(imported("Singing") == 701.0);
	CHECK(imported("Room") == 706.0);
	CHECK(imported("Fader") == 703.0);
	// The end stops at the setup's 24 hours.
	CHECK(imported("Far") == 86400.0);
	// A project that is longer stays as long.
	run(client, "project.set", {{"length", 800.0}});
	run(client, "track.import", {{"path", song.u8string()}, {"tracks", {source("Late")}}});
	CHECK(run(client, "project.get").at("length") == 800.0);
}

TEST_CASE("track.import compares a range in samples at the project's sample rate", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	run(client, "project.set", {{"sampleRate", 44100.0}});
	const Added keys = add(client, {{"type", "midi"}, {"name", "Keys"}, {"timeBase", "linear"}});
	run(client, "midiPart.add", {{"track", keys.track}, {"position", 44100}, {"length", 4410}}); // at 1 second
	run(client, "project.save", {{"path", song.u8string()}});

	run(client, "project.new"); // at 48 kHz
	const auto parts = [&client, &song](int64_t end)
	{
		run(client, "track.import", {{"path", song.u8string()}, {"range", {{"start", 0}, {"end", end}}}});
		const size_t result = run(client, "project.get").at("midiParts").size();
		run(client, "history.undo");
		return result;
	};
	// 1 second is 48000 samples here: a range up to 46000 ends before the part, one up to 48001 has it.
	CHECK(parts(46000) == 0);
	CHECK(parts(48001) == 1);
}

TEST_CASE("track.import merges a musical track onto a linear one with its versions and automation", "[integration]")
{
	constexpr int64_t quarter = gnubase::time::g_ticksPerQuarter; // half a second at 120 BPM
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added lead = add(client, {{"type", "instrument"}, {"name", "Lead"}}); // musical
	run(client, "midiPart.add", {{"track", lead.track}, {"position", quarter}, {"length", quarter}});
	run(client, "trackVersion.add", {{"tracks", {lead.track}}});
	run(client, "midiPart.add", {{"track", lead.track}, {"position", 2 * quarter}, {"length", quarter}});
	const ObjectId lane = run(client, "automationLane.add", {{"track", lead.track}, {"target", {{"type", "volume"}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	run(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 4 * quarter}, {"value", -6.0}}}}});
	run(client, "project.save", {{"path", song.u8string()}});
	const Json source = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks")[0].at("id");

	run(client, "project.new");
	const Added mine = add(client, {{"type", "instrument"}, {"name", "Mine"}, {"timeBase", "linear"}});
	run(client, "track.import",
		{{"path", song.u8string()}, {"tracks", {source}}, {"existing", "replace"},
			{"destinations", {{{"source", source}, {"track", mine.track}}}}});
	const Json project = run(client, "project.get");
	// In samples: the active version's part, the file's other version's part and the automation point.
	REQUIRE(count(project, "midiParts", mine.track) == 1);
	CHECK(project.at("midiParts")[0].at("position") == 48000);
	CHECK(project.at("midiParts")[0].at("length") == 24000);
	Json other;
	for (const Json& contents : project.at("versionContents"))
		if (contents.at("track") == mine.track)
			other = contents;
	REQUIRE(other.is_object());
	REQUIRE(other.at("midiParts").size() == 1);
	CHECK(other.at("midiParts")[0].at("position") == 24000);
	CHECK(other.at("midiParts")[0].at("length") == 24000);
	REQUIRE(count(project, "automation", mine.track) == 1);
	CHECK(project.at("automation")[0].at("points")[0].at("position") == 96000);
}

TEST_CASE("track.import returns the media files it added, not the ones it shares with the project", "[integration]")
{
	const test::TempDir dir;
	const fs::path shared = dir.path() / "shared.wav";
	const fs::path added = dir.path() / "added.wav";
	test::writeWav(shared, {std::vector<float>(4800, 0.25f), 1, 48000});
	test::writeWav(added, {std::vector<float>(4800, 0.5f), 1, 48000});
	const fs::path archive = dir.path() / "Takes.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added take = add(client, {{"type", "audio"}, {"name", "Take"}});
	int64_t position = 0;
	for (const fs::path& wav : {shared, added})
	{
		const ObjectId media =
			run(client, "media.add", {{"path", wav.u8string()}}).at("media").at("id").get<ObjectId>();
		const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
		run(client, "audioEvent.add",
			{{"track", take.track}, {"clip", clip}, {"position", position}, {"offset", 0}, {"length", 4800}});
		position += 4800;
	}
	run(client, "track.export", {{"tracks", {take.track}}, {"path", archive.u8string()}, {"copyMedia", false}});

	run(client, "project.new");
	const ObjectId mine = run(client, "media.add", {{"path", shared.u8string()}}).at("media").at("id").get<ObjectId>();
	const Json media = run(client, "track.import", {{"path", archive.u8string()}}).at("media");
	REQUIRE(media.size() == 1);
	CHECK(media[0] != mine);
	const Json project = run(client, "project.get");
	CHECK(project.at("media").size() == 2);
	CHECK(fs::equivalent(
		fs::u8path(find(project.at("media"), media[0].get<ObjectId>()).at("path").get<std::string>()), added));
}

TEST_CASE("track.import shares the project's clips that are the same, also those of an import before", "[integration]")
{
	const test::TempDir dir;
	const fs::path wav = dir.path() / "loop.wav";
	test::writeWav(wav, {std::vector<float>(4800, 0.25f), 1, 48000});
	const fs::path archive = dir.path() / "Loops.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added take = add(client, {{"type", "audio"}, {"name", "Take"}});
	const ObjectId media = run(client, "media.add", {{"path", wav.u8string()}}).at("media").at("id").get<ObjectId>();
	// Two clips of the file that differ only in their ids: the import makes them one.
	for (int i = 0; i < 2; ++i)
	{
		const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
		run(client, "audioEvent.add",
			{{"track", take.track}, {"clip", clip}, {"position", i * 4800}, {"offset", 0}, {"length", 4800}});
	}
	run(client, "track.export", {{"tracks", {take.track}}, {"path", archive.u8string()}});

	run(client, "project.new");
	const auto clipsOfEvents = [&client]
	{
		std::set<ObjectId> clips;
		const Json project = run(client, "project.get");
		for (const Json& event : project.at("audioEvents"))
			clips.insert(event.at("clip").get<ObjectId>());
		return clips;
	};
	run(client, "track.import", {{"path", archive.u8string()}});
	CHECK(run(client, "project.get").at("clips").size() == 1);
	CHECK(clipsOfEvents().size() == 1);
	// The archive imported again shares the clip and the media file of the first import.
	run(client, "track.import", {{"path", archive.u8string()}});
	const Json project = run(client, "project.get");
	CHECK(project.at("tracks").size() == 2);
	CHECK(project.at("audioEvents").size() == 4);
	CHECK(project.at("clips").size() == 1);
	CHECK(project.at("media").size() == 1);
	CHECK(clipsOfEvents().size() == 1);
}

TEST_CASE("track.importSources lists the sample rates of the media files each track of a file plays", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added takes = add(client, {{"type", "audio"}, {"name", "Takes"}});
	add(client, {{"type", "audio"}, {"name", "Silent"}}); // a track whose events play no media file
	int64_t position = 0;
	const auto event = [&](int rate, const std::string& name)
	{
		const fs::path wav = dir.path() / name;
		test::writeWav(wav, {std::vector<float>(100, 0.25f), 1, rate});
		const ObjectId media =
			run(client, "media.add", {{"path", wav.u8string()}}).at("media").at("id").get<ObjectId>();
		const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
		run(client, "audioEvent.add",
			{{"track", takes.track}, {"clip", clip}, {"position", position}, {"offset", 0}, {"length", 100}});
		position += 100;
	};
	// One rate twice and out of order, and one in a version that isn't active.
	event(44100, "middle.wav");
	event(48000, "high.wav");
	event(22050, "low.wav");
	event(44100, "middle again.wav");
	run(client, "trackVersion.add", {{"tracks", {takes.track}}});
	event(96000, "other.wav");
	run(client, "trackVersion.activate", {{"tracks", {takes.track}}, {"version", 1}});
	// A media file in the pool that no event plays is nobody's.
	const fs::path unused = dir.path() / "unused.wav";
	test::writeWav(unused, {std::vector<float>(100, 0.25f), 1, 32000});
	run(client, "media.add", {{"path", unused.u8string()}});
	run(client, "project.save", {{"path", song.u8string()}, {"embedMedia", false}});
	const Json tracks = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	REQUIRE(tracks.size() == 2);
	CHECK(tracks[0].at("name") == "Takes");
	CHECK(tracks[0].at("mediaSampleRates") == Json{22050.0, 44100.0, 48000.0, 96000.0});
	CHECK(tracks[1].at("name") == "Silent");
	CHECK(tracks[1].at("mediaSampleRates") == Json::array());
}

TEST_CASE("track.importSources counts a sampler's sample and, for a range, only what starts in it", "[integration]")
{
	const test::TempDir dir;
	const fs::path song = dir.path() / "Song.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	run(client, "project.set", {{"sampleRate", 44100.0}}); // the file's rate: the range is at the project's
	const auto clipOf = [&](int rate, const std::string& name)
	{
		const fs::path wav = dir.path() / name;
		test::writeWav(wav, {std::vector<float>(100, 0.25f), 1, rate});
		const ObjectId media =
			run(client, "media.add", {{"path", wav.u8string()}}).at("media").at("id").get<ObjectId>();
		return run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	};
	const Added takes = add(client, {{"type", "audio"}, {"name", "Takes"}, {"timeBase", "linear"}}); // in samples
	const auto event = [&](int rate, const std::string& name, int64_t position)
	{
		run(client, "audioEvent.add",
			{{"track", takes.track}, {"clip", clipOf(rate, name)}, {"position", position}, {"offset", 0},
				{"length", 100}});
	};
	event(22050, "before.wav", 0);     // at 0 seconds
	event(48000, "inside.wav", 44100); // at 1 second
	event(32000, "after.wav", 88200);  // at 2 seconds, where the range ends
	run(client, "trackVersion.add", {{"tracks", {takes.track}}});
	event(96000, "version.wav", 66150); // at 1.5 seconds, in a version that isn't active
	run(client, "trackVersion.activate", {{"tracks", {takes.track}}, {"version", 1}});
	// A sampler plays its sample wherever the range is.
	run(client, "sampler.create", {{"clip", clipOf(16000, "sample.wav")}, {"name", "Sampler"}});
	run(client, "project.save", {{"path", song.u8string()}, {"embedMedia", false}});

	run(client, "project.new"); // at 48 kHz: from 1 to 2 seconds
	const Json whole = run(client, "track.importSources", {{"path", song.u8string()}}).at("tracks");
	REQUIRE(whole.size() == 2);
	CHECK(whole[0].at("mediaSampleRates") == Json{22050.0, 32000.0, 48000.0, 96000.0});
	CHECK(whole[1].at("name") == "Sampler");
	CHECK(whole[1].at("mediaSampleRates") == Json{16000.0});
	const Json ranged =
		run(client, "track.importSources", {{"path", song.u8string()}, {"range", {{"start", 48000}, {"end", 96000}}}})
			.at("tracks");
	REQUIRE(ranged.size() == 2);
	CHECK(ranged[0].at("mediaSampleRates") == Json{48000.0, 96000.0});
	CHECK(ranged[1].at("mediaSampleRates") == Json{16000.0});
	CHECK_THAT(reject(client, "track.importSources",
				   {{"path", song.u8string()}, {"range", {{"start", 96000}, {"end", 48000}}}}),
		ContainsSubstring("range must end after its start"));
}

TEST_CASE("listing the tracks of a file reads no media file in its place", "[integration]")
{
	const test::TempDir dir;
	const fs::path wav = dir.path() / "take.wav";
	test::writeWav(wav, {std::vector<float>(4800, 0.25f), 1, 48000});
	const fs::path archive = dir.path() / "Take.dawproject";
	{
		Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
		Client client(engine);
		const Added take = add(client, {{"type", "audio"}, {"name", "Take"}});
		const ObjectId media =
			run(client, "media.add", {{"path", wav.u8string()}}).at("media").at("id").get<ObjectId>();
		const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
		run(client, "audioEvent.add",
			{{"track", take.track}, {"clip", clip}, {"position", 0}, {"offset", 0}, {"length", 4800}});
		run(client, "track.export", {{"tracks", {take.track}}, {"path", archive.u8string()}});
	}
	const Project listed = loadDawProject(archive.u8string(), (dir.path() / "media").u8string(), false);
	REQUIRE(listed.media.size() == 1);
	const fs::path entry = fs::u8path(listed.media.begin()->second.path); // its name in the file
	REQUIRE(entry.is_relative());
	// A file of that name in the working folder isn't read instead.
	const test::TempDir work;
	fs::create_directories(work.path() / entry.parent_path());
	test::writeWav(work.path() / entry, {std::vector<float>(100, 0.25f), 1, 22050});
	const fs::path previous = fs::current_path();
	fs::current_path(work.path());
	const Project again = loadDawProject(archive.u8string(), (dir.path() / "media").u8string(), false);
	fs::current_path(previous);
	CHECK(again.media.begin()->second.sampleRate == 48000.0);
	CHECK(again.media.begin()->second.length == 4800);
}

TEST_CASE("track archives and presets keep the fader top, and volumes come down to a lower one", "[integration]")
{
	const test::TempDir dir;
	const fs::path archive = dir.path() / "Loud.dawproject";
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	run(client, "project.set", {{"maxVolume", 12.0}});
	const Added loud = add(client, {{"type", "audio"}, {"name", "Loud"}});
	const Added fx = add(client, {{"type", "effect"}, {"name", "FX"}});
	run(client, "channel.set",
		{{"channel", loud.channel}, {"volume", 10.0}, {"pre", {{"gain", 4.0}}},
			{"sends", {{{"send", 1}, {"destination", fx.channel}, {"on", true}, {"level", 10.0}}}}});
	const ObjectId lane = run(client, "automationLane.add", {{"track", loud.track}, {"target", {{"type", "volume"}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	run(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", 10.0}}}}});
	run(client, "track.export", {{"tracks", {loud.track, fx.track}}, {"path", archive.u8string()}});
	run(client, "track.savePreset", {{"tracks", {loud.track}}, {"name", "Loud"}});

	for (const double top : {12.0, 6.0})
	{
		INFO("fader top " << top);
		run(client, "project.new");
		run(client, "project.set", {{"maxVolume", top}});
		const double expected = top == 12.0 ? 10.0 : 6.0;
		run(client, "track.import", {{"path", archive.u8string()}});
		Json project = run(client, "project.get");
		const Json channel = channelOf(project, "Loud");
		CHECK(channel.at("volume") == expected);
		CHECK(channel.at("pre").at("gain") == 4.0); // read from GNUbase's own data, not project.xml
		CHECK(channel.at("sends")[0].at("level") == expected);
		REQUIRE(project.at("automation").size() == 1);
		CHECK(project.at("automation")[0].at("points")[0].at("value") == expected);
		run(client, "history.undo");

		run(client, "track.addFromPreset", {{"name", "Loud"}});
		project = run(client, "project.get");
		CHECK(channelOf(project, "Loud").at("volume") == expected);
		CHECK(channelOf(project, "Loud").at("pre").at("gain") == 4.0);
	}
}

TEST_CASE("File > Export > Selected Tracks and File > Import > Tracks go through a track archive", "[ui]")
{
	test::UiApp app{{1000, 700}};
	const test::TempDir dir;
	app.appUi().showProject();
	for (const char* command : {"file.exportTracks", "file.importTracks"})
		CHECK(app.ui()
				  .dom(std::nullopt, std::string("#file-menu menuitem[command=\"") + command + "\"]", 0)
				  .at("elements")
				  .size() == 1);
	CHECK(!app.keys().canRun("file.exportTracks", app.main(), nullptr)); // nothing selected
	const ObjectId vocals =
		app.call("track.add", {{"type", "audio"}, {"name", "Vocals"}}).at("track").at("id").get<ObjectId>();
	app.call("track.add", {{"type", "audio"}, {"name", "Other"}});
	select(app, {vocals});
	CHECK(app.keys().canRun("file.exportTracks", app.main(), nullptr));

	// The file chosen gets the extension .dawproject if it has none.
	app.keys().run("file.exportTracks", Json::object(), app.main(), nullptr);
	answer(app, dir.path() / "stems");
	REQUIRE(app.waitUntil([&dir] { return fs::exists(dir.path() / "stems.dawproject"); }));

	app.call("project.new");
	const ObjectId top =
		app.call("track.add", {{"type", "audio"}, {"name", "Top"}}).at("track").at("id").get<ObjectId>();
	app.call("track.add", {{"type", "audio"}, {"name", "Bottom"}});
	select(app, {top});
	// The file chosen opens in the Import Options dialog, which imports below the selected track.
	const auto importTracks = [&app](const fs::path& file)
	{
		app.keys().run("file.importTracks", Json::object(), app.main(), nullptr);
		answer(app, file);
		REQUIRE(app.waitUntil([&app] { return shown(app, "#track-import-dialog"); }));
	};
	const auto text = [&app](const std::string& selector)
	{ return app.ui().dom(std::nullopt, selector, 0).at("elements")[0].at("text").get<std::string>(); };
	importTracks(dir.path() / "stems.dawproject");
	click(app, "#import-confirm");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").size() == 3; }));
	CHECK(outline(app.call("project.get")) == std::vector<std::string>{"Top:audio", "Vocals:audio", "Bottom:audio"});
	// Select Matching imports onto the track of the same name, into a new version of it.
	importTracks(dir.path() / "stems.dawproject");
	click(app, "#import-select-matching");
	click(app, "#import-confirm");
	REQUIRE(app.waitUntil([&app] { return trackNamed(app.call("project.get"), "Vocals").at("versions").size() == 2; }));
	CHECK(app.call("project.get").at("tracks").size() == 3);

	// The engine's refusals show, in the dialog or the project error dialog, and the tracks left out.
	const auto error = [&app, &text](const std::string& title)
	{
		REQUIRE(app.waitUntil([&app] { return shown(app, "#project-error-dialog"); }));
		CHECK(text("#project-error-title") == title);
		const std::string message = text("#project-error-message");
		click(app, "#project-error-dialog button[result=ok]");
		REQUIRE(app.waitUntil([&app] { return !shown(app, "#project-error-dialog"); }));
		return message;
	};
	importTracks(dir.path() / "none.dawproject");
	CHECK_THAT(text("#import-message"), ContainsSubstring("none.dawproject"));
	click(app, "#track-import-dialog button[result=cancel]");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#track-import-dialog"); }));
	test::writeWav(dir.path() / "take.wav", {std::vector<float>(10, 0.0f), 1, 48000});
	select(app, {top});
	app.keys().run("file.exportTracks", Json::object(), app.main(), nullptr);
	answer(app, dir.path() / "take.wav" / "stems.dawproject"); // not a folder
	CHECK_THAT(error("Project Error"), ContainsSubstring("take.wav"));
	const ObjectId folder =
		app.call("track.add", {{"type", "folder"}, {"name", "Band"}}).at("track").at("id").get<ObjectId>();
	app.call("track.add", {{"type", "vca"}, {"name", "Faders"}, {"parent", folder}});
	app.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	select(app, {folder});
	app.keys().run("file.exportTracks", Json::object(), app.main(), nullptr);
	answer(app, dir.path() / "band.dawproject");
	CHECK_THAT(error("Export Selected Tracks"), ContainsSubstring("Faders"));
	// The dialog shows the tracks that can't be imported.
	importTracks(dir.path() / "song.dawproject");
	CHECK_THAT(text("#import-tracks"), ContainsSubstring("Faders (can't be imported yet)"));
	click(app, "#track-import-dialog button[result=cancel]");
}

TEST_CASE("Import Options copies the imported media into the project folder, converting other sample rates", "[ui]")
{
	test::UiApp app{{1000, 700}};
	const test::TempDir dir;
	const fs::path low = dir.path() / "library" / "low.wav";
	const fs::path high = dir.path() / "library" / "high.wav";
	fs::create_directories(low.parent_path());
	test::writeWav(low, {std::vector<float>(4410, 0.25f), 1, 44100});
	test::writeWav(high, {std::vector<float>(4800, 0.5f), 1, 48000});
	const fs::path archive = dir.path() / "takes.dawproject";
	const fs::path converted = dir.path() / "converted.dawproject";
	app.appUi().showProject();
	app.call("project.set", {{"sampleRate", 44100.0}});
	int64_t position = 0;
	Json takes = Json::array();
	ObjectId upper = 0;
	for (const auto& named : {std::pair{"Low", low}, std::pair{"High", high}})
	{
		upper = app.call("track.add", {{"type", "audio"}, {"name", named.first}}).at("track").at("id").get<ObjectId>();
		takes.push_back(upper);
		const ObjectId media =
			app.call("media.add", {{"path", named.second.u8string()}}).at("media").at("id").get<ObjectId>();
		const ObjectId clip = app.call("clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
		app.call("audioEvent.add",
			{{"track", upper}, {"clip", clip}, {"position", position}, {"offset", 0}, {"length", 4410}});
		position += 44100;
	}
	// An archive with both media files, and one whose only media file is at the rate the project imports into, though
	// the file itself is at another.
	app.call("track.export", {{"tracks", takes}, {"path", archive.u8string()}, {"copyMedia", false}});
	app.call("track.export", {{"tracks", {upper}}, {"path", converted.u8string()}, {"copyMedia", false}});
	const Json archiveTracks = app.call("track.importSources", {{"path", archive.u8string()}}).at("tracks");
	REQUIRE(archiveTracks.size() == 2);
	REQUIRE(archiveTracks[0].at("name") == "Low");
	const std::string lowRow = "#import-source-" + std::to_string(archiveTracks[0].at("id").get<ObjectId>());

	app.call("project.new"); // at 48 kHz
	const fs::path song = dir.path() / "song" / "Song.dawproject";
	fs::create_directories(song.parent_path());
	app.call("project.save", {{"path", song.u8string()}, {"embedMedia", false}});
	app.keys().run("file.importTracks", Json::object(), app.main(), nullptr);
	answer(app, archive);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#track-import-dialog"); }));
	click(app, "#import-copy");
	click(app, "#import-convert");
	click(app, "#import-confirm");
	// The file at 48 kHz is copied, the one at 44.1 kHz converted, both into the project's media folder.
	const fs::path folder = song.parent_path() / "Audio";
	const auto inFolder = [&folder](const Json& media)
	{
		const fs::path path = fs::u8path(media.at("path").get<std::string>());
		return fs::exists(path) && fs::equivalent(path.parent_path(), folder) && media.at("sampleRate") == 48000.0;
	};
	REQUIRE(app.waitUntil(
		[&app, &inFolder]
		{
			const Json media = app.call("project.get").at("media");
			return media.size() == 2 && std::all_of(media.begin(), media.end(), inFolder);
		}));
	CHECK(fs::exists(folder / "high.wav"));

	// A file at another rate than the project's whose media files are at the project's has nothing to convert.
	const auto convertDisabled = [&app]
	{
		const Json found = app.ui().dom(std::nullopt, "#import-convert", 0).at("elements");
		return found.size() == 1 && found[0].at("attributes").contains("disabled");
	};
	const auto openDialog = [&app](const fs::path& file)
	{
		app.keys().run("file.importTracks", Json::object(), app.main(), nullptr);
		answer(app, file);
		REQUIRE(app.waitUntil([&app] { return shown(app, "#track-import-dialog"); }));
	};
	const auto close = [&app]
	{
		click(app, "#track-import-dialog button[result=cancel]");
		REQUIRE(app.waitUntil([&app] { return !shown(app, "#track-import-dialog"); }));
	};
	openDialog(converted);
	click(app, "#import-copy");
	CHECK(convertDisabled());
	close();

	// Convert Sample Rate follows the checked tracks: of the file's two tracks only the one at 44.1 kHz has anything
	// to convert, so it goes and comes with its checkbox.
	openDialog(archive);
	click(app, "#import-copy");
	REQUIRE(app.waitUntil([&convertDisabled] { return !convertDisabled(); }));
	click(app, lowRow);
	REQUIRE(app.waitUntil([&convertDisabled] { return convertDisabled(); }));
	click(app, lowRow);
	REQUIRE(app.waitUntil([&convertDisabled] { return !convertDisabled(); }));
	close();
}

TEST_CASE(
	"Import Options shows the project settings, folds, searches, matches and imports a range at a position", "[ui]")
{
	test::UiApp app{{1000, 700}};
	const test::TempDir dir;
	const fs::path wav = dir.path() / "low.wav";
	test::writeWav(wav, {std::vector<float>(4410, 0.25f), 1, 44100});
	app.appUi().showProject();
	// The file starts at 10 seconds, at the project's 48 kHz, with a media file at 44.1 kHz.
	app.call("project.set", {{"startTime", 10.0}});
	const ObjectId keys = app.call("track.add", {{"type", "midi"}, {"name", "Keys"}, {"timeBase", "linear"}})
							  .at("track")
							  .at("id")
							  .get<ObjectId>();
	for (const int position : {48000, 240000}) // 1 and 5 seconds after its start
		app.call("midiPart.add", {{"track", keys}, {"position", position}, {"length", 4800}});
	const ObjectId band =
		app.call("track.add", {{"type", "folder"}, {"name", "Band"}}).at("track").at("id").get<ObjectId>();
	const ObjectId bass = app.call("track.add", {{"type", "audio"}, {"name", "Bass"}, {"parent", band}})
							  .at("track")
							  .at("id")
							  .get<ObjectId>();
	const ObjectId media = app.call("media.add", {{"path", wav.u8string()}}).at("media").at("id").get<ObjectId>();
	const ObjectId clip = app.call("clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	app.call("audioEvent.add", {{"track", bass}, {"clip", clip}, {"position", 0}, {"offset", 0}, {"length", 4410}});
	const fs::path song = dir.path() / "song.dawproject";
	app.call("project.save", {{"path", song.u8string()}});
	const Json sources = app.call("track.importSources", {{"path", song.u8string()}}).at("tracks");
	const auto source = [&sources](const std::string& name)
	{ return std::to_string(trackNamed({{"tracks", sources}}, name).at("id").get<ObjectId>()); };

	// The project has a folder Band with a MIDI track Keys inside it, and its locators at 10 and 12 seconds.
	app.call("project.new");
	const fs::path active = dir.path() / "active" / "Active.dawproject";
	fs::create_directories(active.parent_path());
	app.call("project.save", {{"path", active.u8string()}, {"embedMedia", false}});
	const ObjectId folder =
		app.call("track.add", {{"type", "folder"}, {"name", "Band"}}).at("track").at("id").get<ObjectId>();
	app.call("track.add", {{"type", "midi"}, {"name", "Keys"}, {"parent", folder}});
	app.call("transport.set", {{"left", 480000}, {"right", 576000}});
	app.keys().run("file.importTracks", Json::object(), app.main(), nullptr);
	answer(app, song);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#track-import-dialog"); }));
	const auto element = [&app](const std::string& selector)
	{
		const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
		INFO(selector);
		REQUIRE(found.size() == 1);
		return found[0];
	};
	const auto disabled = [&element](const std::string& selector)
	{ return element(selector).at("attributes").contains("disabled"); };

	// Project Settings: the file's and the project's file name, sample rate, frame rate and start.
	const std::string file = element("#import-source-settings").at("text").get<std::string>();
	for (const char* part : {"song.dawproject", "48000 Hz", "25 fps", "00:00:10:00"})
		CHECK_THAT(file, ContainsSubstring(part));
	const std::string mine = element("#import-active-settings").at("text").get<std::string>();
	for (const char* part : {"Active.dawproject", "48000 Hz", "25 fps", "00:00:00:00"})
		CHECK_THAT(mine, ContainsSubstring(part));

	// Collapse All and Expand All fold the folder's tracks.
	const std::string bassRow = "#import-source-" + source("Bass");
	CHECK(shown(app, bassRow));
	click(app, "#import-collapse-all");
	REQUIRE(app.waitUntil([&app, &bassRow] { return !shown(app, bassRow); }));
	click(app, "#import-expand-all");
	REQUIRE(app.waitUntil([&app, &bassRow] { return shown(app, bassRow); }));

	// The search shows only the tracks whose names have the text; Clear Search shows them all again.
	click(app, "#import-search");
	app.ui().text(std::nullopt, "KEY");
	REQUIRE(app.waitUntil([&app, &source] { return !shown(app, "#import-track-" + source("Band")); }));
	CHECK(shown(app, "#import-track-" + source("Keys")));
	click(app, "#import-clear-search");
	REQUIRE(app.waitUntil([&app, &source] { return shown(app, "#import-track-" + source("Band")); }));

	// Select Matching: the folder goes onto the project's folder, but Keys not onto the track inside it, which the
	// folder replaces with the tracks inside it. Reset puts New Track back.
	const auto destination = [&element, &source](const std::string& name)
	{ return element("#import-destination-" + source(name)).at("value").get<std::string>(); };
	click(app, "#import-select-matching");
	CHECK(destination("Band") == std::to_string(folder));
	CHECK(destination("Keys") == "0");
	click(app, "#import-reset");
	CHECK(destination("Band") == "0");

	// Convert Sample Rate with Copy, as the file has a media file at another rate than the project's, though the file
	// itself is at the project's rate.
	CHECK(disabled("#import-convert"));
	click(app, "#import-copy");
	CHECK(!disabled("#import-convert"));
	// Bass counts while it goes along with its folder, though its own box isn't checked.
	click(app, "#import-source-" + source("Band"));
	click(app, bassRow);
	REQUIRE(app.waitUntil([&disabled] { return disabled("#import-convert"); }));
	click(app, "#import-source-" + source("Band"));
	REQUIRE(app.waitUntil([&disabled] { return !disabled("#import-convert"); }));
	click(app, "#import-copy");
	CHECK(disabled("#import-convert"));

	// Range from the locators' timecodes: what the file has from 10 to 12 seconds, which is its first second and the
	// part at 1 second, not the one at 5. Use Locators puts them back after an edit.
	CHECK(disabled("#import-range-start"));
	click(app, "#import-range");
	CHECK(!disabled("#import-range-start"));
	CHECK(element("#import-range-start").at("value") == "00:00:10:00");
	CHECK(element("#import-range-end").at("value") == "00:00:12:00");
	click(app, "#import-range-end");
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "00:00:20:00");
	REQUIRE(app.waitUntil([&element] { return element("#import-range-end").at("value") == "00:00:20:00"; }));
	click(app, "#import-locators");
	CHECK(element("#import-range-end").at("value") == "00:00:12:00");
	// Convert Sample Rate follows the range: Bass's media file starts at 10 seconds, so a range from 11 seconds
	// has nothing to convert.
	click(app, "#import-copy");
	REQUIRE(app.waitUntil([&disabled] { return !disabled("#import-convert"); }));
	click(app, "#import-range-start");
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "00:00:11:00");
	REQUIRE(app.waitUntil([&disabled] { return disabled("#import-convert"); }));
	click(app, "#import-locators");
	REQUIRE(app.waitUntil([&disabled] { return !disabled("#import-convert"); }));
	click(app, "#import-copy");
	// Relative Position: as far from the project's start as from the file's, at 1 second rather than at 11.
	click(app, "#import-position");
	app.ui().key(std::nullopt, "Down");
	click(app, "#import-position");
	REQUIRE(app.waitUntil([&element] { return element("#import-position").at("value") == "relative"; }));
	click(app, "#import-confirm");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#track-import-dialog"); }));
	const Json project = app.call("project.get");
	REQUIRE(project.at("midiParts").size() == 1);
	CHECK(project.at("midiParts")[0].at("position") == 48000);
	CHECK(outline(project) ==
		std::vector<std::string>{"Band:folder", "Band/Keys:midi", "Keys:midi", "Band:folder", "Band/Bass:audio"});
}

TEST_CASE(
	"Import Options' Select Matching keeps the upper of two tracks that match one and the tracks of a folder", "[ui]")
{
	test::UiApp app{{1000, 700}};
	const test::TempDir dir;
	const fs::path song = dir.path() / "song.dawproject";
	app.appUi().showProject();
	// The file has two tracks of one name and a folder with a track inside it.
	const ObjectId first =
		app.call("track.add", {{"type", "audio"}, {"name", "Take"}}).at("track").at("id").get<ObjectId>();
	app.call("track.add", {{"type", "audio"}, {"name", "Take"}});
	const ObjectId band =
		app.call("track.add", {{"type", "folder"}, {"name", "Band"}}).at("track").at("id").get<ObjectId>();
	app.call("track.add", {{"type", "audio"}, {"name", "Inside"}, {"parent", band}});
	app.call("project.save", {{"path", song.u8string()}, {"embedMedia", false}});
	const Json sources = app.call("track.importSources", {{"path", song.u8string()}}).at("tracks");
	REQUIRE(sources.size() == 4);
	REQUIRE(sources[0].at("id") == first);

	// The project has one track of each name, its own folder apart.
	app.call("project.new");
	const ObjectId take =
		app.call("track.add", {{"type", "audio"}, {"name", "Take"}}).at("track").at("id").get<ObjectId>();
	const ObjectId inside =
		app.call("track.add", {{"type", "audio"}, {"name", "Inside"}}).at("track").at("id").get<ObjectId>();
	const ObjectId folder =
		app.call("track.add", {{"type", "folder"}, {"name", "Band"}}).at("track").at("id").get<ObjectId>();
	app.keys().run("file.importTracks", Json::object(), app.main(), nullptr);
	answer(app, song);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#track-import-dialog"); }));
	const auto destination = [&app](ObjectId source)
	{
		const Json found =
			app.ui().dom(std::nullopt, "#import-destination-" + std::to_string(source), 0).at("elements");
		REQUIRE(found.size() == 1);
		return found[0].at("value").get<std::string>();
	};
	click(app, "#import-select-matching");
	// Of the file's two tracks called Take the upper one keeps the project's, the other stays at New Track.
	CHECK(destination(sources[0].at("id").get<ObjectId>()) == std::to_string(take));
	CHECK(destination(sources[1].at("id").get<ObjectId>()) == "0");
	// The track that goes along with the folder stays at New Track, though the project has one of its name.
	CHECK(destination(sources[2].at("id").get<ObjectId>()) == std::to_string(folder));
	CHECK(destination(sources[3].at("id").get<ObjectId>()) == "0");
	CHECK(destination(sources[3].at("id").get<ObjectId>()) != std::to_string(inside));
	// The import the dialog sends isn't refused: the folder replaces the project's with the track inside it, the
	// upper Take merges onto the project's and the other becomes a new track.
	click(app, "#import-confirm");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#track-import-dialog"); }));
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").size() == 5; }));
	CHECK(!shown(app, "#project-error-dialog"));
}

TEST_CASE("the track preset dialog saves, loads and adds track presets", "[ui]")
{
	test::UiApp app{{1000, 700}};
	app.appUi().showProject();
	for (const char* command : {"project.addTrackFromPreset", "track.saveTrackPreset", "track.loadTrackPreset"})
		CHECK(app.ui()
				  .dom(std::nullopt, std::string("#track-list-menu menuitem[command=\"") + command + "\"]", 0)
				  .at("elements")
				  .size() == 1);
	const Json vocals = app.call("track.add", {{"type", "audio"}, {"name", "Vocals"}});
	app.call("channel.set", {{"channel", vocals.at("channel").at("id")}, {"volume", -6.0}});
	select(app, {vocals.at("track").at("id").get<ObjectId>()});
	app.keys().run("track.saveTrackPreset", Json::object(), app.main(), nullptr);
	click(app, "#preset-name");
	app.ui().text(std::nullopt, "Vox");
	click(app, "#preset-confirm");
	REQUIRE(app.waitUntil([&app] { return !shown(app, "#track-preset-dialog"); }));
	const Json presets = app.call("track.presets").at("presets");
	REQUIRE(presets.size() == 1);
	CHECK(presets[0].at("name") == "Vox");

	// Load lists the presets of the selected tracks' types: none for a MIDI track.
	const Json midi = app.call("track.add", {{"type", "midi"}, {"name", "Keys"}});
	select(app, {midi.at("track").at("id").get<ObjectId>()});
	app.keys().run("track.loadTrackPreset", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&app] { return shown(app, "#track-preset-dialog"); }));
	CHECK(app.ui().dom(std::nullopt, "#preset-list option", 0).at("elements").empty());
	click(app, "#track-preset-dialog button[result=cancel]");

	const Json other = app.call("track.add", {{"type", "audio"}, {"name", "Other"}});
	select(app, {other.at("track").at("id").get<ObjectId>()});
	app.keys().run("track.loadTrackPreset", Json::object(), app.main(), nullptr);
	click(app, "#preset-confirm");
	REQUIRE(app.waitUntil([&app] { return channelOf(app.call("project.get"), "Other").at("volume") == -6.0; }));

	app.keys().run("project.addTrackFromPreset", Json::object(), app.main(), nullptr);
	click(app, "#preset-confirm");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").size() == 4; }));
	CHECK(outline(app.call("project.get")) ==
		std::vector<std::string>{"Vocals:audio", "Keys:midi", "Other:audio", "Vocals:audio"});
}

TEST_CASE("track presets keep a MIDI send's effect but not its output to another track", "[unit]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added keys = add(client, {{"type", "instrument"}, {"name", "Keys"}});
	const Added bass = add(client, {{"type", "instrument"}, {"name", "Bass"}});
	run(client, "channel.set",
		{{"channel", keys.channel},
			{"midiSends", {{"slots", {{{"effect", "chordGenerator"}, {"output", bass.channel}}}}}}});
	run(client, "track.savePreset", {{"tracks", {keys.track}}, {"name", "Chords"}});
	const Added other = add(client, {{"type", "instrument"}, {"name", "Other"}});
	run(client, "track.loadPreset", {{"tracks", {other.track}}, {"name", "Chords"}});
	const Json slot = channelOf(run(client, "project.get"), "Other").at("midiSends").at("slots")[0];
	CHECK(slot.at("effect") == "chordGenerator");
	CHECK(slot.at("output") == 0);
}

TEST_CASE("a track preset that changes a MIDI slot's effect takes the slot's automation lanes along", "[unit]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const Added keys = add(client, {{"type", "instrument"}, {"name", "Keys"}});
	run(client, "channel.set",
		{{"channel", keys.channel},
			{"midiInserts", {{"slots", {{{"effect", "chordGenerator"}}, {{"effect", "transformer"}}}}}}});
	run(client, "track.savePreset", {{"tracks", {keys.track}}, {"name", "Chords"}});
	const Added other = add(client, {{"type", "instrument"}, {"name", "Other"}});
	run(client, "channel.set",
		{{"channel", other.channel},
			{"midiInserts", {{"slots", {{{"effect", "transformer"}}, {{"effect", "transformer"}}}}}}});
	// Lanes of both slots: the first gets another effect, the second keeps its own.
	for (const int number : {0, 1})
		run(client, "automationLane.add",
			{{"track", other.track}, {"target", {{"type", "midiEffect"}, {"number", number}, {"parameter", 4}}}});
	REQUIRE(run(client, "project.get").at("automation").size() == 2);
	run(client, "track.loadPreset", {{"tracks", {other.track}}, {"name", "Chords"}});
	const Json automation = run(client, "project.get").at("automation");
	REQUIRE(automation.size() == 1);
	CHECK(automation[0].at("target").at("number") == 1);
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("automation").size() == 2);
}
