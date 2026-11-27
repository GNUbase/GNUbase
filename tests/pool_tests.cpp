// SPDX-License-Identifier: GPL-3.0-or-later
// The pool (#93, docs/features/pool.md): pool.get lists the clips with their use and status, folders organize them,
// and the media commands remove unused and missing files, find moved files again and conform files to the project's
// sample rate.
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "testutils/heldjob.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <pugixml.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
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

		void settle() { gnubase::test::settlePeaks(client); }

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

	// The WAV files in folder. The peak files that the peak worker writes next to them whenever it gets to them, and
	// the temporary files it writes them through, don't count.
	size_t filesIn(const fs::path& folder)
	{
		size_t count = 0;
		for (const auto& entry : fs::directory_iterator(folder))
			count += entry.is_regular_file() && entry.path().extension() == ".wav" ? 1 : 0;
		return count;
	}

	// Runs a command that starts a job (media.locate, media.conform), waits for the job and returns its result; the
	// job must succeed.
	Json runJob(Client& client, const std::string& command, Json parameters = Json::object(),
		std::vector<Json>* progress = nullptr)
	{
		const Json started = run(client, command, std::move(parameters)).at("job");
		CHECK(started.at("kind") == command);
		const Json finished = test::waitForJob(client, started.at("id").get<uint64_t>(), progress);
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		return finished.at("result");
	}

	// Empty files for media.locate to look through: count folders in folder with count files each.
	void fillFolder(const fs::path& folder, int count)
	{
		for (int i = 0; i < count; ++i)
		{
			const fs::path inside = folder / ("folder" + std::to_string(i));
			fs::create_directories(inside);
			for (int j = 0; j < count; ++j)
				std::ofstream(inside / ("take" + std::to_string(j) + ".wav")).put('x');
		}
	}

	// Starts a job with the command, cancels it while it is held at its first progress, so that it can't end first
	// (#1079), and returns the job once it ended.
	Json cancelJob(SavedProject& song, const std::string& command, Json parameters = Json::object())
	{
		return test::whileHeld(song.engine, song.client, command, std::move(parameters),
			[&song](uint64_t job)
			{
				const Json cancelling = run(song.client, "job.cancel", {{"job", job}});
				CHECK(cancelling.at("job").at("state") == "running"); // still, as it is held
			});
	}

	size_t steps(Client& client)
	{
		return run(client, "history.get").at("steps").size();
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
	CHECK(pool.at("missingExternalFiles") == 1); // gone.wav, which Prepare Archive can't copy
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

TEST_CASE("media.cleanup looks in the media folder that a load of the project file extracts into", "[integration]")
{
	// Named after the file as its versions are (#798): Mix v2.1 media for a file Mix v2.1, not Mix v2 media, which is
	// Mix v2.dawproject's.
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	writeFile(dir.path() / "library" / "take.wav", 480);
	run(client, "media.add", {{"path", utf8(dir.path() / "library" / "take.wav")}});
	const fs::path file = dir.path() / "Mix v2.1";
	run(client, "project.save", {{"path", utf8(file)}});
	run(client, "project.load", {{"path", utf8(file)}}); // extracts it into Mix v2.1 media
	gnubase::test::settlePeaks(client);
	const fs::path left = dir.path() / "Mix v2.1 media" / "audio" / "2-take.wav";
	writeFile(left, 240);
	writeFile(dir.path() / "Mix v2 media" / "audio" / "take.wav", 240);
	std::vector<std::string> files;
	const Json result = run(client, "media.cleanup");
	for (const Json& path : result.at("files"))
		if (fs::u8path(path.get<std::string>()).extension() == ".wav")
			files.push_back(path.get<std::string>());
	CHECK(files == std::vector<std::string>{utf8(left)});
}

TEST_CASE(
	"media.cleanup removes the files of the media folders that neither the project nor its file uses", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const fs::path library = dir.path() / "library";
	const fs::path kit = dir.path() / "settings" / "Track Presets" / "Kit media";
	const fs::path song = dir.path() / "Song media" / "audio";
	const fs::path file = dir.path() / "Song.dawproject";
	writeFile(library / "take.wav", 480);
	writeFile(library / "old.wav", 480);
	run(client, "media.add", {{"path", utf8(library / "take.wav")}});
	run(client, "media.add", {{"path", utf8(library / "old.wav")}});
	run(client, "project.save", {{"path", utf8(file)}});
	run(client, "project.load", {{"path", utf8(file)}}); // extracts them into Song media
	// A track preset's file that a load took, one that an earlier version of the preset left, and one that an earlier
	// version of the project left in its media folder.
	writeFile(kit / "kick.wav", 480);
	writeFile(kit / "2-kick.wav", 240);
	writeFile(song / "2-take.wav", 240);
	run(client, "media.add", {{"path", utf8(kit / "kick.wav")}});
	gnubase::test::settlePeaks(client);
	run(client, "project.save", {{"path", utf8(file)}, {"embedMedia", false}});
	// The audio files that media.cleanup lists or removes, without the peak files that go with them.
	const auto audioFiles = [&client](Json parameters = Json::object())
	{
		const Json result = run(client, "media.cleanup", std::move(parameters));
		std::vector<std::string> files;
		for (const Json& path : result.at("files"))
			if (fs::u8path(path.get<std::string>()).extension() == ".wav")
				files.push_back(path.get<std::string>());
		std::sort(files.begin(), files.end());
		return files;
	};
	const auto sorted = [](std::vector<std::string> files)
	{
		std::sort(files.begin(), files.end());
		return files;
	};
	const Json listed = run(client, "media.cleanup");
	CHECK(audioFiles() == sorted({utf8(song / "2-take.wav"), utf8(kit / "2-kick.wav")}));
	CHECK(listed.at("size") == fs::file_size(song / "2-take.wav") + fs::file_size(kit / "2-kick.wav"));
	REQUIRE(listed.at("sizes").size() == listed.at("files").size());
	for (size_t i = 0; i < listed.at("files").size(); ++i) // each file's bytes, in the order of files
		CHECK(listed.at("sizes").at(i) == fs::file_size(fs::u8path(listed.at("files").at(i).get<std::string>())));
	CHECK(fs::exists(kit / "2-kick.wav")); // listing removes nothing

	// A media file that the project no longer has stays while the undo history has it, and then while the project
	// file refers to it.
	Json old;
	const Json project = run(client, "project.get");
	for (const Json& media : project.at("media"))
		if (fs::u8path(media.at("path").get<std::string>()).filename() == "old.wav")
			old = media;
	REQUIRE(old.at("path") == utf8(song / "old.wav"));
	run(client, "media.remove", {{"media", old.at("id")}});
	CHECK(audioFiles().size() == 2);
	run(client, "history.set", {{"maxSteps", 1}});
	run(client, "track.add", {{"type", "audio"}}); // the removal's step goes
	REQUIRE(steps(client) == 1);
	CHECK(audioFiles().size() == 2);
	run(client, "project.save", {{"path", utf8(file)}, {"embedMedia", false}});
	const std::vector<std::string> left =
		sorted({utf8(song / "2-take.wav"), utf8(song / "old.wav"), utf8(kit / "2-kick.wav")});
	CHECK(audioFiles() == left);

	// files removes those of them that are unused, with their peak files; the others stay.
	CHECK(fs::exists(song / "old.wav.gbpeaks"));
	Json files = {utf8(song / "take.wav"), utf8(library / "take.wav")};
	for (const std::string& path : left)
		files.push_back(path);
	CHECK(audioFiles({{"files", files}}) == left);
	for (const std::string& path : left)
		CHECK(!fs::exists(fs::u8path(path)));
	CHECK(!fs::exists(song / "old.wav.gbpeaks"));
	CHECK(fs::exists(song / "take.wav"));
	CHECK(fs::exists(song / "take.wav.gbpeaks"));
	CHECK(fs::exists(kit / "kick.wav"));
	CHECK(fs::exists(library / "take.wav"));
	CHECK(run(client, "media.cleanup").at("files").empty());
}

namespace
{
	// The audio files that media.cleanup lists, without the peak files that go with them.
	std::vector<std::string> unusedAudio(Client& client)
	{
		const Json result = run(client, "media.cleanup");
		std::vector<std::string> files;
		for (const Json& path : result.at("files"))
			if (fs::u8path(path.get<std::string>()).extension() == ".wav")
				files.push_back(path.get<std::string>());
		std::sort(files.begin(), files.end());
		return files;
	}

	// Makes a folder the working folder while it lives.
	class WorkingFolder
	{
	public:
		explicit WorkingFolder(const fs::path& folder) { fs::current_path(folder); }
		WorkingFolder(const WorkingFolder&) = delete;
		WorkingFolder& operator=(const WorkingFolder&) = delete;
		WorkingFolder(WorkingFolder&&) = delete;
		WorkingFolder& operator=(WorkingFolder&&) = delete;
		~WorkingFolder()
		{
			std::error_code error;
			fs::current_path(m_previous, error);
		}

	private:
		fs::path m_previous = fs::current_path();
	};
}

TEST_CASE("media.cleanup keeps the files of media added by a path relative to the working folder", "[integration]")
{
	const test::TempDir dir;
	const WorkingFolder working(dir.path());
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const fs::path song = dir.path() / "Song media" / "audio";
	run(client, "project.save", {{"path", utf8(dir.path() / "Song.dawproject")}});
	writeFile(song / "take.wav", 480);
	writeFile(song / "2-take.wav", 240);
	// Added after the save, so that only the project has it, by its path relative to the working folder.
	const Json added = run(client, "media.add", {{"path", "Song media/audio/take.wav"}}).at("media");
	REQUIRE(fs::u8path(added.at("path").get<std::string>()).is_relative());
	gnubase::test::settlePeaks(client);
	CHECK(unusedAudio(client) == std::vector<std::string>{utf8(song / "2-take.wav")});
	run(client, "media.cleanup", {{"files", {utf8(song / "take.wav"), utf8(song / "2-take.wav")}}});
	CHECK(fs::exists(song / "take.wav"));
	CHECK(!fs::exists(song / "2-take.wav"));
}

TEST_CASE("media.cleanup keeps a track preset's file that a project template refers to", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const fs::path kit = dir.path() / "settings" / "Track Presets" / "Kit media";
	writeFile(kit / "kick.wav", 480);
	writeFile(kit / "2-kick.wav", 240);
	run(client, "media.add", {{"path", utf8(kit / "kick.wav")}});
	const fs::path templateFile =
		fs::u8path(run(client, "project.saveTemplate", {{"name", "Drums"}}).at("file").get<std::string>());
	run(client, "project.new"); // another project, which has neither file
	CHECK(unusedAudio(client) == std::vector<std::string>{utf8(kit / "2-kick.wav")});
	fs::remove(templateFile);
	CHECK(unusedAudio(client) == std::vector<std::string>{utf8(kit / "2-kick.wav"), utf8(kit / "kick.wav")});
}

TEST_CASE("media.cleanup refuses while the project file can't be read, and every file stays", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const fs::path file = dir.path() / "Song.dawproject";
	const fs::path song = dir.path() / "Song media" / "audio";
	run(client, "project.save", {{"path", utf8(file)}});
	writeFile(song / "2-take.wav", 240);
	REQUIRE(unusedAudio(client) == std::vector<std::string>{utf8(song / "2-take.wav")});
	std::ofstream(file, std::ios::binary | std::ios::trunc) << "not a project";
	CHECK_THAT(reject(client, "media.cleanup", Json::object()), ContainsSubstring("can't tell which files"));
	CHECK_THAT(reject(client, "media.cleanup", {{"files", {utf8(song / "2-take.wav")}}}),
		ContainsSubstring("can't tell which files"));
	CHECK(fs::exists(song / "2-take.wav"));
}

TEST_CASE(
	"media.cleanup keeps a frozen track's freeze file, also while only an undo step has the track", "[integration]")
{
	const test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	const fs::path file = dir.path() / "Song.dawproject";
	writeFile(dir.path() / "library" / "take.wav", 4800);
	run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}});
	run(client, "project.save", {{"path", utf8(file)}}); // a folder for the freeze file
	const Json media = run(client, "media.add", {{"path", utf8(dir.path() / "library" / "take.wav")}}).at("media");
	const Json clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip").at("id");
	const Json track =
		run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"name", "Take"}}).at("track").at("id");
	run(client, "audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}});
	const Json job = test::waitForJob(
		client, run(client, "track.freeze", {{"tracks", {track}}, {"tail", 0}}).at("job").at("id").get<uint64_t>());
	REQUIRE(job.at("state") == "done");
	// Saved with the freeze file in it and loaded again, which extracts the freeze file into the media folder.
	run(client, "project.save");
	run(client, "project.load", {{"path", utf8(file)}});
	Json frozen;
	const Json loadedProject = run(client, "project.get");
	for (const Json& loaded : loadedProject.at("tracks"))
		if (loaded.at("name") == "Take")
			frozen = loaded;
	const fs::path freeze = fs::u8path(frozen.at("frozen").at("path").get<std::string>());
	REQUIRE(freeze.parent_path() == dir.path() / "Song media" / "freeze");
	gnubase::test::settlePeaks(client);
	CHECK(unusedAudio(client).empty());
	run(client, "track.unfreeze", {{"tracks", {frozen.at("id")}}});
	CHECK(unusedAudio(client).empty()); // the undo step has the frozen track
	run(client, "history.set", {{"maxSteps", 1}});
	run(client, "track.add", {{"type", "audio"}}); // the unfreeze's step goes
	REQUIRE(steps(client) == 1);
	// The project file has it only embedded, which doesn't keep the extracted file.
	CHECK(unusedAudio(client) == std::vector<std::string>{utf8(freeze)});
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

	const Json located = runJob(song.client, "media.locate", {{"folder", utf8(moved)}});
	CHECK(located.at("relinked") == Json::array({song.mediaOf(kick)}));
	CHECK(located.at("missing") == Json::array({song.mediaOf(pad)}));
	CHECK(located.at("changed").empty());
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
	CHECK(runJob(song.client, "media.locate", {{"folder", utf8(other)}, {"media", {song.mediaOf(pad)}}})
			.at("relinked")
			.empty());
	CHECK(runJob(song.client, "media.locate", {{"folder", utf8(other)}}).at("relinked").size() == 1);
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

	const Json located = runJob(song.client, "media.locate", {{"folder", utf8(moved)}});
	CHECK(located.at("relinked") == Json::array({kick, aergerMedia}));
	CHECK(located.at("missing") == Json::array({snare, oelMedia}));
	const Json media = run(song.client, "project.get").at("media");
	CHECK(fs::equivalent(fs::u8path(byId(media, kick).at("path").get<std::string>()), moved / "c" / "kick.wav"));
	CHECK(
		fs::u8path(byId(media, aergerMedia).at("path").get<std::string>()).filename().u8string() == "\xC3\x84RGER.WAV");

	// A media file given twice is looked for and relinked once.
	run(song.client, "history.undo");
	const Json twice = runJob(song.client, "media.locate", {{"folder", utf8(moved)}, {"media", {kick, kick}}});
	CHECK(twice.at("relinked") == Json::array({kick}));
	CHECK(twice.at("missing").empty());
}

TEST_CASE("media.locate takes a file within a millisecond of a length that another application's project.xml rounded",
	"[integration]")
{
	SavedProject song;
	writeFile(song.library / "take.wav", 4820, 1);
	writeFile(song.library / "far.wav", 4900, 1);
	const auto track = run(song.client, "track.add", {{"type", "audio"}}).at("track").at("id");
	for (const char* name : {"take.wav", "far.wav"})
		run(song.client, "audioEvent.add",
			{{"track", track}, {"clip", song.clip(song.library / name)}, {"position", 0}});
	song.save();
	song.settle();

	// Another application's file: no GNUbase entry, and durations with three decimals, so take's length reads as 4800
	// frames once the files are missing, 20 frames off, and far's as 4896.
	std::map<std::string, std::string> entries = gnubase::test::readZip(utf8(song.file));
	REQUIRE(entries.erase("gnubase.json") == 1);
	pugi::xml_document xml;
	REQUIRE(xml.load_string(entries.at("project.xml").c_str()));
	for (const pugi::xpath_node& audio : xml.select_nodes("//Audio"))
	{
		pugi::xml_attribute duration = audio.node().attribute("duration");
		duration.set_value(std::round(duration.as_double() * 1000.0) / 1000.0);
	}
	std::ostringstream written;
	xml.save(written);
	entries.at("project.xml") = written.str();
	gnubase::test::writeZip(utf8(song.file), entries);
	fs::remove_all(song.library);
	const fs::path moved = song.dir.path() / "moved";
	writeFile(moved / "take.wav", 4820, 1);
	// Closer in length, so it wins over the same spelling: the spelling decides only between files equally close.
	writeFile(moved / "near" / "TAKE.wav", 4810, 1);
	writeFile(moved / "far.wav", 4800, 1); // another take, 96 frames off: more than a millisecond
	song.reopen();

	const Json media = run(song.client, "project.get").at("media");
	REQUIRE(media.size() == 2);
	const auto named = [&media](const std::string& name)
	{
		const auto file = std::find_if(media.begin(), media.end(),
			[&name](const Json& item) { return fs::u8path(item.at("path").get<std::string>()).filename() == name; });
		REQUIRE(file != media.end());
		return *file;
	};
	CHECK(named("take.wav").at("length") == 4800);
	const Json located = runJob(song.client, "media.locate", {{"folder", utf8(moved)}});
	CHECK(located.at("relinked") == Json::array({named("take.wav").at("id")}));
	CHECK(located.at("missing") == Json::array({named("far.wav").at("id")}));
	const Json take = byId(run(song.client, "project.get").at("media"), named("take.wav").at("id"));
	CHECK(fs::equivalent(fs::u8path(take.at("path").get<std::string>()), moved / "near" / "TAKE.wav"));
	CHECK(fs::u8path(take.at("path").get<std::string>()).filename().u8string() == "TAKE.wav");
	// The media file takes its new file's length, not the rounded one, and undo gives the old one back (#506).
	CHECK(take.at("length") == 4810);
	run(song.client, "history.undo");
	CHECK(byId(run(song.client, "project.get").at("media"), take.at("id")).at("length") == 4800);

	// A file a few frames shorter than the rounded length: the event, which played up to that length, is trimmed to
	// the new end in the same undo step, so it stays within its clip and can still be changed.
	const auto takeEvent = [&song, &take]
	{
		const Json events = run(song.client, "project.get").at("audioEvents");
		const auto found = std::find_if(events.begin(), events.end(),
			[&song, &take](const Json& event)
			{
				return byId(run(song.client, "project.get").at("clips"), event.at("clip")).at("media") == take.at("id");
			});
		REQUIRE(found != events.end());
		return *found;
	};
	REQUIRE(takeEvent().at("length") == 4800);
	writeFile(moved / "short" / "take.wav", 4795, 1);
	CHECK(runJob(song.client, "media.locate", {{"folder", utf8(moved / "short")}}).at("relinked") ==
		Json::array({take.at("id")}));
	CHECK(byId(run(song.client, "project.get").at("media"), take.at("id")).at("length") == 4795);
	const Json trimmed = takeEvent();
	CHECK(trimmed.at("offset") == 0);
	CHECK(trimmed.at("length") == 4795);
	run(song.client, "audioEvent.set", {{"event", trimmed.at("id")}, {"volume", -3.0}});
	run(song.client, "history.undo");
	run(song.client, "history.undo");
	CHECK(byId(run(song.client, "project.get").at("media"), take.at("id")).at("length") == 4800);
	CHECK(takeEvent().at("length") == 4800);
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

	std::vector<Json> progress;
	const Json conformed = runJob(song.client, "media.conform", Json::object(), &progress);
	CHECK(conformed.at("media") == Json::array({slowMedia}));
	CHECK(conformed.at("changed").empty());
	// The job says which file it converts.
	REQUIRE(!progress.empty());
	for (const Json& step : progress)
	{
		CHECK(step.at("detail") == "slow.wav");
		CHECK(step.at("progress").get<double>() >= 0.0);
		CHECK(step.at("progress").get<double>() <= 1.0);
	}
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

	// A media file given that is missing is refused, and nothing starts.
	writeFile(song.library / "gone.wav", 441, 1, 44100.0);
	const uint64_t gone = song.mediaOf(song.clip(song.library / "gone.wav"));
	song.settle();
	fs::remove(song.library / "gone.wav");
	const size_t jobs = run(song.client, "job.list").at("jobs").size();
	CHECK_THAT(reject(song.client, "media.conform", {{"media", {gone}}}), ContainsSubstring("is missing"));
	CHECK(run(song.client, "job.list").at("jobs").size() == jobs);

	// Without a project file there is no media folder to write into.
	run(song.client, "project.new");
	writeFile(song.library / "other.wav", 441, 1, 44100.0);
	song.clip(song.library / "other.wav");
	CHECK_THAT(reject(song.client, "media.conform", Json::object()), ContainsSubstring("save the project"));
}

TEST_CASE("pool file changes rescale and trim clip analysis and preserve stretched fades", "[integration]")
{
	const bool conform = GENERATE(false, true);
	CAPTURE(conform);
	SavedProject song;
	writeFile(song.library / "source.wav", 24000, 1, 24000);
	const uint64_t clip = song.clip(song.library / "source.wav");
	const uint64_t media = song.mediaOf(clip);
	run(song.client, "clip.setWarp", {{"clip", clip}, {"tempo", 120}, {"gridStart", 100}});
	for (const auto& [beat, frame] : std::vector<std::pair<int, int>>{{1, 6000}, {2, 20000}})
		run(song.client, "clip.adjustGrid", {{"clip", clip}, {"mode", "beat"}, {"beat", beat}, {"frame", frame}});
	for (const auto& [position, target] : std::vector<std::pair<int, int>>{{8000, 10000}, {20000, 21000}})
		run(song.client, "clip.addWarpMarker", {{"clip", clip}, {"position", position}, {"target", target}});
	run(song.client, "clip.setHitpoints",
		{{"clip", clip},
			{"hitpoints",
				Json::array({{{"position", 3000}, {"peak", -3}, {"intensity", 1}, {"locked", true}},
					{{"position", 22000}, {"peak", -6}, {"intensity", 0.5}}})}});
	run(song.client, "clip.setHitpointFilter", {{"clip", clip}, {"minimumLength", 240}});
	for (const auto& [start, end] : std::vector<std::pair<int, int>>{{4000, 16000}, {20000, 23000}})
		run(song.client, "clip.addRegion", {{"clip", clip}, {"start", start}, {"end", end}, {"name", "Section"}});
	const Json stretched = run(song.client, "clip.add", {{"media", media}}).at("clip").at("id");
	run(song.client, "clip.setWarp", {{"clip", stretched}, {"musical", true}, {"tempo", 240}, {"gridStart", 20000}});
	const Json track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const Json sourceEvent =
		run(song.client, "audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}}).at("event").at("id");
	run(song.client, "quantize.createWarpMarkers", {{"events", {sourceEvent}}});
	const Json originals = byId(run(song.client, "project.get").at("clips"), clip).at("warp").at("markers");
	REQUIRE(originals.size() == 4);
	run(song.client, "quantize.set", {{"preset", "1/16"}, {"settings", {{"warpQuantize", true}}}});
	run(song.client, "quantize.apply", {{"events", {sourceEvent}}});
	const Json quantized = byId(run(song.client, "project.get").at("clips"), clip).at("warp");
	REQUIRE(quantized.at("created") == Json::array({3000, 22000}));
	REQUIRE(quantized.at("quantizeOrigin").at("original") == originals);
	REQUIRE(quantized.at("markers") != originals);
	REQUIRE(quantized.at("quantizeOrigin").at("quantized") == quantized.at("markers"));
	const Json event = run(song.client, "audioEvent.add",
		{{"track", track}, {"clip", stretched}, {"offset", 1000}, {"length", 2000}, {"position", 0}})
						   .at("event")
						   .at("id");
	run(song.client, "audioEvent.setFades", {{"event", event}, {"fadeIn", {{"length", 3000}}}});
	const Json before = run(song.client, "project.get");
	if (conform)
	{
		runJob(song.client, "media.conform");
	}
	else
	{
		writeFile(song.library / "short.wav", 12000, 1, 24000);
		run(song.client, "media.relink",
			{{"files", Json::array({{{"media", media}, {"path", utf8(song.library / "short.wav")}}})}});
	}
	const int scale = conform ? 2 : 1;
	const Json after = run(song.client, "project.get");
	const Json changed = byId(after.at("clips"), clip);
	const Json& warp = changed.at("warp");
	CHECK(warp.at("gridStart") == 100 * scale);
	CHECK(warp.at("tempo") == 120);
	REQUIRE(warp.at("beats").size() == (conform ? 2 : 1));
	CHECK(warp.at("beats").at(0) == Json{{"beat", 1}, {"frame", 6000 * scale}});
	const Json created = conform ? Json::array({6000, 44000}) : Json::array({3000});
	CHECK(warp.at("created") == created);
	Json rescaledOriginals = Json::array({{{"position", 3000 * scale}, {"target", 3750 * scale}},
		{{"position", 8000 * scale}, {"target", 10000 * scale}}});
	Json ownMarkers = Json::array({{{"position", 8000 * scale}, {"target", 10000 * scale}}});
	if (conform)
	{
		rescaledOriginals.push_back({{"position", 40000}, {"target", 42000}});
		rescaledOriginals.push_back({{"position", 44000}, {"target", 45000}});
		ownMarkers.push_back({{"position", 40000}, {"target", 42000}});
	}
	Json rescaledQuantized = Json::array();
	for (size_t i = 0; i < rescaledOriginals.size(); ++i)
	{
		const Json& marker = quantized.at("markers").at(i);
		rescaledQuantized.push_back({{"position", marker.at("position").get<int64_t>() * scale},
			{"target", marker.at("target").get<int64_t>() * scale}});
	}
	CHECK(warp.at("markers") == rescaledQuantized);
	CHECK(warp.at("quantizeOrigin").at("original") == rescaledOriginals);
	CHECK(warp.at("quantizeOrigin").at("quantized") == rescaledQuantized);
	REQUIRE(changed.at("hitpoints").size() == (conform ? 2 : 1));
	CHECK(changed.at("hitpoints").at(0).at("position") == 3000 * scale);
	CHECK(changed.at("hitpoints").at(0).at("locked") == true);
	CHECK(changed.at("hitpointFilter").at("minimumLength") == 240 * scale);
	REQUIRE(changed.at("regions").size() == (conform ? 2 : 1));
	CHECK(changed.at("regions").at(0).at("start") == 4000 * scale);
	CHECK(changed.at("regions").at(0).at("end") == (conform ? 32000 : 12000));
	const Json played = byId(after.at("audioEvents"), event);
	CHECK(byId(after.at("clips"), stretched).at("warp").at("gridStart") == (conform ? 40000 : 11999));
	CHECK(played.at("fadeIn").at("length") == 3000 * scale);
	CHECK(played.at("fadeIn").at("length") <= played.at("fadeRoom"));
	run(song.client, "history.undo");
	CHECK(run(song.client, "project.get") == before);
	run(song.client, "history.redo");
	CHECK(run(song.client, "project.get") == after);
	run(song.client, "quantize.reset", {{"events", {sourceEvent}}});
	const Json reset = byId(run(song.client, "project.get").at("clips"), clip).at("warp");
	CHECK(reset.at("markers") == rescaledOriginals);
	CHECK(reset.at("created") == created);
	CHECK(!reset.contains("quantizeOrigin"));
	run(song.client, "quantize.resetWarpMarkers", {{"events", {sourceEvent}}});
	const Json removed = byId(run(song.client, "project.get").at("clips"), clip).at("warp");
	CHECK(removed.at("markers") == ownMarkers);
	CHECK(!removed.contains("created"));
	// The trimmed definition no longer prevents later edits.
	run(song.client, "clip.setWarp", {{"clip", clip}, {"tempo", 121}});
	run(song.client, "clip.setWarpMarker", {{"clip", clip}, {"position", 8000 * scale}, {"target", 10000 * scale}});
}

TEST_CASE("pool file changes move slice and quantize reset origins in every track version", "[integration]")
{
	const std::string change = GENERATE("conform", "resampled", "short");
	const bool inactive = GENERATE(false, true);
	CAPTURE(change, inactive);
	SavedProject song;
	writeFile(song.library / "source.wav", 24000, 1, 24000);
	const uint64_t clip = song.clip(song.library / "source.wav");
	const Json track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const auto add = [&](int64_t position, int64_t offset, int64_t length)
	{
		return run(song.client, "audioEvent.add",
			{{"track", track}, {"clip", clip}, {"position", position}, {"offset", offset}, {"length", length}})
			.at("event")
			.at("id");
	};
	const Json quantized = add(1000, 3000, 18000);
	const Json next = add(30000, 1000, 8000);
	run(song.client, "quantize.set", {{"preset", "1/16"}});
	run(song.client, "quantize.apply", {{"events", {quantized, next}}});
	run(song.client, "quantize.crossfade", {{"events", {quantized, next}}, {"length", 2.0}});
	const Json quantizeOrigin = song.event(quantized).at("quantizeOrigin");
	REQUIRE(quantizeOrigin.at("original").at("length") != quantizeOrigin.at("quantized").at("length"));
	const Json sliced = add(100000, 3000, 18000);
	run(song.client, "event.setSnapPoint", {{"events", {sliced}}, {"position", 132000}});
	run(song.client, "audioEvent.setFades",
		{{"event", sliced}, {"fadeIn", {{"length", 1000}}}, {"fadeOut", {{"length", 12000}, {"shape", "sCurve"}}}});
	run(song.client, "clip.setHitpoints",
		{{"clip", clip}, {"hitpoints", Json::array({{{"position", 7000}, {"peak", -3}, {"intensity", 1}}})}});
	const Json pieces = run(song.client, "quantize.slice", {{"events", {sliced}}}).at("events");
	REQUIRE(pieces.size() == 2);
	const Json sliceOrigin = song.event(sliced).at("sliceOrigin");
	if (inactive)
		run(song.client, "trackVersion.add", {{"tracks", {track}}});
	const Json before = run(song.client, "project.get");
	const int64_t scale = change == "short" ? 1 : 2;
	const int64_t fileLength = change == "conform" ? 48000 : 12000 * scale;
	if (change == "conform")
	{
		runJob(song.client, "media.conform");
	}
	else
	{
		writeFile(song.library / "replacement.wav", fileLength, 1, 24000.0 * static_cast<double>(scale));
		run(song.client, change == "short" ? "media.relink" : "media.relinkResampled",
			{{"files",
				Json::array({{{"media", song.mediaOf(clip)}, {"path", utf8(song.library / "replacement.wav")}}})}});
	}
	const Json after = run(song.client, "project.get");
	run(song.client, "history.undo");
	CHECK(run(song.client, "project.get") == before);
	run(song.client, "history.redo");
	CHECK(run(song.client, "project.get") == after);
	if (inactive)
		run(song.client, "trackVersion.activate", {{"tracks", {track}}, {"version", 1}});
	const auto movedTiming = [&](Json timing)
	{
		const int64_t offset = timing.at("offset").get<int64_t>() * scale;
		const int64_t end = offset + (timing.at("length").get<int64_t>() * scale);
		timing["offset"] = std::min(offset, fileLength - 1);
		timing["length"] = std::max<int64_t>(std::min(end, fileLength) - timing.at("offset").get<int64_t>(), 1);
		return timing;
	};
	const Json original = movedTiming(quantizeOrigin.at("original"));
	const Json changedOrigin = song.event(quantized).at("quantizeOrigin");
	CHECK(changedOrigin.at("original") == original);
	CHECK(changedOrigin.at("quantized") == movedTiming(quantizeOrigin.at("quantized")));
	Json expectedSlice = sliceOrigin;
	expectedSlice["timing"] = movedTiming(sliceOrigin.at("timing"));
	expectedSlice["snapPoint"] = std::min(sliceOrigin.at("snapPoint").get<int64_t>() * scale, fileLength - 1);
	expectedSlice["fadeOut"]["length"] =
		std::min<int64_t>(12000 * scale, expectedSlice.at("timing").at("length").get<int64_t>() - (1000 * scale));
	CHECK(song.event(sliced).at("sliceOrigin") == expectedSlice);
	run(song.client, "quantize.reset", {{"events", {quantized, next}}});
	const Json reset = song.event(quantized);
	for (const auto* field : {"position", "offset", "length", "crossfade"})
		CHECK(reset.at(field) == original.at(field));
	run(song.client, "quantize.resetSlices", {{"events", pieces}});
	const Json joined = song.event(sliced);
	for (const auto* field : {"position", "offset", "length", "crossfade"})
		CHECK(joined.at(field) == expectedSlice.at("timing").at(field));
	CHECK(joined.at("snapPoint") == expectedSlice.at("snapPoint"));
	CHECK(joined.at("fadeOut") == expectedSlice.at("fadeOut"));
	CHECK(joined.at("offset").get<int64_t>() + joined.at("length").get<int64_t>() <= fileLength);
	CHECK(joined.at("fadeIn").at("length").get<int64_t>() + joined.at("fadeOut").at("length").get<int64_t>() <=
		joined.at("length").get<int64_t>());
}

TEST_CASE("pool reset origins rescale crossfades using each source event's file", "[integration]")
{
	const bool slice = GENERATE(false, true);
	const bool changedDestination = GENERATE(false, true);
	const bool shorten = GENERATE(false, true);
	CAPTURE(slice, changedDestination, shorten);
	SavedProject song;
	writeFile(song.library / "source.wav", 24000, 1, 24000);
	writeFile(song.library / "other.wav", 48000, 1);
	const uint64_t source = song.clip(song.library / "source.wav");
	const uint64_t destination = changedDestination ? source : song.clip(song.library / "other.wav");
	const Json track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const Json from =
		run(song.client, "audioEvent.add", {{"track", track}, {"clip", source}, {"position", 0}, {"length", 9000}})
			.at("event")
			.at("id");
	const Json to = run(song.client, "audioEvent.add",
		{{"track", track}, {"clip", destination}, {"position", 1000}, {"offset", 2000}, {"length", 18000}})
						.at("event")
						.at("id");
	run(song.client, "crossfade.set", {{"from", from}, {"to", to}, {"length", 6000}, {"outLength", 8000}});
	const Json crossfade = song.event(to).at("crossfade");
	if (slice)
	{
		run(song.client, "clip.setHitpoints",
			{{"clip", destination},
				{"hitpoints", Json::array({{{"position", 7000}, {"peak", -3}, {"intensity", 1}}})}});
		run(song.client, "quantize.slice", {{"events", {to}}});
		// Only the saved origin now refers to the source file, so it must be visited independently.
		run(song.client, "crossfade.remove", {{"event", to}});
	}
	else
	{
		run(song.client, "quantize.set", {{"preset", "1/16"}});
		run(song.client, "quantize.apply", {{"events", {to}}});
	}
	run(song.client, "trackVersion.add", {{"tracks", {track}}});
	if (shorten)
	{
		writeFile(song.library / "short.wav", 6000, 1, 24000);
		run(song.client, "media.relink",
			{{"files", Json::array({{{"media", song.mediaOf(source)}, {"path", utf8(song.library / "short.wav")}}})}});
	}
	else
	{
		runJob(song.client, "media.conform");
	}
	run(song.client, "trackVersion.activate", {{"tracks", {track}}, {"version", 1}});
	Json expected = crossfade;
	expected["outLength"] = shorten ? 6000 : 16000;
	if (changedDestination)
		expected["length"] = shorten ? 4000 : 12000;
	if (slice)
	{
		CHECK(song.event(to).at("sliceOrigin").at("timing").at("crossfade") == expected);
		run(song.client, "quantize.resetSlices", {{"events", {to}}});
	}
	else
	{
		const Json origin = song.event(to).at("quantizeOrigin");
		CHECK(origin.at("original").at("crossfade") == expected);
		CHECK(origin.at("quantized").at("crossfade") == expected);
		run(song.client, "quantize.reset", {{"events", {to}}});
	}
	CHECK(song.event(to).at("crossfade") == expected);
}

TEST_CASE("pool slice origins keep stretched fade room and clamp offsets beyond shorter files", "[integration]")
{
	const bool conform = GENERATE(false, true);
	const int64_t offset = GENERATE(1000, 2000);
	CAPTURE(conform, offset);
	SavedProject song;
	writeFile(song.library / "source.wav", 4000, 1, 24000);
	const uint64_t clip = song.clip(song.library / "source.wav");
	run(song.client, "clip.setWarp", {{"clip", clip}, {"musical", true}, {"tempo", 240}});
	const Json track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const Json event = run(song.client, "audioEvent.add",
		{{"track", track}, {"clip", clip}, {"position", 1000}, {"offset", offset}, {"length", 2000}})
						   .at("event")
						   .at("id");
	run(song.client, "audioEvent.setFades", {{"event", event}, {"fadeOut", {{"length", 3000}}}});
	run(song.client, "clip.setHitpoints",
		{{"clip", clip}, {"hitpoints", Json::array({{{"position", offset + 500}, {"peak", -3}, {"intensity", 1}}})}});
	REQUIRE(run(song.client, "quantize.slice", {{"events", {event}}}).at("events").size() == 2);
	if (conform)
	{
		runJob(song.client, "media.conform");
	}
	else
	{
		writeFile(song.library / "short.wav", 1200, 1, 24000);
		run(song.client, "media.relink",
			{{"files", Json::array({{{"media", song.mediaOf(clip)}, {"path", utf8(song.library / "short.wav")}}})}});
	}
	run(song.client, "quantize.resetSlices", {{"events", {event}}});
	const Json joined = song.event(event);
	const int64_t length = conform ? 4000 : std::max<int64_t>(1200 - offset, 1);
	CHECK(joined.at("offset") == (conform ? offset * 2 : std::min<int64_t>(offset, 1199)));
	CHECK(joined.at("length") == length);
	CHECK(joined.at("fadeOut").at("length") == (conform ? 6000 : length * 2));
	CHECK(joined.at("fadeOut").at("length") <= joined.at("fadeRoom"));
	CHECK(joined.at("snapPoint").is_null());
}

TEST_CASE("pool file changes keep empty quantize originals removable", "[integration]")
{
	const bool conform = GENERATE(false, true);
	CAPTURE(conform);
	SavedProject song;
	writeFile(song.library / "source.wav", 24000, 1, 24000);
	const uint64_t clip = song.clip(song.library / "source.wav");
	const Json track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const Json first =
		run(song.client, "audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 1000}, {"length", 10000}})
			.at("event")
			.at("id");
	const Json next = run(song.client, "audioEvent.add",
		{{"track", track}, {"clip", clip}, {"position", 21000}, {"offset", 10000}, {"length", 14000}})
						  .at("event")
						  .at("id");
	run(song.client, "quantize.set", {{"preset", "1/4"}});
	run(song.client, "quantize.apply", {{"events", {first, next}}});
	run(song.client, "quantize.crossfade", {{"events", {first, next}}, {"length", 2.0}});
	const Json tail = run(song.client, "event.split", {{"events", {first}}, {"positions", {20048}}}).at("events").at(0);
	REQUIRE(song.event(tail).at("quantizeOrigin").at("original").at("length") == 0);
	if (conform)
	{
		runJob(song.client, "media.conform");
	}
	else
	{
		writeFile(song.library / "short.wav", 10000, 1, 24000);
		run(song.client, "media.relink",
			{{"files", Json::array({{{"media", song.mediaOf(clip)}, {"path", utf8(song.library / "short.wav")}}})}});
	}
	CHECK(song.event(tail).at("quantizeOrigin").at("original").at("length") == 0);
	CHECK(run(song.client, "quantize.reset", {{"events", {first, next, tail}}}).at("removed") == Json::array({tail}));
}

TEST_CASE("pool resampling drops coincident anchors and empty regions", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "source.wav", 24, 1);
	writeFile(song.library / "resampled.wav", 4, 1, 8000);
	const uint64_t clip = song.clip(song.library / "source.wav");
	run(song.client, "clip.setWarp", {{"clip", clip}, {"tempo", 120}});
	for (const auto& [beat, frame] : std::vector<std::pair<int, int>>{{1, 1}, {2, 2}, {3, 7}})
		run(song.client, "clip.adjustGrid", {{"clip", clip}, {"mode", "beat"}, {"beat", beat}, {"frame", frame}});
	Json hitpoints = Json::array();
	for (const int position : {1, 2, 3, 4, 23})
	{
		run(song.client, "clip.addWarpMarker", {{"clip", clip}, {"position", position}});
		hitpoints.push_back({{"position", position}, {"peak", -3}, {"intensity", 1}});
	}
	run(song.client, "clip.setHitpoints", {{"clip", clip}, {"hitpoints", hitpoints}});
	for (const auto& [start, end] : std::vector<std::pair<int, int>>{{1, 2}, {3, 23}})
		run(song.client, "clip.addRegion", {{"clip", clip}, {"start", start}, {"end", end}});
	const Json before = run(song.client, "project.get");
	run(song.client, "media.relinkResampled",
		{{"files", Json::array({{{"media", song.mediaOf(clip)}, {"path", utf8(song.library / "resampled.wav")}}})}});
	const Json changed = byId(run(song.client, "project.get").at("clips"), clip);
	CHECK(changed.at("warp").at("beats") == Json::array({{{"beat", 3}, {"frame", 1}}}));
	CHECK(changed.at("warp").at("markers") == Json::array({{{"position", 1}, {"target", 1}}}));
	REQUIRE(changed.at("hitpoints").size() == 2);
	CHECK(changed.at("hitpoints").at(0).at("position") == 0);
	CHECK(changed.at("hitpoints").at(1).at("position") == 1);
	REQUIRE(changed.at("regions").size() == 1);
	CHECK(changed.at("regions").at(0).at("start") == 1);
	CHECK(changed.at("regions").at(0).at("end") == 4);
	run(song.client, "clip.setWarp", {{"clip", clip}, {"tempo", 121}});
	run(song.client, "history.undo");
	run(song.client, "history.undo");
	CHECK(run(song.client, "project.get") == before);
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

	CHECK(runJob(song.client, "media.conform", {{"media", {media, media}}}).at("media") == Json::array({media}));
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

	runJob(song.client, "media.conform");
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

TEST_CASE("media.conform with a merge key joins the last step if it has the key", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "fast.wav", 4800, 1);
	const uint64_t media = song.mediaOf(song.clip(song.library / "fast.wav"));
	const auto setRate = [&song](double rate, const std::string& key)
	{
		run(song.client, "history.transaction",
			{{"description", "Project Setup"}, {"merge", key},
				{"commands", Json::array({{{"command", "project.set"}, {"parameters", {{"sampleRate", rate}}}}})}});
	};

	// It runs as a job, so a transaction can't wait for it.
	CHECK_THAT(reject(song.client, "history.transaction",
				   {{"description", "Project Setup"}, {"commands", Json::array({{{"command", "media.conform"}}})}}),
		ContainsSubstring("can't be part of a transaction"));

	// As the project setup dialog does: a new sample rate, then the files conformed to it, as one undo step.
	setRate(44100.0, "setup 1");
	const size_t before = steps(song.client);
	CHECK(runJob(song.client, "media.conform", {{"merge", "setup 1"}}).at("media") == Json::array({media}));
	CHECK(steps(song.client) == before);
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Project Setup");
	const Json conformed = byId(run(song.client, "project.get").at("media"), media);
	CHECK(conformed.at("sampleRate") == 44100.0);
	CHECK(conformed.at("length") == 4410);
	CHECK(fs::u8path(conformed.at("path").get<std::string>()).parent_path() == song.media);
	run(song.client, "history.undo");
	CHECK(run(song.client, "project.get").at("sampleRate") == 48000.0);
	CHECK(byId(run(song.client, "project.get").at("media"), media).at("sampleRate") == 48000.0);

	// After another step, or with another key, the files are a step of their own.
	setRate(32000.0, "setup 2");
	run(song.client, "track.add", {{"type", "audio"}});
	CHECK(runJob(song.client, "media.conform", {{"merge", "setup 2"}}).at("media") == Json::array({media}));
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Conform files");
	setRate(22050.0, "setup 3");
	CHECK(runJob(song.client, "media.conform", {{"merge", "setup 4"}}).at("media") == Json::array({media}));
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Conform files");
	run(song.client, "history.undo");
	CHECK(byId(run(song.client, "project.get").at("media"), media).at("sampleRate") == 32000.0);
	CHECK(run(song.client, "project.get").at("sampleRate") == 22050.0);
}

TEST_CASE("media.locate looks through a folder as a job, while other commands are answered", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "kick.wav", 4800, 1);
	const uint64_t kick = song.mediaOf(song.clip(song.library / "kick.wav"));
	song.save();
	song.settle();
	// A few thousand files to look through, and the missing one among them.
	const fs::path moved = song.dir.path() / "moved";
	fillFolder(moved, 50);
	fs::rename(song.library / "kick.wav", moved / "folder49" / "kick.wav");
	song.reopen();

	// transport.locate is answered while the job looks.
	std::vector<Json> progress;
	const Json finished = test::whileHeld(
		song.engine, song.client, "media.locate", {{"folder", utf8(moved)}},
		[&song](uint64_t /*job*/) { run(song.client, "transport.locate", {{"position", 0}}); }, &progress);
	INFO(finished.dump());
	REQUIRE(finished.at("state") == "done");
	CHECK(finished.at("result").at("relinked") == Json::array({kick}));
	CHECK(finished.at("result").at("missing").empty());
	CHECK(fs::equivalent(
		fs::u8path(byId(run(song.client, "project.get").at("media"), kick).at("path").get<std::string>()),
		moved / "folder49" / "kick.wav"));
	// It says where it looks.
	REQUIRE(!progress.empty());
	CHECK(progress.front().at("detail").get<std::string>().find(utf8(moved)) == 0);
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Locate missing files");
}

TEST_CASE("media.locate relinks the files it found but one that no longer fits, which it reports", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "kick.wav", 4800, 1);
	writeFile(song.library / "snare.wav", 48000, 1);
	const uint64_t kick = song.mediaOf(song.clip(song.library / "kick.wav"));
	const uint64_t snare = song.mediaOf(song.clip(song.library / "snare.wav"));
	song.settle();
	// snare is pointed at a 44.1 kHz file at its path, which then goes missing with kick: media.locate looks for both
	// and finds them in a folder of 2500 files.
	writeFile(song.library / "snare.wav", 44100, 1, 44100.0);
	run(song.client, "media.relinkResampled",
		{{"files", Json::array({{{"media", snare}, {"path", utf8(song.library / "snare.wav")}}})}});
	song.settle();
	const fs::path moved = song.dir.path() / "moved";
	fillFolder(moved, 50);
	fs::rename(song.library / "kick.wav", moved / "folder49" / "kick.wav");
	fs::rename(song.library / "snare.wav", moved / "folder49" / "snare.wav");

	// While the job looks, undo takes snare back to 48 kHz: it keeps its path, so the job's entry for it still
	// applies by from, but the file found no longer fits. The job relinks kick all the same.
	const Json finished = test::whileHeld(song.engine, song.client, "media.locate", {{"folder", utf8(moved)}},
		[&song](uint64_t /*job*/) { run(song.client, "history.undo"); });
	INFO(finished.dump());
	REQUIRE(finished.at("state") == "done");
	CHECK(finished.at("result").at("relinked") == Json::array({kick}));
	CHECK(finished.at("result").at("changed") == Json::array({snare}));
	CHECK(finished.at("result").at("missing").empty());
	const Json project = run(song.client, "project.get");
	CHECK(fs::equivalent(
		fs::u8path(byId(project.at("media"), kick).at("path").get<std::string>()), moved / "folder49" / "kick.wav"));
	const Json kept = byId(project.at("media"), snare);
	CHECK(kept.at("path") == utf8(song.library / "snare.wav"));
	CHECK(kept.at("sampleRate") == 48000.0);
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Locate missing files");
}

TEST_CASE("job.cancel stops media.locate, which then changes nothing", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "kick.wav", 4800, 1);
	const uint64_t kick = song.mediaOf(song.clip(song.library / "kick.wav"));
	song.save();
	song.settle();
	const fs::path moved = song.dir.path() / "moved";
	fillFolder(moved, 50);
	fs::rename(song.library / "kick.wav", moved / "folder49" / "kick.wav");
	song.reopen();
	const size_t before = steps(song.client);

	const Json cancelled = cancelJob(song, "media.locate", {{"folder", utf8(moved)}});
	CHECK(cancelled.at("state") == "cancelled");
	CHECK(run(song.client, "pool.get").at("missingFiles") == 1);
	CHECK(byId(run(song.client, "project.get").at("media"), kick).at("path") == utf8(song.library / "kick.wav"));
	CHECK(steps(song.client) == before);
}

TEST_CASE("job.cancel stops media.conform, which then leaves no file behind and changes nothing", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "slow.wav", 441000, 2, 44100.0); // ten seconds
	const uint64_t slow = song.mediaOf(song.clip(song.library / "slow.wav"));
	const size_t before = steps(song.client);

	const Json cancelled = cancelJob(song, "media.conform", {{"media", {slow}}});
	CHECK(cancelled.at("state") == "cancelled");
	CHECK((!fs::exists(song.media) || filesIn(song.media) == 0));
	const Json media = byId(run(song.client, "project.get").at("media"), slow);
	CHECK(media.at("path") == utf8(song.library / "slow.wav"));
	CHECK(media.at("sampleRate") == 44100.0);
	CHECK(steps(song.client) == before);
}

TEST_CASE("an edit made while media.locate or media.conform runs is kept", "[integration]")
{
	SECTION("a missing file removed while media.locate looks for it")
	{
		SavedProject song;
		writeFile(song.library / "kick.wav", 4800, 1);
		const uint64_t kick = song.mediaOf(song.clip(song.library / "kick.wav"));
		song.save();
		song.settle();
		const fs::path moved = song.dir.path() / "moved";
		fillFolder(moved, 50);
		fs::rename(song.library / "kick.wav", moved / "folder49" / "kick.wav");
		song.reopen();

		const Json finished = test::whileHeld(song.engine, song.client, "media.locate", {{"folder", utf8(moved)}},
			[&song](uint64_t /*job*/) { run(song.client, "media.removeMissing"); });
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		CHECK(finished.at("result").at("relinked").empty());
		CHECK(finished.at("result").at("changed") == Json::array({kick}));
		const Json project = run(song.client, "project.get");
		for (const Json& media : project.at("media"))
			CHECK(media.at("id") != kick);
		CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Remove missing files");
	}
	SECTION("a media file relinked by hand while media.conform converts it")
	{
		SavedProject song;
		writeFile(song.library / "slow.wav", 441000, 2, 44100.0);
		writeFile(song.library / "other.wav", 4410, 2, 44100.0);
		const uint64_t slow = song.mediaOf(song.clip(song.library / "slow.wav"));

		const Json finished = test::whileHeld(song.engine, song.client, "media.conform", {{"media", {slow}}},
			[&song, slow](uint64_t /*job*/)
			{
				run(song.client, "media.relink",
					{{"files", Json::array({{{"media", slow}, {"path", utf8(song.library / "other.wav")}}})}});
			});
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		CHECK(finished.at("result").at("media").empty());
		CHECK(finished.at("result").at("changed") == Json::array({slow}));
		const Json media = byId(run(song.client, "project.get").at("media"), slow);
		CHECK(media.at("path") == utf8(song.library / "other.wav"));
		CHECK(media.at("sampleRate") == 44100.0);
		CHECK((!fs::exists(song.media) || filesIn(song.media) == 0));
		CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Relink media files");
	}
	SECTION("the project's sample rate changed while media.conform converts to the old one")
	{
		SavedProject song;
		writeFile(song.library / "slow.wav", 441000, 2, 44100.0);
		const uint64_t slow = song.mediaOf(song.clip(song.library / "slow.wav"));

		const Json finished = test::whileHeld(song.engine, song.client, "media.conform", {{"media", {slow}}},
			[&song](uint64_t /*job*/) { run(song.client, "project.set", {{"sampleRate", 96000.0}}); });
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		CHECK(finished.at("result").at("media").empty());
		CHECK(finished.at("result").at("changed") == Json::array({slow}));
		const Json media = byId(run(song.client, "project.get").at("media"), slow);
		CHECK(media.at("path") == utf8(song.library / "slow.wav"));
		CHECK(media.at("sampleRate") == 44100.0);
		CHECK((!fs::exists(song.media) || filesIn(song.media) == 0));
		CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Change project");
	}
}

TEST_CASE("media.relinkResampled points media files at files at another rate and checks them", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "slow.wav", 44100, 1, 44100.0);
	writeFile(song.library / "fast.wav", 48000, 1);
	writeFile(song.library / "stereo.wav", 48000, 2);
	writeFile(song.library / "empty.wav", 0, 1);
	const uint64_t slow = song.clip(song.library / "slow.wav");
	const uint64_t media = song.mediaOf(slow);
	const auto track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	const auto event = run(song.client, "audioEvent.add",
		{{"track", track}, {"clip", slow}, {"position", 0}, {"offset", 4410}, {"length", 22050}})
						   .at("event")
						   .at("id");
	run(song.client, "audioEvent.setVolumeCurve",
		{{"event", event}, {"points", {{{"position", 8820}, {"volume", -6.0}}}}});
	const auto relink = [&song](Json file)
	{ return song.client.call("media.relinkResampled", {{"files", Json::array({std::move(file)})}}); };
	const std::string fast = utf8(song.library / "fast.wav");
	const std::string from = utf8(song.library / "slow.wav");
	const size_t before = steps(song.client);

	// Files that don't fit, and media files that aren't there, are refused and change nothing.
	CHECK_THAT(relink({{"media", 999}, {"path", fast}}).error, ContainsSubstring("999"));
	CHECK_THAT(
		relink({{"media", media}, {"path", utf8(song.library / "stereo.wav")}}).error, ContainsSubstring("channels"));
	CHECK(!relink({{"media", media}, {"path", utf8(song.library / "nowhere.wav")}}).ok());
	CHECK_THAT(relink({{"media", media}, {"path", utf8(song.library / "empty.wav")}}).error,
		ContainsSubstring("no sample frames"));
	CHECK_THAT(reject(song.client, "media.relinkResampled",
				   {{"files", Json::array({{{"media", media}, {"path", fast}}, {{"media", media}, {"path", fast}}})}}),
		ContainsSubstring("twice"));
	// Also when an entry wouldn't apply: each media file can be given once.
	CHECK_THAT(reject(song.client, "media.relinkResampled",
				   {{"files",
					   Json::array({{{"media", media}, {"path", fast}, {"from", fast}},
						   {{"media", media}, {"path", fast}, {"from", from}}})}}),
		ContainsSubstring("twice"));
	CHECK(steps(song.client) == before);

	// A media file that moved on, was removed or is at another rate meanwhile is skipped.
	const auto skippedBy = [&song](const Json& file)
	{ return run(song.client, "media.relinkResampled", {{"files", Json::array({file})}}).at("skipped"); };
	CHECK(skippedBy({{"media", media}, {"path", fast}, {"from", fast}}) == Json::array({media}));
	CHECK(skippedBy({{"media", 999}, {"path", fast}, {"from", from}}) == Json::array({999}));
	CHECK(skippedBy({{"media", media}, {"path", fast}, {"from", from}, {"fromSampleRate", 48000.0}}) ==
		Json::array({media}));
	// So is every entry once the project is at another sample rate than the files were made for, or another project
	// is open.
	Json skipped = run(song.client, "media.relinkResampled",
		{{"files", Json::array({{{"media", media}, {"path", fast}}})}, {"projectSampleRate", 44100.0}});
	CHECK(skipped.at("skipped") == Json::array({media}));
	const auto project = run(song.client, "project.revision").at("project").get<uint64_t>();
	skipped = run(song.client, "media.relinkResampled",
		{{"files", Json::array({{{"media", media}, {"path", fast}}})}, {"project", project + 1}});
	CHECK(skipped.at("skipped") == Json::array({media}));
	CHECK(steps(song.client) == before);

	// What plays it moves with the new sample frames, as one undo step.
	const Json relinked = run(song.client, "media.relinkResampled",
		{{"files", Json::array({{{"media", media}, {"path", fast}, {"from", from}, {"fromSampleRate", 44100.0}}})},
			{"project", project}});
	CHECK(relinked.at("relinked") == Json::array({media}));
	CHECK(relinked.at("skipped").empty());
	const Json changed = byId(run(song.client, "project.get").at("media"), media);
	CHECK(changed.at("path") == fast);
	CHECK(changed.at("sampleRate") == 48000.0);
	CHECK(changed.at("length") == 48000);
	CHECK(song.event(event).at("offset") == 4800);
	CHECK(song.event(event).at("length") == 24000);
	CHECK(song.event(event).at("volumeCurve").at(0).at("position") == 9600);
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Conform files");
	run(song.client, "history.undo");
	CHECK(song.event(event).at("offset") == 4410);
	CHECK(byId(run(song.client, "project.get").at("media"), media).at("path") == from);
}

TEST_CASE("media.relink skips an entry with length whose file went or changed, and another project", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "a.wav", 4800, 1);
	writeFile(song.library / "b.wav", 4800, 1);
	writeFile(song.library / "c.wav", 4800, 1);
	writeFile(song.library / "a2.wav", 4800, 1);
	writeFile(song.library / "b2.wav", 4799, 1);
	writeFile(song.library / "c2.wav", 4800, 2);
	const uint64_t a = song.mediaOf(song.clip(song.library / "a.wav"));
	const uint64_t b = song.mediaOf(song.clip(song.library / "b.wav"));
	const uint64_t c = song.mediaOf(song.clip(song.library / "c.wav"));
	const uint64_t d = song.mediaOf(song.clip(song.library / "a.wav"));
	const auto entry = [&song](uint64_t media, const std::string& file)
	{ return Json{{"media", media}, {"path", utf8(song.library / file)}, {"length", 4800}}; };
	const size_t before = steps(song.client);

	// Without length, a file that isn't there or doesn't fit is refused.
	CHECK(!song.client
			.call("media.relink", {{"files", Json::array({{{"media", a}, {"path", utf8(song.library / "gone.wav")}}})}})
			.ok());
	CHECK_THAT(reject(song.client, "media.relink",
				   {{"files", Json::array({{{"media", c}, {"path", utf8(song.library / "c2.wav")}}})}}),
		ContainsSubstring("channels"));

	// With it, such entries are skipped and the others relinked: a file that went, one of another length and one
	// that doesn't fit.
	const Json relinked = run(song.client, "media.relink",
		{{"files", Json::array({entry(a, "a2.wav"), entry(b, "b2.wav"), entry(c, "c2.wav"), entry(d, "gone.wav")})}});
	CHECK(relinked.at("relinked") == Json::array({a}));
	CHECK(relinked.at("skipped") == Json::array({b, c, d}));
	CHECK(byId(run(song.client, "project.get").at("media"), a).at("path") == utf8(song.library / "a2.wav"));
	CHECK(byId(run(song.client, "project.get").at("media"), b).at("path") == utf8(song.library / "b.wav"));
	CHECK(steps(song.client) == before + 1);

	// With project, every entry is skipped unless that project is still open.
	const auto project = run(song.client, "project.revision").at("project").get<uint64_t>();
	const Json other = run(song.client, "media.relink",
		{{"files", Json::array({{{"media", b}, {"path", utf8(song.library / "c.wav")}}})}, {"project", project + 1}});
	CHECK(other.at("relinked").empty());
	CHECK(other.at("skipped") == Json::array({b}));
	CHECK(steps(song.client) == before + 1);
	CHECK(run(song.client, "media.relink",
			  {{"files", Json::array({{{"media", b}, {"path", utf8(song.library / "c.wav")}}})}, {"project", project}})
			  .at("relinked") == Json::array({b}));
}

TEST_CASE("media.relink refuses a file without sample frames for a used media file", "[integration]")
{
	SavedProject song;
	writeFile(song.library / "a.wav", 4800, 1);
	writeFile(song.library / "short.wav", 4795, 1);
	writeFile(song.library / "empty.wav", 0, 1);
	const uint64_t clip = song.clip(song.library / "a.wav");
	const uint64_t media = song.mediaOf(clip);
	const auto track = run(song.client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id");
	run(song.client, "audioEvent.add", {{"track", track}, {"clip", clip}, {"position", 0}});
	const auto entry = [&song, media](const std::string& file)
	{ return Json{{"media", media}, {"path", utf8(song.library / file)}}; };
	const Json before = run(song.client, "project.get");
	const size_t steps0 = steps(song.client);

	// Alone, and after a shorter file for the same media file in the same call: refused, nothing changes.
	CHECK_THAT(reject(song.client, "media.relink", {{"files", Json::array({entry("empty.wav")})}}),
		ContainsSubstring("no sample frames"));
	CHECK_THAT(reject(song.client, "media.relink", {{"files", Json::array({entry("short.wav"), entry("empty.wav")})}}),
		ContainsSubstring("no sample frames"));
	const Json after = run(song.client, "project.get");
	CHECK(after.at("media") == before.at("media"));
	CHECK(after.at("audioEvents") == before.at("audioEvents"));
	CHECK(steps(song.client) == steps0);

	// An entry with length is skipped instead.
	Json checked = entry("empty.wav");
	checked["length"] = 0;
	const Json skipped = run(song.client, "media.relink", {{"files", Json::array({checked})}});
	CHECK(skipped.at("relinked").empty());
	CHECK(skipped.at("skipped") == Json::array({media}));
	CHECK(byId(run(song.client, "project.get").at("media"), media).at("length") == 4800);
}

TEST_CASE("a job's final command leaves a project opened while the job runs alone", "[integration]")
{
	// Another version of the song, with the same media files at the same paths, is opened while a job runs for the
	// first one: what the job found, converted or copied isn't for it.
	SavedProject song;
	writeFile(song.library / "kick.wav", 4800, 1);
	writeFile(song.library / "slow.wav", 441000, 2, 44100.0);
	const uint64_t kick = song.mediaOf(song.clip(song.library / "kick.wav"));
	const uint64_t slow = song.mediaOf(song.clip(song.library / "slow.wav"));
	song.save();
	song.settle();
	const fs::path other = song.folder / "Song-2.dawproject";
	fs::copy_file(song.file, other);
	const auto mediaPath = [&song](uint64_t media)
	{ return byId(run(song.client, "project.get").at("media"), media).at("path").get<std::string>(); };
	const auto openOther = [&song, &other](uint64_t /*job*/)
	{ run(song.client, "project.load", {{"path", utf8(other)}}); };

	SECTION("media.locate")
	{
		const fs::path moved = song.dir.path() / "moved";
		fillFolder(moved, 50);
		fs::rename(song.library / "kick.wav", moved / "folder49" / "kick.wav");
		const Json finished = test::whileHeld(
			song.engine, song.client, "media.locate", {{"folder", utf8(moved)}, {"media", {kick}}}, openOther);
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		CHECK(finished.at("result").at("relinked").empty());
		CHECK(finished.at("result").at("changed") == Json::array({kick}));
		CHECK(mediaPath(kick) == utf8(song.library / "kick.wav"));
	}
	SECTION("media.conform")
	{
		const Json finished =
			test::whileHeld(song.engine, song.client, "media.conform", {{"media", {slow}}}, openOther);
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		CHECK(finished.at("result").at("media").empty());
		CHECK(finished.at("result").at("changed") == Json::array({slow}));
		CHECK(mediaPath(slow) == utf8(song.library / "slow.wav"));
		CHECK((!fs::exists(song.media) || filesIn(song.media) == 0));
	}
	SECTION("media.prepareArchive")
	{
		const Json finished =
			test::whileHeld(song.engine, song.client, "media.prepareArchive", Json::object(), openOther);
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		CHECK(finished.at("result").at("copied").empty());
		CHECK(finished.at("result").at("changed") == Json::array({kick, slow}));
		CHECK(mediaPath(slow) == utf8(song.library / "slow.wav"));
		CHECK((!fs::exists(song.media) || filesIn(song.media) == 0));
	}
	CHECK(run(song.client, "project.location").at("file") == utf8(other));
	CHECK(steps(song.client) == 0);
	CHECK(!run(song.client, "project.revision").at("modified").get<bool>());
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
