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
#include <fstream>
#include <iterator>
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
				"<rml><head><style>body { margin: 0; } event-lanes, event-names, part-contents, waveform, curve, ruler "
				"{ display: block; } " +
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

TEST_CASE("fade lines draw their handles in color, as large as handle-size", "[unit]")
{
	OneWindow fixture;
	fixture.show("fade-lines { display: block; width: 400dp; height: 100dp; color: #ff0000; }",
		R"(<fade-lines id="fades" scale="10"/>)");
	auto& fades = fixture.element<ui::FadeLines>("fades");
	fades.setLanes({{40.0F, {}, {1000.0}, {}, {}}});
	// 6 dp by default, centered on the handle's place.
	const std::vector<float> small = extent(fixture.draw(), red());
	CHECK(small[2] - small[0] == 6.0F);
	CHECK(small[3] - small[1] == 6.0F);
	fades.SetAttribute("style", "handle-size: 10dp");
	const std::vector<float> large = extent(fixture.draw(), red());
	CHECK(large[2] - large[0] == 10.0F);
	CHECK(large[3] - large[1] == 10.0F);
	CHECK(large[0] + large[2] == small[0] + small[2]);
	CHECK(large[1] == small[1]); // at the top of the lane
	// An event volume control at the bottom of the lane, the same size.
	fades.setLanes({{40.0F, {}, {}, {}, {1000.0}}});
	const std::vector<float> fader = extent(fixture.draw(), red());
	CHECK(fader[2] - fader[0] == 10.0F);
	CHECK(fader[3] - large[1] == 40.0F);
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
	CHECK(count(vertices, Colour{0, 0, 0, 0}) == 0); // a transparent body isn't drawn
	CHECK(count(vertices, Colour{128, 255, 128, 255}) == 8);

	// The element's opacity applies to both.
	lanes.SetAttribute("style", "opacity: 0.5; own-color-tint: #ffffff80; own-fill-opacity: 1");
	vertices = fixture.draw();
	CHECK(count(vertices, Rml::Colourb(0, 255, 0, 255).ToPremultiplied(0.5F)) == 4);
	CHECK(count(vertices, Rml::Colourb(128, 255, 128, 255).ToPremultiplied(0.5F)) == 8);
}

TEST_CASE("an own-color-tint outlines events with a color in stroke-width, also without a stroke-color", "[unit]")
{
	// #1332: the tint alone decides that events with a color are outlined; events without one follow stroke-color.
	OneWindow fixture;
	fixture.show("event-lanes { width: 400dp; height: 100dp; color: #ff0000; stroke-color: transparent;"
				 " stroke-width: 2dp; own-color-tint: #ffffff80; }",
		R"(<event-lanes id="lanes" scale="10"/>)");
	auto& lanes = fixture.element<ui::EventLanes>("lanes");
	ui::TimeSpan own{2000.0, 1000.0};
	own.color = 0x00ff00ffU;
	lanes.setLanes({{20.0F, {{1000.0, 500.0}, own}}});
	std::vector<Rml::Vertex> vertices = fixture.draw();
	const Colour tinted{128, 255, 128, 255};
	CHECK(count(vertices, red()) == 4); // no outline
	CHECK(count(vertices, tinted) == 8);
	std::vector<float> ring = extent(vertices, tinted);
	CHECK(ring[2] - ring[0] == 100.0F);
	CHECK(ring[3] - ring[1] == 20.0F);
	// The ring is stroke-width (2 pixels) wide: the event's body sits that far inside it on every side.
	const std::vector<float> body = extent(vertices, green());
	CHECK(body[0] - ring[0] == 2.0F);
	CHECK(ring[2] - body[2] == 2.0F);
	CHECK(body[1] - ring[1] == 2.0F);
	CHECK(ring[3] - body[3] == 2.0F);

	// An event reaching beyond the built frame (a view's width on each side: 1200 pixels) ends the drawn outline's
	// width and a pixel outside it, so that outline stays out of view there.
	ui::TimeSpan wide{-1e6, 2e6};
	wide.color = 0x00ff00ffU;
	lanes.setLanes({{20.0F, {wide}}});
	vertices = fixture.draw();
	ring = extent(vertices, tinted);
	CHECK(ring[0] == -3.0F);
	CHECK(ring[2] == 1203.0F);
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

TEST_CASE("a waveform is drawn with the volume and volume curve of its event", "[unit]")
{
	// Reference manual pp. 399, 402: the waveform image follows the event volume and its curve.
	OneWindow fixture;
	fixture.show("waveform { width: 4dp; height: 40dp; color: #00ff00; }", R"(<waveform id="wave" scale="100"/>)");
	auto& wave = fixture.element<ui::Waveform>("wave");
	ui::PeakColumns peaks;
	peaks.low.assign(8, -32767);
	peaks.high.assign(8, 32767);
	wave.setPeaks({0.0, 400.0}, {peaks});
	// At half the gain up to 200 seconds, a quarter after it: a clip frame a second from the event's start at 0.
	wave.setVolume({0.0, 400.0, 1.0, 0, 0.5, {{200, 1.0, 0.0}, {200, 0.5, 0.0}}, false});
	const std::vector<Rml::Vertex> vertices = fixture.draw();
	// Two quads, each widened over two pixels; the element's 40 dp start at 40.
	REQUIRE(vertices.size() == 8);
	const auto extentOf = [&vertices](size_t quad)
	{
		float top = 1e9F;
		float bottom = -1e9F;
		for (size_t v = quad * 4; v < (quad + 1) * 4; ++v)
		{
			top = std::min(top, vertices[v].position.y);
			bottom = std::max(bottom, vertices[v].position.y);
		}
		return std::pair{top, bottom};
	};
	CHECK(extentOf(0) == std::pair{50.0F, 70.0F});
	CHECK(extentOf(1) == std::pair{55.0F, 65.0F});
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
	// A new color for the same peaks, as when the event's or its track's color changes.
	wave.setColor(0x0000ffffU);
	vertices = fixture.draw();
	CHECK(count(vertices, Colour{128, 128, 255, 255}) == 4);
	CHECK(wave.channels().size() == 1);
	// Without an own color, the style sheet's.
	wave.setPeaks({0.0, 400.0}, {peaks});
	CHECK(count(fixture.draw(), green()) == 4);
}

TEST_CASE("a waveform keeps event-inset off its lane's edges inside its padding, unless the lane is too low", "[unit]")
{
	// #1332: a <waveform> is as high as its lane and takes event-inset as <event-lanes> does, so that it stays inside
	// its event; its padding comes from the shared style sheet.
	OneWindow fixture;
	fixture.show("waveform { width: 4dp; height: 40dp; box-sizing: border-box; padding: 2dp 0; color: #00ff00;"
				 " event-inset: 3dp; }",
		R"(<waveform id="wave" scale="100"/>)");
	auto& wave = fixture.element<ui::Waveform>("wave");
	ui::PeakColumns peaks;
	peaks.low = {-32767, -32767, -32767, -32767};
	peaks.high = {32767, 32767, 32767, 32767};
	wave.setPeaks({0.0, 400.0}, {peaks});
	// Builds reach a view's height above the content, 36 here: the lane from 34 to 74, the waveform 3 and 2 inside it.
	std::vector<float> drawn = extent(fixture.draw(), green());
	CHECK(drawn[1] == 39.0F);
	CHECK(drawn[3] == 69.0F);

	// A lane no higher than twice the inset, as events drop their inset there, or one that would leave the waveform
	// no pixel: the padding only. Heights 6 and 7 leave content of 2 and 3, 10 leaves content 6 and area 0.
	for (const int height : {6, 7, 10})
	{
		INFO(height);
		wave.SetAttribute("style", "height: " + std::to_string(height) + "dp");
		const float content = static_cast<float>(height) - 4.0F;
		drawn = extent(fixture.draw(), green());
		CHECK(drawn[1] == content);
		CHECK(drawn[3] == 2.0F * content);
	}

	// A lane that leaves exactly one pixel (content 7, minus the inset on both sides) keeps the inset.
	wave.SetAttribute("style", "height: 11dp");
	drawn = extent(fixture.draw(), green());
	CHECK(drawn[1] == 10.0F);
	CHECK(drawn[3] == 11.0F);
}

TEST_CASE("in the default theme a full-scale waveform stays inside its event's body", "[unit]")
{
	// The theme keeps events off their lanes' edges, and waveforms off them by the same event-inset (#1312, #1332).
	OneWindow fixture;
	std::ifstream file(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "themes" / "gnubase" / "theme.rcss");
	REQUIRE(file);
	const std::string theme{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	fixture.ui.shell.setTheme("gnubase");
	fixture.show(theme +
			" event-lanes, waveform { position: absolute; left: 0; top: 0; width: 4dp; height: 40dp;"
			" box-sizing: border-box; } waveform { padding: 2dp 0; }", // and the shared style sheet's padding
		R"(<div class="event-display"><div class="event-area"><event-lanes id="lanes" class="events" scale="100"/>)"
		R"(<div class="waveforms"><waveform id="wave" scale="100"/></div></div></div>)");
	auto& lanes = fixture.element<ui::EventLanes>("lanes");
	auto& wave = fixture.element<ui::Waveform>("wave");
	ui::TimeSpan event{0.0, 400.0};
	event.color = 0x00ff00ffU;
	lanes.setLanes({{40.0F, {event}}});
	ui::PeakColumns peaks;
	peaks.low = {-32768, -32768, -32768, -32768};
	peaks.high = {32767, 32767, 32767, 32767};
	wave.setPeaks({0.0, 400.0, 0xff0000ffU}, {peaks});
	const std::vector<Rml::Vertex> vertices = fixture.draw();

	// Canvases build around their view, a view's height above it: from vertex to element coordinates.
	const auto top = [](Rml::Element& element, float y)
	{
		const Rml::Box& box = element.GetBox();
		return y - box.GetSize(Rml::BoxArea::Content).y + box.GetPosition(Rml::BoxArea::Content).y;
	};
	const std::vector<float> body = extent(vertices, Colour{0, 115, 0, 115});   // green at the theme's opacity
	const std::vector<float> line = extent(vertices, Colour{255, 89, 89, 255}); // red tinted
	REQUIRE(body[1] < body[3]);
	REQUIRE(line[1] < line[3]);
	CHECK(top(lanes, body[1]) == 3.0F); // inside the outline, 2 off the lane's edges
	CHECK(top(lanes, body[3]) == 37.0F);
	CHECK(top(wave, line[1]) > top(lanes, body[1]));
	CHECK(top(wave, line[3]) < top(lanes, body[3]));
}

TEST_CASE("in the default theme selected events are lit over their tint, in their events' boxes", "[unit]")
{
	// #1312: the selection overlay is translucent, so the event's own color shows through, and inset as the events.
	OneWindow fixture;
	std::ifstream file(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "themes" / "gnubase" / "theme.rcss");
	REQUIRE(file);
	const std::string theme{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
	fixture.ui.shell.setTheme("gnubase");
	fixture.show(theme + " event-lanes { position: absolute; left: 0; top: 0; width: 4dp; height: 40dp; }",
		R"(<div class="event-display"><div class="event-area"><event-lanes id="events" class="events" scale="100"/>)"
		R"(<event-lanes id="selected" class="selected-events" scale="100"/></div></div>)");
	ui::TimeSpan event{0.0, 400.0};
	event.color = 0x00ff00ffU;
	fixture.element<ui::EventLanes>("events").setLanes({{40.0F, {event}}});
	fixture.element<ui::EventLanes>("selected").setLanes({{40.0F, {{0.0, 400.0}}}}); // the overlay has no color
	const std::vector<Rml::Vertex> vertices = fixture.draw();

	// The event's body in its color at the theme's opacity, inside its outline 2 off the lane's edges.
	const std::vector<float> body = extent(vertices, Colour{0, 115, 0, 115});
	REQUIRE(body[1] < body[3]);
	// The overlay: translucent white in the same box, outlined in the text color where the event is.
	std::vector<Rml::Vertex> light;
	std::copy_if(vertices.begin(), vertices.end(), std::back_inserter(light),
		[](const Rml::Vertex& v) { return v.colour.alpha == 0x29; });
	REQUIRE(!light.empty());
	const Colour lit = light.front().colour;
	CHECK(extent(vertices, lit) == body);
	const std::vector<float> ring = extent(vertices, Colour{241, 244, 255, 255});
	CHECK(ring[1] == body[1] - 1.0F);
	CHECK(ring[3] == body[3] + 1.0F);
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

TEST_CASE("event lanes draw their bodies at body-opacity, their outlines as they are", "[unit]")
{
	// The event display's Event Opacity (reference manual p. 1604, #1313).
	OneWindow fixture;
	fixture.show(
		"event-lanes { width: 400dp; height: 100dp; color: #ff0000; stroke-color: #0000ff; stroke-width: 2dp; }",
		R"(<event-lanes id="lanes" scale="10"/>)");
	auto& lanes = fixture.element<ui::EventLanes>("lanes");
	ui::TimeSpan own{2000.0, 1000.0};
	own.color = 0x00ff00ffU;
	lanes.setLanes({{20.0F, {{1000.0, 500.0}, own}}});
	CHECK(count(fixture.draw(), red()) == 4);
	lanes.SetAttribute("body-opacity", 0.5);
	std::vector<Rml::Vertex> vertices = fixture.draw();
	CHECK(count(vertices, red()) == 0);
	CHECK(count(vertices, Colour{128, 0, 0, 128}) == 4);
	CHECK(count(vertices, Colour{0, 128, 0, 128}) == 4); // an own color too
	CHECK(count(vertices, blue()) == 16);                // the outlines stay
	lanes.SetAttribute("body-opacity", 0.0);
	vertices = fixture.draw();
	CHECK(count(vertices, Colour{0, 0, 0, 0}) == 0); // no bodies at all
	CHECK(count(vertices, blue()) == 16);
}

TEST_CASE("a waveform's brightness dims it or lightens it toward white", "[unit]")
{
	// The event display's Waveform Brightness (reference manual p. 1604, #1313): 0.5 as the style sheet draws it.
	CHECK(ui::brightened(green(), 0.5) == green());
	CHECK(ui::brightened(green(), 1.0) == Colour(255, 255, 255, 255));
	CHECK(ui::brightened(green(), 0.75) == Colour(128, 255, 128, 255));
	CHECK(ui::brightened(green(), 0.25) == Colour(0, 128, 0, 128));
	CHECK(ui::brightened(green(), 0.0) == Colour(0, 0, 0, 0));
	CHECK(ui::brightened(Colour(0, 100, 0, 100), 1.0) == Colour(100, 100, 100, 100)); // white at its own alpha

	OneWindow fixture;
	fixture.show("waveform { width: 4dp; height: 40dp; color: #00ff00; }", R"(<waveform id="wave" scale="100"/>)");
	auto& wave = fixture.element<ui::Waveform>("wave");
	ui::PeakColumns peaks;
	peaks.low = {-100, -100, -100, -100};
	peaks.high = {100, 100, 100, 100};
	wave.setPeaks({0.0, 400.0}, {peaks});
	CHECK(count(fixture.draw(), green()) == 4);
	wave.SetAttribute("brightness", 1.0);
	const std::vector<Rml::Vertex> vertices = fixture.draw();
	CHECK(count(vertices, green()) == 0);
	CHECK(count(vertices, Colour(255, 255, 255, 255)) == 4);
	wave.SetAttribute("brightness", 0.0);
	CHECK(fixture.draw().empty());
}

TEST_CASE("event names show at their events' top left, cut to fit or left out", "[unit]")
{
	// Show Event Names and Hide Truncated Event Names (reference manual pp. 1603-1604, #1313).
	OneWindow fixture;
	ui::Fonts(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "fonts").load();
	fixture.show("event-names { width: 400dp; height: 100dp; font-family: Inter; font-size: 12dp; color: #ffffff; "
				 "event-inset: 2dp; }",
		R"(<event-names id="names" scale="10"/>)");
	auto& names = fixture.element<ui::EventNames>("names");
	const Colour white{255, 255, 255, 255};
	// A name that fits its event: one glyph a quad, from a quarter em and the inset inside the event's top left. The
	// build starts a view left of and above the element.
	names.setLanes({{40.0F, {{1000.0, 1000.0, "Drums"}}}});
	std::vector<Rml::Vertex> vertices = fixture.draw();
	CHECK(count(vertices, white) == 5 * 4);
	std::vector<float> box = extent(vertices, white);
	CHECK(box[0] >= 400.0F + 100.0F + 3.0F);
	CHECK(box[0] <= 400.0F + 100.0F + 3.0F + 2.0F); // the D's own left side bearing
	CHECK(box[1] >= 100.0F + 2.0F);
	CHECK(box[2] < 400.0F + 200.0F);
	const float full = box[2] - box[0];

	// An event too short for it cuts it to the characters that fit; one too short for any shows none.
	names.setLanes({{40.0F, {{1000.0, full * 6.0, "Drums"}, {3000.0, 10.0, "Bass"}}}});
	vertices = fixture.draw();
	const size_t cut = count(vertices, white);
	CHECK(cut > 0);
	CHECK(cut < 5 * 4);
	CHECK(extent(vertices, white)[2] <= 400.0F + 100.0F + (full * 0.6F));
	// Hide Truncated Event Names leaves it out.
	names.SetAttribute("hide-truncated", true);
	CHECK(count(fixture.draw(), white) == 0);
	names.RemoveAttribute("hide-truncated");
	CHECK(count(fixture.draw(), white) == cut);

	// Of a thousand names, only those around the view are built: the view is 4000 units wide.
	std::vector<ui::EventNames::Name> many;
	many.reserve(1000);
	for (int i = 0; i < 1000; ++i)
		many.push_back({i * 1000.0, 900.0, "x"});
	names.setLanes({{40.0F, many}});
	vertices = fixture.draw();
	CHECK(count(vertices, white) >= 4 * 4);
	CHECK(count(vertices, white) <= 4 * 13);
}

TEST_CASE("part contents build only the parts, notes and controllers around the view", "[unit]")
{
	// Like the other canvas elements (#1313): the view is 4000 units wide, a build covers a view either side of it.
	OneWindow fixture;
	fixture.show("part-contents { width: 400dp; height: 100dp; color: #ff0000; stroke-color: #0000ff; "
				 "stroke-width: 1dp; }",
		R"(<part-contents id="contents" scale="10"/>)");
	auto& contents = fixture.element<ui::PartContents>("contents");
	// A thousand parts with a note and a controller each.
	std::vector<ui::PartContents::Part> parts;
	parts.reserve(1000);
	for (int i = 0; i < 1000; ++i)
		parts.push_back({i * 1000.0, 900.0, {{i * 1000.0, 100.0, 60}}, {{i * 1000.0, 1.0}}});
	contents.setLanes({{60.0F, parts}});
	std::vector<Rml::Vertex> vertices = fixture.draw();
	CHECK(count(vertices, red()) >= 4 * 4);
	CHECK(count(vertices, red()) <= 4 * 13);
	CHECK(count(vertices, blue()) <= 4 * 13);
	// One long part with ten thousand notes and controllers, a note every 100 units.
	ui::PartContents::Part part{0.0, 1000000.0, {}, {}};
	part.notes.reserve(10000);
	part.controllers.reserve(10000);
	for (int i = 0; i < 10000; ++i)
	{
		part.notes.push_back({i * 100.0, 50.0, 60 + (i % 12)});
		part.controllers.push_back({i * 100.0, 0.5});
	}
	contents.setLanes({{60.0F, {part}}});
	vertices = fixture.draw();
	CHECK(count(vertices, red()) >= 4 * 40);
	CHECK(count(vertices, red()) <= 4 * 130);
	CHECK(count(vertices, blue()) >= 4 * 40);
	CHECK(count(vertices, blue()) <= 4 * 130);
}

TEST_CASE("part contents show notes as lines over the part's pitch range, controllers as bars", "[unit]")
{
	// Part Data Mode Lines, Show Controllers and Note Brightness (reference manual p. 1607, #1313).
	OneWindow fixture;
	fixture.show("part-contents { width: 400dp; height: 100dp; color: #ff0000; stroke-color: #0000ff; "
				 "stroke-width: 1dp; }",
		R"(<part-contents id="contents" scale="10"/>)");
	auto& contents = fixture.element<ui::PartContents>("contents");
	ui::PartContents::Part part{
		1000.0, 2000.0, {{1000.0, 100.0, 64}, {1500.0, 100.0, 60}, {2000.0, 200.0, 62}}, {{1200.0, 0.5}}};
	contents.setLanes({{60.0F, {part}}});
	std::vector<Rml::Vertex> vertices = fixture.draw();
	CHECK(count(vertices, red()) == 3 * 4);
	// The build starts a view left of and above the element: the part's box is 500 to 700 by 100 to 160. Five pitches
	// share its 60 pixels: the highest at the top, the lowest at the bottom, 10 pixels each, a sixth of the box.
	std::vector<float> notes = extent(vertices, red());
	CHECK(notes == std::vector<float>{500.0F, 100.0F, 620.0F, 160.0F});
	// The controller at half its value: a quarter of the box's height, from its bottom.
	CHECK(extent(vertices, blue()) == std::vector<float>{520.0F, 152.0F, 521.0F, 160.0F});

	// Note Brightness lightens them toward white.
	contents.SetAttribute("brightness", 1.0);
	vertices = fixture.draw();
	CHECK(count(vertices, red()) == 0);
	CHECK(count(vertices, Colour(255, 255, 255, 255)) == 3 * 4);

	// One pitch: its line in the middle of the box, at most a sixth of it high.
	contents.setLanes({{60.0F, {{1000.0, 2000.0, {{1000.0, 100.0, 60}}, {}}}}});
	notes = extent(fixture.draw(), Colour(255, 255, 255, 255));
	CHECK(notes[1] == 125.0F);
	CHECK(notes[3] == 135.0F);
	// A controller at the part's end ends there too.
	contents.setLanes({{60.0F, {{1000.0, 2000.0, {}, {{2995.0, 1.0}}}}}});
	CHECK(extent(fixture.draw(), blue()) == std::vector<float>{699.0F, 145.0F, 700.0F, 160.0F});
	// Notes outside the part's box don't show past it.
	contents.setLanes({{60.0F, {{1000.0, 500.0, {{1400.0, 1000.0, 60}}, {}}}}});
	notes = extent(fixture.draw(), Colour(255, 255, 255, 255));
	CHECK(notes[0] == 540.0F);
	CHECK(notes[2] == 550.0F);
}
