// SPDX-License-Identifier: GPL-3.0-or-later
// The pool (#93, docs/features/pool.md): pool.get lists the clips with their use and status, folders organize them,
// and the media commands remove unused and missing files, find moved files again and conform files to the project's
// sample rate.
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>
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
		REQUIRE(!result.ok());
		return result.error;
	}

	std::string utf8(const fs::path& path)
	{
		return path.u8string();
	}

	void writeFile(const fs::path& path, int64_t frames, int channels = 2, double sampleRate = 48000.0)
	{
		fs::create_directories(path.parent_path());
		WriteOptions options;
		options.sampleFormat = SampleFormat::Float32;
		options.sampleRate = sampleRate;
		options.channels = channels;
		const auto writer = AudioFileWriter::create(utf8(path), options);
		const std::vector<float> samples(static_cast<size_t>(frames) * static_cast<size_t>(channels), 0.25f);
		writer->write(samples.data(), static_cast<size_t>(frames));
		writer->close();
	}

	Json byId(const Json& list, const Json& id)
	{
		for (const Json& item : list)
			if (item.at("id") == id)
				return item;
		FAIL_CHECK("no " << id);
		return {};
	}

	// An engine with a saved project, whose media folder is project/Audio.
	struct SavedProject
	{
		test::TempDir dir;
		Engine engine;
		Client client{engine};
		fs::path folder = dir.path() / "project";
		fs::path media = folder / "Audio";
		fs::path library = dir.path() / "library";
		fs::path file = folder / "Song.dawproject";

		SavedProject()
		{
			fs::create_directories(folder);
			save();
		}

		void save() { run(client, "project.save", {{"path", utf8(file)}, {"embedMedia", false}}); }

		// Adds a file as media file and clip; returns the clip's id.
		uint64_t clip(const fs::path& path)
		{
			const Json added = run(client, "media.add", {{"path", utf8(path)}}).at("media");
			return run(client, "clip.add", {{"media", added.at("id")}}).at("clip").at("id").get<uint64_t>();
		}

		// Waits until the peak worker is done with every media file before a test moves or removes one: Windows
		// can't while the worker has it open.
		void settle()
		{
			const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			for (;;)
			{
				const Json statuses = run(client, "media.peakStatus").at("media");
				if (std::none_of(statuses.begin(), statuses.end(), [](const Json& status)
						{ return status.at("status") == "pending" || status.at("status") == "generating"; }))
					return;
				REQUIRE(std::chrono::steady_clock::now() < until);
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
		}

		uint64_t mediaOf(uint64_t clip)
		{
			return byId(run(client, "project.get").at("clips"), clip).at("media").get<uint64_t>();
		}

		Json event(const Json& id) { return byId(run(client, "project.get").at("audioEvents"), id); }

		// Loads the saved project again, e.g. after files moved while it was closed.
		void reopen()
		{
			run(client, "project.new");
			run(client, "project.load", {{"path", utf8(file)}});
		}
	};

	size_t filesIn(const fs::path& folder)
	{
		size_t count = 0;
		for (const auto& entry : fs::directory_iterator(folder))
			count += entry.is_regular_file() ? 1 : 0;
		return count;
	}
}

TEST_CASE("poolFolderOutside: the Audio folder, folders inside an excluded one, missing folders and loops", "[unit]")
{
	Project project;
	project.poolFolders[1] = {1, "Drums", 0};
	project.poolFolders[2] = {2, "Kicks", 1};
	project.poolFolders[3] = {3, "Loop A", 4};
	project.poolFolders[4] = {4, "Loop B", 3};
	project.poolFolders[5] = {5, "Lost", 99};
	project.poolFolders[6] = {6, "Self", 6};
	CHECK(poolFolderOutside(project, 0, 1));
	CHECK(poolFolderOutside(project, 1, 2));
	CHECK(poolFolderOutside(project, 2, 0));
	CHECK(poolFolderOutside(project, 2, 6));
	CHECK_FALSE(poolFolderOutside(project, 1, 1));  // the excluded folder itself
	CHECK_FALSE(poolFolderOutside(project, 2, 1));  // inside it
	CHECK_FALSE(poolFolderOutside(project, 99, 0)); // not there
	CHECK_FALSE(poolFolderOutside(project, 5, 0));  // inside one that isn't there
	CHECK_FALSE(poolFolderOutside(project, 3, 0));  // in a loop
	CHECK_FALSE(poolFolderOutside(project, 6, 0));  // inside itself
}

TEST_CASE("a project whose pool folders point nowhere or loop loads with each loop broken once", "[integration]")
{
	Engine engine;
	Client client(engine);
	const test::TempDir dir;
	writeFile(dir.path() / "kick.wav", 480);
	const Json media = run(client, "media.add", {{"path", utf8(dir.path() / "kick.wav")}}).at("media");
	const auto clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip").at("id");

	// What a damaged project file holds: folder 9002 is in 9003 and 9003 in 9002, 9001 is in that loop without being
	// part of it, 9004 is inside itself, 9005 in a folder that isn't there, and the clip in another one.
	Json json = run(client, "project.get");
	json["poolFolders"] = Json::array({{{"id", 9001}, {"name", "C"}, {"parent", 9002}},
		{{"id", 9002}, {"name", "A"}, {"parent", 9003}}, {{"id", 9003}, {"name", "B"}, {"parent", 9002}},
		{{"id", 9004}, {"name", "D"}, {"parent", 9004}}, {{"id", 9005}, {"name", "E"}, {"parent", 9999}}});
	for (Json& item : json.at("clips"))
		if (item.at("id") == clip)
			item["folder"] = 9998;
	const Project project = projectFromJson(json);
	REQUIRE(project.poolFolders.size() == 5);
	CHECK(project.poolFolders.at(9002).parent == 0);    // the loop is broken at its first folder
	CHECK(project.poolFolders.at(9003).parent == 9002); // and the other stays inside it
	CHECK(project.poolFolders.at(9001).parent == 9002); // a folder inside the loop keeps its place
	CHECK(project.poolFolders.at(9004).parent == 0);
	CHECK(project.poolFolders.at(9005).parent == 0);
	CHECK(project.clips.at(clip.get<ObjectId>()).folder == 0);
	for (const auto& [id, folder] : project.poolFolders)
		CHECK(poolFolderOutside(project, folder.parent, id));
}

TEST_CASE("pool.get lists the clips with their use, status and info, and the pool's totals", "[integration]")
{
	SavedProject song;
	writeFile(song.media / "kick.wav", 4800, 1);
	writeFile(song.library / "loop.wav", 9600, 2, 44100.0);
	writeFile(song.library / "gone.wav", 100);
	const uint64_t kick = song.clip(song.media / "kick.wav");
	const uint64_t loop = song.clip(song.library / "loop.wav");
	const uint64_t gone = song.clip(song.library / "gone.wav");
	const auto track = run(song.client, "track.add", {{"type", "audio"}}).at("track").at("id");
	run(song.client, "audioEvent.add", {{"track", track}, {"clip", kick}, {"position", 0}});
	run(song.client, "audioEvent.add", {{"track", track}, {"clip", kick}, {"position", 48000}});
	run(song.client, "audioEvent.add", {{"track", track}, {"clip", gone}, {"position", 96000}});
	song.settle();
	fs::remove(song.library / "gone.wav");

	const Json pool = run(song.client, "pool.get");
	CHECK(pool.at("folders").empty());
	REQUIRE(pool.at("clips").size() == 3);
	const Json kickClip = byId(pool.at("clips"), kick);
	CHECK(kickClip.at("name") == "kick");
	CHECK(kickClip.at("folder") == 0);
	CHECK(kickClip.at("used") == 2);
	CHECK(kickClip.at("missing") == false);
	CHECK(kickClip.at("external") == false);
	CHECK(kickClip.at("path") == utf8(song.media / "kick.wav"));
	CHECK(kickClip.at("sampleRate") == 48000.0);
	CHECK(kickClip.at("channels") == 1);
	CHECK(kickClip.at("length") == 4800);
	const Json loopClip = byId(pool.at("clips"), loop);
	CHECK(loopClip.at("used") == 0);
	CHECK(loopClip.at("external") == true);
	CHECK(loopClip.at("sampleRate") == 44100.0);
	const Json goneClip = byId(pool.at("clips"), gone);
	CHECK(goneClip.at("used") == 1);
	CHECK(goneClip.at("missing") == true);

	// The info line (reference manual p. 743): files, files in use, their size on disk and the external ones.
	CHECK(pool.at("audioFiles") == 3);
	CHECK(pool.at("usedFiles") == 2);
	CHECK(pool.at("missingFiles") == 1);
	CHECK(pool.at("externalFiles") == 2);
	CHECK(pool.at("totalSize") ==
		static_cast<int64_t>(fs::file_size(song.media / "kick.wav") + fs::file_size(song.library / "loop.wav")));
}

TEST_CASE("pool folders organize clips as undo steps and are kept in the project file", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "kick.wav", 480);
	writeFile(song.library / "snare.wav", 480);
	const uint64_t kick = song.clip(song.library / "kick.wav");
	const uint64_t snare = song.clip(song.library / "snare.wav");

	const Json drums = run(song.client, "poolFolder.add", {{"name", "Drums"}}).at("folder");
	CHECK(drums.at("parent") == 0);
	const Json kicks = run(song.client, "poolFolder.add", {{"name", "Kicks"}, {"parent", drums.at("id")}}).at("folder");
	CHECK(run(song.client, "poolFolder.add").at("folder").at("name") == "New Folder");
	CHECK_THAT(reject(song.client, "poolFolder.add", {{"parent", 999}}), ContainsSubstring("999"));

	// Clips move into folders and are renamed with clip.set.
	const Json moved =
		run(song.client, "clip.set", {{"clip", kick}, {"folder", kicks.at("id")}, {"name", "Big kick"}}).at("clip");
	CHECK(moved.at("folder") == kicks.at("id"));
	CHECK(moved.at("name") == "Big kick");
	run(song.client, "clip.set", {{"clip", snare}, {"folder", drums.at("id")}});
	CHECK_THAT(reject(song.client, "clip.set", {{"clip", snare}, {"folder", 999}}), ContainsSubstring("999"));
	CHECK_THAT(reject(song.client, "clip.set", {{"clip", snare}, {"name", ""}}), ContainsSubstring("name"));

	// A folder can't go into itself or a folder inside it.
	CHECK_THAT(reject(song.client, "poolFolder.set", {{"folder", drums.at("id")}, {"parent", kicks.at("id")}}),
		ContainsSubstring("inside"));
	CHECK_THAT(reject(song.client, "poolFolder.set", {{"folder", drums.at("id")}, {"parent", drums.at("id")}}),
		ContainsSubstring("inside"));
	run(song.client, "poolFolder.set", {{"folder", kicks.at("id")}, {"name", "Kick drums"}, {"parent", 0}});

	// Saved and loaded again, the folders and clips are where they were.
	song.save();
	run(song.client, "project.new");
	run(song.client, "project.load", {{"path", utf8(song.file)}});
	Json pool = run(song.client, "pool.get");
	REQUIRE(pool.at("folders").size() == 3);
	CHECK(byId(pool.at("folders"), kicks.at("id")).at("name") == "Kick drums");
	CHECK(byId(pool.at("folders"), kicks.at("id")).at("parent") == 0);
	CHECK(byId(pool.at("clips"), kick).at("folder") == kicks.at("id"));
	CHECK(byId(pool.at("clips"), kick).at("name") == "Big kick");
	CHECK(byId(pool.at("clips"), snare).at("folder") == drums.at("id"));

	// Removing a folder moves what is in it into its parent; undo brings it back.
	run(song.client, "poolFolder.set", {{"folder", kicks.at("id")}, {"parent", drums.at("id")}});
	run(song.client, "poolFolder.remove", {{"folder", drums.at("id")}});
	pool = run(song.client, "pool.get");
	CHECK(pool.at("folders").size() == 2);
	CHECK(byId(pool.at("folders"), kicks.at("id")).at("parent") == 0);
	CHECK(byId(pool.at("clips"), snare).at("folder") == 0);
	CHECK(byId(pool.at("clips"), kick).at("folder") == kicks.at("id"));
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Remove pool folder");
	run(song.client, "history.undo");
	pool = run(song.client, "pool.get");
	CHECK(pool.at("folders").size() == 3);
	CHECK(byId(pool.at("clips"), snare).at("folder") == drums.at("id"));
	CHECK(byId(pool.at("folders"), kicks.at("id")).at("parent") == drums.at("id"));
}

TEST_CASE("pool.removeUnused removes the clips that no event uses and the files that no clip uses", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "used.wav", 480);
	writeFile(song.library / "unused.wav", 480);
	const uint64_t used = song.clip(song.library / "used.wav");
	const uint64_t unused = song.clip(song.library / "unused.wav");
	// A second clip of the used file that no event plays.
	const uint64_t spare =
		run(song.client, "clip.add", {{"media", song.mediaOf(used)}}).at("clip").at("id").get<uint64_t>();
	const Json orphan = run(song.client, "media.add", {{"path", utf8(song.library / "unused.wav")}}).at("media");
	const auto track = run(song.client, "track.add", {{"type", "audio"}}).at("track").at("id");
	run(song.client, "audioEvent.add", {{"track", track}, {"clip", used}, {"position", 0}});
	const uint64_t unusedMedia = song.mediaOf(unused);

	const Json removed = run(song.client, "pool.removeUnused");
	CHECK(removed.at("clips") == Json::array({unused, spare}));
	CHECK(removed.at("media") == Json::array({unusedMedia, orphan.at("id")}));
	const Json project = run(song.client, "project.get");
	REQUIRE(project.at("clips").size() == 1);
	CHECK(project.at("clips")[0].at("id") == used);
	CHECK(project.at("media").size() == 1);
	CHECK(fs::exists(song.library / "unused.wav")); // removed from the pool, not from the disk

	// One undo step brings them all back.
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Remove unused media");
	run(song.client, "history.undo");
	CHECK(run(song.client, "project.get").at("clips").size() == 3);
	CHECK(run(song.client, "project.get").at("media").size() == 3);
}

TEST_CASE("media.locate finds missing files in a folder by name and relinks them", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "kick.wav", 4800, 1);
	writeFile(song.library / "pad.wav", 9600, 2);
	writeFile(song.library / "here.wav", 480);
	const uint64_t kick = song.clip(song.library / "kick.wav");
	const uint64_t pad = song.clip(song.library / "pad.wav");
	const uint64_t here = song.clip(song.library / "here.wav");
	const auto track = run(song.client, "track.add", {{"type", "audio"}}).at("track").at("id");
	const auto event =
		run(song.client, "audioEvent.add", {{"track", track}, {"clip", kick}, {"position", 0}, {"offset", 100}})
			.at("event")
			.at("id");
	song.save();

	// The files move away while the project is closed: kick into a subfolder of the new place, with its name in
	// other letters, and pad as a file with another channel count, which doesn't fit the media file.
	const fs::path moved = song.dir.path() / "moved";
	fs::create_directories(moved / "drums");
	song.settle();
	fs::rename(song.library / "kick.wav", moved / "drums" / "KICK.wav");
	fs::remove(song.library / "pad.wav");
	writeFile(moved / "pad.wav", 9600, 1);
	run(song.client, "project.new");
	run(song.client, "project.load", {{"path", utf8(song.file)}});

	Json pool = run(song.client, "pool.get");
	CHECK(byId(pool.at("clips"), kick).at("missing") == true);
	CHECK(byId(pool.at("clips"), pad).at("missing") == true);
	CHECK(byId(pool.at("clips"), here).at("missing") == false);
	CHECK(pool.at("missingFiles") == 2);

	const Json located = run(song.client, "media.locate", {{"folder", utf8(moved)}});
	CHECK(located.at("relinked") == Json::array({song.mediaOf(kick)}));
	CHECK(located.at("missing") == Json::array({song.mediaOf(pad)}));
	pool = run(song.client, "pool.get");
	const Json kickClip = byId(pool.at("clips"), kick);
	CHECK(kickClip.at("missing") == false);
	CHECK(fs::equivalent(fs::u8path(kickClip.at("path").get<std::string>()), moved / "drums" / "KICK.wav"));
	CHECK(byId(pool.at("clips"), pad).at("missing") == true);
	CHECK(pool.at("missingFiles") == 1);
	// The event plays the same section of the file as before.
	const Json playing = byId(run(song.client, "project.get").at("audioEvents"), event);
	CHECK(playing.at("offset") == 100);
	CHECK(playing.at("length") == 4700);

	// Saved again, the project keeps the new path.
	song.save();
	run(song.client, "project.new");
	run(song.client, "project.load", {{"path", utf8(song.file)}});
	CHECK(byId(run(song.client, "pool.get").at("clips"), kick).at("missing") == false);

	// Only the files given are looked for; relinking is one undo step.
	const fs::path other = song.dir.path() / "other";
	fs::create_directories(other);
	song.settle();
	fs::rename(moved / "drums" / "KICK.wav", other / "kick.wav");
	CHECK(byId(run(song.client, "pool.get").at("clips"), kick).at("missing") == true);
	CHECK(run(song.client, "media.locate", {{"folder", utf8(other)}, {"media", {song.mediaOf(pad)}}})
			.at("relinked")
			.empty());
	CHECK(run(song.client, "media.locate", {{"folder", utf8(other)}}).at("relinked").size() == 1);
	CHECK(byId(run(song.client, "pool.get").at("clips"), kick).at("missing") == false);
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Locate missing files");
	run(song.client, "history.undo");
	CHECK(byId(run(song.client, "pool.get").at("clips"), kick).at("missing") == true);

	CHECK_THAT(reject(song.client, "media.locate", {{"folder", utf8(song.dir.path() / "nowhere")}}),
		ContainsSubstring("nowhere"));
}

TEST_CASE("media.locate takes a file of the same length, the same spelling first, matching ASCII letters in any case",
	"[integration]")
{
	SavedProject song;
	const fs::path aerger = fs::u8path("\xC3\x84rger.wav"); // Ärger.wav
	const fs::path oel = fs::u8path("\xC3\x96l.wav");       // Öl.wav
	writeFile(song.library / "kick.wav", 4800, 1);
	writeFile(song.library / "snare.wav", 2400);
	writeFile(song.library / aerger, 480);
	writeFile(song.library / oel, 480);
	const uint64_t kick = song.mediaOf(song.clip(song.library / "kick.wav"));
	const uint64_t snare = song.mediaOf(song.clip(song.library / "snare.wav"));
	const uint64_t aergerMedia = song.mediaOf(song.clip(song.library / aerger));
	const uint64_t oelMedia = song.mediaOf(song.clip(song.library / oel));
	song.save();
	song.settle();
	fs::remove_all(song.library);

	// Three files named kick: one of another length with the same spelling, two that fit, one of them spelled
	// alike. Snare is there only with another length. Ärger is there with other ASCII letters in upper case, Öl
	// with a lower-case Ö, which isn't an ASCII letter.
	const fs::path moved = song.dir.path() / "moved";
	writeFile(moved / "a" / "kick.wav", 9600, 1);
	writeFile(moved / "b" / "KICK.wav", 4800, 1);
	writeFile(moved / "c" / "kick.wav", 4800, 1);
	writeFile(moved / "snare.wav", 4800);
	writeFile(moved / fs::u8path("\xC3\x84RGER.WAV"), 480);
	writeFile(moved / fs::u8path("\xC3\xB6l.wav"), 480);
	song.reopen();

	const Json located = run(song.client, "media.locate", {{"folder", utf8(moved)}});
	CHECK(located.at("relinked") == Json::array({kick, aergerMedia}));
	CHECK(located.at("missing") == Json::array({snare, oelMedia}));
	const Json media = run(song.client, "project.get").at("media");
	CHECK(fs::equivalent(fs::u8path(byId(media, kick).at("path").get<std::string>()), moved / "c" / "kick.wav"));
	CHECK(
		fs::u8path(byId(media, aergerMedia).at("path").get<std::string>()).filename().u8string() == "\xC3\x84RGER.WAV");

	// A media file given twice is looked for and relinked once.
	run(song.client, "history.undo");
	const Json twice = run(song.client, "media.locate", {{"folder", utf8(moved)}, {"media", {kick, kick}}});
	CHECK(twice.at("relinked") == Json::array({kick}));
	CHECK(twice.at("missing").empty());
}

TEST_CASE("media.removeMissing removes missing files with their clips and events", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "gone.wav", 480);
	writeFile(song.library / "here.wav", 480);
	const uint64_t gone = song.clip(song.library / "gone.wav");
	const uint64_t here = song.clip(song.library / "here.wav");
	const auto track = run(song.client, "track.add", {{"type", "audio"}}).at("track").at("id");
	run(song.client, "audioEvent.add", {{"track", track}, {"clip", gone}, {"position", 0}});
	run(song.client, "audioEvent.add", {{"track", track}, {"clip", here}, {"position", 48000}});
	const uint64_t goneMedia = song.mediaOf(gone);
	song.settle();
	fs::remove(song.library / "gone.wav");

	const Json removed = run(song.client, "media.removeMissing");
	CHECK(removed.at("media") == Json::array({goneMedia}));
	CHECK(removed.at("clips") == Json::array({gone}));
	CHECK(removed.at("events").size() == 1);
	const Json project = run(song.client, "project.get");
	CHECK(project.at("media").size() == 1);
	CHECK(project.at("clips").size() == 1);
	REQUIRE(project.at("audioEvents").size() == 1);
	CHECK(project.at("audioEvents")[0].at("clip") == here);
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Remove missing files");
	run(song.client, "history.undo");
	CHECK(run(song.client, "project.get").at("audioEvents").size() == 2);
}

TEST_CASE("media.removeMissing removes the crossfade from a missing file's event into the next one", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "gone.wav", 48000);
	writeFile(song.library / "here.wav", 48000);
	const uint64_t gone = song.clip(song.library / "gone.wav");
	const uint64_t here = song.clip(song.library / "here.wav");
	const auto track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const auto from =
		run(song.client, "audioEvent.add", {{"track", track}, {"clip", gone}, {"position", 0}}).at("event").at("id");
	const auto to = run(song.client, "audioEvent.add", {{"track", track}, {"clip", here}, {"position", 24000}})
						.at("event")
						.at("id");
	run(song.client, "crossfade.set", {{"from", from}, {"to", to}});
	REQUIRE(song.event(to).at("crossfade").at("from") == from);
	song.settle();
	fs::remove(song.library / "gone.wav");

	const Json removed = run(song.client, "media.removeMissing");
	CHECK(removed.at("events") == Json::array({from}));
	const Json project = run(song.client, "project.get");
	REQUIRE(project.at("audioEvents").size() == 1);
	CHECK(project.at("audioEvents")[0].at("crossfade").is_null());
	CHECK(project.at("audioEvents")[0].at("length") == 48000);

	run(song.client, "history.undo");
	CHECK(run(song.client, "project.get").at("audioEvents").size() == 2);
	CHECK(song.event(to).at("crossfade").at("from") == from);
}

TEST_CASE("media.conform converts files to the project's sample rate and keeps what events play", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "slow.wav", 44100, 2, 44100.0);
	writeFile(song.library / "fine.wav", 4800);
	const uint64_t slow = song.clip(song.library / "slow.wav");
	const uint64_t fine = song.clip(song.library / "fine.wav");
	run(song.client, "clip.setFades", {{"clip", slow}, {"fadeIn", {{"length", 4410}}}});
	const auto track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const auto event = run(song.client, "audioEvent.add",
		{{"track", track}, {"clip", slow}, {"position", 1000}, {"offset", 4410}, {"length", 22050}})
						   .at("event")
						   .at("id");
	const uint64_t slowMedia = song.mediaOf(slow);

	const Json conformed = run(song.client, "media.conform");
	CHECK(conformed.at("media") == Json::array({slowMedia}));
	const Json pool = run(song.client, "pool.get");
	const Json slowClip = byId(pool.at("clips"), slow);
	CHECK(slowClip.at("sampleRate") == 48000.0);
	CHECK(slowClip.at("length") == 48000);
	CHECK(fs::u8path(slowClip.at("path").get<std::string>()).parent_path() == song.media);
	CHECK(AudioFileReader::open(slowClip.at("path").get<std::string>())->info().sampleRate == 48000.0);
	CHECK(byId(pool.at("clips"), fine).at("path") == utf8(song.library / "fine.wav"));
	CHECK(fs::exists(song.library / "slow.wav")); // the original stays on disk

	// Events and fades are in the clip's sample frames, which are now the project's.
	const Json project = run(song.client, "project.get");
	const Json played = byId(project.at("audioEvents"), event);
	CHECK(played.at("position") == 1000);
	CHECK(played.at("offset") == 4800);
	CHECK(played.at("length") == 24000);
	CHECK(byId(project.at("clips"), slow).at("fadeIn").at("length") == 4800);

	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Conform files");
	run(song.client, "history.undo");
	CHECK(byId(run(song.client, "pool.get").at("clips"), slow).at("sampleRate") == 44100.0);
	CHECK(byId(run(song.client, "project.get").at("audioEvents"), event).at("offset") == 4410);

	// Without a project file there is no media folder to write into.
	run(song.client, "project.new");
	writeFile(song.library / "other.wav", 441, 1, 44100.0);
	song.clip(song.library / "other.wav");
	CHECK_THAT(reject(song.client, "media.conform", Json::object()), ContainsSubstring("save the project"));
}

TEST_CASE("media.conform converts a media file given twice once", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "slow.wav", 44100, 2, 44100.0);
	const uint64_t slow = song.clip(song.library / "slow.wav");
	const auto track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const auto event = run(song.client, "audioEvent.add",
		{{"track", track}, {"clip", slow}, {"position", 0}, {"offset", 4410}, {"length", 22050}})
						   .at("event")
						   .at("id");
	const uint64_t media = song.mediaOf(slow);

	CHECK(run(song.client, "media.conform", {{"media", {media, media}}}).at("media") == Json::array({media}));
	CHECK(filesIn(song.media) == 1);
	CHECK(byId(run(song.client, "pool.get").at("clips"), slow).at("sampleRate") == 48000.0);
	CHECK(song.event(event).at("offset") == 4800);
	CHECK(song.event(event).at("length") == 24000);
}

TEST_CASE(
	"media.conform scales event fades, snap points and crossfades in the frames each is counted in", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "slow.wav", 44100, 1, 44100.0);
	writeFile(song.library / "fine.wav", 48000, 1);
	const uint64_t slow = song.clip(song.library / "slow.wav");
	const uint64_t fine = song.clip(song.library / "fine.wav");
	run(song.client, "clip.setFades", {{"clip", slow}, {"fadeOut", {{"length", 2205}}}});
	const auto track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const auto add = [&song, &track](uint64_t clip, int64_t position, int64_t offset, int64_t length)
	{
		return run(song.client, "audioEvent.add",
			{{"track", track}, {"clip", clip}, {"position", position}, {"offset", offset}, {"length", length}})
			.at("event")
			.at("id");
	};
	// first (44.1 kHz) crossfades into second (48 kHz), which crossfades into third (44.1 kHz). A crossfade's length
	// is in the frames of the event it goes into, its outLength in those of the event it comes from.
	const auto first = add(slow, 0, 4410, 22050);   // project samples 0 to 24000
	const auto second = add(fine, 12000, 0, 24000); // 12000 to 36000
	const auto third = add(slow, 30000, 0, 22050);  // 30000 to 54000
	run(song.client, "audioEvent.setFades", {{"event", first}, {"fadeIn", {{"length", 441}}}});
	run(song.client, "audioEvent.setFades", {{"event", third}, {"fadeOut", {{"length", 2205}}}});
	run(song.client, "event.setSnapPoint", {{"events", {first}}, {"position", 12000}});
	run(song.client, "crossfade.set", {{"from", first}, {"to", second}, {"length", 9600}, {"outLength", 8820}});
	run(song.client, "crossfade.set", {{"from", second}, {"to", third}, {"length", 4410}, {"outLength", 4800}});
	const auto snapPoint = song.event(first).at("snapPoint").get<int64_t>();

	run(song.client, "media.conform");
	CHECK(byId(run(song.client, "project.get").at("clips"), slow).at("fadeOut").at("length") == 2400);
	const Json a = song.event(first);
	CHECK(a.at("offset") == 4800);
	CHECK(a.at("length") == 24000);
	CHECK(a.at("fadeIn").at("length") == 480);
	CHECK(a.at("snapPoint") == std::llround(static_cast<double>(snapPoint) * 48000.0 / 44100.0));
	const Json b = song.event(second);
	CHECK(b.at("offset") == 0);
	CHECK(b.at("length") == 24000);
	CHECK(b.at("crossfade").at("length") == 9600);    // second's frames, which stay
	CHECK(b.at("crossfade").at("outLength") == 9600); // first's frames, now at 48 kHz
	const Json c = song.event(third);
	CHECK(c.at("length") == 24000);
	CHECK(c.at("fadeOut").at("length") == 2400);
	CHECK(c.at("crossfade").at("length") == 4800);    // third's frames, now at 48 kHz
	CHECK(c.at("crossfade").at("outLength") == 4800); // second's frames, which stay

	run(song.client, "history.undo");
	CHECK(song.event(first).at("snapPoint") == snapPoint);
	CHECK(song.event(second).at("crossfade").at("outLength") == 8820);
	CHECK(song.event(third).at("crossfade").at("length") == 4410);
}

TEST_CASE("audioFile.import with clipsOnly adds files to the pool without tracks or events", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "a.wav", 480);
	writeFile(song.library / "b.wav", 480);
	const Json result = run(song.client, "audioFile.import",
		{{"files", {utf8(song.library / "a.wav"), utf8(song.library / "b.wav")}}, {"clipsOnly", true}});
	CHECK(result.at("tracks").empty());
	CHECK(result.at("events").empty());
	CHECK(result.at("media").size() == 2);
	const Json project = run(song.client, "project.get");
	CHECK(project.at("tracks").empty());
	CHECK(project.at("clips").size() == 2);
	CHECK(run(song.client, "pool.get").at("clips").at(0).at("used") == 0);
	CHECK_THAT(reject(song.client, "audioFile.import",
				   {{"files", {utf8(song.library / "a.wav")}}, {"clipsOnly", true}, {"oneTrack", true}}),
		ContainsSubstring("clipsOnly"));
}
