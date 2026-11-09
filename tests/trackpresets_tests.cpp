// SPDX-License-Identifier: GPL-3.0-or-later
// Track presets and track import and export (#164, docs/features/track-presets.md): saving the sound of one or more
// tracks as a preset and applying it to tracks or adding tracks from it, and exporting tracks as a track archive and
// importing tracks from archives and projects, round trip.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/testutils.h"
#include "ui/automation.h"
#include "ui/tracklist.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
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
	run(client, "channel.set", {{"channel", strings.channel}, {"program", 48}, {"midiVolume", 90}});
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
	const ObjectId lane = run(client, "automationLane.add", {{"track", vocals.track}, {"target", {{"type", "volume"}}}})
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
	app.keys().run("file.importTracks", Json::object(), app.main(), nullptr);
	answer(app, dir.path() / "stems.dawproject");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("tracks").size() == 3; }));
	CHECK(outline(app.call("project.get")) == std::vector<std::string>{"Top:audio", "Vocals:audio", "Bottom:audio"});
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
