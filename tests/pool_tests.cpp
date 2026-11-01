// SPDX-License-Identifier: GPL-3.0-or-later
// The pool (#93, docs/features/pool.md): pool.get lists the clips with their use and status, folders organize them,
// and the media commands remove unused and missing files, find moved files again and conform files to the project's
// sample rate.
#include "engine/audiofile/audiofile.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
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
	};
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
