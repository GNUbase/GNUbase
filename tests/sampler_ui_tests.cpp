// SPDX-License-Identifier: GPL-3.0-or-later
// The sampler panel and Add Sampler Track in the app (#1029, docs/features/sampler-track.md), driven with
// ui::Automation as MCP's UI tools drive it: the panel follows the selected sampler track, its controls, markers and
// keyboard set the sampler's parameters, a dropped file loads into it, Normalize Sample sets its gain, and the add
// track dialog's Sampler page adds sampler tracks. tests/sampler_tests.cpp checks the engine's side.
#include "uiapp.h"

#include "engine/engine.h"
#include "ui/automation.h"
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

	// A project with a sampler track that plays a mono sine, half a second at 48 kHz at half level, and an audio
	// track, the project window shown.
	struct Fixture
	{
		Fixture()
		{
			app.appUi().showProject();
			test::Audio sine;
			for (int frame = 0; frame < 24000; ++frame)
				sine.samples.push_back(static_cast<float>(0.5 * std::sin(frame * 0.05)));
			test::writeWav(file, sine);
			test::writeWav(other, sine);
			audio = app.call("track.add", {{"type", "audio"}}).at("track").at("id").get<uint64_t>();
			const Json created = app.call("sampler.create", {{"path", file.u8string()}});
			track = created.at("track").at("id").get<uint64_t>();
			channel = created.at("channel").at("id").get<uint64_t>();
			app.shell().step(std::chrono::milliseconds(0));
		}

		Json elements(const std::string& selector) { return app.ui().dom(std::nullopt, selector, 0).at("elements"); }

		Json box(const std::string& selector) { return elements(selector).at(0).at("box"); }

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

		// Shows the sampler panel with the sampler track selected.
		void open()
		{
			select(track);
			app.keys().run("window.sampler", Json::object(), app.main(), nullptr);
			REQUIRE(
				app.waitUntil([&] { return !elements("#sampler-panel").empty() && !has("#sampler-panel", "empty"); }));
			REQUIRE(app.waitUntil([&] { return !elements("[parameter]").empty(); }));
		}

		Json samplerChannel()
		{
			const Json project = app.call("project.get");
			for (const Json& each : project.at("channels"))
				if (each.at("id") == channel)
					return each;
			return {};
		}

		// The sampler's parameter, by processor.list's name: its value or default.
		double parameter(const std::string& name)
		{
			const Json processors = app.call("processor.list");
			for (const Json& processor : processors.at("processors"))
				if (processor.at("id") == "sampler")
					for (const Json& each : processor.at("parameters"))
						if (each.at("name") == name)
						{
							const Json values = samplerChannel().at("instrumentParameters");
							for (const Json& item : values)
								if (item.at("parameter") == each.at("id"))
									return item.at("value").get<double>();
							return each.at("default").get<double>();
						}
			return -1.0;
		}

		int id(const std::string& name)
		{
			const Json processors = app.call("processor.list");
			for (const Json& processor : processors.at("processors"))
				if (processor.at("id") == "sampler")
					for (const Json& each : processor.at("parameters"))
						if (each.at("name") == name)
							return each.at("id").get<int>();
			return -1;
		}

		std::string control(const std::string& name) { return "#sampler-parameter-" + std::to_string(id(name)); }

		void set(const std::string& name, double value)
		{
			app.call("parameter.set",
				{{"channel", channel}, {"target", {{"type", "instrument"}, {"parameter", id(name)}}},
					{"value", value}});
		}

		test::TempDir folder;
		std::filesystem::path file = folder.path() / "sine.wav";
		std::filesystem::path other = folder.path() / "other.wav";
		test::UiApp app{{1200, 800}};
		uint64_t audio = 0;
		uint64_t track = 0;
		uint64_t channel = 0;
	};
}

TEST_CASE("the sampler panel shows the selected sampler track and sets its parameters", "[ui]")
{
	Fixture f;
	f.select(f.audio);
	f.app.keys().run("window.sampler", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-panel", "empty"); }));
	f.open();
	CHECK(f.elements("#sampler-file").at(0).value("text", "") == "sine");
	// Every parameter has a control, in its section.
	CHECK(f.elements("[parameter]").size() == 32);
	CHECK(!f.elements(".sampler-section[section=keyboard] " + f.control("Root key")).empty());
	CHECK(f.has(".sampler-key[key=\"60\"]", "root"));
	CHECK(f.has(".sampler-loop", "off"));

	// A toggle and a value field's wheel set the parameters.
	f.click(f.control("Reverse"));
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Reverse") == 1.0; }));
	f.click(f.control("Reverse"));
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Reverse") == 0.0; }));
	f.app.ui().wheel(std::nullopt, of(f.control("Coarse")), 0.0F, -1.0F);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Coarse") == 1.0; }));

	// A click on a key sets the root key, and the keyboard shows the key range.
	f.click(".sampler-key[key=\"48\"]");
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Root key") == 48.0; }));
	CHECK(f.has(".sampler-key[key=\"48\"]", "root"));
	f.set("Key high", 72.0);
	REQUIRE(f.app.waitUntil([&] { return f.has(".sampler-key[key=\"73\"]", "outside"); }));
	CHECK(!f.has(".sampler-key[key=\"72\"]", "outside"));

	// Dragging the start marker to the middle of the display sets Start, in one undo step.
	const Json display = f.box("#sampler-display");
	const double x = display.at("x").get<double>();
	const double width = display.at("width").get<double>();
	const double y = display.at("y").get<double>() + (display.at("height").get<double>() / 2);
	const size_t steps = f.app.call("history.get").at("steps").size();
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"Start\"]"),
		{"", 0, ui::Point{static_cast<int>(x + (width / 2)), static_cast<int>(y)}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Start") - 0.5) < 0.02; }));
	CHECK(f.app.call("history.get").at("steps").size() == steps + 1);
	const double left = f.box(".sampler-marker[marker=\"Start\"]").at("x").get<double>() +
		(f.box(".sampler-marker[marker=\"Start\"]").at("width").get<double>() / 2);
	CHECK_THAT(left, WithinAbs(x + (width / 2), 3.0));
	// The end marker can't go before the start.
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"End\"]"),
		{"", 0, ui::Point{static_cast<int>(x + 2), static_cast<int>(y)}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("End") - f.parameter("Start")) < 1e-9; }));

	// A loop mode shows the loop.
	f.set("Loop mode", 1.0);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".sampler-loop", "off"); }));
}

TEST_CASE("the sampler panel loads dropped files and normalizes the sample", "[ui]")
{
	Fixture f;
	f.open();
	const uint64_t before = f.samplerChannel().at("sample").get<uint64_t>();
	CHECK(!f.app.ui().drop(std::nullopt, of(".sampler-display"), {f.other}).at("element").is_null());
	REQUIRE(f.app.waitUntil([&] { return f.samplerChannel().at("sample") != before; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#sampler-file").at(0).value("text", "") == "other"; }));

	// The sine's peak is half the full level: Normalize Sample raises it by 6 dB.
	f.click("#sampler-normalize");
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Sample gain") - 6.02) < 0.1; }));
}

TEST_CASE("Add Sampler Track opens the add track dialog on its Sampler page", "[ui]")
{
	Fixture f;
	f.select(f.audio);
	f.app.keys().run("project.addSamplerTrack", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#add-track-dialog").empty(); }));
	CHECK(f.elements("#track-types").at(0).at("attributes").at("value") == "sampler");
	CHECK(f.elements("#track-instrument").empty());
	f.click("#add-track-confirm");
	REQUIRE(f.app.waitUntil([&] { return f.elements("#add-track-dialog").empty(); }));
	const Json project = f.app.call("project.get");
	// Below the selected audio track, named as sampler.create names it.
	REQUIRE(project.at("tracks").size() == 3);
	const Json& added = project.at("tracks").at(1);
	CHECK(added.at("name") == "Sampler 02");
	CHECK(added.at("type") == "instrument");
	for (const Json& each : project.at("channels"))
		if (each.at("id") == added.at("channel"))
			CHECK(each.at("instrument") == "sampler");
}
