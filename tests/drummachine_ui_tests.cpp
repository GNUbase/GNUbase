// SPDX-License-Identifier: GPL-3.0-or-later
// The drum machine panel and Add Drum Track in the app (#182, docs/features/drum-track.md), driven with
// ui::Automation as MCP's UI tools drive it: the panel follows the selected drum track, shows its pads in groups,
// selects a pad and sets its parameters and the global ones, renames, copies, cuts, pastes and resets pads, loads a
// dropped file onto a pad, and the add track dialog's Drum page adds drum tracks. tests/drummachine_tests.cpp checks
// the engine's side.
#include "testutils/commandholds.h"
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
#include "ui/enginebinding.h"
#include "ui/shell.h"
#include "ui/tracklist.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Matchers::WithinAbs;
using engine::Json;

namespace
{
	ui::Target of(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	constexpr uint32_t g_padStride = 32;
	constexpr double g_kick = 2.0; // the instrument parameter's value
	constexpr uint32_t g_level = 1;
	constexpr uint32_t g_mute = 3;
	constexpr uint32_t g_bypass = 4097;

	// A project with a drum track and an audio track, the project window shown.
	struct Fixture
	{
		Fixture()
		{
			app.appUi().showProject();
			test::Audio tick;
			for (int frame = 0; frame < 4800; ++frame)
				tick.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 0.05)));
			test::writeWav(file, tick);
			audio = app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
			const Json created = app.call("drum.create");
			track = created.at("track").at("id").get<uint64_t>();
			channel = created.at("channel").at("id").get<uint64_t>();
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		bool has(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.size() != 1)
				return false;
			const Json classes = found[0].value("classes", Json::array());
			return std::find(classes.begin(), classes.end(), name) != classes.end();
		}

		void click(const std::string& selector)
		{
			app.ui().click(std::nullopt, of(selector), ui::MouseButton::Left, 1, {});
			app.shell().step(std::chrono::milliseconds(0));
		}

		void select(uint64_t id)
		{
			REQUIRE(app.waitUntil(
				[&]
				{
					app.appUi().trackList()->select({id});
					return app.appUi().trackList()->selection() == std::vector<uint64_t>{id};
				}));
		}

		void open()
		{
			select(track);
			app.keys().run("window.drumMachine", Json::object(), app.main(), nullptr);
			REQUIRE(
				app.waitUntil([&] { return !elements("#drum-machine").empty() && !has("#drum-machine", "empty"); }));
			REQUIRE(app.waitUntil([&] { return !elements("#drum-machine [parameter]").empty(); }));
		}

		Json drumChannel()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("channels"))
				if (each.at("id") == channel)
					return each;
			return {};
		}

		double parameter(uint32_t id, double fallback)
		{
			const Json drums = drumChannel();
			for (const Json& item : drums.at("instrumentParameters"))
				if (item.at("parameter") == id)
					return item.at("value").get<double>();
			return fallback;
		}

		// A pad's name or another of its fields.
		std::string padName(int pad, const char* field = "name")
		{
			const Json drums = drumChannel();
			for (const Json& item : drums.at("pads"))
				if (item.at("pad") == pad)
					return item.value(field, std::string());
			return {};
		}

		// Opens a drop-down list, moves the selection down that many options with the arrow key, and closes it.
		void choose(const std::string& selector, int down)
		{
			click(selector);
			for (int i = 0; i < down; ++i)
				app.ui().key(std::nullopt, "Down");
			click(selector);
		}

		static std::string pad(int number) { return ".drum-pad[pad=\"" + std::to_string(number) + "\"]"; }

		test::TempDir folder;
		std::filesystem::path file = folder.path() / "tick.wav";
		test::UiApp app{{1200, 800}};
		uint64_t audio = 0;
		uint64_t track = 0;
		uint64_t channel = 0;
	};
}

TEST_CASE("the drum machine panel shows the selected drum track's pads and sets their parameters", "[ui]")
{
	Fixture f;
	f.select(f.audio);
	f.app.keys().run("window.drumMachine", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.has("#drum-machine", "empty"); }));
	f.open();

	// The third group, C1 and up, with the kick selected; pads without names of their own go by the General MIDI
	// sounds', as pattern lanes do.
	CHECK(f.elements(".drum-pad").size() == 16);
	CHECK(f.has(".drum-group[group=\"2\"]", "selected"));
	CHECK(f.has(Fixture::pad(36), "selected"));
	CHECK(f.has(Fixture::pad(36), "assigned"));
	CHECK(!f.has(Fixture::pad(37), "assigned"));
	CHECK(f.padName(36).empty());
	REQUIRE(f.app.waitUntil(
		[&] { return f.elements(Fixture::pad(36) + " .drum-pad-name").at(0).value("text", "") == "Bass Drum 1"; }));
	CHECK(f.elements(Fixture::pad(37) + " .drum-pad-name").at(0).value("text", "") == "Side Stick");
	CHECK(f.elements("#drum-pad-name").at(0).at("attributes").at("value") == "Bass Drum 1");
	// The pads' lowest row is at the bottom.
	CHECK(f.elements(Fixture::pad(36)).at(0).at("box").at("y").get<double>() >
		f.elements(Fixture::pad(40)).at(0).at("box").at("y").get<double>());

	// Another pad: its parameters, set by the page's controls.
	f.click(Fixture::pad(38));
	REQUIRE(f.app.waitUntil([&] { return f.has(Fixture::pad(38), "selected"); }));
	const std::string mute = "#drum-parameter-" + std::to_string((38 * g_padStride) + g_mute);
	REQUIRE(!f.elements(mute).empty());
	f.click(mute);
	REQUIRE(f.app.waitUntil([&] { return f.parameter((38 * g_padStride) + g_mute, 0.0) == 1.0; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(Fixture::pad(38), "muted"); }));
	const std::string level = "#drum-parameter-" + std::to_string((38 * g_padStride) + g_level);
	f.app.ui().wheel(std::nullopt, of(level), 0.0F, -1.0F);
	REQUIRE(f.app.waitUntil([&] { return f.parameter((38 * g_padStride) + g_level, 0.0) > 0.0; }));

	// Another group, and the global pages.
	f.click(".drum-group[group=\"3\"]");
	REQUIRE(f.app.waitUntil([&] { return !f.elements(Fixture::pad(48)).empty(); }));
	f.click("#drum-page-reverb");
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#drum-parameter-4106").empty(); }));
	CHECK(f.elements("#drum-parameter-" + std::to_string((38 * g_padStride) + g_level)).empty());
	f.click("#drum-bypass");
	REQUIRE(f.app.waitUntil([&] { return f.parameter(g_bypass, 0.0) == 1.0; }));

	// Another track: the panel is empty.
	f.select(f.audio);
	REQUIRE(f.app.waitUntil([&] { return f.has("#drum-machine", "empty"); }));
}

TEST_CASE("the drum machine panel shows another track's own value while a turned one's last step is out, and back on "
		  "it the turn's",
	"[ui]")
{
	test::CommandHolds holds; // declared before the app: its engine's command thread waits in hold()
	Fixture f;
	const test::ReleaseHolds releaseHolds{holds};
	f.app.engine().registerCommand(
		{"test.hold", 1, "Waits for the test to release it.", engine::CommandInfo{}.parameters, Json::object(), false},
		[&holds](engine::CommandContext&) { return holds.hold(); });
	engine::Client holder(f.app.engine(), "holder");
	const Json created = f.app.call("drum.create");
	const uint64_t other = created.at("track").at("id").get<uint64_t>();
	const uint32_t id = (36 * g_padStride) + g_level;
	f.app.call("parameter.set",
		{{"channel", created.at("channel").at("id")}, {"target", {{"type", "instrument"}, {"parameter", id}}},
			{"value", -12.0}});
	f.open();
	const std::string level = "#drum-parameter-" + std::to_string(id);
	const auto value = [&] { return test::controlValue(f.app, std::nullopt, level); };
	REQUIRE(f.app.waitUntil([&] { return !f.elements(level).empty(); }));
	const double before = value();
	ui::EngineBinding& binding = f.app.appUi().binding();
	REQUIRE(f.app.waitUntil(
		[&] { return binding.waiting("history.transaction") == 0 && binding.waiting("project.get") == 0; }));

	// A wheel turn that the engine takes before a hold: the project read after it waits behind the hold, so the panel
	// has a read from before the turn (#1305).
	f.app.ui().wheel(std::nullopt, of(level), 0.0F, -1.0F);
	holder.send("test.hold");
	REQUIRE(holds.waitEntered(1));
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(binding.waiting("history.transaction") == 0);
	REQUIRE(binding.waiting("project.get") != 0);
	const double turned = value();
	CHECK(turned > before);

	// The other drum track shows its own value, and back on the turned one the turn's, not the other track's (#1471).
	f.select(other);
	CHECK(f.app.waitUntil([&] { return value() == -12.0; }));
	f.select(f.track);
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(value() == turned);

	REQUIRE(binding.waiting("project.get") != 0);

	// The read after the turn agrees with it.
	holds.release(1);
	REQUIRE(f.app.waitUntil([&] { return binding.waiting("project.get") == 0; }));
	CHECK_THAT(f.parameter(id, 0.0), WithinAbs(turned, 1e-5));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(value() == turned);
}

TEST_CASE("the drum machine panel renames, copies, cuts, pastes, resets and loads pads", "[ui]")
{
	Fixture f;
	f.open();

	// Rename Pad: the name goes out with Enter.
	f.click(Fixture::pad(37));
	f.click("#drum-pad-name");
	f.app.ui().key(std::nullopt, "Mod+A");
	f.app.ui().text(std::nullopt, "Rim");
	f.app.ui().key(std::nullopt, "Enter");
	REQUIRE(f.app.waitUntil([&] { return f.padName(37) == "Rim"; }));
	REQUIRE(f.app.waitUntil(
		[&] { return f.elements(Fixture::pad(37) + " .drum-pad-name").at(0).value("text", "") == "Rim"; }));

	// Set Color: the project's colors, the first after No Color; the pad shows it in its strip.
	const std::string first = f.app.call("project.get").at("colors").at(0).at("color").get<std::string>();
	f.choose("#drum-pad-color", 1);
	REQUIRE(f.app.waitUntil([&] { return f.padName(37, "color") == first; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(Fixture::pad(37), "colored"); }));
	CHECK(f.elements(Fixture::pad(37) + " .drum-pad-color").at(0).at("attributes").at("color") == first);
	CHECK(f.padName(37) == "Rim");

	// Copy and paste the kick onto pad 40, cut it and paste it onto 41.
	const auto kick = [&](int pad) { return f.parameter(pad * g_padStride, 0.0) == g_kick; };
	f.click(Fixture::pad(36));
	f.click("#drum-copy");
	f.click(Fixture::pad(40));
	f.click("#drum-paste");
	REQUIRE(f.app.waitUntil([&] { return kick(40); }));
	CHECK(kick(36));
	// The copy goes by the kick's General MIDI sound, not by its own note's (Low Floor Tom).
	REQUIRE(f.app.waitUntil(
		[&] { return f.elements(Fixture::pad(40) + " .drum-pad-name").at(0).value("text", "") == "Bass Drum 1"; }));
	f.click("#drum-cut");
	f.click(Fixture::pad(41));
	f.click("#drum-paste");
	REQUIRE(f.app.waitUntil([&] { return kick(41) && f.parameter(40 * g_padStride, 0.0) == 0.0; }));
	REQUIRE(
		f.app.waitUntil([&] { return f.has(Fixture::pad(41), "assigned") && !f.has(Fixture::pad(40), "assigned"); }));

	// Reset Pad.
	f.click("#drum-reset");
	REQUIRE(f.app.waitUntil([&] { return !kick(41) && !f.has(Fixture::pad(41), "assigned"); }));

	// A file dropped on a pad loads onto it and names it.
	CHECK(!f.app.ui().drop(std::nullopt, of(Fixture::pad(43)), {f.file}).at("element").is_null());
	REQUIRE(f.app.waitUntil(
		[&] { return f.elements(Fixture::pad(43) + " .drum-pad-name").at(0).value("text", "") == "tick"; }));
	CHECK(f.has(Fixture::pad(43), "assigned"));
}

TEST_CASE("a pad plays at velocity 127 while the mouse button is down on it", "[ui]")
{
	Fixture f;
	f.open();
	f.app.call("audio.setDevice", {{"blockSize", 128}, {"inputChannels", 0}, {"captureSeconds", 1}});
	f.app.call("audio.render", {{"frames", 128}}); // the new graph
	f.app.call("midi.capture", {{"start", true}});
	f.click(Fixture::pad(38));
	std::vector<std::vector<int>> played;
	CHECK(f.app.waitUntil(
		[&]
		{
			f.app.call("audio.render", {{"frames", 128}});
			const Json captured = f.app.call("midi.capture");
			for (const Json& message : captured.at("captured"))
			{
				const auto bytes = message.at("bytes").get<std::vector<int>>();
				if (message.at("channel") == f.channel && (bytes.at(0) & 0xE0) == 0x80)
					played.push_back(bytes);
			}
			return played == std::vector<std::vector<int>>{{0x90, 38, 127}, {0x80, 38, 0}};
		}));
	CHECK(f.has(Fixture::pad(38), "selected"));
}

TEST_CASE("the drum machine's Save Preset and Load Preset open the track preset dialog for its track", "[ui]")
{
	Fixture f;
	f.open();
	const auto dialog = [&]
	{
		const Json found = f.elements("#track-preset-dialog");
		return found.size() == 1 && found[0].at("visible").get<bool>();
	};
	f.app.call("drum.setPad", {{"track", f.track}, {"pad", 36}, {"name", "Boom"}});
	f.click("#drum-save-preset");
	REQUIRE(f.app.waitUntil(dialog));
	f.click("#preset-name");
	f.app.ui().text(std::nullopt, "Kit");
	f.click("#preset-confirm");
	REQUIRE(f.app.waitUntil([&] { return !dialog(); }));
	const Json presets = f.app.call("track.presets").at("presets");
	REQUIRE(presets.size() == 1);
	CHECK(presets[0].at("name") == "Kit");

	// Loading it gives the drum track the pads it had.
	f.app.call("drum.resetPad", {{"track", f.track}});
	REQUIRE(f.padName(36).empty());
	f.click("#drum-load-preset");
	REQUIRE(f.app.waitUntil(dialog));
	f.click("#preset-confirm");
	REQUIRE(f.app.waitUntil([&] { return f.padName(36) == "Boom"; }));
}

TEST_CASE("Add Drum Track opens the add track dialog on its Drum page", "[ui]")
{
	Fixture f;
	f.select(f.audio);
	f.app.keys().run("project.addDrumTrack", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#add-track-dialog").empty(); }));
	CHECK(f.elements("#track-types").at(0).at("attributes").at("value") == "drum");
	CHECK(f.elements("#track-instrument").empty());
	f.click("#add-track-confirm");
	REQUIRE(f.app.waitUntil([&] { return f.elements("#add-track-dialog").empty(); }));
	const Json project = f.app.call("project.get");
	// Below the selected audio track, named as drum.create names it.
	REQUIRE(project.at("tracks").size() == 3);
	const Json& added = project.at("tracks").at(1);
	CHECK(added.at("name") == "Drum 02");
	for (const Json& each : project.at("channels"))
		if (each.at("id") == added.at("channel"))
			CHECK(each.at("instrument") == "drumMachine");

	// The empty drum machine panel's Add Drum Track does the same (p. 146).
	f.select(f.audio);
	f.app.keys().run("window.drumMachine", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.has("#drum-machine", "empty"); }));
	f.click("#drum-add-track");
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#add-track-dialog").empty(); }));
	CHECK(f.elements("#track-types").at(0).at("attributes").at("value") == "drum");
	f.click("#add-track-confirm");
	REQUIRE(f.app.waitUntil([&] { return f.app.call("project.get").at("tracks").size() == 4; }));
}
