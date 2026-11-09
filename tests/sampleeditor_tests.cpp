// SPDX-License-Identifier: GPL-3.0-or-later
// The sample editor's engine side (#140, docs/features/sample-editor.md): the sampleEditor.* view with its selection
// range, and the regions of clips. tests/sampleeditor_ui_tests.cpp drives the editor in the app.
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <filesystem>
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
		Fixture(const std::string& path = "take.wav")
		{
			track = call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<int64_t>();
			const Json media =
				call("media.add", {{"path", path}, {"sampleRate", 48000}, {"channels", 1}, {"length", 9600}})
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
