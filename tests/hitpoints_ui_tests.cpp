// SPDX-License-Identifier: GPL-3.0-or-later
// The sample editor's Hitpoints section in the app (#143, docs/features/hitpoints.md), driven with ui::Automation as
// MCP's UI tools drive it: Edit Hitpoints with the hitpoints in the display, the filters, editing them with the mouse,
// Remove All and the Create section. tests/hitpoints_tests.cpp checks the engine's side.
#include "ui/enginebinding.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_second = 48000;
	constexpr std::array<int64_t, 4> g_bursts = {6000, 18000, 30000, 42000};

	ui::Target of(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	// An audio track with an event at one second playing a second of four decaying tone bursts, open in the sample
	// editor.
	struct Fixture
	{
		Fixture() : display(app.appUi().showProject())
		{
			test::Audio bursts;
			bursts.samples.assign(g_second, 0.0F);
			for (const int64_t burst : g_bursts)
				for (int64_t i = 0; i < 6000; ++i)
					bursts.samples[static_cast<size_t>(burst + i)] = static_cast<float>(0.5 *
						std::sin(static_cast<double>(i) * 2.0 * 3.14159265358979323846 / 48.0) *
						std::exp(-static_cast<double>(i) / 960.0));
			test::writeWav(file, bursts);
			track =
				app.call("track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<uint64_t>();
			const Json media = app.call("media.add", {{"path", file.u8string()}}).at("media");
			clip = app.call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<uint64_t>();
			event =
				app.call("audioEvent.add",
					   {{"track", track}, {"clip", clip}, {"position", g_second}, {"offset", 0}, {"length", g_second}})
					.at("event")
					.at("id")
					.get<uint64_t>();
			REQUIRE(app.waitUntil(
				[&] { return display.project().is_object() && display.project().at("audioEvents").size() == 1; }));
			display.select({event});
			app.keys().run("editor.openClose", Json::object(), app.main(), nullptr);
			REQUIRE(app.waitUntil([&] { return app.appUi().sampleEditorWindow() != nullptr; }));
			window = app.appUi().sampleEditorWindowId();
			REQUIRE(app.waitUntil([&] { return !has(".sample-editor", "empty") && editor().scale() > 0.0; }));
		}

		ui::SampleEditor& editor() { return *app.appUi().sampleEditorWindow(); }

		Json elements(const std::string& selector) { return app.ui().dom(window, selector, 0).at("elements"); }

		Json box(const std::string& selector) { return elements(selector).at(0).at("box"); }

		bool has(const std::string& selector, const std::string& name)
		{
			const Json found = elements(selector);
			if (found.empty())
				return false;
			const Json names = found.at(0).value("classes", Json::array());
			return std::find(names.begin(), names.end(), name) != names.end();
		}

		Json hitpoints()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("clips"))
				if (each.at("id") == clip)
					return each.at("hitpoints");
			return Json::array();
		}

		// The hitpoints shown, once the editor shows count of them.
		Json shown(size_t count)
		{
			REQUIRE(app.waitUntil([&] { return elements(".se-hitpoint").size() == count; }));
			return elements(".se-hitpoint");
		}

		float dpRatio() { return app.shell().find(window)->context().GetDensityIndependentPixelRatio(); }

		double x(double frame)
		{
			return box(".se-display").at("x").get<double>() +
				((frame - editor().start()) / editor().scale() * dpRatio());
		}

		// A point in the waveform display at a clip frame, height from the top in the display's pixels (a hitpoint's
		// handle is at the top).
		ui::Target at(double frame, double height = -1.0)
		{
			const Json display = box(".se-display");
			const double y =
				display.at("y").get<double>() + (height < 0.0 ? display.at("height").get<double>() / 2.0 : height);
			return {"", 0, ui::Point{static_cast<int>(std::lround(x(frame))), static_cast<int>(std::lround(y))}};
		}

		void click(const ui::Target& target, const std::vector<std::string>& modifiers = {})
		{
			app.ui().click(window, target, ui::MouseButton::Left, 1, modifiers);
			app.shell().step(std::chrono::milliseconds(0));
		}

		void drag(const ui::Target& from, const ui::Target& to)
		{
			app.ui().drag(window, from, to);
			app.shell().step(std::chrono::milliseconds(0));
		}

		// Edit Hitpoints, on.
		void edit()
		{
			click(of("#se-edit-hitpoints"));
			REQUIRE(has("#se-edit-hitpoints", "checked"));
			REQUIRE(app.waitUntil([&] { return hitpoints().size() == g_bursts.size(); }));
		}

		test::UiApp app{{1200, 800}};
		ui::EventDisplay& display;
		test::TempDir dir;
		std::filesystem::path file = dir.path() / "bursts.wav";
		ui::WindowId window = 0;
		uint64_t track = 0;
		uint64_t clip = 0;
		uint64_t event = 0;
	};
}

TEST_CASE("Edit Hitpoints detects a clip's hitpoints, shows them and filters them", "[ui]")
{
	Fixture f;
	CHECK(f.elements(".se-hitpoint").empty());
	f.edit();
	CHECK(f.has(".sample-editor", "edit-hitpoints"));
	const Json shown = f.shown(g_bursts.size());
	for (size_t i = 0; i < g_bursts.size(); ++i)
	{
		CHECK(std::abs(f.hitpoints().at(i).at("position").get<int64_t>() - g_bursts[i]) < 48);
		// The line is in the middle of its element.
		const Json box = shown.at(i).at("box");
		const double middle = box.at("x").get<double>() + (box.at("width").get<double>() / 2.0);
		CHECK(std::abs(middle - f.x(static_cast<double>(g_bursts[i]))) < 6.0);
	}

	// The threshold slider at 0 dBFS leaves out every burst, which peak at -6 dBFS: one undo step for the drag.
	const size_t steps = f.app.call("history.get").at("steps").size();
	const Json slider = f.box("#se-threshold");
	const auto y = static_cast<int>(slider.at("y").get<double>() + (slider.at("height").get<double>() / 2.0));
	f.drag({"", 0, ui::Point{static_cast<int>(slider.at("x").get<double>()) + 1, y}},
		{"", 0, ui::Point{static_cast<int>(slider.at("x").get<double>() + slider.at("width").get<double>()) + 20, y}});
	f.shown(0);
	REQUIRE(f.app.waitUntil([&] { return f.app.call("history.get").at("steps").size() == steps + 1; }));
	CHECK(f.app.call("history.get").at("steps").back().at("action") == "Filter hitpoints");
	f.app.call("history.undo");
	f.shown(g_bursts.size());

	// Remove All, and Edit Hitpoints again finds them again.
	f.click(of("#se-remove-hitpoints"));
	f.shown(0);
	f.click(of("#se-edit-hitpoints"));
	CHECK(!f.has(".sample-editor", "edit-hitpoints"));
	f.edit();
	f.shown(g_bursts.size());
}

TEST_CASE("Edit Hitpoints' gestures lock, disable, reset, insert and move hitpoints and play slices", "[ui]")
{
	Fixture f;
	f.edit();
	f.shown(g_bursts.size());
	const auto position = [&f](size_t index) { return f.hitpoints().at(index).at("position").get<double>(); };
	const double second = position(1);

	// A click on the handle locks it, Shift and a click on the line disables it, Ctrl, Alt and a click resets it.
	f.click(f.at(second, 3.0));
	REQUIRE(f.app.waitUntil([&] { return f.hitpoints().at(1).at("locked") == true; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(".se-hitpoint:nth-child(2)", "locked"); }));
	f.click(f.at(second), {"shift"});
	REQUIRE(f.app.waitUntil([&] { return f.hitpoints().at(1).at("disabled") == true; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(".se-hitpoint:nth-child(2)", "disabled"); }));
	f.click(f.at(second), {"ctrl", "alt"});
	REQUIRE(f.app.waitUntil(
		[&] { return f.hitpoints().at(1).at("disabled") == false && f.hitpoints().at(1).at("locked") == false; }));

	// Alt and a click inserts one, locked; a drag moves one, which locks it.
	f.click(f.at(24000), {"alt"});
	REQUIRE(f.app.waitUntil([&] { return f.hitpoints().size() == g_bursts.size() + 1; }));
	CHECK(std::abs(position(2) - 24000) < f.editor().scale() * 2);
	CHECK(f.hitpoints().at(2).at("locked") == true);
	f.shown(g_bursts.size() + 1);
	const double third = position(3);
	f.drag(f.at(third), f.at(third + 3000));
	REQUIRE(f.app.waitUntil([&] { return std::abs(position(3) - (third + 3000)) < f.editor().scale() * 2; }));
	CHECK(f.hitpoints().at(3).at("locked") == true);

	// A click between two hitpoints plays that slice while the mouse is pressed.
	ui::EngineBinding binding(f.app.engine());
	std::vector<std::string> modes;
	const ui::Subscription subscription = binding.on("audition.changed",
		[&modes](const engine::Event& event) { modes.push_back(event.data.at("mode").get<std::string>()); });
	f.click(f.at(10000));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			binding.update();
			return modes == std::vector<std::string>{"play", "off"};
		}));
}

TEST_CASE("the Hitpoints section's Create section makes MIDI notes and slices", "[ui]")
{
	Fixture f;
	f.edit();

	// MIDI Notes asks for its options, OK makes a MIDI track with a note at each hitpoint.
	CHECK(f.has(".se-midi-options", "off"));
	f.click(of(".se-create[create=midiNotes]"));
	CHECK(!f.has(".se-midi-options", "off"));
	f.click(of(".se-velocity-mode[velocity-mode=fixed]"));
	CHECK(f.has(".se-velocity-mode[velocity-mode=fixed]", "checked"));
	f.click(of("#se-midi-ok"));
	CHECK(f.has(".se-midi-options", "off"));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("project.get").at("midiParts").size() == 1; }));
	const Json notes = f.app.call("project.get").at("midiParts").at(0).at("notes");
	REQUIRE(notes.size() == g_bursts.size());
	for (const Json& note : notes)
	{
		CHECK(note.at("pitch") == 36);
		CHECK(std::abs(note.at("velocity").get<double>() - (100.0 / 127.0)) < 1e-4); // as stored
	}

	// Slices: the event in a part of its slices, from its start to the first hitpoint and on.
	f.click(of(".se-create[create=slices]"));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("project.get").at("audioParts").size() == 1; }));
	CHECK(f.app.call("project.get").at("audioEvents").size() == g_bursts.size() + 1);
}
