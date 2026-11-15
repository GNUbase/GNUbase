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
#include <initializer_list>
#include <string>
#include <utility>
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

	// The ids of the themes there are, sorted as ui::Themes keeps them.
	std::vector<std::string> ids(const ui::Themes& themes)
	{
		std::vector<std::string> found;
		for (const ui::Theme& theme : themes.themes())
			found.push_back(theme.id);
		return found;
	}

	// The name the UI shows for a theme; empty for one there isn't.
	std::string name(const ui::Themes& themes, const std::string& id)
	{
		for (const ui::Theme& theme : themes.themes())
			if (theme.id == id)
				return theme.name;
		return {};
	}
}

TEST_CASE("GNUbase's themes are the folders in resources/themes with a theme style sheet", "[unit]")
{
	const test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	const ui::Themes themes(resources());
	CHECK(ids(themes) == std::vector<std::string>{"dark", "gnubase", "high-contrast", "light"});
	// Each theme carries the name the UI shows for it, so that no document names a theme.
	CHECK(name(themes, "gnubase") == "GNUbase");
	CHECK(name(themes, "dark") == "Dark");
	CHECK(name(themes, "light") == "Light");
	CHECK(name(themes, "high-contrast") == "High Contrast");
	CHECK(name(themes, "neon").empty());
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
	const ui::Themes neon(temp.path());
	CHECK(ids(neon) == std::vector<std::string>{"neon"});
	// A theme without a manifest shows its id.
	CHECK(name(neon, "neon") == "neon");
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
	for (const std::string theme : {"gnubase", "dark", "light", "high-contrast", "gnubase"})
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

TEST_CASE("every theme dims the edit history's undone steps, details too, and draws its separator", "[unit]")
{
	test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	ui::Fonts(resources() / "fonts").load();
	const ui::Themes themes(resources());
	ui::Window& window = fake.shell.open(spec());
	// The edit history panel's elements (ui/historypanel.h, docs/features/edit-history.md).
	Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(R"(<rml><body>
		<listview class="history"><table><tbody><tr id="done"><td class="action">Add track</td></tr></tbody></table>
		<div class="history-grip"><div class="history-separator" id="separator"/></div>
		<table><tbody><tr id="undone" class="undone"><td class="action">Add track</td>
			<td class="details"><input type="text" id="details" value="note"/></td></tr></tbody></table></listview>
	</body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	Rml::Element* done = document->GetElementById("done");
	Rml::Element* undone = document->GetElementById("undone");
	Rml::Element* separator = document->GetElementById("separator");
	Rml::Element* details = document->GetElementById("details");
	REQUIRE(done != nullptr);
	REQUIRE(details != nullptr);
	REQUIRE(undone != nullptr);
	REQUIRE(separator != nullptr);

	for (const std::string theme : {"gnubase", "dark", "light"})
	{
		INFO("theme " << theme);
		fake.shell.setTheme(theme);
		window.requestRender();
		fake.shell.step(milliseconds(0));
		const nlohmann::json token = tokens(theme);
		CHECK(done->GetComputedValues().color() == color(token.at("text")));
		CHECK(undone->GetComputedValues().color() == color(token.at("textMuted")));
		CHECK(details->GetComputedValues().color() == color(token.at("textMuted")));
		CHECK(separator->GetComputedValues().background_color() == color(token.at("accent")));
		CHECK(separator->GetBox().GetSize().y > 0.0F);
	}
}

TEST_CASE("every theme draws play probability and velocity variance bars dashed, velocity bars solid", "[unit]")
{
	test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	ui::Fonts(resources() / "fonts").load();
	const ui::Themes themes(resources());
	ui::Window& window = fake.shell.open(spec());
	// The controller lanes' elements (ui/controllerlanes.h, docs/features/controller-lanes.md).
	Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(R"(<rml><body>
		<div class="controller-lane" lane-kind="velocity"><div class="controller-lane-area">
			<div class="controller-bar" id="velocity"/></div></div>
		<div class="controller-lane" lane-kind="probability"><div class="controller-lane-area">
			<div class="controller-bar" id="probability"/><div class="controller-bar selected" id="selected"/></div></div>
		<div class="controller-lane" lane-kind="velocityVariance"><div class="controller-lane-area">
			<div class="controller-bar inactive" id="variance"/></div></div>
	</body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	Rml::Element* velocity = document->GetElementById("velocity");
	REQUIRE(velocity != nullptr);
	for (const std::string theme : {"gnubase", "dark", "light", "high-contrast"})
	{
		INFO("theme " << theme);
		fake.shell.setTheme(theme);
		window.requestRender();
		fake.shell.step(milliseconds(0));
		CHECK_THAT(decorator(*velocity), !ContainsSubstring("gradient"));
		CHECK(velocity->GetComputedValues().background_color().alpha == 255);
		for (const char* id : {"probability", "selected", "variance"})
		{
			INFO(id);
			Rml::Element* bar = document->GetElementById(id);
			REQUIRE(bar != nullptr);
			CHECK_THAT(decorator(*bar), ContainsSubstring("repeating-linear-gradient"));
			CHECK(bar->GetComputedValues().background_color().alpha == 0);
		}
	}
}

TEST_CASE(
	"the bars' look reaches their sections' buttons, not their setup panes, in each state and with the focus", "[unit]")
{
	// #1342: the default theme draws the buttons of toolbars and the transport bar flat (#1328). The transport's setup
	// pane is filled into the transport bar and the transport panel together with its sections, and keeps framed
	// buttons, as the toolbar's setup pane outside its bar does. Menu buttons that show a value look pressed and
	// disabled, and every bar button shows the focus ring over its state. The test sets the states as pseudo-classes,
	// as the widgets do for the disabled attribute (ui/widgets.h).
	test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	ui::Fonts(resources() / "fonts").load();
	const ui::Themes themes(resources());
	ui::Window& window = fake.shell.open(spec());
	Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(R"(<rml><body>
		<div class="toolbar"><button id="tool">Tool</button><button id="tool-checked" class="checked">On</button>
			<button id="value" menu="value-menu">Bar</button>
			<button id="value-off" menu="value-menu">Bar</button>
			<button id="value-checked" class="checked" menu="value-menu">Bar</button>
			<button id="icon-menu" class="icon-only" menu="value-menu">C</button></div>
		<div class="strip transport"><div class="transport-section"><button id="locate">L</button>
			<button id="start" class="icon-only checked">S</button><button id="record" class="icon-only checked">R</button>
			<valuefield id="tempo"/><textdisplay id="signature" class="display"/></div>
			<menu class="setup-pane"><button id="bar-pane">Save Preset</button></menu></div>
		<div class="transport toolbar"><div class="transport-section"><button id="panel-locate">L</button></div>
			<menu class="setup-pane"><button id="panel-pane">Show All</button></menu></div>
		<menu class="setup-pane"><button id="pane">Save Preset</button></menu>
		<menu id="value-menu"><menuitem>Bar</menuitem></menu>
	</body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	const auto element = [document](const char* id)
	{
		Rml::Element* found = document->GetElementById(id);
		REQUIRE(found != nullptr);
		return found;
	};
	const auto update = [&]
	{
		window.requestRender();
		fake.shell.step(milliseconds(0));
	};
	// The decorator in a state, set by pseudo-classes, which are cleared afterwards.
	const auto in = [&](const char* id, std::initializer_list<const char*> states)
	{
		Rml::Element* button = element(id);
		for (const char* state : states)
			button->SetPseudoClass(state, true);
		update();
		const std::string skin = decorator(*button);
		for (const char* state : states)
			button->SetPseudoClass(state, false);
		return skin;
	};
	for (const std::string theme : {"gnubase", "dark", "light", "high-contrast"})
	{
		INFO("theme " << theme);
		fake.shell.setTheme(theme);
		const bool flat = theme == "gnubase";

		// Setup panes inside a bar look like the one outside, in every state.
		for (const char* id : {"bar-pane", "panel-pane"})
		{
			INFO(id);
			for (const auto& states : std::vector<std::vector<const char*>>{{}, {"hover"}, {"active"}})
			{
				Rml::Element* outside = element("pane");
				Rml::Element* inside = element(id);
				for (const char* state : states)
				{
					outside->SetPseudoClass(state, true);
					inside->SetPseudoClass(state, true);
				}
				update();
				CHECK(decorator(*inside) == decorator(*outside));
				CHECK(inside->GetComputedValues().color() == outside->GetComputedValues().color());
				for (const char* state : states)
				{
					outside->SetPseudoClass(state, false);
					inside->SetPseudoClass(state, false);
				}
			}
		}
		CHECK_THAT(in("pane", {}), ContainsSubstring("button-normal.svg"));
		// The sections' buttons in the bar's look: flat in the default theme, framed in the others.
		for (const char* id : {"tool", "locate", "panel-locate"})
		{
			INFO(id);
			if (flat)
				CHECK_THAT(in(id, {}), !ContainsSubstring(".svg"));
			else
				CHECK_THAT(in(id, {}), ContainsSubstring("button-normal.svg"));
		}

		// A menu button that shows a value: a field, pressed under the mouse button, dimmed when disabled.
		CHECK_THAT(in("value", {"hover", "active"}), ContainsSubstring("pressed.svg"));
		CHECK_THAT(in("value-off", {"disabled"}), ContainsSubstring("disabled.svg"));
		if (flat)
		{
			CHECK_THAT(in("value", {"hover", "active"}), ContainsSubstring("dropdown-button-pressed.svg"));
			CHECK_THAT(in("value-off", {"disabled"}), ContainsSubstring("dropdown-button-disabled.svg"));
			// An icon-only menu button stays flat, its icon the arrow.
			for (const char* state : {"active", "disabled"})
			{
				INFO(state);
				CHECK_THAT(in("icon-menu", {state}), !ContainsSubstring(".svg"));
			}
		}

		// The focus ring over every state of a bar button.
		const std::vector<std::pair<const char*, std::vector<const char*>>> focused{{"tool", {}}, {"tool", {"hover"}},
			{"tool-checked", {}}, {"value", {}}, {"value", {"hover"}}, {"value-checked", {}}, {"icon-menu", {"hover"}},
			{"start", {}}, {"record", {}}, {"locate", {"hover"}}};
		for (const auto& [id, states] : focused)
		{
			INFO(id << " " << states.size() << " states");
			std::vector<const char*> with = states;
			with.push_back("focus-visible");
			Rml::Element* button = element(id);
			for (const char* state : with)
				button->SetPseudoClass(state, true);
			update();
			CHECK_THAT(decorator(*button), ContainsSubstring("focus-ring.svg"));
			for (const char* state : with)
				button->SetPseudoClass(state, false);
		}

		// The tempo is plain digits in the transport bar, as the time displays and the signature.
		update();
		CHECK(decorator(*element("tempo")) == decorator(*element("signature")));
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
