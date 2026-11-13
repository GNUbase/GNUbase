// SPDX-License-Identifier: GPL-3.0-or-later
// The canvas elements (#55) on the fake window system: what geometry they build from their data, and when they build
// it. tests/window_tests.cpp draws them through SDL_GPU and checks their pixels; tests/bench/canvas_bench.cpp times
// them.
#include "testutils/fakeui.h"
#include "ui/canvas.h"
#include "ui/fonts.h"
#include "ui/rmlui.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using std::chrono::milliseconds;

namespace
{
	// The fake UI with one window at a monitor scale of 1, so a dp is a pixel.
	struct OneWindow
	{
		static ui::WindowSpec spec()
		{
			ui::WindowSpec spec;
			spec.bounds = ui::Rect{0, 0, 1200, 800};
			return spec;
		}

		// Shows body in a document with the style sheet style.
		Rml::ElementDocument& show(const std::string& style, const std::string& body) const
		{
			Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(
				"<rml><head><style>body { margin: 0; } event-lanes, waveform, curve, ruler { display: block; } " +
				style + "</style></head><body>" + body + "</body></rml>");
			REQUIRE(document != nullptr);
			document->Show();
			return *document;
		}

		template <typename T> T& element(const char* id) const
		{
			auto* found = dynamic_cast<T*>(window.context().GetDocument(0)->GetElementById(id));
			REQUIRE(found != nullptr);
			return *found;
		}

		// Draws a frame; returns the vertices of the geometry compiled for it.
		std::vector<Rml::Vertex> draw()
		{
			const size_t before = ui.renderer.compiledGeometry().size();
			window.requestRender();
			ui.shell.step(milliseconds(0));
			std::vector<Rml::Vertex> vertices;
			const auto& compiled = ui.renderer.compiledGeometry();
			for (size_t i = before; i < compiled.size(); ++i)
				vertices.insert(vertices.end(), compiled[i].begin(), compiled[i].end());
			return vertices;
		}

		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = ui.shell.open(spec());
	};

	using Colour = Rml::ColourbPremultiplied;

	size_t count(const std::vector<Rml::Vertex>& vertices, Colour colour)
	{
		return static_cast<size_t>(
			std::count_if(vertices.begin(), vertices.end(), [&](const Rml::Vertex& v) { return v.colour == colour; }));
	}

	// The extent of the vertices of one colour: left, top, right, bottom.
	std::vector<float> extent(const std::vector<Rml::Vertex>& vertices, Colour colour)
	{
		std::vector<float> box{1e9F, 1e9F, -1e9F, -1e9F};
		for (const Rml::Vertex& v : vertices)
			if (v.colour == colour)
				box = {std::min(box[0], v.position.x), std::min(box[1], v.position.y), std::max(box[2], v.position.x),
					std::max(box[3], v.position.y)};
		return box;
	}

	// 100 tracks of 100 events: each 600 units long, one every 1000 units, a little later on every track.
	std::vector<ui::EventLanes::Lane> tracks()
	{
		std::vector<ui::EventLanes::Lane> lanes(100);
		for (size_t track = 0; track < lanes.size(); ++track)
		{
			lanes[track].height = 60.0F;
			for (int i = 0; i < 100; ++i)
				lanes[track].events.push_back({(i * 1000.0) + static_cast<double>((track * 37) % 400), 600.0});
		}
		return lanes;
	}

	Colour red()
	{
		return {255, 0, 0, 255};
	}

	Colour green()
	{
		return {0, 255, 0, 255};
	}

	Colour blue()
	{
		return {0, 0, 255, 255};
	}
}

TEST_CASE("documents get the canvas elements, with the style properties they draw with", "[unit]")
{
	OneWindow fixture;
	fixture.show("event-lanes { stroke-color: #0000ff; stroke-width: 3dp; }",
		R"(<event-lanes id="lanes"/><waveform id="wave"/><curve id="curve"/><ruler id="ruler"/>)");
	auto& lanes = fixture.element<ui::EventLanes>("lanes");
	fixture.element<ui::Waveform>("wave");
	fixture.element<ui::Curve>("curve");
	fixture.element<ui::Ruler>("ruler");
	fixture.draw();
	const Rml::Property* stroke = lanes.GetProperty("stroke-color");
	REQUIRE(stroke != nullptr);
	CHECK(stroke->Get<Rml::Colourb>() == Rml::Colourb(0, 0, 255, 255));
	const Rml::Property* width = lanes.GetProperty("stroke-width");
	REQUIRE(width != nullptr);
	CHECK(lanes.ResolveLength(width->GetNumericValue()) == 3.0F);
	// How colors that are user data show (#1312): by default as before.
	const Rml::Property* opacity = lanes.GetProperty("own-fill-opacity");
	REQUIRE(opacity != nullptr);
	CHECK(opacity->Get<float>() == 1.0F);
	const Rml::Property* tint = lanes.GetProperty("own-color-tint");
	REQUIRE(tint != nullptr);
	CHECK(tint->unit == Rml::Unit::KEYWORD);
	const Rml::Property* inset = lanes.GetProperty("event-inset");
	REQUIRE(inset != nullptr);
	CHECK(lanes.ResolveLength(inset->GetNumericValue()) == 0.0F);
}

TEST_CASE("event lanes fill events with color, outlined by stroke-color, in their lanes", "[unit]")
{
	OneWindow fixture;
	fixture.show(
		"event-lanes { width: 400dp; height: 100dp; color: #ff0000; stroke-color: #0000ff; stroke-width: 2dp; }"
		"event-lanes.muted { color: #00ff00; }",
		R"(<event-lanes id="lanes" scale="10"/>)");
	auto& lanes = fixture.element<ui::EventLanes>("lanes");
	lanes.setLanes({{20.0F, {{1000.0, 500.0}}}, {30.0F, {{2000.0, 1000.0}}}});
	std::vector<Rml::Vertex> vertices = fixture.draw();
	// Per event a fill and an outline ring: 50 by 20 pixels and 100 by 30, outlines 2 pixels wide.
	CHECK(count(vertices, red()) == 8);
	CHECK(count(vertices, blue()) == 16);
	std::vector<float> fill = extent(vertices, red());
	CHECK(fill[2] - fill[0] == 196.0F); // from the first event's fill to the second's
	CHECK(fill[3] - fill[1] == 46.0F);

	// A change of style builds them again with the new colors; without an outline there is no ring.
	lanes.SetClass("muted", true);
	vertices = fixture.draw();
	CHECK(count(vertices, green()) == 8);
	CHECK(count(vertices, red()) == 0);
	lanes.SetAttribute("style", "stroke-width: 0px");
	vertices = fixture.draw();
	CHECK(count(vertices, blue()) == 0);
	fill = extent(vertices, green());
	CHECK(fill[2] - fill[0] == 200.0F);
	CHECK(fill[3] - fill[1] == 50.0F);
}

TEST_CASE("an event with a color of its own is filled with it, outlined by stroke-color", "[unit]")
{
	OneWindow fixture;
	fixture.show(
		"event-lanes { width: 400dp; height: 100dp; color: #ff0000; stroke-color: #0000ff; stroke-width: 2dp; }",
		R"(<event-lanes id="lanes" scale="10"/>)");
	auto& lanes = fixture.element<ui::EventLanes>("lanes");
	// The second event's own color (#94): user data, drawn instead of the style sheet's color.
	ui::TimeSpan own{2000.0, 1000.0};
	own.color = 0x00ff00ff;
	lanes.setLanes({{20.0F, {{1000.0, 500.0}, own}}});
	const std::vector<Rml::Vertex> vertices = fixture.draw();
	CHECK(count(vertices, red()) == 4);
	CHECK(count(vertices, green()) == 4);
	CHECK(count(vertices, blue()) == 16);
	const std::vector<float> fill = extent(vertices, green());
	CHECK(fill[2] - fill[0] == 96.0F);
}

TEST_CASE("the style sheet decides how an event's own color shows: its body's opacity and its outline's tint", "[unit]")
{
	// The default theme tints events in their colors (#1312): a translucent body, an outline in a lighter shade.
	OneWindow fixture;
	fixture.show("event-lanes { width: 400dp; height: 100dp; color: #ff0000; stroke-color: #0000ff; stroke-width: 2dp;"
				 " own-fill-opacity: 0.5; own-color-tint: #ffffff80; }",
		R"(<event-lanes id="lanes" scale="10"/>)");
	auto& lanes = fixture.element<ui::EventLanes>("lanes");
	ui::TimeSpan own{2000.0, 1000.0};
	own.color = 0x00ff00ff;
	lanes.setLanes({{20.0F, {{1000.0, 500.0}, own}}});
	std::vector<Rml::Vertex> vertices = fixture.draw();
	// The event without a color of its own shows as before: the style sheet's color, outlined by stroke-color.
	CHECK(count(vertices, red()) == 4);
	CHECK(count(vertices, blue()) == 8);
	// The other: its color at half opacity, outlined by its color with half of white over it.
	CHECK(count(vertices, Rml::Colourb(0, 255, 0, 128).ToPremultiplied()) == 4);
	CHECK(count(vertices, Colour{128, 255, 128, 255}) == 8);

	// A transparent tint outlines it in its own color; opacities beyond 0 to 1 are clamped.
	lanes.SetAttribute("style", "own-color-tint: transparent; own-fill-opacity: 3");
	vertices = fixture.draw();
	CHECK(count(vertices, green()) == 12);
	lanes.SetAttribute("style", "own-color-tint: #ffffff80; own-fill-opacity: -1");
	vertices = fixture.draw();
	CHECK(count(vertices, Colour{0, 0, 0, 0}) == 4);
	CHECK(count(vertices, Colour{128, 255, 128, 255}) == 8);

	// The element's opacity applies to both.
	lanes.SetAttribute("style", "opacity: 0.5; own-color-tint: #ffffff80; own-fill-opacity: 1");
	vertices = fixture.draw();
	CHECK(count(vertices, Rml::Colourb(0, 255, 0, 255).ToPremultiplied(0.5F)) == 4);
	CHECK(count(vertices, Rml::Colourb(128, 255, 128, 255).ToPremultiplied(0.5F)) == 8);
}

TEST_CASE("event-inset keeps events off their lanes' top and bottom edges", "[unit]")
{
	OneWindow fixture;
	fixture.show("event-lanes { width: 400dp; height: 100dp; color: #ff0000; stroke-color: #0000ff; stroke-width: 0;"
				 " event-inset: 3dp; }",
		R"(<event-lanes id="lanes" scale="10"/>)");
	auto& lanes = fixture.element<ui::EventLanes>("lanes");
	lanes.setLanes({{20.0F, {{1000.0, 500.0}}}, {30.0F, {{2000.0, 1000.0}}}});
	std::vector<Rml::Vertex> vertices = fixture.draw();
	// From 3 below the first lane's top to 3 above the second lane's bottom: 50 - 6.
	std::vector<float> fill = extent(vertices, red());
	CHECK(fill[3] - fill[1] == 44.0F);
	CHECK(count(vertices, red()) == 8);

	// A lane too low to keep the room shows its events at its full height.
	lanes.setLanes({{6.0F, {{1000.0, 500.0}}}});
	vertices = fixture.draw();
	fill = extent(vertices, red());
	CHECK(fill[3] - fill[1] == 6.0F);
	lanes.setLanes({{7.0F, {{1000.0, 500.0}}}});
	vertices = fixture.draw();
	fill = extent(vertices, red());
	CHECK(fill[3] - fill[1] == 1.0F);
}

TEST_CASE("event lanes build only the events around the view, and scrolling within that only moves them", "[unit]")
{
	// 10,000 events on 100 tracks, ten views wide and ten high: building one view and its surroundings takes the
	// events of about three by three views.
	OneWindow fixture;
	fixture.show(
		"event-lanes { width: 1000dp; height: 600dp; color: #ff0000; stroke-color: #0000ff; stroke-width: 1dp; }",
		R"(<event-lanes id="lanes" scale="10" start="20000" scroll-top="1200"/>)");
	auto& lanes = fixture.element<ui::EventLanes>("lanes");
	lanes.setLanes(tracks());
	const std::vector<Rml::Vertex> built = fixture.draw();
	CHECK(lanes.builds() == 1);
	const size_t events = count(built, red()) / 4;
	CHECK(events > 0);
	CHECK(events <= 31 * 31); // 31 events per lane in 3000 dp, 31 lanes of 60 dp in 1800 dp
	CHECK(count(built, blue()) == events * 8);

	// Scrolling less than a view's width or height moves what's built.
	for (int frame = 1; frame <= 99; ++frame)
	{
		lanes.SetAttribute("start", std::to_string(20000 + (frame * 100)));
		lanes.SetAttribute("scroll-top", std::to_string(1200 + (frame * 5)));
		CHECK(fixture.draw().empty());
	}
	CHECK(lanes.builds() == 1);
	lanes.SetAttribute("start", "30100");
	CHECK(count(fixture.draw(), red()) / 4 <= 31 * 31);
	CHECK(lanes.builds() == 2);
	lanes.SetAttribute("scroll-top", "2400");
	fixture.draw();
	CHECK(lanes.builds() == 3);

	// Zooming, resizing and new data build again.
	lanes.SetAttribute("scale", "20");
	fixture.draw();
	CHECK(lanes.builds() == 4);
	lanes.SetAttribute("style", "width: 900dp");
	fixture.draw();
	CHECK(lanes.builds() == 5);
	lanes.setLanes({});
	CHECK(fixture.draw().empty());
	CHECK(lanes.builds() == 6);
}

TEST_CASE("a waveform shows in each pixel the peaks of the columns it covers", "[unit]")
{
	OneWindow fixture;
	fixture.show("waveform { width: 4dp; height: 40dp; color: #00ff00; }", R"(<waveform id="wave" scale="100"/>)");
	auto& wave = fixture.element<ui::Waveform>("wave");
	// Eight columns over four pixels: each pixel shows the smallest low and the largest high of two.
	ui::PeakColumns peaks;
	peaks.low = {-32767, 0, -16384, -16384, 0, 0, 0, -8192};
	peaks.high = {0, 0, 16384, 0, 0, 0, 8192, 0};
	wave.setPeaks({0.0, 400.0}, {peaks});
	std::vector<Rml::Vertex> vertices = fixture.draw();
	REQUIRE(vertices.size() == 16);
	// Pixel by pixel, as top and bottom of the quad: full scale reaches the edges, silence is a line one pixel high.
	// Builds reach one view above the element, and vertices count from there: the element's top is at 40.
	const std::vector<std::pair<float, float>> expected{{60.0F, 80.0F}, {50.0F, 70.0F}, {60.0F, 61.0F}, {55.0F, 65.0F}};
	std::vector<std::pair<float, float>> actual;
	for (size_t quad = 0; quad < 4; ++quad)
	{
		float top = 1e9F;
		float bottom = -1e9F;
		for (size_t v = quad * 4; v < (quad + 1) * 4; ++v)
		{
			top = std::min(top, vertices[v].position.y);
			bottom = std::max(bottom, vertices[v].position.y);
		}
		actual.emplace_back(top, bottom);
	}
	CHECK(actual == expected);

	// Two channels share the height, the first above.
	wave.setPeaks({0.0, 400.0}, {peaks, peaks});
	vertices = fixture.draw();
	CHECK(vertices.size() == 32);
	CHECK(extent(vertices, green())[1] == 45.0F); // the first channel's 16384 at a quarter of its 20
	CHECK(extent(vertices, green())[3] == 80.0F); // the second channel's -32767 at the bottom
}

TEST_CASE("a waveform with a color of its own shows in it when the style sheet tints own colors", "[unit]")
{
	// An audio event's waveform in its event's color (#1312).
	OneWindow fixture;
	fixture.show("waveform { width: 4dp; height: 40dp; color: #00ff00; }", R"(<waveform id="wave" scale="100"/>)");
	auto& wave = fixture.element<ui::Waveform>("wave");
	ui::PeakColumns peaks;
	peaks.low = {-100, -100, -100, -100};
	peaks.high = {100, 100, 100, 100};
	ui::TimeSpan span{0.0, 400.0};
	span.color = 0xff0000ffU;
	wave.setPeaks(span, {peaks});
	// By default (own-color-tint none) in the style sheet's color, as before.
	std::vector<Rml::Vertex> vertices = fixture.draw();
	CHECK(count(vertices, green()) == 4); // one quad: every pixel the same
	wave.SetAttribute("style", "own-color-tint: #ffffff80");
	vertices = fixture.draw();
	CHECK(count(vertices, green()) == 0);
	CHECK(count(vertices, Colour{255, 128, 128, 255}) == 4);
	// Without an own color, the style sheet's.
	wave.setPeaks({0.0, 400.0}, {peaks});
	CHECK(count(fixture.draw(), green()) == 4);
}

TEST_CASE("a curve builds only the segments around the view", "[unit]")
{
	OneWindow fixture;
	fixture.show("curve { width: 400dp; height: 40dp; stroke-color: #0000ff; stroke-width: 2dp; }",
		R"(<curve id="curve" scale="10"/>)");
	auto& curve = fixture.element<ui::Curve>("curve");
	std::vector<ui::CurvePoint> points;
	points.reserve(10000);
	for (int i = 0; i < 10000; ++i)
		points.push_back({i * 100.0, (i % 2) * 1.0, engine::AutomationShape::Linear, 0.0});
	curve.setPoints(points);
	// 40 points per view: a quad per segment for three views.
	const size_t vertices = fixture.draw().size();
	CHECK(vertices > 0);
	CHECK(vertices <= 4 * 130);
}

TEST_CASE("a curve leaves its gaps empty: after a terminator, and in virgin territory outside its points", "[unit]")
{
	OneWindow fixture;
	fixture.show("curve { width: 400dp; height: 40dp; stroke-color: #0000ff; stroke-width: 2dp; }",
		R"(<curve id="curve" scale="10"/>)");
	auto& curve = fixture.element<ui::Curve>("curve");
	using engine::AutomationShape;
	// At 50, 150, 250 and 350 dp, the one at 150 a terminator, so that there is no line from it to the next.
	std::vector<ui::CurvePoint> points{{500.0, 0.5, AutomationShape::Linear, 0.0},
		{1500.0, 0.5, AutomationShape::Step, 0.0}, {2500.0, 0.5, AutomationShape::Linear, 0.0},
		{3500.0, 0.5, AutomationShape::Linear, 0.0}};
	points[1].terminator = true;
	// The lines drawn, from left to right, in the frame's pixels: a quad each, the line's width (2 dp) around its
	// ends. The frame reaches a view's width (400 dp) to the left of the view.
	const auto lines = [&fixture]()
	{
		std::vector<Rml::Vertex> vertices = fixture.draw();
		vertices.erase(std::remove_if(vertices.begin(), vertices.end(),
						   [](const Rml::Vertex& vertex) { return vertex.colour != blue(); }),
			vertices.end());
		std::vector<std::pair<float, float>> result;
		for (size_t i = 0; i + 4 <= vertices.size(); i += 4)
			result.emplace_back(std::min(vertices[i].position.x, vertices[i + 1].position.x) + 1.0F,
				std::max(vertices[i].position.x, vertices[i + 1].position.x) - 1.0F);
		return result;
	};
	using Lines = std::vector<std::pair<float, float>>;

	// With initial values the curve is level with its first point before it and with its last point after it.
	curve.setPoints(points);
	CHECK(lines() == Lines{{-1.0F, 450.0F}, {450.0F, 550.0F}, {650.0F, 750.0F}, {750.0F, 1201.0F}});
	// A terminator as the last point ends the curve there.
	points[3].terminator = true;
	curve.setPoints(points);
	CHECK(lines() == Lines{{-1.0F, 450.0F}, {450.0F, 550.0F}, {650.0F, 750.0F}});
	points[3].terminator = false;

	// Virgin territory: no curve before the first point or after the last one.
	curve.setVirginTerritory(true);
	curve.setPoints(points);
	CHECK(lines() == Lines{{450.0F, 550.0F}, {650.0F, 750.0F}});
	// Scrolled so that the frame starts between the terminator and the next point (at 2000): no line comes from the
	// terminator.
	curve.SetAttribute("start", 6000);
	CHECK(lines() == Lines{{50.0F, 150.0F}});
}

TEST_CASE("a ruler's default scale counts in decimal steps of 1, 2 and 5", "[unit]")
{
	CHECK(ui::decimalStep(1.0) == std::pair<double, int>{1.0, 5});
	CHECK(ui::decimalStep(1.2) == std::pair<double, int>{2.0, 4});
	CHECK(ui::decimalStep(330.0) == std::pair<double, int>{500.0, 5});
	CHECK(ui::decimalStep(7000.0) == std::pair<double, int>{10000.0, 5});
	CHECK(ui::decimalStep(0.03).first == Catch::Approx(0.05));
	CHECK(ui::decimalLabel(1500.0, 500.0) == "1500");
	CHECK(ui::decimalLabel(-1000.0, 500.0) == "-1000");
	CHECK(ui::decimalLabel(0.30000000000000004, 0.1) == "0.3");
	CHECK(ui::decimalLabel(0.25, 0.05) == "0.25");
	CHECK(ui::decimalLabel(123456789.0, 10000.0) == "123456789");

	// Labelled marks at every step from the first at or after the start, minor ones between them.
	std::vector<ui::RulerMark> marks;
	ui::RulerScale().marks(-120.0, 401.0, 150.0, marks);
	REQUIRE(marks.size() == size_t{12}); // 3 labelled marks with 3 minor ones each
	CHECK(marks[0].position == 0.0);
	CHECK(marks[0].label == "0");
	CHECK(marks[1].position == 50.0);
	CHECK(marks[1].label.empty());
	CHECK(marks[8].label == "400");
}

TEST_CASE("a ruler labels the major ticks around the view, far enough apart for their labels", "[unit]")
{
	OneWindow fixture;
	ui::Fonts(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "fonts").load();
	fixture.show("ruler { width: 400dp; height: 20dp; font-family: Inter; font-size: 11dp; color: #ffffff; "
				 "stroke-color: #ffff00; stroke-width: 1dp; }",
		R"(<ruler id="ruler" scale="10" start="1000"/>)");
	auto& ruler = fixture.element<ui::Ruler>("ruler");
	double minimum = 0.0;
	std::set<double> labelled;
	ui::RulerScale scale;
	scale.marks = [&](double from, double to, double units, std::vector<ui::RulerMark>& marks)
	{
		minimum = units;
		ui::evenMarks(
			from, to, 1000.0, 4,
			[&labelled](double position)
			{
				labelled.insert(position);
				return "1";
			},
			marks);
	};
	ruler.setScale(scale);
	const std::vector<Rml::Vertex> vertices = fixture.draw();
	CHECK(minimum > 10.0 * 10.0); // labels are wider than 10 dp
	CHECK(minimum < 10.0 * 100.0);
	// The built range: one view either side of 1000 to 5000.
	const std::set<double> ticks{
		-3000.0, -2000.0, -1000.0, 0.0, 1000.0, 2000.0, 3000.0, 4000.0, 5000.0, 6000.0, 7000.0, 8000.0};
	CHECK(std::includes(labelled.begin(), labelled.end(), ticks.begin(), ticks.end()));
	// Ticks: a major and three minor ones per step.
	CHECK(count(vertices, Colour(255, 255, 0, 255)) == 4 * 4 * 12);
	CHECK(count(vertices, Colour(255, 255, 255, 255)) > 0); // the labels
}
