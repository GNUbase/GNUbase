// SPDX-License-Identifier: GPL-3.0-or-later
// The sampler panel and Add Sampler Track in the app (#1029, docs/features/sampler-track.md), driven with
// ui::Automation as MCP's UI tools drive it: the panel follows the selected sampler track, its controls, markers and
// keyboard set the sampler's parameters, a dropped file loads into it, Normalize Sample sets its gain, and the add
// track dialog's Sampler page adds sampler tracks. tests/sampler_tests.cpp checks the engine's side.
#include "testutils/commandholds.h"
#include "uiapp.h"

#include "engine/engine.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/docking.h"
#include "ui/enginebinding.h"
#include "ui/rmlui.h"
#include "ui/shell.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>

#include <SDL3/SDL.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
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
		explicit Fixture(std::vector<std::unique_ptr<midi::Backend>> backends = midi::makeBackends())
			: app({1200, 800}, std::move(backends))
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

		// The display's waveform in the main window.
		ui::Waveform* wave()
		{
			Rml::Context& context = app.main().context();
			for (int i = 0; i < context.GetNumDocuments(); ++i)
				if (auto* found = dynamic_cast<ui::Waveform*>(context.GetDocument(i)->QuerySelector(".sampler-wave")))
					return found;
			return nullptr;
		}

		// The waveform's peaks span the frames it shows, from its start for its scale's width.
		bool peaksSpanShown()
		{
			ui::Waveform* shown = wave();
			if (shown == nullptr || shown->channels().empty() || shown->GetContext() == nullptr)
				return false;
			const auto start = shown->GetAttribute<double>("start", -1.0);
			const double end = start +
				(shown->GetAttribute<double>("scale", 0.0) * shown->GetClientWidth() /
					shown->GetContext()->GetDensityIndependentPixelRatio());
			const ui::TimeSpan& span = shown->span();
			return span.start <= start && span.start > start - 1.0 && span.end() >= end - 1e-6 &&
				span.end() < end + 1.0;
		}

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
			REQUIRE(app.waitUntil([&] { return !elements("#sampler-panel [parameter]").empty(); }));
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
		test::UiApp app;
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
	CHECK(f.elements("#sampler-panel [parameter]").size() == 37);
	CHECK(!f.elements(".sampler-section[section=keyboard] " + f.control("Root key")).empty());
	CHECK(!f.elements(".sampler-section[section=pitch] " + f.control("Glide")).empty());
	CHECK(!f.elements(".sampler-section[section=playback] " + f.control("Loop crossfade")).empty());
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

TEST_CASE("the bars between the markers move start and end, or the loop, together", "[ui]")
{
	Fixture f;
	f.set("Start", 0.2);
	f.set("End", 0.4);
	f.set("Loop mode", 1.0);
	f.set("Loop start", 0.05);
	f.set("Loop end", 0.15);
	f.open();
	REQUIRE(f.app.waitUntil([&] { return !f.has(".sampler-bar[bar=loop]", "off"); }));
	const Json display = f.box("#sampler-display");
	const double x = display.at("x").get<double>();
	const double width = display.at("width").get<double>();
	const auto centre = [&](const std::string& selector)
	{
		const Json bar = f.box(selector);
		return ui::Point{static_cast<int>(bar.at("x").get<double>() + (bar.at("width").get<double>() / 2)),
			static_cast<int>(bar.at("y").get<double>() + (bar.at("height").get<double>() / 2))};
	};
	// The bar lies between its markers.
	CHECK_THAT(f.box(".sampler-bar[bar=range]").at("x").get<double>(), WithinAbs(x + (0.2 * width), 2.0));
	CHECK_THAT(f.box(".sampler-bar[bar=range]").at("width").get<double>(), WithinAbs(0.2 * width, 2.0));

	// Dragging it keeps the distance, in one undo step.
	const size_t steps = f.app.call("history.get").at("steps").size();
	const ui::Point from = centre(".sampler-bar[bar=range]");
	f.app.ui().drag(std::nullopt, of(".sampler-bar[bar=range]"),
		{"", 0, ui::Point{from.x + static_cast<int>(0.3 * width), from.y}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Start") - 0.5) < 0.01; }));
	CHECK_THAT(f.parameter("End") - f.parameter("Start"), WithinAbs(0.2, 1e-9));
	CHECK(f.app.call("history.get").at("steps").size() == steps + 1);
	REQUIRE(f.app.waitUntil(
		[&] { return std::abs(f.box(".sampler-bar[bar=range]").at("x").get<double>() - (x + (0.5 * width))) < 2.0; }));

	// It stops at the end of the sample.
	const ui::Point moved = centre(".sampler-bar[bar=range]");
	f.app.ui().drag(std::nullopt, of(".sampler-bar[bar=range]"),
		{"", 0, ui::Point{moved.x + static_cast<int>(0.35 * width), moved.y}});
	REQUIRE(f.app.waitUntil([&] { return f.parameter("End") == 1.0; }));
	CHECK_THAT(f.parameter("Start"), WithinAbs(0.8, 1e-9));

	// The loop's bar moves the loop.
	const ui::Point loop = centre(".sampler-bar[bar=loop]");
	f.app.ui().drag(
		std::nullopt, of(".sampler-bar[bar=loop]"), {"", 0, ui::Point{loop.x + static_cast<int>(0.3 * width), loop.y}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Loop start") - 0.35) < 0.01; }));
	CHECK_THAT(f.parameter("Loop end") - f.parameter("Loop start"), WithinAbs(0.1, 1e-9));

	// Without a loop mode the loop's bar is gone with the loop.
	f.set("Loop mode", 0.0);
	REQUIRE(f.app.waitUntil([&] { return f.has(".sampler-bar[bar=loop]", "off"); }));
}

TEST_CASE("the display's ruler shows the sample's time in the format chosen, and its sliders zoom", "[ui]")
{
	// Reference manual p. 717 (#1122): the ruler over the display, its format a user setting, and the zoom sliders.
	Fixture f;
	f.set("Start", 0.5);
	f.open();
	const Json display = f.box("#sampler-display");
	const double x = display.at("x").get<double>();
	const double width = display.at("width").get<double>();
	const double y = display.at("y").get<double>() + (display.at("height").get<double>() / 2);
	const Json ruler = f.box(".sampler-ruler");
	CHECK_THAT(ruler.at("x").get<double>(), WithinAbs(x, 1.0));
	CHECK_THAT(ruler.at("width").get<double>(), WithinAbs(width, 1.0));
	CHECK(ruler.at("y").get<double>() < display.at("y").get<double>());
	const auto attribute = [&](const std::string& selector, const char* name)
	{ return f.elements(selector).at(0).at("attributes").value(name, std::string()); };
	CHECK(attribute(".sampler-ruler", "format") == "seconds");
	CHECK(std::stod(attribute(".sampler-ruler", "start")) == 0.0);
	// Its scale follows its width once it is laid out.
	REQUIRE(f.app.waitUntil([&] { return !attribute(".sampler-ruler", "scale").empty(); }));
	const double full = std::stod(attribute(".sampler-ruler", "scale"));
	CHECK(full > 0.0);

	// The format: the next one, samples, is the user setting.
	f.click("#sampler-ruler-format");
	f.app.ui().key(std::nullopt, "Down");
	f.click("#sampler-ruler-format");
	REQUIRE(f.app.waitUntil([&] { return attribute(".sampler-ruler", "format") == "samples"; }));
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return f.app.call("settings.get", {{"key", "ui.samplerPanel"}})
					   .at("value")
					   .value("rulerFormat", std::string()) == "samples";
		}));

	const auto slide = [&](const char* id, double fraction, bool vertical = false)
	{
		const Json box = f.box(id);
		const double from = box.at("x").get<double>() + (box.at("width").get<double>() * 0.5);
		const double top = box.at("y").get<double>() + (box.at("height").get<double>() * 0.5);
		f.app.ui().drag(std::nullopt, {"", 0, ui::Point{static_cast<int>(from), static_cast<int>(top)}},
			{"", 0,
				ui::Point{static_cast<int>(from + (vertical ? 0 : box.at("width").get<double>() * fraction)),
					static_cast<int>(top + (vertical ? box.at("height").get<double>() * fraction : 0))}});
		f.app.shell().step(std::chrono::milliseconds(0));
	};
	// Zoomed in around the middle: Start stays there, End goes out of sight and the ruler shows less.
	slide("#sampler-horizontal-zoom", 0.3);
	REQUIRE(f.app.waitUntil([&] { return std::stod(attribute(".sampler-ruler", "scale")) < full / 2; }));
	CHECK(std::stod(attribute(".sampler-ruler", "start")) > 0.0);
	REQUIRE(f.app.waitUntil([&] { return f.peaksSpanShown(); }));
	CHECK(f.wave()->span().length < 24000.0 / 2);
	// Below the finest cached peak level, columns follow the samples rather than repeating a block's peak.
	REQUIRE(f.app.waitUntil(
		[&]
		{
			if (!f.peaksSpanShown())
				return false;
			const auto& high = f.wave()->channels().at(0).high;
			return !high.empty() && f.wave()->span().length / static_cast<double>(high.size()) < 256 &&
				*std::max_element(high.begin(), high.end()) - *std::min_element(high.begin(), high.end()) > 4000;
		}));
	const auto& high = f.wave()->channels().at(0).high;
	REQUIRE(!high.empty());
	CHECK(f.wave()->span().length / static_cast<double>(high.size()) < 256);
	CHECK(*std::max_element(high.begin(), high.end()) - *std::min_element(high.begin(), high.end()) > 4000);
	const auto centre = [&](const std::string& marker)
	{
		const Json box = f.box(".sampler-marker[marker=\"" + marker + "\"]");
		return box.at("x").get<double>() + (box.at("width").get<double>() / 2);
	};
	CHECK_THAT(centre("Start"), WithinAbs(x + (width / 2), 3.0));
	CHECK(f.has(".sampler-marker[marker=\"End\"]", "out-of-view"));
	CHECK(!f.has(".sampler-marker[marker=\"Start\"]", "out-of-view"));
	// A marker's drag counts in the part shown.
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"Start\"]"),
		{"", 0, ui::Point{static_cast<int>(x + (width * 0.75)), static_cast<int>(y)}});
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Start") > 0.5; }));
	CHECK(f.parameter("Start") < 0.7);
	CHECK_THAT(centre("Start"), WithinAbs(x + (width * 0.75), 3.0));

	// Scrolled to the start, the ruler starts at the sample's.
	slide("#sampler-scroll", -1.0);
	REQUIRE(f.app.waitUntil([&] { return std::stod(attribute(".sampler-ruler", "start")) == 0.0; }));
	CHECK(f.has(".sampler-marker[marker=\"Start\"]", "out-of-view"));
	REQUIRE(f.app.waitUntil([&] { return f.peaksSpanShown() && f.wave()->span().start == 0.0; }));

	// The vertical zoom raises the waveform's gain.
	CHECK(std::stod(attribute(".sampler-wave", "gain")) == 1.0);
	slide("#sampler-vertical-zoom", 0.3, true);
	REQUIRE(f.app.waitUntil([&] { return std::stod(attribute(".sampler-wave", "gain")) > 1.0; }));

	// Another sample shows whole again, at the gain of 1.
	f.app.call("sampler.load", {{"track", f.track}, {"path", f.other.u8string()}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return std::stod(attribute(".sampler-ruler", "start")) == 0.0 &&
				std::abs(std::stod(attribute(".sampler-ruler", "scale")) - full) < full * 1e-6 &&
				std::stod(attribute(".sampler-wave", "gain")) == 1.0;
		}));
	REQUIRE(f.app.waitUntil([&] { return f.peaksSpanShown() && f.wave()->span().length == 24000.0; }));
}

TEST_CASE("the display's ruler shows the format of the user setting", "[ui]")
{
	// Reference manual p. 717 (#1122): Bars+Beats set with settings.set shows on the ruler and in its drop-down.
	Fixture f;
	f.app.call("settings.set", {{"key", "ui.samplerPanel"}, {"value", {{"rulerFormat", "barsBeats"}}}});
	f.open();
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json ruler = f.elements(".sampler-ruler");
			const Json format = f.elements("#sampler-ruler-format");
			return ruler.size() == 1 && ruler[0].at("attributes").value("format", std::string()) == "barsBeats" &&
				format.size() == 1 && format[0].at("attributes").value("value", std::string()) == "barsBeats";
		}));
}

TEST_CASE("the markers of the fades and the loop crossfade set their lengths", "[ui]")
{
	Fixture f;
	f.set("Start", 0.2);
	f.set("End", 0.8);
	f.set("Loop mode", 1.0);
	f.set("Loop start", 0.4);
	f.set("Loop end", 0.6);
	f.open();
	REQUIRE(f.app.waitUntil([&] { return !f.has(".sampler-marker[marker=\"Loop crossfade\"]", "off"); }));
	const Json display = f.box("#sampler-display");
	const double x = display.at("x").get<double>();
	const double width = display.at("width").get<double>();
	const auto centre = [&](const std::string& name)
	{
		const Json marker = f.box(".sampler-marker[marker=\"" + name + "\"]");
		return std::pair<double, double>{marker.at("x").get<double>() + (marker.at("width").get<double>() / 2),
			marker.at("y").get<double>() + (marker.at("height").get<double>() / 2)};
	};
	const auto dragTo = [&](const std::string& name, double share)
	{
		f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"" + name + "\"]"),
			{"", 0, ui::Point{static_cast<int>(x + (share * width)), static_cast<int>(centre(name).second)}});
	};
	// Without fades they sit on Start and End, the crossfade's on the loop's end.
	CHECK_THAT(centre("Fade in").first, WithinAbs(x + (0.2 * width), 2.0));
	CHECK_THAT(centre("Fade out").first, WithinAbs(x + (0.8 * width), 2.0));
	CHECK_THAT(centre("Loop crossfade").first, WithinAbs(x + (0.6 * width), 2.0));
	// Start's and Loop end's markers can still be dragged from their flags where a length of 0 puts a marker on them.
	const auto dragFlag = [&](const std::string& name, double share)
	{
		const Json flag = f.box(".sampler-marker[marker=\"" + name + "\"] .sampler-marker-flag");
		const ui::Point from{static_cast<int>(flag.at("x").get<double>() + (flag.at("width").get<double>() / 2)),
			static_cast<int>(flag.at("y").get<double>() + (flag.at("height").get<double>() / 2))};
		f.app.ui().drag(std::nullopt, {"", 0, from}, {"", 0, ui::Point{static_cast<int>(x + (share * width)), from.y}});
	};
	dragFlag("Start", 0.25);
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Start") - 0.25) < 0.01; }));
	CHECK(f.parameter("Fade in") == 0.0);
	CHECK_THAT(f.parameter("End"), WithinAbs(0.8, 1e-9));
	dragFlag("Loop end", 0.65);
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Loop end") - 0.65) < 0.01; }));
	CHECK(f.parameter("Loop crossfade") == 0.0);
	CHECK_THAT(f.parameter("Loop start"), WithinAbs(0.4, 1e-9));
	f.set("Start", 0.2);
	f.set("Loop end", 0.6);
	REQUIRE(f.app.waitUntil([&] { return std::abs(centre("Fade in").first - (x + (0.2 * width))) < 3.0; }));
	REQUIRE(f.app.waitUntil([&] { return std::abs(centre("Loop crossfade").first - (x + (0.6 * width))) < 3.0; }));

	// Dragging the fade-in's marker sets its length from Start, in one undo step.
	const size_t steps = f.app.call("history.get").at("steps").size();
	dragTo("Fade in", 0.3);
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Fade in") - 0.1) < 0.01; }));
	CHECK(f.app.call("history.get").at("steps").size() == steps + 1);
	REQUIRE(f.app.waitUntil([&] { return std::abs(centre("Fade in").first - (x + (0.3 * width))) < 3.0; }));
	// The fade-out's from End; neither goes past the range.
	dragTo("Fade out", 0.6);
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Fade out") - 0.2) < 0.01; }));
	dragTo("Fade in", 0.95);
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Fade in") - 0.6) < 1e-9; }));
	// The crossfade's from the loop's end, no longer than the loop.
	dragTo("Loop crossfade", 0.5);
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Loop crossfade") - 0.1) < 0.01; }));
	dragTo("Loop crossfade", 0.05);
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Loop crossfade") - 0.2) < 1e-9; }));

	// Reversed, they are mirrored like the others.
	f.set("Reverse", 1.0);
	REQUIRE(f.app.waitUntil([&] { return std::abs(centre("Fade out").first - (x + (0.4 * width))) < 3.0; }));
	// Without a loop mode the crossfade's marker is gone with the loop.
	f.set("Loop mode", 0.0);
	REQUIRE(f.app.waitUntil([&] { return f.has(".sampler-marker[marker=\"Loop crossfade\"]", "off"); }));
	CHECK(!f.has(".sampler-marker[marker=\"Fade in\"]", "off"));
}

TEST_CASE("the loop crossfade's marker goes no farther than the sampler's crossfade", "[ui]")
{
	// Between Start and the loop's start lies less than the loop: the sampler crossfades no more than that.
	Fixture f;
	f.set("Start", 0.3);
	f.set("End", 0.8);
	f.set("Loop mode", 1.0);
	f.set("Loop start", 0.4);
	f.set("Loop end", 0.6);
	f.set("Loop crossfade", 0.2);
	f.open();
	const std::string selector = ".sampler-marker[marker=\"Loop crossfade\"]";
	REQUIRE(f.app.waitUntil([&] { return !f.has(selector, "off"); }));
	const Json display = f.box("#sampler-display");
	const double x = display.at("x").get<double>();
	const double width = display.at("width").get<double>();
	const auto centre = [&]
	{
		const Json marker = f.box(selector);
		return std::pair<double, double>{marker.at("x").get<double>() + (marker.at("width").get<double>() / 2),
			marker.at("y").get<double>() + (marker.at("height").get<double>() / 2)};
	};
	REQUIRE(f.app.waitUntil([&] { return std::abs(centre().first - (x + (0.5 * width))) < 3.0; }));
	f.set("Loop crossfade", 0.0);
	REQUIRE(f.app.waitUntil([&] { return std::abs(centre().first - (x + (0.6 * width))) < 3.0; }));
	f.app.ui().drag(std::nullopt, of(selector),
		{"", 0, ui::Point{static_cast<int>(x + (0.35 * width)), static_cast<int>(centre().second)}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Loop crossfade") - 0.1) < 1e-9; }));
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

TEST_CASE("MIDI Reset sends All Sound Off and Reset All Controllers to the sampler on every channel", "[ui]")
{
	Fixture f;
	f.open();
	f.app.call("audio.setDevice", {{"blockSize", 128}, {"inputChannels", 0}, {"captureSeconds", 1}});
	f.app.call("audio.render", {{"frames", 128}}); // the new graph
	f.app.call("midi.capture", {{"start", true}});
	f.click("#sampler-midi-reset");
	std::vector<std::vector<int>> played;
	CHECK(f.app.waitUntil(
		[&]
		{
			f.app.call("audio.render", {{"frames", 128}});
			const Json captured = f.app.call("midi.capture");
			for (const Json& message : captured.at("captured"))
				if (message.at("channel") == f.channel)
					played.push_back(message.at("bytes").get<std::vector<int>>());
			return played.size() >= 32;
		}));
	std::vector<std::vector<int>> expected;
	for (int channel = 0; channel < 16; ++channel)
		for (const int controller : {120, 121})
			expected.push_back({0xB0 + channel, controller, 0});
	CHECK(played == expected);
	CHECK(f.elements("#sampler-message").at(0).value("text", "").empty());
}

TEST_CASE(
	"Snap to Zero Crossing ends a marker's or a bar's drag where the sample crosses zero, in the drag's undo step",
	"[ui]")
{
	Fixture f;
	f.open();
	const Json display = f.box("#sampler-display");
	const double x = display.at("x").get<double>();
	const double width = display.at("width").get<double>();
	const int y = static_cast<int>(display.at("y").get<double>() + (display.at("height").get<double>() / 2));
	const auto dragStart = [&](double share)
	{
		f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"Start\"]"),
			{"", 0, ui::Point{static_cast<int>(x + (share * width)), y}});
	};
	// The fixture's sine, sin(frame * 0.05), crosses zero every 20 pi frames.
	const auto onCrossing = [](double share)
	{
		const double frame = share * 24000.0;
		const double period = 20.0 * 3.14159265358979323846;
		return std::abs(frame - (std::round(frame / period) * period)) <= 1.0;
	};
	const auto setting = [&] { return f.app.call("settings.get", {{"key", "ui.samplerPanel"}}).at("value"); };
	dragStart(0.3);
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Start") - 0.3) < 0.01; }));

	f.click("#sampler-zero");
	REQUIRE(f.app.waitUntil([&] { return setting().value("snapToZero", false); }));
	CHECK(!f.elements("#sampler-zero[checked]").empty());
	const size_t steps = f.app.call("history.get").at("steps").size();
	dragStart(0.4);
	REQUIRE(f.app.waitUntil(
		[&] { return std::abs(f.parameter("Start") - 0.4) < 0.01 && onCrossing(f.parameter("Start")); }));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.app.call("history.get").at("steps").size() == steps + 1);
	// End stays after Start.
	CHECK(f.parameter("End") >= f.parameter("Start"));

	// A bar's drag ends with both its markers on zero crossings, in the same undo step.
	f.set("End", 0.6);
	const std::string range = ".sampler-bar[bar=range]";
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json bar = f.box(range);
			return std::abs(bar.at("x").get<double>() + bar.at("width").get<double>() - (x + (0.6 * width))) < 2.0;
		}));
	const size_t barSteps = f.app.call("history.get").at("steps").size();
	const Json bar = f.box(range);
	const int barX = static_cast<int>(bar.at("x").get<double>() + (bar.at("width").get<double>() / 2));
	const int barY = static_cast<int>(bar.at("y").get<double>() + (bar.at("height").get<double>() / 2));
	f.app.ui().drag(std::nullopt, of(range), {"", 0, ui::Point{barX + static_cast<int>(0.1 * width), barY}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			return std::abs(f.parameter("Start") - 0.5) < 0.01 && onCrossing(f.parameter("Start")) &&
				std::abs(f.parameter("End") - 0.7) < 0.01 && onCrossing(f.parameter("End"));
		}));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.app.call("history.get").at("steps").size() == barSteps + 1);

	// Off again, the setting says so.
	f.click("#sampler-zero");
	REQUIRE(f.app.waitUntil([&] { return !setting().value("snapToZero", true); }));
}

TEST_CASE("the event received indicator lights up at notes and controllers from the track's MIDI input", "[ui]")
{
	auto backend = std::make_unique<midi::DummyBackend>();
	backend->addLoopback("Keys");
	midi::DummyBackend& dummy = *backend;
	std::vector<std::unique_ptr<midi::Backend>> backends;
	backends.push_back(std::move(backend));
	Fixture f(std::move(backends));
	f.open();
	// RmlUi's clock stands still, so that the indicator stays lit until the test moves it on.
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true);
	const std::shared_ptr<void> thaw(nullptr, [clock](void*) { clock->freeze(false); });
	const std::unique_ptr<midi::Output> device = dummy.openOutput("Keys");
	// The device plays a message, and the panel hears of it (midi.activity, which offline the test runs).
	const auto play = [&](const std::vector<uint8_t>& bytes)
	{
		audio::MidiMessage message;
		message.size = static_cast<uint8_t>(bytes.size());
		std::copy(bytes.begin(), bytes.end(), message.bytes.begin());
		device->send(message, hostTime() - 1000000000);
		if (!f.app.call("audio.get").at("running").get<bool>())
		{
			f.app.call("audio.render", {{"frames", 128}});
			f.app.call("midi.activity");
		}
		for (int i = 0; i < 3; ++i)
			f.app.shell().step(std::chrono::milliseconds(0));
	};
	const auto lit = [&] { return f.has("#sampler-received", "lit"); };
	const auto putOut = [&]
	{
		clock->advance(1.0);
		return f.app.waitUntil([&] { return !lit(); });
	};
	CHECK(!lit());
	// A note-off doesn't light it, a note-on and a controller do, until it goes out on its own.
	play({0x80, 60, 0});
	CHECK(!lit());
	play({0x90, 60, 100});
	CHECK(lit());
	CHECK(putOut());
	play({0xB3, 1, 64});
	CHECK(lit());
	CHECK(putOut());

	// An input that isn't among all MIDI inputs, which the track takes, doesn't light it.
	std::string id;
	const Json ports = f.app.call("midi.ports");
	for (const Json& port : ports.at("ports"))
		if (port.at("input") == true && port.at("name") == "Keys")
			id = port.at("id").get<std::string>();
	REQUIRE(!id.empty());
	f.app.call("midi.setPort", {{"id", id}, {"input", true}, {"inAllInputs", false}});
	for (int i = 0; i < 3; ++i)
		f.app.shell().step(std::chrono::milliseconds(0));
	play({0x90, 62, 100});
	CHECK(!lit());
}

TEST_CASE("Add Sampler Track opens the add track dialog on its Sampler page", "[ui]")
{
	Fixture f;
	f.select(f.audio);
	f.app.keys().run("project.addSamplerTrack", Json::object(), f.app.main(), nullptr);
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#add-track-dialog").empty(); }));
	CHECK(f.elements("#track-types").at(0).at("attributes").at("value") == "sampler");
	CHECK(f.elements("#track-instrument").empty());
	// Input Routing (p. 144): All MIDI Inputs, then Chord Pads and Not Connected.
	CHECK(f.elements("#track-midi-input").at(0).value("value", "") == "all");
	f.click("#track-midi-input");
	f.app.ui().key(std::nullopt, "Down");
	f.app.ui().key(std::nullopt, "Down");
	f.click("#track-midi-input");
	REQUIRE(f.elements("#track-midi-input").at(0).value("value", "") == "none");
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
		{
			CHECK(each.at("instrument") == "sampler");
			CHECK(each.at("midiInput") == "none");
		}
}

TEST_CASE("Read Automation in the sampler panel's toolbar reads all of the track's automation lanes", "[ui]")
{
	Fixture f;
	f.open();
	// Without lanes there is nothing to read.
	const auto disabled = [&] { return f.elements("#sampler-read").at(0).at("attributes").contains("disabled"); };
	CHECK(disabled());
	const auto lanes = [&]
	{
		std::vector<bool> read;
		const Json project = f.app.call("project.get");
		for (const Json& lane : project.at("automation"))
			if (lane.at("track") == f.track)
				read.push_back(lane.at("read").get<bool>());
		return read;
	};
	f.app.call("automationLane.add", {{"track", f.track}, {"target", {{"type", "volume"}}}});
	f.app.call("automationLane.add", {{"track", f.track}, {"target", {{"type", "pan"}}}, {"read", false}});
	REQUIRE(f.app.waitUntil([&] { return !disabled(); }));
	CHECK(f.elements("#sampler-read[checked]").empty()); // not every lane reads

	const size_t steps = f.app.call("history.get").at("steps").size();
	f.click("#sampler-read");
	REQUIRE(f.app.waitUntil([&] { return lanes() == std::vector<bool>{true, true}; }));
	CHECK(f.app.call("history.get").at("steps").size() == steps + 1);
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#sampler-read[checked]").empty(); }));
	f.click("#sampler-read");
	REQUIRE(f.app.waitUntil([&] { return lanes() == std::vector<bool>{false, false}; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#sampler-read[checked]").empty(); }));

	// A lane read elsewhere shows.
	f.app.call("history.undo");
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#sampler-read[checked]").empty(); }));
}

TEST_CASE("a change in a value field and a drag of its marker are two undo steps", "[ui]")
{
	Fixture f;
	f.open();
	const size_t steps = f.app.call("history.get").at("steps").size();
	// The session's first value field gesture, then the first marker drag: their numbers mustn't meet.
	f.app.ui().wheel(std::nullopt, of(f.control("Start")), 0.0F, -1.0F);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Start") > 0.0; }));
	REQUIRE(f.app.waitUntil([&] { return f.app.call("history.get").at("steps").size() == steps + 1; }));
	const Json display = f.box("#sampler-display");
	const double y = display.at("y").get<double>() + (display.at("height").get<double>() / 2);
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"Start\"]"),
		{"", 0,
			ui::Point{static_cast<int>(display.at("x").get<double>() + (display.at("width").get<double>() / 2)),
				static_cast<int>(y)}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Start") - 0.5) < 0.02; }));
	CHECK(f.app.call("history.get").at("steps").size() == steps + 2);
}

TEST_CASE("the sampler panel shows another track's own value while a turned one's last step is out, and back on it the "
		  "turn's",
	"[ui]")
{
	test::CommandHolds holds; // declared before the app: its engine's command thread waits in hold()
	Fixture f;
	const test::ReleaseHolds releaseHolds{holds};
	f.app.engine().registerCommand(
		{"test.hold", 1, "Waits for the test to release it.", engine::CommandInfo{}.parameters, Json::object(), false},
		[&holds](engine::CommandContext&) { return holds.hold(); });
	engine::Client holder(f.app.engine(), "holder");
	const Json created = f.app.call("sampler.create", {{"path", f.other.u8string()}});
	const uint64_t other = created.at("track").at("id").get<uint64_t>();
	f.app.call("parameter.set",
		{{"channel", created.at("channel").at("id")},
			{"target", {{"type", "instrument"}, {"parameter", f.id("Coarse")}}}, {"value", 5.0}});
	f.open();
	const std::string coarse = f.control("Coarse");
	const auto shows = [&](double value)
	{ return f.app.waitUntil([&] { return test::controlValue(f.app, std::nullopt, coarse) == value; }); };
	REQUIRE(shows(0.0));
	ui::EngineBinding& binding = f.app.appUi().binding();
	REQUIRE(f.app.waitUntil(
		[&] { return binding.waiting("history.transaction") == 0 && binding.waiting("project.get") == 0; }));

	// A wheel turn whose step waits behind a hold, so the panel's project is older than the turn (#1305). A step the
	// engine took brings its channel.changed, which the panel's project takes before the answer (#1881).
	holder.send("test.hold");
	REQUIRE(holds.waitEntered(1));
	f.app.ui().wheel(std::nullopt, of(coarse), 0.0F, -1.0F);
	f.app.shell().step(std::chrono::milliseconds(0));
	REQUIRE(binding.waiting("history.transaction") != 0);
	CHECK(test::controlValue(f.app, std::nullopt, coarse) == 1.0);

	// The other sampler track shows its own value, and back on the turned one the turn's, not the other track's
	// (#1471).
	f.select(other);
	CHECK(shows(5.0));
	f.select(f.track);
	REQUIRE(f.app.waitUntil([&] { return f.elements("#sampler-file").at(0).value("text", "") == "sine"; }));
	CHECK(test::controlValue(f.app, std::nullopt, coarse) == 1.0);
	REQUIRE(binding.waiting("history.transaction") != 0);

	// The engine's value once it took the turn agrees with it.
	holds.release(1);
	REQUIRE(f.app.waitUntil([&] { return binding.waiting("history.transaction") == 0; }));
	CHECK(f.parameter("Coarse") == 1.0);
	CHECK(shows(1.0));
}

TEST_CASE("the sampler panel shows why a sample didn't load or normalize", "[ui]")
{
	Fixture f;
	f.open();
	CHECK(!f.has("#sampler-message", "error"));
	const std::filesystem::path text = f.folder.path() / "notes.txt";
	std::ofstream(text) << "not audio";
	f.app.ui().drop(std::nullopt, of(".sampler-display"), {text});
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-message", "error"); }));
	const Json message = f.elements("#sampler-message").at(0);
	CHECK(!message.value("text", "").empty());
	CHECK(message.at("attributes").value("title", "") == message.value("text", "")); // the whole text

	// Another track shown takes the message away, as it concerned the track before.
	f.select(f.audio);
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-panel", "empty"); }));
	CHECK(!f.has("#sampler-message", "error"));
	f.select(f.track);
	REQUIRE(f.app.waitUntil([&] { return !f.has("#sampler-panel", "empty"); }));
	CHECK(f.elements("#sampler-message").at(0).value("text", "").empty());
	f.app.ui().drop(std::nullopt, of(".sampler-display"), {text});
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-message", "error"); }));

	// A file that loads clears the message.
	f.app.ui().drop(std::nullopt, of(".sampler-display"), {f.other});
	REQUIRE(f.app.waitUntil([&] { return !f.has("#sampler-message", "error"); }));
	CHECK(f.elements("#sampler-message").at(0).value("text", "").empty());

	// Normalize Sample is refused for a silent sample.
	test::Audio silence;
	silence.samples.assign(4800, 0.0f);
	const std::filesystem::path silent = f.folder.path() / "silent.wav";
	test::writeWav(silent, silence);
	f.app.ui().drop(std::nullopt, of(".sampler-display"), {silent});
	REQUIRE(f.app.waitUntil([&] { return f.elements("#sampler-file").at(0).value("text", "") == "silent"; }));
	f.click("#sampler-normalize");
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-message", "error"); }));
	CHECK(f.elements("#sampler-message").at(0).value("text", "").find("silent between its start and end") !=
		std::string::npos);
}

TEST_CASE("Lock Parameter Settings and the normalization level last when the panel is made anew", "[ui]")
{
	Fixture f;
	// What a later version adds to the setting stays in it.
	f.app.call("settings.set", {{"key", "ui.samplerPanel"}, {"value", {{"later", 1}}}});
	f.open();
	const auto setting = [&] { return f.app.call("settings.get", {{"key", "ui.samplerPanel"}}).at("value"); };
	f.click("#sampler-lock");
	REQUIRE(f.app.waitUntil([&] { return setting().value("lockParameters", false); }));
	CHECK(setting().value("later", 0) == 1);
	f.app.ui().wheel(std::nullopt, of("#sampler-level"), 0.0F, 1.0F);
	REQUIRE(f.app.waitUntil([&] { return setting().value("normalizeLevel", 0.0) < 0.0; }));
	const double step = setting().at("normalizeLevel").get<double>();
	// Changes in quick succession, as a drag of the field makes them: the older ones that settings.changed brings back
	// don't set the field back meanwhile (#1180).
	for (int i = 0; i < 20; ++i)
	{
		f.app.ui().wheel(std::nullopt, of("#sampler-level"), 0.0F, 1.0F);
		f.app.shell().step(std::chrono::milliseconds(0));
	}
	REQUIRE(f.app.waitUntil([&] { return std::abs(setting().value("normalizeLevel", 0.0) - (21 * step)) < 1e-6; }));
	const double level = setting().at("normalizeLevel").get<double>();

	// In a window of its own the panel is a new one, with the lock and the level as they were.
	f.app.appUi().docking().setFloating("sampler", true);
	const ui::Window* window = nullptr;
	REQUIRE(f.app.waitUntil(
		[&]
		{
			window = f.app.appUi().docking().windowOf("sampler");
			return window != nullptr &&
				!f.app.ui().dom(window->id(), "#sampler-lock[checked]", 0).at("elements").empty();
		}));
	REQUIRE(f.app.waitUntil([&] { return !f.app.ui().dom(window->id(), "[parameter]", 0).at("elements").empty(); }));
	const Json field = f.app.ui().dom(window->id(), "#sampler-level", 0).at("elements").at(0);
	CHECK_THAT(std::stod(field.at("attributes").at("value").get<std::string>()), WithinAbs(level, 1e-3));
	// Normalize Sample brings the sine's peak, half the full level, to the level.
	f.app.ui().click(window->id(), of("#sampler-normalize"), ui::MouseButton::Left, 1, {});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Sample gain") - (6.02 + level)) < 0.1; }));

	// A change from elsewhere right after quick changes of the panel's own, before their answers came: the field
	// shows it, as the setting holds it (#1200).
	for (int i = 0; i < 5; ++i)
	{
		f.app.ui().wheel(window->id(), of("#sampler-level"), 0.0F, 1.0F);
		f.app.shell().step(std::chrono::milliseconds(0));
	}
	f.app.call("settings.set",
		{{"key", "ui.samplerPanel"}, {"value", {{"later", 1}, {"lockParameters", true}, {"normalizeLevel", -1.5}}}});
	REQUIRE(f.app.waitUntil(
		[&]
		{
			const Json shown = f.app.ui().dom(window->id(), "#sampler-level", 0).at("elements").at(0);
			return std::abs(std::stod(shown.at("attributes").at("value").get<std::string>()) + 1.5) < 1e-3;
		}));
	CHECK(setting().at("normalizeLevel") == -1.5);
}

TEST_CASE("the keyboard's handles drag the root key and the key range", "[ui]")
{
	Fixture f;
	f.open();
	const Json keyboard = f.box("#sampler-keyboard");
	const double x = keyboard.at("x").get<double>();
	const double key = keyboard.at("width").get<double>() / 128.0;
	const auto at = [&](int pitch) { return static_cast<int>(x + ((pitch + 0.5) * key)); };
	const auto centre = [&](const std::string& selector)
	{
		const Json handle = f.box(selector);
		return handle.at("x").get<double>() + (handle.at("width").get<double>() / 2);
	};
	const auto drag = [&](const std::string& handle, int pitch)
	{
		const std::string selector = ".sampler-handle[handle=" + handle + "]";
		const Json box = f.box(selector);
		const int y = static_cast<int>(box.at("y").get<double>() + (box.at("height").get<double>() / 2));
		f.app.ui().drag(std::nullopt, of(selector), {"", 0, ui::Point{at(pitch), y}});
	};
	CHECK_THAT(centre(".sampler-handle[handle=root]"), WithinAbs(at(60), 2.0));

	// A drag of the root key handle is one undo step.
	const size_t steps = f.app.call("history.get").at("steps").size();
	drag("root", 50);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Root key") == 50.0; }));
	CHECK(f.app.call("history.get").at("steps").size() == steps + 1);
	REQUIRE(f.app.waitUntil([&] { return std::abs(centre(".sampler-handle[handle=root]") - at(50)) < 2.0; }));
	CHECK(f.has(".sampler-key[key=\"50\"]", "root"));

	// The key range's handles, the high one not below the low one.
	drag("low", 40);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key low") == 40.0; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(".sampler-key[key=\"39\"]", "outside"); }));
	drag("high", 30);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key high") == 40.0; }));
	REQUIRE(f.app.waitUntil([&] { return f.has(".sampler-key[key=\"41\"]", "outside"); }));
	// Nor the low one above the high one.
	f.set("Key high", 50.0);
	REQUIRE(f.app.waitUntil([&] { return !f.has(".sampler-key[key=\"50\"]", "outside"); }));
	drag("low", 70);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key low") == 50.0; }));

	// On the same key the three handles lie side by side, so each can be grabbed (#1180).
	f.set("Root key", 50.0);
	REQUIRE(f.app.waitUntil([&] { return std::abs(centre(".sampler-handle[handle=root]") - at(50)) < 2.0; }));
	const std::vector<std::string> handles = {"root", "low", "high"};
	for (size_t i = 0; i < handles.size(); ++i)
		for (size_t j = i + 1; j < handles.size(); ++j)
		{
			const Json a = f.box(".sampler-handle[handle=" + handles[i] + "]");
			const Json b = f.box(".sampler-handle[handle=" + handles[j] + "]");
			const auto apart = [](const Json& first, const Json& second, const char* position, const char* size)
			{
				return first.at(position).get<double>() + first.at(size).get<double>() <=
					second.at(position).get<double>() + 1.0; // rounded to whole pixels
			};
			INFO(handles[i] << " and " << handles[j]);
			CHECK((apart(a, b, "x", "width") || apart(b, a, "x", "width") || apart(a, b, "y", "height") ||
				apart(b, a, "y", "height")));
		}
	drag("high", 60);
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key high") == 60.0; }));
	CHECK(f.parameter("Key low") == 50.0);
}

TEST_CASE("on a narrow keyboard the handles lie apart too", "[ui]")
{
	Fixture f;
	f.open();
	// In a narrow window of its own, as in a narrow zone, a key is narrower than 4dp.
	f.app.appUi().docking().setFloating("sampler", true);
	const ui::Window* window = nullptr;
	REQUIRE(f.app.waitUntil(
		[&]
		{
			window = f.app.appUi().docking().windowOf("sampler");
			return window != nullptr && !f.app.ui().dom(window->id(), "[parameter]", 0).at("elements").empty();
		}));
	f.app.shell().setBounds(window->id(), {100, 100, 320, 600});
	const auto box = [&](const std::string& selector)
	{ return f.app.ui().dom(window->id(), selector, 0).at("elements").at(0).at("box"); };
	REQUIRE(f.app.waitUntil([&] { return box("#sampler-keyboard").at("width").get<double>() < 4.0 * 128; }));

	// The three handles on one key, and the range's on keys side by side: none over another (#1200).
	for (const auto& range : {std::pair{50.0, 50.0}, std::pair{50.0, 51.0}})
	{
		const double low = range.first;
		const double high = range.second;
		f.set("Key high", high);
		f.set("Key low", low);
		f.set("Root key", low);
		const auto on = [&](const std::string& handle, double key)
		{
			const Json keyboard = box("#sampler-keyboard");
			const double x = keyboard.at("x").get<double>() + (key * keyboard.at("width").get<double>() / 128.0);
			return std::abs(box(".sampler-handle[handle=" + handle + "]").at("x").get<double>() - x) < 1.0;
		};
		REQUIRE(f.app.waitUntil([&] { return on("root", low) && on("low", low) && on("high", high); }));
		const std::vector<std::string> handles = {"root", "low", "high"};
		for (size_t i = 0; i < handles.size(); ++i)
			for (size_t j = i + 1; j < handles.size(); ++j)
			{
				const Json a = box(".sampler-handle[handle=" + handles[i] + "]");
				const Json b = box(".sampler-handle[handle=" + handles[j] + "]");
				const auto apart = [](const Json& first, const Json& second, const char* position, const char* size)
				{
					return first.at(position).get<double>() + first.at(size).get<double>() <=
						second.at(position).get<double>() + 1.0; // rounded to whole pixels
				};
				INFO(handles[i] << " and " << handles[j] << " on " << low << " and " << high);
				CHECK((apart(a, b, "x", "width") || apart(b, a, "x", "width") || apart(a, b, "y", "height") ||
					apart(b, a, "y", "height")));
			}
	}
}

TEST_CASE("a drag of a handle or a marker ends when the button goes up outside the window", "[ui]")
{
	Fixture f;
	f.open();
	const Json keyboard = f.box("#sampler-keyboard");
	const double x = keyboard.at("x").get<double>();
	const double key = keyboard.at("width").get<double>() / 128.0;
	const auto at = [&](int pitch) { return static_cast<int>(x + ((pitch + 0.5) * key)); };
	const auto handleY = [&]
	{
		const Json box = f.box(".sampler-handle[handle=low]");
		return static_cast<int>(box.at("y").get<double>() + (box.at("height").get<double>() / 2));
	};
	f.app.ui().drag(std::nullopt, of(".sampler-handle[handle=low]"), {"", 0, ui::Point{at(40), handleY()}});
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key low") == 40.0; }));

	// Dragged past the window's left edge, where the button goes up: the lowest key.
	f.app.ui().drag(std::nullopt, of(".sampler-handle[handle=low]"), {"", 0, ui::Point{-40, handleY()}});
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key low") == 0.0; }));
	// The mouse back over the keyboard with no button held moves the handle no more.
	f.app.ui().move(std::nullopt, {"", 0, ui::Point{at(60), handleY()}});
	f.app.ui().move(std::nullopt, {"", 0, ui::Point{at(70), handleY()}});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.parameter("Key low") == 0.0);
	CHECK(!f.has(".sampler-handle[handle=low]", "dragged"));

	// The same for the display's markers: Start dragged past the left edge, then the mouse over the display.
	const Json display = f.box("#sampler-display");
	const int y = static_cast<int>(display.at("y").get<double>() + (display.at("height").get<double>() / 2));
	const auto across = [&](double share)
	{ return static_cast<int>(display.at("x").get<double>() + (display.at("width").get<double>() * share)); };
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"Start\"]"), {"", 0, ui::Point{across(0.3), y}});
	REQUIRE(f.app.waitUntil([&] { return std::abs(f.parameter("Start") - 0.3) < 0.02; }));
	f.app.ui().drag(std::nullopt, of(".sampler-marker[marker=\"Start\"]"), {"", 0, ui::Point{-40, y}});
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Start") == 0.0; }));
	f.app.ui().move(std::nullopt, {"", 0, ui::Point{across(0.5), y}});
	f.app.ui().move(std::nullopt, {"", 0, ui::Point{across(0.6), y}});
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.parameter("Start") == 0.0);
	CHECK(!f.has(".sampler-marker[marker=\"Start\"]", "dragged"));
}

TEST_CASE("a drag of a handle ends when its window loses the focus", "[ui]")
{
	Fixture f;
	f.open();
	const Json keyboard = f.box("#sampler-keyboard");
	const double x = keyboard.at("x").get<double>();
	const double key = keyboard.at("width").get<double>() / 128.0;
	const auto at = [&](int pitch) { return static_cast<float>(x + ((pitch + 0.5) * key)); };
	const Json box = f.box(".sampler-handle[handle=low]");
	const auto y = static_cast<float>(box.at("y").get<double>() + (box.at("height").get<double>() / 2));
	// Through SDL's own events, as the OS sends them: the button down on the low key's handle, a drag to key 40.
	const auto push = [&](SDL_EventType type, float to)
	{
		SDL_Event event{};
		event.type = type;
		if (type == SDL_EVENT_MOUSE_MOTION)
		{
			event.motion.windowID = f.app.main().id();
			event.motion.x = to;
			event.motion.y = y;
			event.motion.state = SDL_BUTTON_LMASK;
		}
		else if (type == SDL_EVENT_WINDOW_FOCUS_LOST)
		{
			event.window.windowID = f.app.main().id();
		}
		else
		{
			event.button.windowID = f.app.main().id();
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			event.button.clicks = 1;
			event.button.x = to;
			event.button.y = y;
		}
		REQUIRE(SDL_PushEvent(&event));
		f.app.shell().step(std::chrono::milliseconds(0));
	};
	push(SDL_EVENT_MOUSE_MOTION, at(0));
	push(SDL_EVENT_MOUSE_BUTTON_DOWN, at(0));
	for (int pitch = 5; pitch <= 40; pitch += 5)
		push(SDL_EVENT_MOUSE_MOTION, at(pitch));
	REQUIRE(f.app.waitUntil([&] { return f.parameter("Key low") == 40.0; }));
	REQUIRE(f.has(".sampler-handle[handle=low]", "dragged"));

	// The focus goes, e.g. to Alt+Tab, and the button goes up elsewhere: the drag ends (#1191).
	push(SDL_EVENT_WINDOW_FOCUS_LOST, at(40));
	REQUIRE(f.app.waitUntil([&] { return !f.has(".sampler-handle[handle=low]", "dragged"); }));
	push(SDL_EVENT_MOUSE_MOTION, at(60));
	push(SDL_EVENT_MOUSE_MOTION, at(70));
	f.app.shell().step(std::chrono::milliseconds(0));
	CHECK(f.parameter("Key low") == 40.0);
}

TEST_CASE("Import Audio File loads the file chosen, with the lock, and shows why one didn't load", "[ui]")
{
	Fixture f;
	f.open();
	const auto answer = [&](const std::filesystem::path& file)
	{
		f.click("#sampler-import");
		REQUIRE(f.app.waitUntil([&] { return f.app.shell().fileDialogs().size() == 1; }));
		CHECK(f.app.shell().answerFileDialog(f.app.shell().fileDialogs().begin()->first, {file}));
	};
	f.click("#sampler-lock");
	REQUIRE(f.app.waitUntil([&] { return !f.elements("#sampler-lock[checked]").empty(); }));
	f.set("Cutoff", 1000.0);

	// A file that isn't audio: the panel says why, and the sample stays.
	const uint64_t before = f.samplerChannel().at("sample").get<uint64_t>();
	const std::filesystem::path text = f.folder.path() / "notes.txt";
	std::ofstream(text) << "not audio";
	answer(text);
	REQUIRE(f.app.waitUntil([&] { return f.has("#sampler-message", "error"); }));
	CHECK(!f.elements("#sampler-message").at(0).value("text", "").empty());
	CHECK(f.samplerChannel().at("sample") == before);

	// An audio file loads, keeping the settings as the lock asks, and the message goes.
	answer(f.other);
	REQUIRE(f.app.waitUntil([&] { return f.samplerChannel().at("sample") != before; }));
	REQUIRE(f.app.waitUntil([&] { return f.elements("#sampler-file").at(0).value("text", "") == "other"; }));
	CHECK(f.parameter("Cutoff") == 1000.0);
	REQUIRE(f.app.waitUntil([&] { return !f.has("#sampler-message", "error"); }));
	CHECK(f.elements("#sampler-message").at(0).value("text", "").empty());

	// Closed while the dialog is open, the panel can't show why: the file loads all the same, with the lock.
	f.set("Cutoff", 2000.0);
	f.click("#sampler-import");
	REQUIRE(f.app.waitUntil([&] { return f.app.shell().fileDialogs().size() == 1; }));
	f.app.appUi().docking().setTabHidden("sampler", true);
	REQUIRE(f.app.waitUntil([&] { return f.elements("#sampler-panel").empty(); }));
	const uint64_t loaded = f.samplerChannel().at("sample").get<uint64_t>();
	CHECK(f.app.shell().answerFileDialog(f.app.shell().fileDialogs().begin()->first, {f.file}));
	REQUIRE(f.app.waitUntil([&] { return f.samplerChannel().at("sample") != loaded; }));
	CHECK(f.parameter("Cutoff") == 2000.0);
}
