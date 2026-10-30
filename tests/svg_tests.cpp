// SPDX-License-Identifier: GPL-3.0-or-later
// SVG graphics (#210, docs/features/svg.md): GNUbase's icons and theme graphics load through LunaSVG, and RmlUi's SVG
// plugin rasterizes each at the pixel size it has on screen, once per size, again only when the size changes. Icons
// draw currentColor white, so that `image-color` gives them the theme's colors without new rasters. On the fake
// window system, whose renderer keeps the textures RmlUi generates; tests/window_tests.cpp draws icons through
// SDL_GPU at 100 %, 150 % and 200 % and compares them with golden images.
#include "testutils/fakeui.h"
#include "testutils/testutils.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using std::chrono::milliseconds;

namespace
{
	std::filesystem::path resources()
	{
		return {GNUBASE_RESOURCES_DIR};
	}

	// The SVG files in the folder, sorted.
	std::vector<std::filesystem::path> svgFiles(const std::filesystem::path& folder)
	{
		std::vector<std::filesystem::path> files;
		for (const auto& entry : std::filesystem::directory_iterator(folder))
			if (entry.path().extension() == ".svg")
				files.push_back(entry.path());
		std::sort(files.begin(), files.end());
		return files;
	}

	void write(const std::filesystem::path& file, const std::string& content)
	{
		std::ofstream out(file, std::ios::binary);
		out << content;
	}

	// GNUbase's icons (resources/icons), all monochrome.
	std::vector<std::filesystem::path> icons()
	{
		return svgFiles(resources() / "icons");
	}

	// Every theme's graphics (resources/themes), in their own colors.
	std::vector<std::filesystem::path> themeGraphics()
	{
		std::vector<std::filesystem::path> files;
		for (const char* theme : {"gnubase", "dark", "light"})
			for (const std::filesystem::path& file : svgFiles(resources() / "themes" / theme))
				files.push_back(file);
		return files;
	}

	// An <svg> element that shows the file, with the attributes given.
	std::string svg(const std::filesystem::path& file, const std::string& attributes = {})
	{
		return "<svg src=\"" + file.generic_u8string() + "\" " + attributes + "/>";
	}

	// One window of the fake UI with an empty document, whose body the tests fill.
	struct Graphics
	{
		static ui::WindowSpec spec()
		{
			ui::WindowSpec spec;
			spec.bounds = ui::Rect{0, 0, 1600, 1000};
			return spec;
		}

		Graphics()
		{
			document = window.context().LoadDocumentFromMemory(R"(<rml><head><style>
				body { display: block; margin: 0; padding: 0; }
				svg { display: inline-block; }
				.small { width: 16dp; height: 16dp; }
				.medium { width: 24dp; height: 24dp; }
				.large { width: 32dp; height: 32dp; }
				.text { image-color: #e8eaed; }
				.accent { image-color: #10dff2; }
				.hover { image-color: #ffffff; }
				.danger { image-color: #e64b67; }
				.muted { image-color: #9aa3ad; }
			</style></head><body/></rml>)");
			REQUIRE(document != nullptr);
			document->Show();
		}

		// Replaces what the document shows.
		void show(const std::string& rml) const { document->SetInnerRML(rml); }

		// Draws a frame, and returns the textures generated for it.
		std::vector<ui::Image> draw()
		{
			const size_t before = fake.renderer.generatedTextures().size();
			window.requestRender();
			fake.shell.step(milliseconds(0));
			const std::vector<ui::Image>& textures = fake.renderer.generatedTextures();
			return {textures.begin() + static_cast<std::ptrdiff_t>(before), textures.end()};
		}

		test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = fake.shell.open(spec());
		Rml::ElementDocument* document = nullptr;
	};

	// The pixels of a texture with any coverage, premultiplied RGBA.
	std::vector<std::array<uint8_t, 4>> visiblePixels(const ui::Image& texture)
	{
		std::vector<std::array<uint8_t, 4>> visible;
		for (int y = 0; y < texture.height; ++y)
			for (int x = 0; x < texture.width; ++x)
				if (texture.pixel(x, y)[3] != 0)
					visible.push_back(texture.pixel(x, y));
		return visible;
	}

	// Whether the texture shows anything. (Not CHECK_FALSE on empty(): Catch2's ContinueOnFailure | FalseTest trips
	// clang-analyzer's EnumCastOutOfRange check.)
	bool showsSomething(const ui::Image& texture)
	{
		return !visiblePixels(texture).empty();
	}
}

TEST_CASE("GNUbase's icons and theme graphics all load through LunaSVG and draw at their size", "[unit]")
{
	Graphics graphics;
	std::vector<std::filesystem::path> files = icons();
	for (const std::filesystem::path& file : themeGraphics())
		files.push_back(file);
	CHECK(files.size() > 200);

	for (const std::filesystem::path& file : files)
	{
		INFO(file);
		graphics.show(svg(file, "id=\"graphic\""));
		const std::vector<ui::Image> textures = graphics.draw();
		REQUIRE(textures.size() == 1);
		Rml::Element* element = graphics.document->GetElementById("graphic");
		REQUIRE(element != nullptr);
		const Rml::Vector2f size = element->GetBox().GetSize();
		CHECK(size.x > 0.0F);
		CHECK(size.y > 0.0F);
		CHECK(textures[0].width == static_cast<int>(size.x));
		CHECK(textures[0].height == static_cast<int>(size.y));
		CHECK(showsSomething(textures[0]));
	}
}

TEST_CASE("an icon is rasterized at the pixel size it has at 100 %, 150 % and 200 % UI scale", "[unit]")
{
	Graphics graphics;
	const std::filesystem::path play = resources() / "icons" / "transport-play.svg";
	graphics.show(svg(play, "class=\"medium\"") + svg(play));
	for (const float scale : {1.0F, 1.5F, 2.0F})
	{
		INFO("UI scale " << scale);
		graphics.fake.shell.setUiScale(scale);
		// Both icons are 24 dp: the first by its style, the second by the size its file gives. They share a raster.
		const std::vector<ui::Image> textures = graphics.draw();
		REQUIRE(textures.size() == 1);
		const int pixels = static_cast<int>(24.0F * scale);
		CHECK(textures[0].width == pixels);
		CHECK(textures[0].height == pixels);
	}
}

TEST_CASE("a graphic drawn as a decorator is rasterized at the element's pixel size too", "[unit]")
{
	Graphics graphics;
	const std::filesystem::path knob = resources() / "themes" / "gnubase" / "knob-base.svg";
	graphics.show("<div style=\"display: block; width: 30dp; height: 30dp; decorator: svg(&quot;" +
		knob.generic_u8string() + "&quot;);\"/>");
	for (const float scale : {1.0F, 2.0F})
	{
		INFO("UI scale " << scale);
		graphics.fake.shell.setUiScale(scale);
		const std::vector<ui::Image> textures = graphics.draw();
		REQUIRE(textures.size() == 1);
		const int pixels = static_cast<int>(30.0F * scale);
		CHECK(textures[0].width == pixels);
		CHECK(textures[0].height == pixels);
		CHECK(showsSomething(textures[0]));
	}
}

TEST_CASE("icons rasterize currentColor white, for image-color to give them the theme's colors", "[unit]")
{
	Graphics graphics;
	for (const std::filesystem::path& file : icons())
	{
		INFO(file);
		graphics.show(svg(file, "class=\"medium accent\""));
		const std::vector<ui::Image> textures = graphics.draw();
		REQUIRE(textures.size() == 1);
		// White in premultiplied alpha: each color channel equals the coverage, so that the tint shows unchanged.
		for (const auto& pixel : visiblePixels(textures[0]))
		{
			CHECK(pixel[0] == pixel[3]);
			CHECK(pixel[1] == pixel[3]);
			CHECK(pixel[2] == pixel[3]);
		}
	}

	// A filled shape is opaque where it is solid.
	graphics.show(svg(resources() / "icons" / "transport-stop.svg", "class=\"medium\""));
	const std::vector<ui::Image> stop = graphics.draw();
	REQUIRE(stop.size() == 1);
	CHECK(stop[0].pixel(12, 12) == std::array<uint8_t, 4>{255, 255, 255, 255});

	// Graphics with colors of their own keep them, and so does an SVG that sets its currentColor.
	graphics.show(svg(resources() / "themes" / "gnubase" / "toggle-record.svg"));
	const std::vector<ui::Image> record = graphics.draw();
	REQUIRE(record.size() == 1);
	const std::array<uint8_t, 4> red = record[0].pixel(10, 10);
	CHECK(red[3] == 255);
	CHECK(red[0] > red[1]);
	CHECK(red[0] > red[2]);

	graphics.show(R"(<svg width="10" height="10" viewBox="0 0 10 10" color="#00ff00">
		<rect width="10" height="10" fill="currentColor"/></svg>)");
	const std::vector<ui::Image> green = graphics.draw();
	REQUIRE(green.size() == 1);
	CHECK(green[0].pixel(5, 5) == std::array<uint8_t, 4>{0, 255, 0, 255});
}

TEST_CASE("several hundred icons are rasterized once per icon and size, never in steady frames", "[unit]")
{
	Graphics graphics;
	const std::vector<std::filesystem::path> files = icons();
	// Every icon in three sizes and four colors: 672 on screen.
	std::string rml;
	for (const char* size : {"small", "medium", "large"})
		for (const char* color : {"text", "accent", "danger", "muted"})
			for (const std::filesystem::path& file : files)
				rml += svg(file, std::string("class=\"") + size + " " + color + "\"");
	graphics.show(rml);
	Rml::ElementList shown;
	graphics.document->GetElementsByTagName(shown, "svg");
	CHECK(shown.size() == 672);

	// The first frame rasterizes each icon once per size, whatever its color.
	const std::vector<ui::Image> first = graphics.draw();
	CHECK(first.size() == files.size() * 3);

	// Steady frames rasterize nothing, nor does a change of color, as on hover.
	for (int frame = 0; frame < 100; ++frame)
		CHECK(graphics.draw().empty());
	Rml::ElementList accents;
	graphics.document->GetElementsByClassName(accents, "accent");
	for (Rml::Element* element : accents)
		element->SetClass("hover", true);
	CHECK(graphics.draw().empty());

	// A new UI scale rasterizes them again at their new pixel sizes and releases the old rasters, and then they are
	// steady again. At 125 % no new size is an old one, whose rasters would be kept.
	const size_t released = graphics.fake.renderer.releasedTextures();
	graphics.fake.shell.setUiScale(1.25F);
	const std::vector<ui::Image> scaled = graphics.draw();
	CHECK(scaled.size() == files.size() * 3);
	CHECK(graphics.fake.renderer.releasedTextures() == released + (files.size() * 3));
	for (const ui::Image& texture : scaled)
	{
		const bool known = texture.width == 20 || texture.width == 30 || texture.width == 40;
		CHECK(known);
		CHECK(texture.height == texture.width);
	}
	for (int frame = 0; frame < 10; ++frame)
		CHECK(graphics.draw().empty());
}

TEST_CASE("an SVG decorator finds its file next to the style sheet that names it", "[unit]")
{
	// A theme's style sheet names its graphics next to itself, wherever the document that uses it is.
	const test::TempDir temp;
	std::filesystem::create_directories(temp.path() / "theme");
	std::filesystem::create_directories(temp.path() / "documents");
	std::filesystem::copy_file(
		resources() / "themes" / "gnubase" / "knob-base.svg", temp.path() / "theme" / "knob.svg");
	write(temp.path() / "theme" / "theme.rcss", R"(
		div { display: block; width: 30dp; height: 30dp; decorator: svg(knob.svg); }
		div.skin { decorator: svg-ninepatch(knob.svg, 4); })");
	write(temp.path() / "documents" / "page.rml", R"(<rml><head>
		<link type="text/rcss" href="../theme/theme.rcss"/></head><body><div/><div class="skin"/></body></rml>)");

	test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	const ui::Window& window = fake.shell.open(Graphics::spec());
	Rml::ElementDocument* document =
		window.context().LoadDocument((temp.path() / "documents" / "page.rml").generic_u8string());
	REQUIRE(document != nullptr);
	document->Show();
	fake.shell.step(milliseconds(0));

	// The plain decorator at the element's size, the skin at the file's own size.
	std::vector<std::pair<int, int>> sizes;
	for (const ui::Image& texture : fake.renderer.generatedTextures())
		sizes.emplace_back(texture.width, texture.height);
	std::sort(sizes.begin(), sizes.end());
	CHECK(sizes == std::vector<std::pair<int, int>>{{30, 30}, {40, 40}});
}

TEST_CASE("a 9-slice skin keeps its corners at their size and stretches its edges and middle", "[unit]")
{
	Graphics graphics;
	const std::filesystem::path button = resources() / "themes" / "gnubase" / "button-normal.svg"; // 24 x 24 dp
	graphics.show("<div style=\"display: block; width: 100dp; height: 40dp; decorator: svg-ninepatch(&quot;" +
		button.generic_u8string() + "&quot;, 6 6 6 6);\"/>");
	for (const float scale : {1.0F, 1.5F, 2.0F})
	{
		INFO("UI scale " << scale);
		graphics.fake.shell.setUiScale(scale);
		const size_t compiled = graphics.fake.renderer.compiledGeometry().size();
		const std::vector<ui::Image> textures = graphics.draw();

		// The skin is rasterized at its own size in pixels, not the element's.
		REQUIRE(textures.size() == 1);
		CHECK(textures[0].width == static_cast<int>(24.0F * scale));
		CHECK(textures[0].height == static_cast<int>(24.0F * scale));

		// Nine patches on a grid of four by four vertices: the corners are 6 dp, the edges and middle stretch.
		std::vector<std::vector<Rml::Vertex>> patches;
		const auto& geometry = graphics.fake.renderer.compiledGeometry();
		for (size_t i = compiled; i < geometry.size(); ++i)
			if (geometry[i].size() == 16)
				patches.push_back(geometry[i]);
		REQUIRE(patches.size() == 1);
		const std::array<float, 4> columns = {0.0F, 6.0F * scale, 94.0F * scale, 100.0F * scale};
		const std::array<float, 4> rows = {0.0F, 6.0F * scale, 34.0F * scale, 40.0F * scale};
		const std::array<float, 4> texture = {0.0F, 0.25F, 0.75F, 1.0F};
		for (size_t y = 0; y < 4; ++y)
		{
			for (size_t x = 0; x < 4; ++x)
			{
				const Rml::Vertex& vertex = patches[0][(y * 4) + x];
				CHECK(vertex.position.x == Catch::Approx(columns.at(x)));
				CHECK(vertex.position.y == Catch::Approx(rows.at(y)));
				CHECK(vertex.tex_coord.x == Catch::Approx(texture.at(x)));
				CHECK(vertex.tex_coord.y == Catch::Approx(texture.at(y)));
			}
		}
	}
}

TEST_CASE("a 9-slice skin is rasterized once per scale, whatever the sizes of the elements it draws", "[unit]")
{
	Graphics graphics;
	const std::filesystem::path field = resources() / "themes" / "gnubase" / "field-normal.svg"; // 24 x 24 dp
	std::string rml;
	for (const int width : {30, 80, 200, 420})
		rml += "<div style=\"display: block; width: " + std::to_string(width) +
			"dp; height: 24dp; decorator: svg-ninepatch(&quot;" + field.generic_u8string() + "&quot;, 6);\"/>";
	graphics.show(rml);
	const std::vector<ui::Image> first = graphics.draw();
	REQUIRE(first.size() == 1);
	CHECK(first[0].width == 24);
	for (int frame = 0; frame < 10; ++frame)
		CHECK(graphics.draw().empty());
	graphics.fake.shell.setUiScale(2.0F);
	const std::vector<ui::Image> scaled = graphics.draw();
	REQUIRE(scaled.size() == 1);
	CHECK(scaled[0].width == 48);
}
