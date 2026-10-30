// SPDX-License-Identifier: GPL-3.0-or-later
// Themes (#53, docs/features/themes.md): the shared style sheets of the design system in resources/styles and the
// themes in resources/themes reach every document, the active theme gives it its colors, fonts and skins, and the
// shell switches themes without loading documents again. tests/theme_tests.cpp checks the generated theme files;
// tests/window_tests.cpp draws a page of widgets in every theme and compares it with golden images.
#include "testutils/fakeui.h"
#include "testutils/testutils.h"
#include "ui/fonts.h"
#include "ui/shell.h"
#include "ui/themes.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Property.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using std::chrono::milliseconds;

namespace
{
	std::filesystem::path resources()
	{
		return {GNUBASE_RESOURCES_DIR};
	}

	void write(const std::filesystem::path& file, const std::string& content)
	{
		std::filesystem::create_directories(file.parent_path());
		std::ofstream out(file, std::ios::binary);
		out << content;
	}

	// A theme's design tokens (resources/themes/<theme>/tokens.json).
	nlohmann::json tokens(const std::string& theme)
	{
		return nlohmann::json::parse(test::readText(resources() / "themes" / theme / "tokens.json"));
	}

	// A color token, such as "#f1f4ff", as RmlUi's color.
	Rml::Colourb color(const nlohmann::json& token)
	{
		const std::string hex = token.get<std::string>();
		REQUIRE(hex.size() == 7);
		const auto channel = [&](size_t at) { return static_cast<uint8_t>(std::stoi(hex.substr(at, 2), nullptr, 16)); };
		return {channel(1), channel(3), channel(5)};
	}

	ui::WindowSpec spec()
	{
		ui::WindowSpec spec;
		spec.bounds = ui::Rect{0, 0, 800, 600};
		return spec;
	}

	// The element's decorators as RCSS, e.g. "svg-ninepatch(button-normal.svg, 6 6 6 6)".
	std::string decorator(Rml::Element& element)
	{
		const Rml::Property* property = element.GetProperty("decorator");
		return property != nullptr ? property->ToString() : std::string();
	}
}

TEST_CASE("GNUbase's themes are the folders in resources/themes with a theme style sheet", "[unit]")
{
	const test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	const ui::Themes themes(resources());
	CHECK(themes.names() == std::vector<std::string>{"dark", "gnubase", "light"});
	CHECK(themes.has(ui::g_defaultTheme));
	CHECK(themes.has("light"));
	CHECK(!themes.has("neon"));
	CHECK(!themes.has(""));
}

TEST_CASE("resources without themes, or a theme style sheet outside its media query, are an error", "[unit]")
{
	const test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	const test::TempDir temp;
	write(temp.path() / "styles" / "base.rcss", "body { display: block; }");
	CHECK_THROWS_WITH(ui::Themes(temp.path()), ContainsSubstring("themes"));

	// A theme's rules apply only while it is active.
	write(temp.path() / "themes" / "neon" / "theme.rcss", "body { color: #ff00ff; }");
	CHECK_THROWS_WITH(ui::Themes(temp.path()), ContainsSubstring("neon") && ContainsSubstring("@media (theme: neon)"));
	write(temp.path() / "themes" / "neon" / "theme.rcss", "@media (theme: neon) { body { color: #ff00ff; } }");
	CHECK(ui::Themes(temp.path()).names() == std::vector<std::string>{"neon"});
}

TEST_CASE("every document gets the design system and the active theme's colors, fonts and skins", "[unit]")
{
	test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	ui::Fonts(resources() / "fonts").load();
	const ui::Themes themes(resources());
	ui::Window& window = fake.shell.open(spec());
	fake.shell.setTheme(ui::g_defaultTheme);
	// The document only lays out; it names no theme and no style sheet.
	Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(R"(<rml><head><style>
		button.wide { padding-left: 40dp; }
	</style></head><body><button id="ok">OK</button><button id="wide" class="wide">Wide</button>
		<input type="text" id="name" value="Lead Synth"/></body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	Rml::Element* ok = document->GetElementById("ok");
	Rml::Element* wide = document->GetElementById("wide");
	Rml::Element* name = document->GetElementById("name");
	REQUIRE(ok != nullptr);
	REQUIRE(wide != nullptr);
	REQUIRE(name != nullptr);

	const auto update = [&]
	{
		window.requestRender();
		fake.shell.step(milliseconds(0));
	};
	for (const std::string theme : {"gnubase", "dark", "light", "gnubase"})
	{
		INFO("theme " << theme);
		fake.shell.setTheme(theme);
		update();
		const nlohmann::json token = tokens(theme);

		// The theme's colors and fonts.
		CHECK(document->GetComputedValues().background_color() == color(token.at("background")));
		CHECK(document->GetComputedValues().color() == color(token.at("text")));
		CHECK(document->GetComputedValues().font_family() == "inter");
		CHECK(document->GetComputedValues().font_size() == token.at("fontSizes").at("body").get<float>());

		// The theme's skins for each state.
		CHECK_THAT(decorator(*ok), ContainsSubstring("button-normal.svg"));
		ok->SetPseudoClass("hover", true);
		update();
		CHECK_THAT(decorator(*ok), ContainsSubstring("button-hover.svg"));
		ok->SetPseudoClass("hover", false);
		CHECK_THAT(decorator(*name), ContainsSubstring("field-normal.svg"));
		name->Focus();
		update();
		CHECK_THAT(decorator(*name), ContainsSubstring("field-focus.svg"));
		name->Blur();

		// The design system's layout, and the document's own, which comes last.
		CHECK(ok->GetComputedValues().padding_left().value == 10.0F);
		CHECK(wide->GetComputedValues().padding_left().value == 40.0F);
	}
}

TEST_CASE("a theme's style sheet that changed on disk applies to documents loaded again", "[unit]")
{
	// Live reload (ui/livereload.h) clears RmlUi's style sheet cache and loads the documents again.
	test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	const test::TempDir temp;
	write(temp.path() / "styles" / "base.rcss", "body { display: block; }");
	write(temp.path() / "themes" / "neon" / "theme.rcss", "@media (theme: neon) { body { color: #ff00ff; } }");
	const ui::Themes themes(temp.path());
	const ui::Window& window = fake.shell.open(spec());
	fake.shell.setTheme("neon");
	const char* rml = "<rml><body/></rml>";
	Rml::ElementDocument* before = window.context().LoadDocumentFromMemory(rml);
	REQUIRE(before != nullptr);
	fake.shell.step(milliseconds(0));
	CHECK(before->GetComputedValues().color() == Rml::Colourb(255, 0, 255));

	write(temp.path() / "themes" / "neon" / "theme.rcss", "@media (theme: neon) { body { color: #00ff00; } }");
	Rml::Factory::ClearStyleSheetCache();
	Rml::ElementDocument* after = window.context().LoadDocumentFromMemory(rml);
	REQUIRE(after != nullptr);
	fake.shell.step(milliseconds(0));
	CHECK(after->GetComputedValues().color() == Rml::Colourb(0, 255, 0));
}
