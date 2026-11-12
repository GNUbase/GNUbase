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
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
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

	// The WAV files in folder. The peak files that the peak worker writes next to them whenever it gets to them, and
	// the temporary files it writes them through, don't count.
	size_t filesIn(const fs::path& folder)
	{
		size_t count = 0;
		for (const auto& entry : fs::directory_iterator(folder))
			count += entry.is_regular_file() && entry.path().extension() == ".wav" ? 1 : 0;
		return count;
	}

	// Waits for the job's job.finished event and returns the job. Progress events go to progress, if given.
	Json waitForJob(Client& client, uint64_t job, std::vector<Json>* progress = nullptr)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		Json finished; // null until the job finishes
		while (finished.is_null() && std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> message = client.wait(std::chrono::milliseconds(100));
			if (!message)
			{
				// Its events may have been taken by an earlier wait: once none are left, job.get tells.
				if (Json state = run(client, "job.get", {{"job", job}}).at("job"); state.at("state") != "running")
					finished = std::move(state);
				continue;
			}
			const auto* event = std::get_if<Event>(&*message);
			if (event == nullptr)
				continue;
			if (event->name == "job.progress" && event->data.at("job") == job && progress != nullptr)
				progress->push_back(event->data);
			if (event->name == "job.finished" && event->data.at("job").at("id") == job)
				finished = event->data.at("job");
		}
		INFO("job " << job << " didn't finish");
		REQUIRE(!finished.is_null());
		return finished;
	}

	// Runs a command that starts a job (media.locate, media.conform), waits for the job and returns its result; the
	// job must succeed.
	Json runJob(Client& client, const std::string& command, Json parameters = Json::object(),
		std::vector<Json>* progress = nullptr)
	{
		const Json started = run(client, command, std::move(parameters)).at("job");
		CHECK(started.at("kind") == command);
		const Json finished = waitForJob(client, started.at("id").get<uint64_t>(), progress);
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

	// Starts a job with the command and holds it at its first job.progress while during runs with its id, then lets it
	// go on and returns the job once it ended (#1154): what during does comes before anything the job does after it
	// got going, however the threads are scheduled. Progress events go to progress, if given.
	Json whileHeld(SavedProject& song, const std::string& command, Json parameters,
		const std::function<void(uint64_t job)>& during, std::vector<Json>* progress = nullptr)
	{
		test::HeldJob held(song.engine, command);
		const Json started = run(song.client, command, std::move(parameters)).at("job");
		CHECK(started.at("kind") == command);
		const auto job = started.at("id").get<uint64_t>();
		REQUIRE(held.waitUntilHeld());
		CHECK(held.job() == job);
		during(job);
		CHECK(run(song.client, "job.get", {{"job", job}}).at("job").at("state") == "running");
		held.release();
		return waitForJob(song.client, job, progress);
	}

	// Starts a job with the command, cancels it while it is held at its first progress, so that it can't end first
	// (#1079), and returns the job once it ended.
	Json cancelJob(SavedProject& song, const std::string& command, Json parameters = Json::object())
	{
		return whileHeld(song, command, std::move(parameters),
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
	const Json finished = whileHeld(
		song, "media.locate", {{"folder", utf8(moved)}},
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
	const Json finished = whileHeld(song, "media.locate", {{"folder", utf8(moved)}},
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

		const Json finished = whileHeld(song, "media.locate", {{"folder", utf8(moved)}},
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

		const Json finished = whileHeld(song, "media.conform", {{"media", {slow}}},
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

		const Json finished = whileHeld(song, "media.conform", {{"media", {slow}}},
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
		const Json finished = whileHeld(song, "media.locate", {{"folder", utf8(moved)}, {"media", {kick}}}, openOther);
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		CHECK(finished.at("result").at("relinked").empty());
		CHECK(finished.at("result").at("changed") == Json::array({kick}));
		CHECK(mediaPath(kick) == utf8(song.library / "kick.wav"));
	}
	SECTION("media.conform")
	{
		const Json finished = whileHeld(song, "media.conform", {{"media", {slow}}}, openOther);
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		CHECK(finished.at("result").at("media").empty());
		CHECK(finished.at("result").at("changed") == Json::array({slow}));
		CHECK(mediaPath(slow) == utf8(song.library / "slow.wav"));
		CHECK((!fs::exists(song.media) || filesIn(song.media) == 0));
	}
	SECTION("media.prepareArchive")
	{
		const Json finished = whileHeld(song, "media.prepareArchive", Json::object(), openOther);
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		CHECK(finished.at("result").at("copied").empty());
		const Json changed = finished.at("result").at("changed");
		CHECK(std::find(changed.begin(), changed.end(), Json(slow)) != changed.end());
		CHECK(std::find(changed.begin(), changed.end(), Json(kick)) != changed.end());
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
