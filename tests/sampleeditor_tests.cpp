// SPDX-License-Identifier: GPL-3.0-or-later
// The sample editor's engine side (#140, docs/features/sample-editor.md): the sampleEditor.* view with its selection
// range, and the regions of clips. tests/sampleeditor_ui_tests.cpp drives the editor in the app.
#include "engine/engine.h"
#include "testutils/heldjob.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <filesystem>
#include <iterator>
#include <map>
#include <string>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;
using gnubase::test::readZip;
using gnubase::test::writeZip;

namespace
{
	constexpr int64_t g_second = 48000;

	struct Fixture
	{
		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			const engine::Result result = client.call(command, parameters);
			INFO(command << ": " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, const Json& parameters)
		{
			const engine::Result result = client.call(command, parameters);
			REQUIRE(!result.ok());
			return result.error;
		}

		// A linear audio track with an event at one second playing frames 1000 to 5000 of a 9600-frame file.
		Fixture(const std::string& path = "take.wav", int channels = 1)
		{
			track = call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<int64_t>();
			const Json media =
				call("media.add", {{"path", path}, {"sampleRate", 48000}, {"channels", channels}, {"length", 9600}})
					.at("media");
			clip = call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
			event = call("audioEvent.add",
				{{"track", track}, {"clip", clip}, {"position", g_second}, {"offset", 1000}, {"length", 4000}})
						.at("event")
						.at("id")
						.get<int64_t>();
		}

		Json regions()
		{
			const Json project = call("project.get");
			for (const Json& item : project.at("clips"))
				if (item.at("id") == clip)
					return item.at("regions");
			FAIL("no clip " << clip);
		}

		engine::Engine engine;
		engine::Client client{engine};
		int64_t track = 0;
		int64_t clip = 0;
		int64_t event = 0;
	};

}

TEST_CASE("the sample editor shows an audio event and its selection range as view state", "[unit]")
{
	Fixture f;
	const size_t steps = f.call("history.get").at("steps").size();
	Json view = f.call("sampleEditor.get");
	CHECK(view.at("open") == false);
	CHECK(view.at("event") == 0);
	CHECK_THAT(f.fails("sampleEditor.set", {{"select", "all"}}), ContainsSubstring("isn't open"));
	CHECK_THAT(f.fails("sampleEditor.open", {{"event", 999}}), ContainsSubstring("no audio event 999"));

	view = f.call("sampleEditor.open", {{"event", f.event}});
	CHECK(view.at("open") == true);
	CHECK(view.at("event") == f.event);
	CHECK(view.at("clip") == f.clip);
	CHECK(view.at("track") == f.track);
	CHECK(view.at("length") == 9600);
	CHECK(view.at("offset") == 1000);
	CHECK(view.at("eventLength") == 4000);
	CHECK(view.at("clipStart") == g_second - 1000); // frame f plays at clipStart + f * ratio
	CHECK(view.at("ratio") == 1.0);

	// The Range section's Select menu (p. 618), and a range of its own.
	view = f.call("sampleEditor.set", {{"select", "all"}});
	CHECK(view.at("selectionStart") == 0);
	CHECK(view.at("selectionEnd") == 9600);
	view = f.call("sampleEditor.set", {{"select", "event"}});
	CHECK(view.at("selectionStart") == 1000);
	CHECK(view.at("selectionEnd") == 5000);
	f.call("transport.set", {{"left", g_second + 500}, {"right", 3 * g_second}});
	view = f.call("sampleEditor.set", {{"select", "loop"}});
	CHECK(view.at("selectionStart") == 1500);
	CHECK(view.at("selectionEnd") == 9600); // the right locator is beyond the clip
	view = f.call("sampleEditor.set", {{"select", "none"}});
	CHECK(view.at("selectionStart") == view.at("selectionEnd"));
	view = f.call("sampleEditor.set", {{"selectionStart", 200}, {"selectionEnd", 300}});
	CHECK(view.at("selectionStart") == 200);
	CHECK(view.at("selectionEnd") == 300);
	view = f.call("sampleEditor.set", {{"selectionEnd", 700}});
	CHECK(view.at("selectionStart") == 200);
	CHECK(view.at("selectionEnd") == 700);
	CHECK_THAT(f.fails("sampleEditor.set", {{"selectionStart", 800}}), ContainsSubstring("start before it ends"));
	CHECK_THAT(f.fails("sampleEditor.set", {{"selectionEnd", 9601}}), ContainsSubstring("0 to 9600"));

	CHECK(f.call("history.get").at("steps").size() == steps); // no undo steps

	// An event that goes leaves the editor empty; undoing brings it back.
	f.call("audioEvent.remove", {{"event", f.event}});
	view = f.call("sampleEditor.get");
	CHECK(view.at("open") == true);
	CHECK(view.at("event") == 0);
	CHECK_THAT(f.fails("sampleEditor.set", {{"select", "all"}}), ContainsSubstring("shows no audio event"));
	f.call("history.undo");
	CHECK(f.call("sampleEditor.get").at("event") == f.event);

	// Opening the same event again keeps the selection, another event drops it; closing and a new project close it.
	f.call("sampleEditor.set", {{"select", "all"}});
	view = f.call("sampleEditor.open", {{"event", f.event}});
	CHECK(view.at("selectionEnd") == 9600);
	const Json other = f.call("audioEvent.add",
		{{"track", f.track}, {"clip", f.clip}, {"position", 3 * g_second}, {"offset", 0}, {"length", 2000}});
	view = f.call("sampleEditor.open", {{"event", other.at("event").at("id")}});
	CHECK(view.at("event") == other.at("event").at("id"));
	CHECK(view.at("selectionStart") == 0);
	CHECK(view.at("selectionEnd") == 0);
	view = f.call("sampleEditor.close");
	CHECK(view.at("open") == false);
	CHECK(view.at("event") == 0);
	f.call("sampleEditor.open", {{"event", f.event}});
	f.call("project.new");
	CHECK(f.call("sampleEditor.get").at("open") == false);
}

TEST_CASE("the sample editor's selection range snaps to zero crossings", "[unit]")
{
	// A sine that crosses zero every 100 frames.
	const test::TempDir dir;
	const std::filesystem::path file = dir.path() / "sine.wav";
	test::Audio sine;
	for (int frame = 0; frame < 9600; ++frame)
		sine.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 3.14159265358979323846 / 100.0)));
	test::writeWav(file, sine);

	Fixture f(file.u8string());
	f.call("sampleEditor.open", {{"event", f.event}});
	Json view = f.call("sampleEditor.set", {{"selectionStart", 1230}, {"selectionEnd", 2860}, {"zeroCrossing", true}});
	CHECK(view.at("selectionStart") == 1200);
	CHECK(view.at("selectionEnd") == 2900);
	view = f.call("sampleEditor.set", {{"selectionStart", 1230}, {"selectionEnd", 2860}});
	CHECK(view.at("selectionStart") == 1230);
}

TEST_CASE("clips have regions, which are undo steps and saved with the project", "[unit]")
{
	Fixture f;
	Json added = f.call("clip.addRegion", {{"clip", f.clip}, {"start", 3000}, {"end", 4000}});
	CHECK(added.at("clip") == f.clip);
	CHECK(added.at("region").at("name") == "Region 1");
	const auto second = added.at("region").at("id").get<int64_t>();
	added = f.call("clip.addRegion", {{"clip", f.clip}, {"start", 100}, {"end", 900}, {"name", "Intro"}});
	const auto first = added.at("region").at("id").get<int64_t>();
	Json regions = f.regions();
	REQUIRE(regions.size() == 2);
	CHECK(regions.at(0).at("id") == first); // by start
	CHECK(regions.at(1).at("id") == second);
	CHECK(f.call("history.get").at("steps").back().at("action") == "Add region");

	CHECK_THAT(f.fails("clip.addRegion", {{"clip", f.clip}, {"start", 500}, {"end", 500}}),
		ContainsSubstring("start before it ends"));
	CHECK_THAT(
		f.fails("clip.addRegion", {{"clip", f.clip}, {"start", 500}, {"end", 9601}}), ContainsSubstring("0 to 9600"));
	CHECK_THAT(f.fails("clip.addRegion", {{"clip", 999}, {"start", 0}, {"end", 1}}), ContainsSubstring("no clip 999"));
	CHECK_THAT(f.fails("clip.setRegion", {{"clip", f.clip}, {"region", 999}}), ContainsSubstring("has no region 999"));

	const Json changed =
		f.call("clip.setRegion", {{"clip", f.clip}, {"region", second}, {"start", 0}, {"name", "Whole"}});
	CHECK(changed.at("region").at("start") == 0);
	CHECK(changed.at("region").at("end") == 4000);
	CHECK(f.regions().at(0).at("name") == "Whole");
	CHECK_THAT(f.fails("clip.setRegion", {{"clip", f.clip}, {"region", second}, {"start", 4000}}),
		ContainsSubstring("start before it ends"));

	f.call("clip.removeRegion", {{"clip", f.clip}, {"region", first}});
	CHECK(f.regions().size() == 1);
	f.call("history.undo");
	CHECK(f.regions().size() == 2);
	f.call("history.undo");
	CHECK(f.regions().at(1).at("name") == "Region 1");
	CHECK(f.regions().at(1).at("start") == 3000);

	// A new region's name is Region and a number no other region of the clip has: after a removal, the count of
	// regions would name a second Region 2.
	const Json third = f.call("clip.addRegion", {{"clip", f.clip}, {"start", 5000}, {"end", 6000}}).at("region");
	CHECK(third.at("name") == "Region 3");
	f.call("clip.removeRegion", {{"clip", f.clip}, {"region", first}});
	CHECK(f.call("clip.addRegion", {{"clip", f.clip}, {"start", 7000}, {"end", 8000}}).at("region").at("name") ==
		"Region 4");
	f.call("history.undo");
	f.call("history.undo");
	f.call("history.undo");
	REQUIRE(f.regions().size() == 2);

	// Saved in GNUbase's entry of the project file.
	const test::TempDir dir;
	const std::string file = (dir.path() / "regions.dawproject").u8string();
	const Json before = f.regions();
	f.call("project.save", {{"path", file}, {"embedMedia", false}});
	f.call("project.new");
	f.call("project.load", {{"path", file}});
	CHECK(f.call("project.get").at("clips").at(0).at("regions") == before);
}

TEST_CASE("regions that aren't sections of their clip are left out when a project loads", "[unit]")
{
	Fixture f;
	f.call("clip.addRegion", {{"clip", f.clip}, {"start", 3000}, {"end", 4000}, {"name", "Kept"}});
	const test::TempDir dir;
	const std::string file = (dir.path() / "regions.dawproject").u8string();
	f.call("project.save", {{"path", file}, {"embedMedia", false}});

	// A hand-edited file: regions past the clip's end, empty, before its start and out of order.
	std::map<std::string, std::string> entries = readZip(file);
	Json gnubase = Json::parse(entries.at("gnubase.json"));
	Json& regions = gnubase.at("project").at("clips").at(0).at("regions");
	regions.push_back({{"id", 90001}, {"name", "Past the end"}, {"start", 9000}, {"end", 9601}});
	regions.push_back({{"id", 90002}, {"name", "Empty"}, {"start", 500}, {"end", 500}});
	regions.push_back({{"id", 90003}, {"name", "Before the start"}, {"start", -1}, {"end", 10}});
	regions.push_back({{"id", 90004}, {"name", "To the end"}, {"start", 100}, {"end", 9600}});
	entries["gnubase.json"] = gnubase.dump();
	const std::string edited = (dir.path() / "edited.dawproject").u8string();
	writeZip(edited, entries);

	f.call("project.load", {{"path", edited}});
	const Json loaded = f.call("project.get").at("clips").at(0).at("regions");
	REQUIRE(loaded.size() == 2);
	CHECK(loaded.at(0).at("name") == "To the end"); // by start
	CHECK(loaded.at(1).at("name") == "Kept");
}

namespace
{
	Json process(Fixture& f, const std::string& operation, int64_t start, int64_t end,
		const std::string& sharedCopies = "newVersion")
	{
		const Json job = f.call("sampleEditor.process",
							  {{"event", f.event}, {"operation", operation}, {"start", start}, {"end", end},
								  {"sharedCopies", sharedCopies}})
							 .at("job");
		const Json done = test::waitForJob(f.client, job.at("id").get<uint64_t>());
		INFO(done.dump());
		REQUIRE(done.at("state") == "done");
		return done.at("result");
	}

	Json objectWithId(const Json& objects, const Json& id)
	{
		for (const Json& object : objects)
			if (object.at("id") == id)
				return object;
		FAIL("missing object " << id);
	}

	test::Audio processAudio()
	{
		test::Audio audio;
		for (int i = 0; i < 9600; ++i)
			audio.samples.push_back(static_cast<float>(i) / 10000.0F);
		return audio;
	}

	test::Audio eventAudio(Fixture& f)
	{
		const Json project = f.call("project.get");
		const Json event = objectWithId(project.at("audioEvents"), f.event);
		const Json clip = objectWithId(project.at("clips"), event.at("clip"));
		return test::readWav(objectWithId(project.at("media"), clip.at("media")).at("path").get<std::string>());
	}
}

TEST_CASE("sample editor Process edits samples and lengths with one undo step", "[integration]")
{
	for (const std::string operation : {"cut", "delete", "insertSilence", "paste"})
	{
		CAPTURE(operation);
		const test::TempDir dir;
		const auto path = dir.path() / "source.wav";
		test::Audio audio = processAudio();
		test::writeWav(path, audio);
		Fixture f(path.u8string());
		f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
		if (operation == "paste")
			process(f, "copy", 7000, 7300);
		const Json before = f.call("project.get");
		const size_t steps = f.call("history.get").at("steps").size();
		process(f, operation, 2000, 3000);
		const Json after = f.call("project.get");
		CHECK(f.call("history.get").at("steps").size() == steps + 1);
		test::Audio expected = audio;
		if (operation == "insertSilence")
		{
			expected.samples.insert(expected.samples.begin() + 2000, 1000, 0.0F);
		}
		else
		{
			expected.samples.erase(expected.samples.begin() + 2000, expected.samples.begin() + 3000);
			if (operation == "paste")
				expected.samples.insert(
					expected.samples.begin() + 2000, audio.samples.begin() + 7000, audio.samples.begin() + 7300);
		}
		CHECK(test::compareAudio(eventAudio(f), expected, 0.0F).equal);
		const Json event = objectWithId(after.at("audioEvents"), f.event);
		CHECK(event.at("offset") == 1000);
		CHECK(event.at("length") == (operation == "insertSilence" ? 5000 : operation == "paste" ? 3300 : 3000));
		CHECK(test::readWav(path).samples == audio.samples);
		f.call("history.undo");
		CHECK(f.call("project.get").at("audioEvents") == before.at("audioEvents"));
		CHECK(f.call("project.get").at("clips") == before.at("clips"));
		CHECK(f.call("project.get").at("media") == before.at("media"));
		f.call("history.redo");
		CHECK(f.call("project.get").at("audioEvents") == after.at("audioEvents"));
		CHECK(test::compareAudio(eventAudio(f), expected, 0.0F).equal);
		if (operation == "cut")
		{
			process(f, "paste", 1000, 2000);
			CHECK(eventAudio(f).samples.at(1000) == audio.samples.at(2000));
		}
	}
}

TEST_CASE("sample editor Process copies replace the clipboard without an undo step", "[integration]")
{
	const test::TempDir dir;
	const auto path = dir.path() / "source.wav";
	const test::Audio audio = processAudio();
	test::writeWav(path, audio);
	Fixture f(path.u8string());
	f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	const size_t steps = f.call("history.get").at("steps").size();
	process(f, "copy", 100, 200);
	process(f, "copy", 700, 900);
	CHECK(f.call("history.get").at("steps").size() == steps);
	process(f, "paste", 2000, 3000);
	const auto result = eventAudio(f);
	CHECK(result.frames() == 8800);
	CHECK(result.samples.at(2000) == audio.samples.at(700));
	CHECK(result.samples.at(2199) == audio.samples.at(899));
	CHECK(result.samples.at(2200) == audio.samples.at(3000));
}

TEST_CASE("sample editor Process chooses a new version or all shared copies", "[integration]")
{
	for (const std::string sharedCopies : {"newVersion", "all"})
	{
		CAPTURE(sharedCopies);
		const test::TempDir dir;
		const auto path = dir.path() / "source.wav";
		test::writeWav(path, processAudio());
		Fixture f(path.u8string());
		f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
		const Json other =
			f.call("audioEvent.add",
				 {{"track", f.track}, {"clip", f.clip}, {"position", 96000}, {"offset", 4000}, {"length", 2000}})
				.at("event")
				.at("id");
		f.call("clip.addRegion", {{"clip", f.clip}, {"start", 3500}, {"end", 6500}});
		const Json before = f.call("project.get");
		process(f, "delete", 2000, 3000, sharedCopies);
		const Json project = f.call("project.get");
		const Json edited = objectWithId(project.at("audioEvents"), f.event);
		const Json copy = objectWithId(project.at("audioEvents"), other);
		CHECK((edited.at("clip") == copy.at("clip")) == (sharedCopies == "all"));
		CHECK(copy.at("offset") == (sharedCopies == "all" ? 3000 : 4000));
		const Json clip = objectWithId(project.at("clips"), edited.at("clip"));
		CHECK(clip.at("regions").at(0).at("start") == 2500);
		CHECK(clip.at("regions").at(0).at("end") == 5500);
		f.call("history.undo");
		CHECK(f.call("project.get").at("audioEvents") == before.at("audioEvents"));
		CHECK(f.call("project.get").at("clips") == before.at("clips"));
	}
}

TEST_CASE("sample editor Process rejects errors and discards cancelled or stale work", "[integration]")
{
	const test::TempDir dir;
	const auto path = dir.path() / "source.wav";
	test::writeWav(path, processAudio());
	Fixture f(path.u8string());
	f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	Json parameters = {{"event", f.event}, {"operation", "paste"}, {"start", 2000}, {"end", 3000}};
	CHECK_THAT(f.fails("sampleEditor.process", parameters), ContainsSubstring("clipboard"));
	parameters["operation"] = "delete";
	parameters["end"] = 10000;
	CHECK_THAT(f.fails("sampleEditor.process", parameters), ContainsSubstring("range"));
	parameters["end"] = 3000;
	f.call("event.set", {{"events", {f.event}}, {"lock", true}});
	CHECK_THAT(f.fails("sampleEditor.process", parameters), ContainsSubstring("locked"));
	f.call("event.set", {{"events", {f.event}}, {"lock", false}});
	f.call("audioEvent.add", {{"track", f.track}, {"clip", f.clip}, {"position", 96000}, {"length", 4000}});
	CHECK_THAT(f.fails("sampleEditor.process", parameters), ContainsSubstring("sharedCopies"));
	parameters["sharedCopies"] = "all";
	for (const bool cancel : {true, false})
	{
		const Json before = f.call("project.get");
		const Json job = test::whileHeld(f.engine, f.client, "sampleEditor.process", parameters,
			[&](uint64_t id)
			{
				CHECK(f.call("sampleEditor.get").is_object());
				if (cancel)
					f.call("job.cancel", {{"job", id}});
				else
					f.call("project.set", {{"name", "Changed"}});
			});
		CHECK(job.at("state") == (cancel ? "cancelled" : "failed"));
		if (!cancel)
			CHECK_THAT(job.at("error").get<std::string>(), ContainsSubstring("changed meanwhile"));
		CHECK(f.call("project.get").at("media") == before.at("media"));
		CHECK(f.call("project.get").at("audioEvents") == before.at("audioEvents"));
	}
}

TEST_CASE("sample editor Process keeps inactive shared copies consistent and removes empty events", "[integration]")
{
	const test::TempDir dir;
	const auto path = dir.path() / "source.wav";
	test::writeWav(path, processAudio());
	Fixture f(path.u8string());
	f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	const int64_t inactive = f.event;
	f.call("trackVersion.add", {{"tracks", {f.track}}});
	f.event = f.call("audioEvent.add",
				   {{"track", f.track}, {"clip", f.clip}, {"position", 48000}, {"offset", 1000}, {"length", 4000}})
				  .at("event")
				  .at("id")
				  .get<int64_t>();
	process(f, "delete", 2000, 3000, "all");
	const Json project = f.call("project.get");
	const Json stored = objectWithId(project.at("versionContents").at(0).at("audioEvents"), inactive);
	CHECK(stored.at("offset") == 1000);
	CHECK(stored.at("length") == 3000);
	f.call("history.undo");
	process(f, "delete", 0, 9600, "all");
	CHECK(f.call("project.get").at("audioEvents").empty());
	CHECK(f.call("project.get").at("versionContents").at(0).at("audioEvents").empty());
	f.call("history.undo");
	CHECK(objectWithId(f.call("project.get").at("audioEvents"), f.event).at("length") == 4000);
}

TEST_CASE("sample editor Process keeps ranges ordered, fades valid and progress increasing", "[integration]")
{
	const test::TempDir dir;
	const auto path = dir.path() / "source.wav";
	test::writeWav(path, processAudio());
	Fixture f(path.u8string());
	f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	f.call("clip.setWarp", {{"clip", f.clip}, {"gridStart", 500}});
	CHECK_THAT(
		f.fails("sampleEditor.process", {{"event", f.event}, {"operation", "delete"}, {"start", 2000}, {"end", 3000}}),
		ContainsSubstring("definition grid"));
	f.call("clip.setWarp", {{"clip", f.clip}, {"gridStart", 0}, {"tempo", 120}}); // tempo alone does not warp playback
	f.call("clip.addRegion", {{"clip", f.clip}, {"start", 2500}, {"end", 5000}, {"name", "First id"}});
	f.call("clip.addRegion", {{"clip", f.clip}, {"start", 2200}, {"end", 5000}, {"name", "Second id"}});
	f.call(
		"audioEvent.setFades", {{"event", f.event}, {"fadeIn", {{"length", 1500}}}, {"fadeOut", {{"length", 1500}}}});
	f.call("audioEvent.setVolumeCurve",
		{{"event", f.event},
			{"points", Json::array({{{"position", 1500}, {"volume", -6}}, {{"position", 4500}, {"volume", -3}}})}});
	f.call("event.setSnapPoint", {{"events", {f.event}}, {"position", 48000 + 3500}});
	std::vector<Json> progress;
	const Json job = test::whileHeld(
		f.engine, f.client, "sampleEditor.process",
		{{"event", f.event}, {"operation", "cut"}, {"start", 2000}, {"end", 3000}, {"sharedCopies", "all"}},
		[](uint64_t) {}, &progress);
	REQUIRE(job.at("state") == "done");
	for (size_t i = 1; i < progress.size(); ++i)
		CHECK(progress.at(i).at("progress").get<double>() >= progress.at(i - 1).at("progress").get<double>());
	const Json project = f.call("project.get");
	const Json event = objectWithId(project.at("audioEvents"), f.event);
	CHECK(event.at("fadeIn").at("length") == 1000);
	CHECK(event.at("fadeOut").at("length") == 1500);
	CHECK(event.at("volumeCurve").at(1).at("position") == 3500);
	CHECK(event.at("snapPoint") == 3500);
	const Json regions = objectWithId(project.at("clips"), f.clip).at("regions");
	CHECK(regions.at(0).at("name") == "First id");
	CHECK(regions.at(1).at("name") == "Second id");
}

TEST_CASE("sample editor Process failures preserve the clipboard and clean their files", "[integration]")
{
	const test::TempDir dir;
	const auto path = dir.path() / "source.wav";
	const test::Audio original = processAudio();
	test::writeWav(path, original);
	Fixture f(path.u8string());
	f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	process(f, "copy", 700, 900);
	const Json parameters = {{"event", f.event}, {"operation", "cut"}, {"start", 2000}, {"end", 3000}};
	const auto filesBefore =
		std::distance(std::filesystem::directory_iterator(dir.path() / "Audio"), std::filesystem::directory_iterator());
	const Json stale = test::whileHeld(f.engine, f.client, "sampleEditor.process", parameters,
		[&](uint64_t) { f.call("project.set", {{"name", "Changed"}}); });
	CHECK(stale.at("state") == "failed");
	CHECK(std::distance(std::filesystem::directory_iterator(dir.path() / "Audio"),
			  std::filesystem::directory_iterator()) == filesBefore);
	test::settlePeaks(f.client);
	test::Audio shortFile = original;
	shortFile.samples.resize(2500);
	test::writeWav(path, shortFile);
	const Json job = f.call("sampleEditor.process", parameters).at("job");
	const Json failed = test::waitForJob(f.client, job.at("id").get<uint64_t>());
	CHECK(failed.at("state") == "failed");
	CHECK_THAT(failed.at("error").get<std::string>(), ContainsSubstring("file ends"));
	test::writeWav(path, original);
	process(f, "paste", 2000, 3000);
	CHECK(eventAudio(f).samples.at(2000) == original.samples.at(700));
	CHECK(eventAudio(f).frames() == 8800);
	Json completion = parameters;
	completion.update({{"revision", 0}, {"inserted", 0}});
	CHECK_THAT(f.fails("sampleEditor.finishProcess", completion), ContainsSubstring("only a Process job"));
	CHECK_THAT(f.fails("sampleEditor.applyProcess", completion), ContainsSubstring("only a Process job"));
}

TEST_CASE("sample editor Process preserves interleaved channels", "[integration]")
{
	const test::TempDir dir;
	const auto path = dir.path() / "stereo.wav";
	test::Audio stereo;
	stereo.channels = 2;
	for (const float sample : processAudio().samples)
	{
		stereo.samples.push_back(sample);
		stereo.samples.push_back(-sample);
	}
	test::writeWav(path, stereo);
	Fixture f(path.u8string(), 2);
	f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	process(f, "copy", 100, 200);
	process(f, "paste", 2000, 3000);
	const test::Audio result = eventAudio(f);
	CHECK(result.frames() == 8700);
	CHECK(result.channels == 2);
	CHECK(result.samples.at(4000) == stereo.samples.at(200));
	CHECK(result.samples.at(4001) == stereo.samples.at(201));
	CHECK(result.samples.at(4200) == stereo.samples.at(6000));
}

TEST_CASE("sample editor Paste converts clipboard rate and channels before replacing the range", "[integration]")
{
	for (const int mode : {0, 1, 2, 3, 4, 5})
	{
		CAPTURE(mode);
		const bool monoSource = mode % 2 == 0;
		const test::TempDir dir;
		test::Audio target;
		target.channels = monoSource ? 2 : 1;
		target.samples.resize(9600 * static_cast<size_t>(target.channels), 0.1F);
		const auto path = dir.path() / "target.wav";
		test::writeWav(path, target);
		Fixture f(path.u8string(), target.channels);
		f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
		test::Audio source;
		source.channels = monoSource ? 1 : 2;
		if (mode == 2 || mode == 3)
			source.channels = target.channels;
		source.sampleRate = monoSource ? 24000 : 96000;
		if (mode >= 4)
			source.sampleRate = 48000;
		for (int i = 0; i < 9600; ++i)
		{
			source.samples.push_back(0.25F);
			if (source.channels == 2)
				source.samples.push_back(0.5F);
		}
		const auto copyPath = dir.path() / "clipboard.wav";
		test::writeWav(copyPath, source);
		const Json media = f.call("media.add", {{"path", copyPath.u8string()}}).at("media").at("id");
		const Json clip = f.call("clip.add", {{"media", media}}).at("clip").at("id");
		const Json event =
			f.call("audioEvent.add", {{"track", f.track}, {"clip", clip}, {"position", 96000}}).at("event").at("id");
		const Json copy =
			f.call("sampleEditor.process", {{"event", event}, {"operation", "copy"}, {"start", 0}, {"end", 9600}})
				.at("job");
		REQUIRE(test::waitForJob(f.client, copy.at("id").get<uint64_t>()).at("state") == "done");
		const auto inserted = 9600 * 48000 / source.sampleRate;
		process(f, "paste", 2000, 3000);
		const test::Audio result = eventAudio(f);
		CHECK(result.frames() == 8600 + inserted);
		CHECK(result.sampleRate == 48000);
		CHECK(result.channels == target.channels);
		for (int channel = 0; channel < target.channels; ++channel)
		{
			const size_t middle = 2000U + (static_cast<size_t>(inserted) / 2);
			const size_t index = (middle * static_cast<size_t>(target.channels)) + static_cast<size_t>(channel);
			double expected = monoSource ? 0.25 : 0.75 * std::sqrt(0.5);
			if (source.channels == target.channels)
				expected = channel == 0 ? 0.25 : 0.5;
			CHECK(std::abs(result.samples.at(index) - expected) < 0.0001);
		}
		CHECK(result.samples.at(0) == target.samples.at(0));
		CHECK(result.samples.back() == target.samples.back());
		f.call("history.undo");
		CHECK(eventAudio(f).samples == target.samples);
		f.call("history.redo");
		CHECK(eventAudio(f).samples == result.samples);
		CHECK(test::readWav(copyPath).samples == source.samples);
	}
}

TEST_CASE("sample editor Process cuts an unshared clip in place without a shared-copy choice", "[integration]")
{
	const test::TempDir dir;
	const auto path = dir.path() / "source.wav";
	test::writeWav(path, processAudio());
	Fixture f(path.u8string());
	f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	f.call("clip.addRegion", {{"clip", f.clip}, {"start", 500}, {"end", 4000}});
	f.call("clip.addRegion", {{"clip", f.clip}, {"start", 7000}, {"end", 8000}});
	const Json before = f.call("project.get");
	const Json job =
		f.call("sampleEditor.process", {{"event", f.event}, {"operation", "cut"}, {"start", 2000}, {"end", 3000}})
			.at("job");
	const Json done = test::waitForJob(f.client, job.at("id").get<uint64_t>());
	REQUIRE(done.at("state") == "done");
	CHECK(done.at("result").at("clip") == f.clip);
	const Json after = f.call("project.get");
	CHECK(after.at("clips").size() == before.at("clips").size());
	CHECK(objectWithId(after.at("audioEvents"), f.event).at("clip") == f.clip);
	const Json edited = objectWithId(after.at("clips"), objectWithId(after.at("audioEvents"), f.event).at("clip"));
	const Json regions = objectWithId(before.at("clips"), f.clip).at("regions");
	REQUIRE(edited.at("regions").size() == regions.size());
	for (size_t i = 0; i < regions.size(); ++i)
		CHECK(edited.at("regions").at(i).at("id") == regions.at(i).at("id"));
	f.call("history.undo");
	CHECK(f.call("project.get").at("clips") == before.at("clips"));
	f.call("history.redo");
	CHECK(f.call("project.get").at("clips") == after.at("clips"));
}

TEST_CASE("sample editor Process requires a choice for clips also used by sampler tracks or drum pads", "[integration]")
{
	const test::TempDir dir;
	const auto path = dir.path() / "source.wav";
	test::writeWav(path, processAudio());
	Fixture f(path.u8string());
	f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	SECTION("sampler track")
	{
		f.call("sampler.create", {{"clip", f.clip}});
	}
	SECTION("drum pad")
	{
		const Json track = f.call("drum.create", {{"kit", false}}).at("track").at("id");
		f.call("drum.loadPad", {{"track", track}, {"pad", 36}, {"clip", f.clip}});
	}
	const Json before = f.call("project.get");
	CHECK_THAT(
		f.fails("sampleEditor.process", {{"event", f.event}, {"operation", "cut"}, {"start", 2000}, {"end", 3000}}),
		ContainsSubstring("sharedCopies"));
	CHECK(f.call("project.get").at("clips") == before.at("clips"));
	const Json result = process(f, "cut", 2000, 3000, "newVersion");
	CHECK(result.at("clip") != f.clip);
	const Json after = f.call("project.get");
	CHECK(objectWithId(after.at("clips"), f.clip) == objectWithId(before.at("clips"), f.clip));
	CHECK(after.at("channels") == before.at("channels"));
}

TEST_CASE("sample editor Draw replaces one channel without moving clip metadata", "[integration]")
{
	const test::TempDir dir;
	const auto path = dir.path() / "draw.wav";
	test::Audio audio = processAudio();
	audio.channels = 2;
	audio.samples.resize(19200, 0.25F);
	test::writeWav(path, audio);
	Fixture f(path.u8string(), 2);
	f.call("project.save", {{"path", (dir.path() / "song.dawproject").u8string()}});
	f.call("clip.addRegion", {{"clip", f.clip}, {"start", 2001}, {"end", 2003}});
	const Json before = f.call("project.get");
	Json parameters = {{"event", f.event}, {"operation", "draw"}, {"start", 2000}, {"end", 2005}, {"channel", 1},
		{"samples", {0.5, 0.25, 0.0, -0.25, -0.5}}};
	const Json job = f.call("sampleEditor.process", parameters).at("job");
	REQUIRE(test::waitForJob(f.client, job.at("id").get<uint64_t>()).at("state") == "done");
	test::Audio expected = audio;
	for (size_t i = 0; i < 5; ++i)
		expected.samples.at(((2000 + i) * 2) + 1) = parameters.at("samples").at(i).get<float>();
	CHECK(test::compareAudio(eventAudio(f), expected, 0.0F).equal);
	CHECK(f.regions() == objectWithId(before.at("clips"), f.clip).at("regions"));
	CHECK(f.call("project.get").at("audioEvents") == before.at("audioEvents"));
	CHECK(test::readWav(path).samples == audio.samples);
	f.call("history.undo");
	CHECK(eventAudio(f).samples == audio.samples);
	f.call("history.redo");
	CHECK(eventAudio(f).samples == expected.samples);
	parameters["channel"] = 2;
	CHECK_THAT(f.fails("sampleEditor.process", parameters), ContainsSubstring("channel"));
	parameters["channel"] = 0;
	parameters["samples"] = {0.0};
	CHECK_THAT(f.fails("sampleEditor.process", parameters), ContainsSubstring("sample count"));
}
