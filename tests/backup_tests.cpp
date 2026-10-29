// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/jobs/jobs.h"
#include "engine/media/backup.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <random>
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
		REQUIRE(!result.ok());
		return result.error;
	}

	ObjectId idOf(const Json& value, const char* key)
	{
		return value.at(key).at("id").get<ObjectId>();
	}

	std::string utf8(const fs::path& path)
	{
		return path.u8string();
	}

	// Noise, so that every frame differs and a misplaced range shows.
	void writeNoise(const fs::path& path, int64_t frames, const WriteOptions& options, unsigned seed)
	{
		fs::create_directories(path.parent_path());
		const int channels = options.channels;
		const auto writer = AudioFileWriter::create(utf8(path), options);
		std::mt19937 random(seed);
		std::uniform_real_distribution<float> sample(-0.5f, 0.5f);
		std::vector<float> samples(static_cast<size_t>(frames) * static_cast<size_t>(channels));
		for (float& value : samples)
			value = sample(random);
		writer->write(samples.data(), static_cast<size_t>(frames));
		writer->close();
	}

	// 48 kHz noise, Int24 by default, as most recordings.
	void writeNoise(const fs::path& path, int64_t frames, int channels = 2, unsigned seed = 1,
		SampleFormat format = SampleFormat::Int24)
	{
		WriteOptions options;
		options.sampleFormat = format;
		options.sampleRate = 48000.0;
		options.channels = channels;
		writeNoise(path, frames, options, seed);
	}

	std::string bytesOf(const fs::path& path)
	{
		std::ifstream file(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	}

	// Compared here, so that a failure doesn't print binary files.
	bool sameFiles(const fs::path& a, const fs::path& b)
	{
		return bytesOf(a) == bytesOf(b);
	}

	// Frames [start, start + frames) of a file as the engine reads it.
	test::Audio readFrames(const std::string& path, int64_t start, int64_t frames)
	{
		const auto reader = AudioFileReader::open(path);
		test::Audio audio;
		audio.channels = reader->info().channels;
		audio.sampleRate = static_cast<int>(reader->info().sampleRate);
		audio.samples.resize(static_cast<size_t>(frames) * static_cast<size_t>(audio.channels));
		reader->seek(start);
		const size_t read = reader->read(audio.samples.data(), static_cast<size_t>(frames));
		audio.samples.resize(read * static_cast<size_t>(audio.channels));
		return audio;
	}

	int64_t framesOf(const std::string& path)
	{
		return AudioFileReader::open(path)->info().frames;
	}

	std::map<std::string, std::string> readZip(const std::string& path)
	{
		mz_zip_archive zip{};
		REQUIRE(mz_zip_reader_init_file(&zip, path.c_str(), 0) != MZ_FALSE);
		std::map<std::string, std::string> entries;
		for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); ++i)
		{
			char name[512];
			mz_zip_reader_get_filename(&zip, i, name, sizeof name);
			size_t size = 0;
			void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
			entries[name] = std::string(static_cast<const char*>(data), size);
			mz_free(data);
		}
		mz_zip_reader_end(&zip);
		return entries;
	}

	// Waits for the job's job.finished event and returns the job. Progress events go to progress, if given.
	Json waitForJob(Client& client, uint64_t job, std::vector<Json>* progress = nullptr)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
		Json finished; // null until the job finishes
		while (finished.is_null() && std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> message = client.wait(std::chrono::milliseconds(100));
			const auto* event = message ? std::get_if<Event>(&*message) : nullptr;
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

	// Runs a command that starts a job and waits for the job; requires it to succeed and returns its result.
	Json runJob(Client& client, const std::string& command, Json parameters)
	{
		const uint64_t job = run(client, command, std::move(parameters)).at("job").at("id").get<uint64_t>();
		const Json finished = waitForJob(client, job);
		INFO(finished.dump());
		REQUIRE(finished.at("state") == "done");
		CHECK(run(client, "job.get", {{"job", job}}).at("job") == finished);
		return finished.at("result");
	}

	// Starts a backup and waits for it; requires it to succeed and returns its result.
	Json backup(Client& client, Json parameters)
	{
		return runJob(client, "project.backup", std::move(parameters));
	}

	// The result of the command with the given request id, from the messages that came; null if it isn't there.
	// Events go to events, if given.
	Result resultOf(Client& client, RequestId request, std::vector<Event>* events = nullptr)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> message = client.wait(std::chrono::milliseconds(100));
			if (!message)
				continue;
			if (const auto* event = std::get_if<Event>(&*message))
			{
				if (events != nullptr)
					events->push_back(*event);
				continue;
			}
			const auto& result = std::get<Result>(*message);
			if (result.request == request)
				return result;
		}
		return {request, nullptr, "no result"};
	}

	// Plays the first frames of the project through the dummy device and returns both output channels.
	std::vector<std::vector<float>> render(Client& client, int64_t frames)
	{
		run(client, "audio.setDevice", {{"blockSize", 256}, {"inputChannels", 0}, {"captureSeconds", 12}});
		run(client, "transport.locate", {{"position", 0}});
		run(client, "transport.play");
		run(client, "audio.render", {{"frames", frames}});
		run(client, "transport.stop");
		std::vector<std::vector<float>> channels;
		channels.reserve(2);
		for (int channel = 0; channel < 2; ++channel)
			channels.push_back(run(client, "audio.capture", {{"channel", channel}, {"start", 0}, {"frames", frames}})
					.at("samples")
					.get<std::vector<float>>());
		return channels;
	}

	bool inside(const std::string& file, const fs::path& folder)
	{
		const fs::path relative = fs::u8path(file).lexically_normal().lexically_relative(folder.lexically_normal());
		return !relative.empty() && *relative.begin() != fs::path("..");
	}

	std::set<std::string> filesIn(const fs::path& folder)
	{
		std::set<std::string> names;
		if (fs::exists(folder))
			for (const fs::directory_entry& entry : fs::recursive_directory_iterator(folder))
				if (entry.is_regular_file())
					names.insert(entry.path().lexically_relative(folder).generic_u8string());
		return names;
	}

	struct Song
	{
		Engine engine;
		Client client{engine};
		ObjectId track = 0;

		Song() { track = idOf(run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}), "track"); }

		ObjectId addMedia(const fs::path& file)
		{
			return idOf(run(client, "media.add", {{"path", utf8(file)}}), "media");
		}

		ObjectId addClip(ObjectId media) { return idOf(run(client, "clip.add", {{"media", media}}), "clip"); }

		void addEvent(ObjectId clip, int64_t position, int64_t offset, int64_t length)
		{
			run(client, "audioEvent.add",
				{{"track", track}, {"clip", clip}, {"position", position}, {"offset", offset}, {"length", length}});
		}
	};
}

TEST_CASE("a backup opens on a clean machine", "[integration]")
{
	const test::TempDir dir;
	const fs::path elsewhere = dir.path() / "library";
	writeNoise(elsewhere / "drums.wav", 48000, 2, 1);
	writeNoise(elsewhere / "sub" / "bass.wav", 24000, 1, 2);
	writeNoise(elsewhere / "other" / "drums.wav", 12000, 2, 3); // the same name as the first one

	auto owner = std::make_unique<Song>(); // closed before the media files move
	Song& song = *owner;
	run(song.client, "project.set", {{"name", "Song"}});
	const ObjectId drums = song.addClip(song.addMedia(elsewhere / "drums.wav"));
	const ObjectId bass = song.addClip(song.addMedia(elsewhere / "sub" / "bass.wav"));
	const ObjectId other = song.addClip(song.addMedia(elsewhere / "other" / "drums.wav"));
	song.addEvent(drums, 0, 1000, 20000);
	song.addEvent(bass, 15000, 0, 24000);
	song.addEvent(other, 40000, 500, 10000);
	const Json before = run(song.client, "project.get");
	const std::vector<std::vector<float>> played = render(song.client, 60000);

	const fs::path folder = dir.path() / "backup";
	const Json result = backup(song.client, {{"folder", utf8(folder)}, {"name", "Song Backup"}});
	CHECK(result.at("missing").empty());
	const fs::path file = folder / "Song Backup.dawproject";
	CHECK(result.at("file") == utf8(file));
	CHECK(filesIn(folder) ==
		std::set<std::string>{"Song Backup.dawproject", "Audio/drums.wav", "Audio/drums-2.wav", "Audio/bass.wav"});
	// The project stays as it was, with its own media.
	CHECK(run(song.client, "project.get") == before);
	// The project file refers to the media relative to itself.
	const std::string xml = readZip(utf8(file)).at("project.xml");
	CHECK_THAT(xml, ContainsSubstring("path=\"Audio/bass.wav\""));
	CHECK_THAT(xml, !ContainsSubstring(utf8(elsewhere.filename())));

	// Another machine: the folder is somewhere else and the original media are gone.
	owner.reset();
	const fs::path moved = dir.path() / "moved";
	fs::rename(folder, moved);
	fs::remove_all(elsewhere);
	Engine engine;
	Client client(engine);
	run(client, "project.load", {{"path", utf8(moved / "Song Backup.dawproject")}});
	Json after = run(client, "project.get");
	REQUIRE(after.at("media").size() == 3);
	for (Json& media : after.at("media"))
	{
		const std::string path = media.at("path").get<std::string>();
		INFO(path);
		CHECK(inside(path, moved / "Audio"));
		CHECK(fs::exists(fs::u8path(path)));
		media["path"] = nullptr;
	}
	Json expected = before;
	for (Json& media : expected.at("media"))
		media["path"] = nullptr;
	expected["name"] = "Song Backup";
	CHECK(after == expected);
	CHECK(render(client, 60000) == played);
}

TEST_CASE("a backup names its project and file", "[integration]")
{
	const test::TempDir dir;
	writeNoise(dir.path() / "take.wav", 4800);
	Song song;
	run(song.client, "project.set", {{"name", "My Song"}});
	song.addEvent(song.addClip(song.addMedia(dir.path() / "take.wav")), 0, 0, 4800);

	// Without a name, the backup is named after the project.
	const Json result = backup(song.client, {{"folder", utf8(dir.path() / "a")}});
	CHECK(result.at("file") == utf8(dir.path() / "a" / "My Song.dawproject"));

	// Once the project has a file, after that file.
	run(song.client, "project.save", {{"path", utf8(dir.path() / "Session 3.dawproject")}});
	CHECK(backup(song.client, {{"folder", utf8(dir.path() / "b")}}).at("file") ==
		utf8(dir.path() / "b" / "Session 3.dawproject"));

	// A new name is the backup project's name too.
	backup(song.client, {{"folder", utf8(dir.path() / "c")}, {"name", "Final"}});
	Engine engine;
	Client client(engine);
	run(client, "project.load", {{"path", utf8(dir.path() / "c" / "Final.dawproject")}});
	CHECK(run(client, "project.get").at("name") == "Final");
}

TEST_CASE("minimized audio files keep the used ranges with their handles, sample for sample", "[integration]")
{
	const test::TempDir dir;
	const fs::path take = dir.path() / "take.wav";
	writeNoise(take, 480000); // 10 s
	const fs::path loop = dir.path() / "loop.wav";
	writeNoise(loop, 48000, 2, 7, SampleFormat::Float32);

	Song song;
	const ObjectId takeMedia = song.addMedia(take);
	const ObjectId clip = song.addClip(takeMedia);
	const ObjectId loopClip = song.addClip(song.addMedia(loop));
	// Two events whose ranges overlap with their handles, one far away.
	song.addEvent(clip, 0, 48000, 24000);
	song.addEvent(clip, 30000, 72400, 10000);
	song.addEvent(clip, 50000, 400000, 4800);
	// An event that starts at the beginning of the file and one that ends at its end: handles stop there.
	song.addEvent(loopClip, 70000, 0, 1000);
	song.addEvent(loopClip, 80000, 47000, 1000);
	const int64_t frames = 90000;
	const std::vector<std::vector<float>> played = render(song.client, frames);

	const fs::path folder = dir.path() / "backup";
	const Json result = backup(song.client, {{"folder", utf8(folder)}, {"minimize", true}, {"handles", 0.01}});
	CHECK(result.at("missing").empty());

	Engine engine;
	Client client(engine);
	run(client, "project.load", {{"path", result.at("file").get<std::string>()}});
	const Json project = run(client, "project.get");
	const std::string minimized = utf8(folder / "Audio" / "take.wav");
	// 480 handle frames on each side: [47520, 82880) and [399520, 405280).
	CHECK(framesOf(minimized) == (82880 - 47520) + (405280 - 399520));
	CHECK(framesOf(utf8(folder / "Audio" / "loop.wav")) == 1480 + 1480);
	CHECK(AudioFileReader::open(minimized)->info().sampleFormat == SampleFormat::Int24);

	// Every event plays the same frames from the minimized file as from the original.
	const Json& events = project.at("audioEvents");
	const Json original = run(song.client, "project.get").at("audioEvents");
	REQUIRE(events.size() == original.size());
	for (size_t i = 0; i < events.size(); ++i)
	{
		const Json& media = project.at("media")[i < 3 ? 0 : 1];
		const std::string source = utf8(i < 3 ? take : loop);
		const int64_t length = events[i].at("length").get<int64_t>();
		CHECK(length == original[i].at("length").get<int64_t>());
		const int handle = 480;
		const int64_t before = std::min<int64_t>(handle, original[i].at("offset").get<int64_t>());
		const test::AudioDiff diff =
			test::compareAudio(readFrames(media.at("path").get<std::string>(),
								   events[i].at("offset").get<int64_t>() - before, before + length + handle),
				readFrames(source, original[i].at("offset").get<int64_t>() - before, before + length + handle), 0.0f);
		INFO("event " << i << ": " << diff.report);
		CHECK(diff.equal);
	}
	CHECK(render(client, frames) == played);
}

TEST_CASE("a backup can leave out unused media", "[integration]")
{
	const test::TempDir dir;
	writeNoise(dir.path() / "used.wav", 4800, 2, 1);
	writeNoise(dir.path() / "unused clip.wav", 4800, 2, 2);
	writeNoise(dir.path() / "no clip.wav", 4800, 2, 3);
	Song song;
	const ObjectId used = song.addClip(song.addMedia(dir.path() / "used.wav"));
	song.addClip(song.addMedia(dir.path() / "unused clip.wav"));
	song.addMedia(dir.path() / "no clip.wav");
	song.addClip(song.addMedia(dir.path() / "used.wav")); // a second clip of the same file, unused
	song.addEvent(used, 0, 0, 4800);

	// By default every media file goes along.
	backup(song.client, {{"folder", utf8(dir.path() / "all")}});
	CHECK(filesIn(dir.path() / "all" / "Audio").size() == 4);

	const fs::path folder = dir.path() / "used";
	const Json result = backup(song.client, {{"folder", utf8(folder)}, {"removeUnused", true}});
	CHECK(filesIn(folder / "Audio") == std::set<std::string>{"used.wav"});
	Engine engine;
	Client client(engine);
	run(client, "project.load", {{"path", result.at("file").get<std::string>()}});
	const Json project = run(client, "project.get");
	CHECK(project.at("media").size() == 1);
	REQUIRE(project.at("clips").size() == 1);
	CHECK(project.at("clips")[0].at("id") == used);
	CHECK(project.at("audioEvents").size() == 1);
}

TEST_CASE("missing media are reported and the rest is backed up", "[integration]")
{
	const test::TempDir dir;
	writeNoise(dir.path() / "here.wav", 4800);
	Song song;
	const ObjectId here = song.addClip(song.addMedia(dir.path() / "here.wav"));
	const std::string gone = utf8(dir.path() / "gone.wav");
	const ObjectId goneMedia = idOf(
		run(song.client, "media.add", {{"path", gone}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 48000}}),
		"media");
	song.addEvent(here, 0, 0, 4800);
	song.addEvent(song.addClip(goneMedia), 10000, 0, 4800);

	for (const bool minimize : {false, true})
	{
		INFO("minimize " << minimize);
		const fs::path folder = dir.path() / (minimize ? "minimized" : "whole");
		const Json result = backup(song.client, {{"folder", utf8(folder)}, {"minimize", minimize}});
		REQUIRE(result.at("missing").size() == 1);
		CHECK(result.at("missing")[0].at("media") == goneMedia);
		CHECK(result.at("missing")[0].at("path") == gone);
		CHECK(filesIn(folder / "Audio") == std::set<std::string>{"here.wav"});

		// The backup still opens; the missing file keeps its old path, so it can be found later.
		Engine engine;
		Client client(engine);
		run(client, "project.load", {{"path", result.at("file").get<std::string>()}});
		const Json project = run(client, "project.get");
		REQUIRE(project.at("media").size() == 2);
		CHECK(project.at("media")[1].at("path") == gone);
		CHECK(project.at("audioEvents").size() == 2);
	}
}

TEST_CASE("a backup reports its progress and can be cancelled", "[integration][slow]")
{
	const test::TempDir dir;
	const fs::path big = dir.path() / "big.wav";
	writeNoise(big, int64_t{48000} * 60, 2, 1, SampleFormat::Float32); // 23 MB
	Song song;
	for (int i = 0; i < 8; ++i)
		song.addEvent(song.addClip(song.addMedia(big)), 0, 0, 1000);

	// Progress events count up to 1.
	const uint64_t first =
		run(song.client, "project.backup", {{"folder", utf8(dir.path() / "done")}}).at("job").at("id").get<uint64_t>();
	std::vector<Json> progress;
	CHECK(waitForJob(song.client, first, &progress).at("progress") == 1.0);
	REQUIRE(!progress.empty());
	for (size_t i = 1; i < progress.size(); ++i)
		CHECK(progress[i].at("progress") >= progress[i - 1].at("progress"));

	// Cancelled, a backup leaves nothing behind.
	const fs::path folder = dir.path() / "cancelled";
	const uint64_t job =
		run(song.client, "project.backup", {{"folder", utf8(folder)}}).at("job").at("id").get<uint64_t>();
	run(song.client, "job.cancel", {{"job", job}});
	const Json cancelled = waitForJob(song.client, job);
	CHECK(cancelled.at("state") == "cancelled");
	CHECK(!fs::exists(folder));
	CHECK(run(song.client, "job.list").at("jobs").size() == 2);
}

TEST_CASE("a backup goes into an empty folder", "[integration]")
{
	const test::TempDir dir;
	Song song;
	fs::create_directories(dir.path() / "full");
	test::writeWav(dir.path() / "full" / "something.wav", {{0.0f}, 1, 48000});
	CHECK_THAT(
		reject(song.client, "project.backup", {{"folder", utf8(dir.path() / "full")}}), ContainsSubstring("empty"));
	CHECK_THAT(reject(song.client, "project.backup", {{"folder", ""}}), ContainsSubstring("folder"));
	for (const char* name : {"", "a/b", "a\\b", "..", "x:y"})
	{
		INFO(name);
		CHECK_THAT(reject(song.client, "project.backup", {{"folder", utf8(dir.path() / "new")}, {"name", name}}),
			ContainsSubstring("name"));
	}
	CHECK_THAT(reject(song.client, "project.backup", {{"folder", utf8(dir.path() / "new")}, {"handles", -1.0}}),
		ContainsSubstring("handles"));
	CHECK_THAT(reject(song.client, "job.cancel", {{"job", 999}}), ContainsSubstring("999"));

	// An existing empty folder is fine.
	fs::create_directories(dir.path() / "empty");
	backup(song.client, {{"folder", utf8(dir.path() / "empty")}});
	CHECK(fs::exists(dir.path() / "empty" / "Untitled.dawproject"));
}

TEST_CASE("media can be copied into the project folder when they are added", "[integration]")
{
	const test::TempDir dir;
	const fs::path elsewhere = dir.path() / "library";
	writeNoise(elsewhere / "take.wav", 4800);
	Song song;
	CHECK_THAT(reject(song.client, "media.add", {{"path", utf8(elsewhere / "take.wav")}, {"copy", true}}),
		ContainsSubstring("save"));

	const fs::path project = dir.path() / "project";
	fs::create_directories(project);
	const std::string file = utf8(project / "Song.dawproject");
	run(song.client, "project.save", {{"path", file}, {"embedMedia", false}});
	CHECK(run(song.client, "project.location") ==
		Json{{"file", file}, {"folder", utf8(project)}, {"mediaFolder", utf8(project / "Audio")}});
	// The file is added at once where it is, and copied by a job, which then points the media file at the copy.
	const Json added = run(song.client, "media.add", {{"path", utf8(elsewhere / "take.wav")}, {"copy", true}});
	const ObjectId media = idOf(added, "media");
	CHECK(added.at("media").at("path") == utf8(elsewhere / "take.wav"));
	CHECK(added.at("media").at("length") == 4800);
	REQUIRE(added.at("job").is_object());
	CHECK(added.at("job").at("kind") == "media.add");
	const Json copied = waitForJob(song.client, added.at("job").at("id").get<uint64_t>());
	REQUIRE(copied.at("state") == "done");
	const std::string copy = utf8(project / "Audio" / "take.wav");
	CHECK(copied.at("result") ==
		Json{{"copied", Json::array({{{"media", media}, {"from", utf8(elsewhere / "take.wav")}, {"to", copy}}})},
			{"missing", Json::array()}, {"changed", Json::array()}});
	CHECK(run(song.client, "project.get").at("media")[0].at("path") == copy);
	CHECK(sameFiles(project / "Audio" / "take.wav", elsewhere / "take.wav"));
	// Pointing it at the copy is an undo step of its own.
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Copy media file into the project");
	run(song.client, "history.undo");
	CHECK(run(song.client, "project.get").at("media")[0].at("path") == utf8(elsewhere / "take.wav"));
	run(song.client, "history.redo");

	// A second copy gets a name of its own; a file already in the project folder stays where it is, without a job.
	const Json second = run(song.client, "media.add", {{"path", utf8(elsewhere / "take.wav")}, {"copy", true}});
	CHECK(waitForJob(song.client, second.at("job").at("id").get<uint64_t>()).at("result").at("copied")[0].at("to") ==
		utf8(project / "Audio" / "take-2.wav"));
	const Json inside = run(song.client, "media.add", {{"path", copy}, {"copy", true}});
	CHECK(inside.at("media").at("path") == copy);
	CHECK(inside.at("job").is_null());
	CHECK(run(song.client, "media.add", {{"path", copy}}).at("job").is_null());
	// A file that isn't there or isn't audio is refused and not copied.
	CHECK_THAT(reject(song.client, "media.add",
				   {{"path", utf8(elsewhere / "gone.wav")}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 10},
					   {"copy", true}}),
		ContainsSubstring("gone.wav"));
	test::writeWav(elsewhere / "fine.wav", {{0.0f}, 1, 48000});
	fs::copy_file(elsewhere / "fine.wav", elsewhere / "broken.wav");
	fs::resize_file(elsewhere / "broken.wav", 10);
	reject(song.client, "media.add", {{"path", utf8(elsewhere / "broken.wav")}, {"copy", true}});
	CHECK(!fs::exists(project / "Audio" / "broken.wav"));

	run(song.client, "project.new");
	CHECK(
		run(song.client, "project.location") == Json{{"file", nullptr}, {"folder", nullptr}, {"mediaFolder", nullptr}});
}

TEST_CASE("preparing an archive gathers the media in the project folder", "[integration]")
{
	const test::TempDir dir;
	const fs::path elsewhere = dir.path() / "library";
	writeNoise(elsewhere / "a.wav", 4800, 2, 1);
	writeNoise(elsewhere / "b.wav", 4800, 2, 2);
	const fs::path project = dir.path() / "project";
	writeNoise(project / "Audio" / "inside.wav", 4800, 2, 3);
	auto owner = std::make_unique<Song>(); // closed before the media files move
	Song& song = *owner;
	CHECK_THAT(reject(song.client, "media.prepareArchive", Json::object()), ContainsSubstring("save"));

	const ObjectId a = song.addMedia(elsewhere / "a.wav");
	const ObjectId b = song.addMedia(elsewhere / "b.wav");
	const ObjectId in = song.addMedia(project / "Audio" / "inside.wav");
	const std::string gone = utf8(elsewhere / "gone.wav");
	const ObjectId missing =
		idOf(run(song.client, "media.add", {{"path", gone}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 10}}),
			"media");
	song.addEvent(song.addClip(a), 0, 0, 4800);
	song.addEvent(song.addClip(b), 4800, 0, 4800);
	song.addEvent(song.addClip(in), 9600, 0, 4800);
	const std::string file = utf8(project / "Song.dawproject");
	run(song.client, "project.save", {{"path", file}, {"embedMedia", false}});
	const std::vector<std::vector<float>> played = render(song.client, 15000);

	const Json result = runJob(song.client, "media.prepareArchive", Json::object());
	REQUIRE(result.at("copied").size() == 2);
	CHECK(result.at("changed").empty());
	CHECK(result.at("copied")[0] ==
		Json{{"media", a}, {"from", utf8(elsewhere / "a.wav")}, {"to", utf8(project / "Audio" / "a.wav")}});
	CHECK(result.at("missing") == Json::array({{{"media", missing}, {"path", gone}}}));
	Json media = run(song.client, "project.get").at("media");
	CHECK(media[0].at("path") == utf8(project / "Audio" / "a.wav"));
	CHECK(media[1].at("path") == utf8(project / "Audio" / "b.wav"));
	CHECK(media[2].at("path") == utf8(project / "Audio" / "inside.wav"));
	CHECK(media[3].at("path") == gone);

	// One undo step, which puts the paths back.
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Prepare archive");
	run(song.client, "history.undo");
	CHECK(run(song.client, "project.get").at("media")[0].at("path") == utf8(elsewhere / "a.wav"));
	run(song.client, "history.redo");

	// Saved again, the project folder can move.
	run(song.client, "project.save", {{"path", file}, {"embedMedia", false}});
	owner.reset();
	const fs::path moved = dir.path() / "moved";
	fs::rename(project, moved);
	fs::remove_all(elsewhere);
	Engine engine;
	Client client(engine);
	run(client, "project.load", {{"path", utf8(moved / "Song.dawproject")}});
	media = run(client, "project.get").at("media");
	CHECK(media[0].at("path") == utf8(moved / "Audio" / "a.wav"));
	CHECK(media[2].at("path") == utf8(moved / "Audio" / "inside.wav"));
	CHECK(render(client, 15000) == played);
}

TEST_CASE("a job announces job.started before its progress and its end", "[integration]")
{
	const test::TempDir dir;
	writeNoise(dir.path() / "take.wav", 480);
	Song song;
	song.addEvent(song.addClip(song.addMedia(dir.path() / "take.wav")), 0, 0, 480);

	// A small backup can end before the command that started it returns; job.started still comes first.
	for (int i = 0; i < 20; ++i)
	{
		const RequestId request =
			song.client.send("project.backup", {{"folder", utf8(dir.path() / ("backup " + std::to_string(i)))}});
		std::vector<Message> messages;
		uint64_t job = 0;
		bool finished = false;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!finished && std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> message = song.client.wait(std::chrono::milliseconds(100));
			if (!message)
				continue;
			messages.push_back(*message);
			if (const auto* result = std::get_if<Result>(&*message); result != nullptr && result->request == request)
			{
				REQUIRE(result->ok());
				job = result->value.at("job").at("id").get<uint64_t>();
			}
			for (const Message& earlier : messages)
				if (const auto* event = std::get_if<Event>(&earlier); job != 0 && event != nullptr &&
					event->name == "job.finished" && event->data.at("job").at("id") == job)
					finished = true;
		}
		REQUIRE(finished);

		std::vector<std::string> names;
		for (const Message& message : messages)
		{
			const auto* event = std::get_if<Event>(&message);
			if (event == nullptr || event->name.rfind("job.", 0) != 0)
				continue;
			const Json& data = event->data;
			const Json id = event->name == "job.progress" ? data.at("job") : data.at("job").at("id");
			if (id == job)
				names.push_back(event->name);
			if (event->name != "job.progress")
				CHECK(validate(data.at("job"), Jobs::schema(), "job").empty());
		}
		REQUIRE(!names.empty());
		CHECK(names.front() == "job.started");
		CHECK(names.back() == "job.finished");
	}
}

TEST_CASE("a backup is opened when it's done only if the project didn't change meanwhile", "[integration]")
{
	const test::TempDir dir;
	writeNoise(dir.path() / "take.wav", 480000); // 10 s, so the backup takes a moment
	Song song;
	run(song.client, "project.set", {{"name", "Song"}});
	song.addEvent(song.addClip(song.addMedia(dir.path() / "take.wav")), 0, 0, 4800);

	// Without activate, the project stays open.
	const Json kept = backup(song.client, {{"folder", utf8(dir.path() / "kept")}});
	CHECK(kept.at("activated") == false);
	CHECK(kept.at("activationError").is_null());
	CHECK(run(song.client, "project.location").at("file").is_null());

	// Unchanged, the backup takes the project's place.
	const Json opened = backup(song.client, {{"folder", utf8(dir.path() / "opened")}, {"activate", true}});
	CHECK(opened.at("activated") == true);
	CHECK(opened.at("activationError").is_null());
	CHECK(run(song.client, "project.location").at("file") == opened.at("file"));
	CHECK(run(song.client, "history.get").at("steps").empty());

	// An edit while the backup runs stays, with its undo history; the result says why the backup wasn't opened.
	const uint64_t revision = run(song.client, "project.revision").at("revision").get<uint64_t>();
	const RequestId request =
		song.client.send("project.backup", {{"folder", utf8(dir.path() / "edited")}, {"activate", true}});
	run(song.client, "project.set", {{"name", "Edited"}}); // queued before the job can ask to open the backup
	const Result started = resultOf(song.client, request);
	REQUIRE(started.ok());
	const Json finished = waitForJob(song.client, started.value.at("job").at("id").get<uint64_t>());
	REQUIRE(finished.at("state") == "done");
	CHECK(finished.at("result").at("activated") == false);
	CHECK_THAT(finished.at("result").at("activationError").get<std::string>(), ContainsSubstring("changed"));
	CHECK(fs::exists(fs::u8path(finished.at("result").at("file").get<std::string>())));
	CHECK(run(song.client, "project.get").at("name") == "Edited");
	CHECK(run(song.client, "project.location").at("file") == opened.at("file"));
	CHECK(run(song.client, "history.get").at("steps").size() == 1);
	CHECK(run(song.client, "project.revision").at("revision").get<uint64_t>() > revision);

	// project.load with an old revision fails and changes nothing.
	CHECK_THAT(reject(song.client, "project.load", {{"path", opened.at("file")}, {"revision", revision}}),
		ContainsSubstring("changed"));
	CHECK(run(song.client, "project.get").at("name") == "Edited");
}

TEST_CASE("a folder that a running job writes into takes no other backup", "[integration][slow]")
{
	const test::TempDir dir;
	const fs::path big = dir.path() / "big.wav";
	writeNoise(big, int64_t{48000} * 30, 2, 1, SampleFormat::Float32); // 11.5 MB
	Song song;
	for (int i = 0; i < 8; ++i)
		song.addEvent(song.addClip(song.addMedia(big)), 0, 0, 1000);

	const fs::path folder = dir.path() / "backup";
	const RequestId first = song.client.send("project.backup", {{"folder", utf8(folder)}});
	// Queued right behind it, so they run while it copies.
	const RequestId same = song.client.send("project.backup", {{"folder", utf8(folder)}});
	const RequestId inside = song.client.send("project.backup", {{"folder", utf8(folder / "inner")}});
	const RequestId around = song.client.send("project.backup", {{"folder", utf8(dir.path())}});
	const Result started = resultOf(song.client, first);
	REQUIRE(started.ok());
	for (const RequestId request : {same, inside})
		CHECK_THAT(resultOf(song.client, request).error, ContainsSubstring("running job"));
	CHECK_THAT(resultOf(song.client, around).error, ContainsSubstring("running job"));

	const uint64_t job = started.value.at("job").at("id").get<uint64_t>();
	run(song.client, "job.cancel", {{"job", job}});
	CHECK(waitForJob(song.client, job).at("state") == "cancelled");
	CHECK(!fs::exists(folder));
	// Once the job has ended, only the usual checks apply.
	CHECK_THAT(reject(song.client, "project.backup", {{"folder", utf8(dir.path())}}), ContainsSubstring("empty"));
}

TEST_CASE("files made at the same time get names of their own", "[integration]")
{
	const test::TempDir dir;
	const fs::path folder = dir.path() / "folder";
	fs::create_directories(folder);
	std::ofstream(folder / "a.wav") << "there";
	CHECK(createFreeFile(folder, "a.wav") == folder / "a-2.wav");
	CHECK(createFreeFile(folder, "a.wav") == folder / "a-3.wav");
	CHECK(createFreeFile(folder, "b.wav") == folder / "b.wav");
	CHECK(bytesOf(folder / "a.wav") == "there");
	CHECK(bytesOf(folder / "a-2.wav").empty());

	// Two copies of files with the same name, made by jobs that run at once.
	writeNoise(dir.path() / "one" / "take.wav", 480000, 2, 1);
	writeNoise(dir.path() / "two" / "take.wav", 480000, 2, 2);
	Song song;
	const fs::path project = dir.path() / "project";
	fs::create_directories(project);
	run(song.client, "project.save", {{"path", utf8(project / "Song.dawproject")}, {"embedMedia", false}});
	std::vector<uint64_t> jobs;
	for (const char* from : {"one", "two"})
		jobs.push_back(run(song.client, "media.add", {{"path", utf8(dir.path() / from / "take.wav")}, {"copy", true}})
				.at("job")
				.at("id")
				.get<uint64_t>());
	std::set<std::string> copies;
	for (const uint64_t job : jobs)
	{
		const Json finished = waitForJob(song.client, job);
		REQUIRE(finished.at("state") == "done");
		const Json& copy = finished.at("result").at("copied").at(0);
		copies.insert(copy.at("to").get<std::string>());
		CHECK(sameFiles(fs::u8path(copy.at("to").get<std::string>()), fs::u8path(copy.at("from").get<std::string>())));
	}
	CHECK(
		copies == std::set<std::string>{utf8(project / "Audio" / "take.wav"), utf8(project / "Audio" / "take-2.wav")});
}

TEST_CASE("a copy whose media file changed meanwhile isn't used", "[integration]")
{
	const test::TempDir dir;
	writeNoise(dir.path() / "library" / "take.wav", 480000);
	writeNoise(dir.path() / "library" / "other.wav", 4800, 2, 2);
	Song song;
	const fs::path project = dir.path() / "project";
	fs::create_directories(project);
	run(song.client, "project.save", {{"path", utf8(project / "Song.dawproject")}, {"embedMedia", false}});
	const ObjectId media = song.addMedia(dir.path() / "library" / "take.wav");

	// Relinked by hand while the job copies: the hand-made link stays, and the copy goes.
	const RequestId request = song.client.send("media.prepareArchive");
	run(song.client, "media.relink",
		{{"files", Json::array({{{"media", media}, {"path", utf8(dir.path() / "library" / "other.wav")}}})}});
	const Result started = resultOf(song.client, request);
	REQUIRE(started.ok());
	const Json finished = waitForJob(song.client, started.value.at("job").at("id").get<uint64_t>());
	REQUIRE(finished.at("state") == "done");
	CHECK(finished.at("result").at("copied").empty());
	CHECK(finished.at("result").at("changed") == Json::array({media}));
	CHECK(run(song.client, "project.get").at("media")[0].at("path") == utf8(dir.path() / "library" / "other.wav"));
	CHECK(filesIn(project / "Audio").empty());
}

TEST_CASE("media files can be pointed at other files", "[integration]")
{
	const test::TempDir dir;
	writeNoise(dir.path() / "a.wav", 4800);
	fs::copy_file(dir.path() / "a.wav", dir.path() / "b.wav");
	WriteOptions other;
	other.sampleRate = 44100.0;
	writeNoise(dir.path() / "44k.wav", 4800, other, 3);
	Song song;
	const ObjectId media = song.addMedia(dir.path() / "a.wav");

	const Json relinked = run(song.client, "media.relink",
		{{"files", Json::array({{{"media", media}, {"path", utf8(dir.path() / "b.wav")}}})}});
	CHECK(relinked == Json{{"relinked", Json::array({media})}, {"skipped", Json::array()}});
	CHECK(run(song.client, "project.get").at("media")[0].at("path") == utf8(dir.path() / "b.wav"));
	CHECK(run(song.client, "history.get").at("steps").back().at("action") == "Relink media files");

	// With from, only if the media file still points there.
	CHECK(run(song.client, "media.relink",
			  {{"files",
				  Json::array(
					  {{{"media", media}, {"from", utf8(dir.path() / "a.wav")}, {"path", utf8(dir.path() / "a.wav")}},
						  {{"media", 999}, {"from", "x"}, {"path", "y"}}})}}) ==
		Json{{"relinked", Json::array()}, {"skipped", Json::array({media, 999})}});
	CHECK_THAT(reject(song.client, "media.relink",
				   {{"files", Json::array({{{"media", media}, {"path", utf8(dir.path() / "44k.wav")}}})}}),
		ContainsSubstring("44100 Hz"));
	CHECK_THAT(reject(song.client, "media.relink", {{"files", Json::array({{{"media", 999}, {"path", "y"}}})}}),
		ContainsSubstring("999"));
	CHECK(run(song.client, "project.get").at("media")[0].at("path") == utf8(dir.path() / "b.wav"));
}

TEST_CASE("a save that fails leaves no file behind", "[integration]")
{
	const test::TempDir dir;
	Song song;
	run(song.client, "media.add",
		{{"path", utf8(dir.path() / "gone.wav")}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 10}});
	// Embedding a media file that isn't there fails after the file was begun.
	const fs::path file = dir.path() / "Song.dawproject";
	reject(song.client, "project.save", {{"path", utf8(file)}});
	CHECK(filesIn(dir.path()).empty());
}

TEST_CASE("minimized files at another sample rate play sample for sample the same", "[integration]")
{
	const test::TempDir dir;
	const fs::path take = dir.path() / "take.wav";
	WriteOptions options;
	options.sampleRate = 44100.0;
	writeNoise(take, 441000, options, 5); // 10 s at 44.1 kHz, in a 48 kHz project
	Song song;
	REQUIRE(run(song.client, "project.get").at("sampleRate") == 48000.0);
	const ObjectId clip = song.addClip(song.addMedia(take));
	// Ranges that don't start on the resampling grid (147 frames of 44.1 kHz are 160 of 48 kHz) unless moved there.
	song.addEvent(clip, 0, 50000, 20000);
	song.addEvent(clip, 30000, 200000, 20000);
	const int64_t frames = 60000;
	const std::vector<std::vector<float>> played = render(song.client, frames);

	const Json result = backup(song.client, {{"folder", utf8(dir.path() / "backup")}, {"minimize", true}});
	Engine engine;
	Client client(engine);
	run(client, "project.load", {{"path", result.at("file").get<std::string>()}});
	const Json events = run(client, "project.get").at("audioEvents");
	REQUIRE(events.size() == 2);
	CHECK((50000 - events[0].at("offset").get<int64_t>()) % 147 == 0);
	CHECK((200000 - events[1].at("offset").get<int64_t>()) % 147 == 0);
	CHECK(framesOf(utf8(dir.path() / "backup" / "Audio" / "take.wav")) < 441000);
	CHECK(render(client, frames) == played);
}

TEST_CASE("minimizing copies 32-bit integer and Broadcast Wave files whole", "[integration]")
{
	const test::TempDir dir;
	writeNoise(dir.path() / "int32.wav", 96000, 2, 1, SampleFormat::Int32);
	WriteOptions broadcast;
	broadcast.broadcast = BroadcastInfo{};
	broadcast.broadcast->description = "Take 1";
	broadcast.broadcast->timeReference = uint64_t{48000} * 3600;
	writeNoise(dir.path() / "bwf.wav", 96000, broadcast, 2);
	writeNoise(dir.path() / "plain.wav", 96000, 2, 3);
	Song song;
	for (const char* name : {"int32.wav", "bwf.wav", "plain.wav"})
		song.addEvent(song.addClip(song.addMedia(dir.path() / name)), 0, 50000, 1000);

	const fs::path folder = dir.path() / "backup";
	const Json result = backup(song.client, {{"folder", utf8(folder)}, {"minimize", true}, {"handles", 0.01}});
	CHECK(sameFiles(folder / "Audio" / "int32.wav", dir.path() / "int32.wav"));
	CHECK(sameFiles(folder / "Audio" / "bwf.wav", dir.path() / "bwf.wav"));
	CHECK(framesOf(utf8(folder / "Audio" / "plain.wav")) == 1960);
	Engine engine;
	Client client(engine);
	run(client, "project.load", {{"path", result.at("file").get<std::string>()}});
	const Json events = run(client, "project.get").at("audioEvents");
	REQUIRE(events.size() == 3);
	CHECK(events[0].at("offset") == 50000);
	CHECK(events[1].at("offset") == 50000);
	CHECK(events[2].at("offset") == 480);
}
