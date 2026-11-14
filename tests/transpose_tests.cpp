// SPDX-License-Identifier: GPL-3.0-or-later
// The project root key and the transpose track (#168, reference manual pp. 165, 414-421): global transposition of the
// MIDI parts that follow it, Keep Transpose in Octave Range, muting and locking the transpose track, parts excluded
// with Global Transpose Independent, Set Root Key for Unassigned Events, recording and the project file.
#include "engine/document/document.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;

namespace
{
	// 120 BPM at 48 kHz: a beat is 24000 samples.
	constexpr int64_t g_beat = 24000;
	constexpr int64_t g_beatTicks = 1720320;

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	// A MIDI track with a part of four beats from 0 that has a C4 (60) on every beat.
	struct Song
	{
		Song()
		{
			const Json added = run(client, "track.add", {{"type", "midi"}});
			channel = added.at("channel").at("id").get<ObjectId>();
			track = added.at("track").at("id").get<ObjectId>();
			part = run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * g_beatTicks}})
					   .at("part")
					   .at("id")
					   .get<ObjectId>();
			Json notes = Json::array();
			for (int64_t beat = 0; beat < 4; ++beat)
				notes.push_back(
					{{"position", beat * g_beatTicks}, {"length", g_beatTicks / 2}, {"pitch", 60}, {"velocity", 0.75}});
			run(client, "note.add", {{"part", part}, {"notes", notes}});
		}

		// The pitches the track plays, by position, from the render snapshot.
		std::vector<int> pitches()
		{
			std::vector<std::pair<int64_t, int>> notes;
			for (const RenderChannel& render : engine.renderSnapshots().acquire().channels)
				if (render.id == channel)
					for (const RenderNote& note : render.notes)
						notes.emplace_back(note.start, note.pitch);
			std::sort(notes.begin(), notes.end());
			std::vector<int> result;
			result.reserve(notes.size());
			for (const auto& [start, pitch] : notes)
				result.push_back(pitch);
			return result;
		}

		Json partJson()
		{
			const Json project = run(client, "project.get");
			for (const Json& item : project.at("midiParts"))
				if (item.at("id") == part)
					return item;
			return {};
		}

		Engine engine;
		Client client{engine};
		ObjectId channel = 0;
		ObjectId track = 0;
		ObjectId part = 0;
	};

	// A song with an audio track too, linear, with a mono event of a second of silence at 48 kHz from 0 (#1368).
	struct AudioSong : Song
	{
		AudioSong()
		{
			gnubase::test::Audio audio;
			audio.samples.assign(48000, 0.0F);
			gnubase::test::writeWav(file, audio);
			const Json added = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 1}});
			audioChannel = added.at("channel").at("id").get<ObjectId>();
			audioTrack = added.at("track").at("id").get<ObjectId>();
			const Json media = run(client, "media.add", {{"path", file.u8string()}}).at("media");
			clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<ObjectId>();
			event = run(client, "audioEvent.add",
				{{"track", audioTrack}, {"clip", clip}, {"position", 0}, {"offset", 0}, {"length", 48000}})
						.at("event")
						.at("id")
						.get<ObjectId>();
		}

		Json eventJson(ObjectId id = 0)
		{
			const Json project = run(client, "project.get");
			for (const Json& item : project.at("audioEvents"))
				if (item.at("id") == (id != 0 ? id : event))
					return item;
			return {};
		}

		// The pitch map the event plays with, as {target, semitones} pairs: empty while it plays its file as it is.
		std::vector<std::pair<double, double>> pitch()
		{
			const auto snapshot = engine.renderSnapshots().acquire();
			std::vector<std::pair<double, double>> result;
			for (const RenderChannel& render : snapshot.channels)
				if (render.id == audioChannel)
					for (const RenderAudioEvent& played : render.audioEvents)
						if (played.id == event)
							for (const dsp::PitchPoint& point :
								snapshot.media.at(static_cast<size_t>(played.media)).pitch)
								result.emplace_back(point.target, point.semitones);
			return result;
		}

		gnubase::test::TempDir dir;
		std::filesystem::path file = dir.path() / "silence.wav";
		ObjectId audioChannel = 0;
		ObjectId audioTrack = 0;
		ObjectId clip = 0;
		ObjectId event = 0;
	};

	using Pitch = std::vector<std::pair<double, double>>;
}

TEST_CASE("the project root key transposes parts with a root key of their own to it", "[integration]")
{
	Song song;
	CHECK(run(song.client, "project.get").at("rootKey") == "");
	CHECK(song.partJson().at("rootKey") == "");
	CHECK(song.partJson().at("globalTranspose") == "follow");
	// Neither a project root key nor one of the part's: nothing moves.
	run(song.client, "project.set", {{"rootKey", "D"}});
	CHECK(song.pitches() == std::vector<int>{60, 60, 60, 60});
	// A part in C in a project in D plays a whole tone higher; in G, a fourth lower, the nearer way.
	CHECK(song.partJson().at("rootKeyTranspose") == 0);
	run(song.client, "midiPart.set", {{"part", song.part}, {"rootKey", "C"}});
	CHECK(song.pitches() == std::vector<int>{62, 62, 62, 62});
	CHECK(song.partJson().at("rootKeyTranspose") == 2); // as the info line's Transpose field shows it (p. 420)
	run(song.client, "project.set", {{"rootKey", "G"}});
	CHECK(song.pitches() == std::vector<int>{55, 55, 55, 55});
	CHECK(song.partJson().at("rootKeyTranspose") == -5);
	run(song.client, "project.set", {{"rootKey", "F#"}});
	CHECK(song.pitches() == std::vector<int>{66, 66, 66, 66});
	// The part's own transpose adds to it.
	run(song.client, "midiPart.set", {{"part", song.part}, {"transpose", -1}});
	CHECK(song.pitches() == std::vector<int>{65, 65, 65, 65});
	// Independent parts keep their pitch.
	run(song.client, "midiPart.set", {{"part", song.part}, {"globalTranspose", "independent"}});
	CHECK(song.pitches() == std::vector<int>{59, 59, 59, 59});
	CHECK(song.partJson().at("globalTranspose") == "independent");
	CHECK(song.partJson().at("rootKeyTranspose") == 0);
	// Undo brings back what the part followed; the root key is an undo step too.
	run(song.client, "history.undo");
	CHECK(song.pitches() == std::vector<int>{65, 65, 65, 65});
	CHECK(reject(song.client, "project.set", {{"rootKey", "H"}}).find("rootKey") != std::string::npos);
	CHECK(reject(song.client, "midiPart.set", {{"part", song.part}, {"globalTranspose", "both"}})
			  .find("globalTranspose") != std::string::npos);
}

TEST_CASE("transpose events transpose what follows global transpose from their position on", "[integration]")
{
	Song song;
	CHECK(run(song.client, "transpose.get").at("track") == 0);
	// The first event adds the transpose track.
	const Json first = run(song.client, "transpose.add", {{"position", 2 * g_beatTicks}, {"value", 5}});
	const Json transpose = run(song.client, "transpose.get");
	const auto track = transpose.at("track").get<ObjectId>();
	CHECK(track != 0);
	CHECK(transpose.at("events").size() == 1);
	CHECK(transpose.at("events")[0].at("start") == 2 * g_beat);
	CHECK(song.pitches() == std::vector<int>{60, 60, 65, 65});
	CHECK(reject(song.client, "track.add", {{"type", "transpose"}}) == "a project has one transpose track at most");

	// An event before it sets the beats up to it; values add to the root key's transposition.
	const auto second =
		run(song.client, "transpose.add", {{"position", g_beatTicks}, {"value", -2}}).at("event").at("id");
	CHECK(song.pitches() == std::vector<int>{60, 58, 65, 65});
	run(song.client, "project.set", {{"rootKey", "D"}});
	run(song.client, "midiPart.set", {{"part", song.part}, {"rootKey", "C"}});
	CHECK(song.pitches() == std::vector<int>{62, 60, 67, 67});
	CHECK(run(song.client, "transpose.get").at("events")[0].at("id") == second); // in timeline order

	// Keep Transpose in Octave Range: never more than seven semitones either way.
	run(song.client, "transpose.set", {{"events", {{{"event", first.at("event").at("id")}, {"value", 8}}}}});
	CHECK(song.pitches() == std::vector<int>{62, 60, 70, 70});
	run(song.client, "transpose.setTrack", {{"keepInOctave", true}});
	CHECK(song.pitches() == std::vector<int>{62, 60, 58, 58}); // 2 + 8 = 10 is -2
	// Muted, the events don't play; the root key still does.
	run(song.client, "transpose.setTrack", {{"mute", true}});
	CHECK(song.pitches() == std::vector<int>{62, 62, 62, 62});
	run(song.client, "history.undo");
	CHECK(song.pitches() == std::vector<int>{62, 60, 58, 58});

	// Moving an event moves where it starts.
	run(song.client, "transpose.set", {{"events", {{{"event", second}, {"position", 3 * g_beatTicks}}}}});
	CHECK(song.pitches() == std::vector<int>{62, 62, 58, 60});
	CHECK(reject(song.client, "transpose.set", {{"events", {{{"event", second}, {"value", 25}}}}}) ==
		"a transpose event's value must be -24 to 24 semitones");
	CHECK(reject(song.client, "transpose.add", {{"position", 2 * g_beatTicks}}) ==
		"there is a transpose event at that position already");

	// Locked, the events can't be changed.
	run(song.client, "transpose.setTrack", {{"lock", true}});
	CHECK(reject(song.client, "transpose.remove", {{"events", {second}}}) == "the transpose track is locked");
	CHECK(reject(song.client, "transpose.add", {{"position", 0}}) == "the transpose track is locked");
	run(song.client, "transpose.setTrack", {{"lock", false}});
	run(song.client, "transpose.remove", {{"events", {second}}});
	CHECK(run(song.client, "transpose.get").at("events").size() == 1);

	// Removing the transpose track removes its events.
	run(song.client, "track.remove", {{"track", track}});
	CHECK(run(song.client, "transpose.get").at("events").empty());
	CHECK(song.pitches() == std::vector<int>{62, 62, 62, 62});
}

TEST_CASE("Keep Transpose in Octave Range folds the part's own transpose into the sum", "[integration]")
{
	Song song;
	run(song.client, "project.set", {{"rootKey", "D"}});
	run(song.client, "midiPart.set", {{"part", song.part}, {"rootKey", "C"}, {"transpose", 3}});
	// The root key's step, the transpose event and the part's own transpose add up to 2 + 8 + 3 = 13 semitones.
	run(song.client, "transpose.add", {{"position", 0}, {"value", 8}});
	CHECK(song.pitches() == std::vector<int>{73, 73, 73, 73}); // the option off changes nothing
	run(song.client, "transpose.setTrack", {{"keepInOctave", true}});
	CHECK(song.pitches() == std::vector<int>{61, 61, 61, 61}); // 13 is 1 up
	// Seven semitones either way stay as they are: 2 + 8 - 3 = 7 is 7 up...
	run(song.client, "midiPart.set", {{"part", song.part}, {"transpose", -3}});
	CHECK(song.pitches() == std::vector<int>{67, 67, 67, 67});
	// ... and 2 + 8 - 17 = -7 is 7 down.
	run(song.client, "midiPart.set", {{"part", song.part}, {"transpose", -17}});
	CHECK(song.pitches() == std::vector<int>{53, 53, 53, 53});
	// Eight fold: 2 + 8 - 2 = 8 is 4 down, and 2 + 8 - 18 = -8 is 4 up.
	run(song.client, "midiPart.set", {{"part", song.part}, {"transpose", -2}});
	CHECK(song.pitches() == std::vector<int>{56, 56, 56, 56});
	run(song.client, "midiPart.set", {{"part", song.part}, {"transpose", -18}});
	CHECK(song.pitches() == std::vector<int>{64, 64, 64, 64});
	// Muted, the event drops out of the sum, which the option still folds: 2 + 12 = 14 is 2 up.
	run(song.client, "midiPart.set", {{"part", song.part}, {"transpose", 12}});
	run(song.client, "transpose.setTrack", {{"mute", true}});
	CHECK(song.pitches() == std::vector<int>{62, 62, 62, 62});
	// An Independent part is outside global transpose: its own transpose stays as it is, unwrapped.
	run(song.client, "midiPart.set", {{"part", song.part}, {"globalTranspose", "independent"}});
	CHECK(song.pitches() == std::vector<int>{72, 72, 72, 72});
}

TEST_CASE("Set Root Key for Unassigned Events gives parts without a root key the project's", "[integration]")
{
	Song song;
	CHECK(reject(song.client, "transpose.assignRootKey") == "the project has no root key");
	const ObjectId other = run(
		song.client, "midiPart.add", {{"track", song.track}, {"position", 4 * g_beatTicks}, {"length", g_beatTicks}})
							   .at("part")
							   .at("id")
							   .get<ObjectId>();
	run(song.client, "midiPart.set", {{"part", other}, {"rootKey", "A"}});
	run(song.client, "project.set", {{"rootKey", "E"}});
	run(song.client, "transpose.assignRootKey");
	const Json project = run(song.client, "project.get");
	for (const Json& part : project.at("midiParts"))
		CHECK(part.at("rootKey") == (part.at("id") == other ? "A" : "E"));
	run(song.client, "history.undo");
	CHECK(song.partJson().at("rootKey") == "");
}

TEST_CASE("glue refuses MIDI parts that follow global transpose differently", "[integration]")
{
	Song song;
	const ObjectId other = run(song.client, "midiPart.add",
		{{"track", song.track}, {"position", 4 * g_beatTicks}, {"length", 4 * g_beatTicks}})
							   .at("part")
							   .at("id")
							   .get<ObjectId>();
	// Two parts that follow the same way glue: both without a root key, both following.
	run(song.client, "event.glue", {{"events", {song.part, other}}});
	CHECK(run(song.client, "project.get").at("midiParts").size() == 1);

	// A part with another root key doesn't glue with one that follows differently (#896): the glued part keeps the
	// first one's root key, so the other's notes would play at another pitch.
	run(song.client, "history.undo");
	run(song.client, "project.set", {{"rootKey", "D"}});
	run(song.client, "midiPart.set", {{"part", song.part}, {"rootKey", "C"}});
	run(song.client, "midiPart.set", {{"part", other}, {"rootKey", "D"}});
	CHECK(reject(song.client, "event.glue", {{"events", {song.part, other}}}).find("Global Transpose") !=
		std::string::npos);
	// The same root key glues again.
	run(song.client, "midiPart.set", {{"part", other}, {"rootKey", "C"}});
	run(song.client, "event.glue", {{"events", {song.part, other}}});
	CHECK(run(song.client, "project.get").at("midiParts").size() == 1);

	// An Independent part doesn't glue with one that follows.
	run(song.client, "history.undo");
	run(song.client, "midiPart.set", {{"part", other}, {"globalTranspose", "independent"}});
	CHECK(reject(song.client, "event.glue", {{"events", {song.part, other}}}).find("Global Transpose") !=
		std::string::npos);
	// Two Independent parts glue, whatever their root keys: neither is transposed.
	run(song.client, "midiPart.set", {{"part", song.part}, {"globalTranspose", "independent"}});
	run(song.client, "event.glue", {{"events", {song.part, other}}});
	CHECK(run(song.client, "project.get").at("midiParts").size() == 1);
}

TEST_CASE("glue keeps the sound of parts that Keep Transpose in Octave Range folds differently", "[integration]")
{
	Song song;
	const ObjectId other = run(
		song.client, "midiPart.add", {{"track", song.track}, {"position", 4 * g_beatTicks}, {"length", g_beatTicks}})
							   .at("part")
							   .at("id")
							   .get<ObjectId>();
	run(song.client, "note.add",
		{{"part", other},
			{"notes", {{{"position", 0}, {"length", g_beatTicks / 2}, {"pitch", 60}, {"velocity", 0.75}}}}});
	run(song.client, "project.set", {{"rootKey", "D"}});
	run(song.client, "midiPart.set", {{"part", song.part}, {"rootKey", "C"}});
	run(song.client, "midiPart.set", {{"part", other}, {"rootKey", "C"}, {"transpose", 6}});
	run(song.client, "transpose.add", {{"position", 0}, {"value", 0}});
	run(song.client, "transpose.setTrack", {{"keepInOctave", true}});
	// The first part plays 2 up; the other's sum is 2 + 0 + 6 = 8, which the option folds to 4 down.
	CHECK(song.pitches() == std::vector<int>{62, 62, 62, 62, 56});
	run(song.client, "event.glue", {{"events", {song.part, other}}});
	const Json project = run(song.client, "project.get");
	REQUIRE(project.at("midiParts").size() == 1);
	CHECK(project.at("midiParts")[0].at("transpose") == 0);
	// The glued part keeps the first part's transpose; the other's note gets the difference of what the two parts
	// play their notes with at its position, -4 - 2 = -6, written into it, so it plays 56 as before.
	CHECK(song.pitches() == std::vector<int>{62, 62, 62, 62, 56});
}

TEST_CASE("Set Root Key for Unassigned Events reaches the parts of inactive track versions", "[integration]")
{
	Song song;
	run(song.client, "project.set", {{"rootKey", "E"}});
	// A new version stashes the part with the version that was active; the part is no longer in project.midiParts.
	run(song.client, "trackVersion.add", {{"tracks", {song.track}}});
	CHECK(run(song.client, "project.get").at("midiParts").empty());
	run(song.client, "transpose.assignRootKey");
	// Activating the stashed version brings the part back, with the project root key (#896).
	run(song.client, "trackVersion.activate", {{"tracks", {song.track}}, {"version", 1}});
	CHECK(song.partJson().at("rootKey") == "E");
}

TEST_CASE("audio events follow the root key and the transpose track with their pitch", "[integration]")
{
	AudioSong song;
	// Without a root key nothing shifts them, and they play their file as it is.
	CHECK(song.eventJson().at("rootKey") == "");
	CHECK(song.eventJson().at("globalTranspose") == "follow");
	run(song.client, "project.set", {{"rootKey", "D"}});
	CHECK(song.pitch().empty());
	// An event in C plays 2 semitones up in a project in D (#1368, reference manual p. 415).
	run(song.client, "audioEvent.set", {{"event", song.event}, {"rootKey", "C"}});
	CHECK(song.eventJson().at("rootKey") == "C");
	CHECK(song.pitch() == Pitch{{0.0, 2.0}});
	// A transpose event within it shifts it from there on (p. 418), in the event's frames.
	const auto transpose =
		run(song.client, "transpose.add", {{"position", g_beatTicks}, {"value", 5}}).at("event").at("id");
	CHECK(song.pitch() == Pitch{{0.0, 2.0}, {24000.0, 7.0}});
	// Muted transpose events don't play; Keep Transpose in Octave Range folds the sum.
	run(song.client, "transpose.setTrack", {{"mute", true}});
	CHECK(song.pitch() == Pitch{{0.0, 2.0}});
	run(song.client, "transpose.setTrack", {{"mute", false}, {"keepInOctave", true}});
	run(song.client, "transpose.set", {{"events", {{{"event", transpose}, {"value", 6}}}}});
	CHECK(song.pitch() == Pitch{{0.0, 2.0}, {24000.0, -4.0}});
	// Independent events are left as they are (p. 421).
	run(song.client, "audioEvent.set", {{"event", song.event}, {"globalTranspose", "independent"}});
	CHECK(song.eventJson().at("globalTranspose") == "independent");
	CHECK(song.pitch().empty());
	run(song.client, "history.undo");
	CHECK(song.eventJson().at("globalTranspose") == "follow");
	CHECK(song.pitch() == Pitch{{0.0, 2.0}, {24000.0, -4.0}});
	// Splits keep the root key.
	run(song.client, "event.split", {{"events", {song.event}}, {"positions", {12000}}});
	const Json split = run(song.client, "project.get");
	REQUIRE(split.at("audioEvents").size() == 2);
	for (const Json& event : split.at("audioEvents"))
		CHECK(event.at("rootKey") == "C");
	reject(song.client, "audioEvent.set", {{"event", song.event}, {"rootKey", "H"}});
	reject(song.client, "audioEvent.set", {{"event", song.event}, {"globalTranspose", "sometimes"}});
}

TEST_CASE("audio events get the project root key, and glue only when they follow transpose alike", "[integration]")
{
	AudioSong song;
	run(song.client, "project.set", {{"rootKey", "E"}});
	run(song.client, "transpose.assignRootKey");
	CHECK(song.eventJson().at("rootKey") == "E");
	// The pieces of a split continue each other in the clip and glue into one event, unless one of them follows
	// global transpose differently.
	run(song.client, "event.split", {{"events", {song.event}}, {"positions", {24000}}});
	std::vector<ObjectId> pieces;
	const Json split = run(song.client, "project.get");
	for (const Json& event : split.at("audioEvents"))
		pieces.push_back(event.at("id").get<ObjectId>());
	REQUIRE(pieces.size() == 2);
	run(song.client, "audioEvent.set", {{"event", pieces[1]}, {"rootKey", "C"}});
	CHECK(reject(song.client, "event.glue", {{"events", pieces}}).find("follow global transpose differently") !=
		std::string::npos);
	run(song.client, "audioEvent.set", {{"event", pieces[1]}, {"rootKey", "E"}});
	run(song.client, "event.glue", {{"events", pieces}});
	CHECK(run(song.client, "project.get").at("audioEvents").size() == 1);
}

TEST_CASE("recorded audio events follow the root key without transpose events, and are independent with them",
	"[integration]")
{
	AudioSong song;
	run(song.client, "project.set", {{"rootKey", "D"}});
	const auto record = [&song](int64_t at)
	{
		const Json added = run(song.client, "recording.add",
			{{"tracks",
				{{{"track", song.audioTrack}, {"path", song.file.u8string()},
					{"takes", {{{"position", at}, {"offset", 0}, {"length", g_beat}}}}}}}});
		return song.eventJson(added.at("events")[0].get<ObjectId>());
	};
	// p. 420: without transpose events with the project root key, following it.
	Json recorded = record(4 * g_beat);
	CHECK(recorded.at("rootKey") == "D");
	CHECK(recorded.at("globalTranspose") == "follow");
	run(song.client, "transpose.add", {{"position", 0}, {"value", 3}});
	recorded = record(8 * g_beat);
	CHECK(recorded.at("rootKey") == "");
	CHECK(recorded.at("globalTranspose") == "independent");
}

TEST_CASE("an audio event's root key and Global Transpose are saved with the project", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "Song.dawproject").u8string();
	AudioSong song;
	run(song.client, "audioEvent.set", {{"event", song.event}, {"rootKey", "F#"}, {"globalTranspose", "independent"}});
	run(song.client, "project.save", {{"path", path}});
	Engine engine;
	Client client{engine};
	run(client, "project.load", {{"path", path}});
	const Json loaded = run(client, "project.get").at("audioEvents")[0];
	CHECK(loaded.at("rootKey") == "F#");
	CHECK(loaded.at("globalTranspose") == "independent");
}

TEST_CASE("Delete Time and Insert Silence move the transpose events with the music", "[integration]")
{
	Song song;
	const auto positions = [&song]
	{
		std::vector<int64_t> result;
		const Json transpose = run(song.client, "transpose.get");
		for (const Json& event : transpose.at("events"))
			result.push_back(event.at("position").get<int64_t>());
		return result;
	};
	run(song.client, "transpose.add", {{"position", 0}, {"value", 2}});
	run(song.client, "transpose.add", {{"position", 2 * g_beatTicks}, {"value", 5}});
	run(song.client, "transpose.add", {{"position", 8 * g_beatTicks}, {"value", -1}});
	CHECK(positions() == std::vector<int64_t>{0, 2 * g_beatTicks, 8 * g_beatTicks});

	const auto values = [&song]
	{
		std::vector<int> result;
		const Json transpose = run(song.client, "transpose.get");
		for (const Json& event : transpose.at("events"))
			result.push_back(event.at("value").get<int>());
		return result;
	};
	// Delete Time over beats 1 to 3: the event at 2 goes with the range, the one at 8 moves back by two beats. The
	// transposition in force at the range's end, +5, goes on from the cut (#1367), so that the music after the range
	// keeps its pitch, as Process Bars carries the signature in force.
	run(song.client, "range.delete", {{"start", g_beat}, {"end", 3 * g_beat}, {"time", true}});
	CHECK(positions() == std::vector<int64_t>{0, g_beatTicks, 6 * g_beatTicks});
	CHECK(values() == std::vector<int>{2, 5, -1});
	CHECK(song.pitches() == std::vector<int>{62, 65});
	run(song.client, "history.undo");
	CHECK(values() == std::vector<int>{2, 5, -1});
	// An event at the range's end moves to the cut and carries nothing more; a range without events in it neither.
	run(song.client, "range.delete", {{"start", g_beat}, {"end", 2 * g_beat}, {"time", true}});
	CHECK(positions() == std::vector<int64_t>{0, g_beatTicks, 7 * g_beatTicks});
	run(song.client, "history.undo");
	run(song.client, "range.delete", {{"start", 3 * g_beat}, {"end", 4 * g_beat}, {"time", true}});
	CHECK(positions() == std::vector<int64_t>{0, 2 * g_beatTicks, 7 * g_beatTicks});
	run(song.client, "history.undo");
	// A range that drops events whose last one has the value in force before it carries nothing either.
	run(song.client, "transpose.add", {{"position", 3 * g_beatTicks}, {"value", 2}});
	run(song.client, "range.delete", {{"start", g_beat}, {"end", 4 * g_beat}, {"time", true}});
	CHECK(positions() == std::vector<int64_t>{0, 5 * g_beatTicks});
	run(song.client, "history.undo");
	run(song.client, "history.undo");
	CHECK(positions() == std::vector<int64_t>{0, 2 * g_beatTicks, 8 * g_beatTicks});

	// Insert Silence of two beats at beat 1: the events at and after it move later by two beats.
	run(song.client, "range.insertSilence", {{"start", g_beat}, {"end", 3 * g_beat}});
	CHECK(positions() == std::vector<int64_t>{0, 4 * g_beatTicks, 10 * g_beatTicks});
	run(song.client, "history.undo");

	// A locked transpose track keeps its events where they are.
	run(song.client, "transpose.setTrack", {{"lock", true}});
	run(song.client, "range.delete", {{"start", g_beat}, {"end", 3 * g_beat}, {"time", true}});
	CHECK(positions() == std::vector<int64_t>{0, 2 * g_beatTicks, 8 * g_beatTicks});
}

TEST_CASE("recorded parts follow the root key without transpose events, and are independent with them", "[integration]")
{
	Song song;
	run(song.client, "project.set", {{"rootKey", "D"}});
	const auto record = [&song](int64_t at)
	{
		const Json added = run(song.client, "recording.add",
			{{"tracks",
				{{{"track", song.track}, {"takes", {{{"position", at}, {"length", 4 * g_beat}}}},
					{"notes", {{{"position", at}, {"length", g_beat}, {"pitch", 60}, {"velocity", 0.75}}}}}}}});
		const auto id = added.at("parts")[0];
		const Json project = run(song.client, "project.get");
		for (const Json& part : project.at("midiParts"))
			if (part.at("id") == id)
				return part;
		return Json();
	};
	Json part = record(8 * g_beat);
	CHECK(part.at("rootKey") == "D");
	CHECK(part.at("globalTranspose") == "follow");
	run(song.client, "transpose.add", {{"position", 0}, {"value", 3}});
	part = record(16 * g_beat);
	CHECK(part.at("rootKey") == "");
	CHECK(part.at("globalTranspose") == "independent");
}

TEST_CASE("undo and redo of a transpose change announce transpose.changed", "[integration]")
{
	Song song;
	Client listener{song.engine};
	const auto names = [&listener]
	{
		std::vector<std::string> result;
		while (const auto message = listener.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				result.push_back(event->name);
		return result;
	};
	const auto has = [](const std::vector<std::string>& list, const char* name)
	{ return std::find(list.begin(), list.end(), name) != list.end(); };

	run(song.client, "transpose.add", {{"position", 0}, {"value", 3}});
	CHECK(has(names(), "transpose.changed")); // the command announces it
	// Undo and redo announce it too, so a client that follows the event keeps the right state (#896).
	run(song.client, "history.undo");
	CHECK(has(names(), "transpose.changed"));
	CHECK(run(song.client, "transpose.get").at("events").empty());
	run(song.client, "history.redo");
	const std::vector<std::string> redone = names();
	CHECK(has(redone, "transpose.changed"));
	CHECK(run(song.client, "transpose.get").at("events").size() == 1);
}

TEST_CASE("undo and redo of an arranger change announce arranger.changed", "[integration]")
{
	Song song;
	Client listener{song.engine};
	const auto names = [&listener]
	{
		std::vector<std::string> result;
		while (const auto message = listener.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				result.push_back(event->name);
		return result;
	};
	const auto has = [](const std::vector<std::string>& list, const char* name)
	{ return std::find(list.begin(), list.end(), name) != list.end(); };

	run(song.client, "arranger.add", {{"position", 0}, {"end", 4 * g_beatTicks}});
	CHECK(has(names(), "arranger.changed"));
	run(song.client, "history.undo");
	CHECK(has(names(), "arranger.changed"));
	CHECK(run(song.client, "arranger.get").at("events").empty());
	run(song.client, "history.redo");
	CHECK(has(names(), "arranger.changed"));
	CHECK(run(song.client, "arranger.get").at("events").size() == 1);
}

TEST_CASE("the root key and the transpose track are saved with the project", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "Song.dawproject").u8string();
	Json saved;
	Json part;
	{
		Song song;
		run(song.client, "project.set", {{"rootKey", "A#"}});
		run(song.client, "track.add", {{"type", "transpose"}, {"timeBase", "linear"}});
		run(song.client, "transpose.add", {{"position", 3 * g_beat}, {"value", -12}});
		run(song.client, "transpose.setTrack", {{"keepInOctave", true}, {"mute", true}, {"lock", true}});
		run(song.client, "midiPart.set", {{"part", song.part}, {"rootKey", "C#"}, {"globalTranspose", "independent"}});
		saved = run(song.client, "transpose.get");
		part = song.partJson();
		run(song.client, "project.save", {{"path", path}});
	}
	Engine engine;
	Client client{engine};
	run(client, "project.load", {{"path", path}});
	CHECK(run(client, "transpose.get") == saved);
	CHECK(saved.at("events")[0].at("position") == 3 * g_beat); // linear: in samples
	CHECK(run(client, "project.get").at("rootKey") == "A#");
	const Json loaded = run(client, "project.get").at("midiParts")[0];
	CHECK(loaded.at("rootKey") == part.at("rootKey"));
	CHECK(loaded.at("globalTranspose") == part.at("globalTranspose"));
}
