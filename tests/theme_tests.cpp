// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	// What LunaSVG renders: shapes, groups, gradients, clipping and masks; no text, filters, images or CSS.
	constexpr std::array<std::string_view, 16> g_allowedElements = {"svg", "rect", "circle", "ellipse", "line",
		"polyline", "polygon", "path", "g", "defs", "use", "linearGradient", "radialGradient", "stop", "clipPath",
		"mask"};

	std::filesystem::path themesDir()
	{
		return std::filesystem::path(GNUBASE_RESOURCES_DIR) / "themes";
	}

	std::vector<std::string> themes()
	{
		return gnubase::test::themes(GNUBASE_RESOURCES_DIR);
	}

	nlohmann::json readJson(const std::filesystem::path& file)
	{
		return nlohmann::json::parse(gnubase::test::readText(file));
	}

	// The selectors of a theme style sheet's rules, in order: the lines before those that open a block, without the
	// media query, which names the theme.
	std::vector<std::string> selectors(const std::string& sheet)
	{
		std::vector<std::string> result;
		std::string previous;
		size_t start = 0;
		while (start < sheet.size())
		{
			size_t end = sheet.find('\n', start);
			if (end == std::string::npos)
				end = sheet.size();
			const std::string line = sheet.substr(start, end - start);
			if (line.find('{') != std::string::npos && previous.rfind("@media", 0) != 0)
				result.push_back(previous);
			previous = line;
			start = end + 1;
		}
		return result;
	}

	// The rules of a theme style sheet: each selector with the declarations between its braces.
	std::vector<std::pair<std::string, std::string>> rules(const std::string& sheet)
	{
		std::vector<std::pair<std::string, std::string>> result;
		const std::vector<std::string> names = selectors(sheet);
		size_t at = sheet.find('{', sheet.find('{') + 1); // past the media query's brace
		for (const std::string& name : names)
		{
			const size_t close = sheet.find('}', at);
			result.emplace_back(name, sheet.substr(at, close - at));
			at = sheet.find('{', close);
		}
		return result;
	}

	// WCAG 2's relative luminance of a #rrggbb color, from 0 (black) to 1 (white).
	double luminance(const std::string& color)
	{
		double sum = 0.0;
		const std::array<double, 3> weights = {0.2126, 0.7152, 0.0722};
		for (size_t i = 0; i < 3; ++i)
		{
			const double c = std::stoi(color.substr(1 + (2 * i), 2), nullptr, 16) / 255.0;
			sum += weights[i] * (c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
		}
		return sum;
	}

	// WCAG 2's contrast ratio of two #rrggbb colors, from 1 (the same) to 21 (black and white).
	double contrast(const std::string& a, const std::string& b)
	{
		const double la = luminance(a);
		const double lb = luminance(b);
		return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
	}

	void checkSvg(const std::filesystem::path& file, const nlohmann::json& asset)
	{
		const std::string text = gnubase::test::readText(file);
		CHECK(text.rfind("<!-- SPDX-License-Identifier: GPL-3.0-or-later -->", 0) == 0);
		pugi::xml_document document;
		REQUIRE(document.load_string(text.c_str()));
		const pugi::xml_node svg = document.child("svg");
		REQUIRE(svg);
		const std::string width = std::to_string(asset.at("width").get<int>());
		const std::string height = std::to_string(asset.at("height").get<int>());
		CHECK(std::string(svg.attribute("viewBox").value()) == "0 0 " + width + " " + height);
		CHECK(std::string(svg.attribute("width").value()) == width);
		CHECK(std::string(svg.attribute("height").value()) == height);

		for (const pugi::xpath_node& node : document.select_nodes("//*"))
		{
			const pugi::xml_node element = node.node();
			CHECK(std::find(g_allowedElements.begin(), g_allowedElements.end(), element.name()) !=
				g_allowedElements.end());
			CHECK(element.attribute("style").empty());
			CHECK(element.attribute("class").empty());
			const std::string href = element.attribute("href").value();
			CHECK((href.empty() || href[0] == '#')); // no external references
		}
	}
}

TEST_CASE("every theme has the same assets, each a renderable SVG of its manifest's size", "[unit]")
{
	const nlohmann::json reference = readJson(themesDir() / "gnubase" / "manifest.json").at("assets");
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		const std::filesystem::path dir = themesDir() / theme;
		const nlohmann::json manifest = readJson(dir / "manifest.json");
		CHECK(manifest.at("theme") == theme);
		// The name the UI shows for the theme (ui::Themes), so that no document has to name one.
		// Not CHECK_FALSE: Catch2's ContinueOnFailure | FalseTest trips clang-analyzer's EnumCastOutOfRange check
		CHECK(!manifest.value("displayName", std::string()).empty());
		const nlohmann::json& assets = manifest.at("assets");
		CHECK(assets.size() == reference.size());

		for (const auto& [name, asset] : assets.items())
		{
			INFO("asset " << name);
			CHECK(reference.contains(name));
			checkSvg(dir / asset.at("file").get<std::string>(), asset);
			if (const auto slice = asset.find("slice"); slice != asset.end())
			{
				// The corners stay as they are; something must be left in the middle to stretch.
				CHECK(slice->at("left").get<int>() + slice->at("right").get<int>() < asset.at("width").get<int>());
				CHECK(slice->at("top").get<int>() + slice->at("bottom").get<int>() < asset.at("height").get<int>());
			}
		}

		// The theme's style sheet uses every skin but those its manifest marks unused, which belong to another look
		// (#1314), and styles the same widgets and states as every other theme.
		const std::string sheet = gnubase::test::readText(dir / "theme.rcss");
		CHECK(sheet.find("@media (theme: " + std::string(theme) + ")") != std::string::npos);
		for (const auto& [name, asset] : assets.items())
		{
			INFO("asset " << name);
			const std::string file = asset.at("file").get<std::string>();
			const bool used =
				sheet.find("(" + file + ")") != std::string::npos || sheet.find("(" + file + ",") != std::string::npos;
			CHECK(used != asset.value("unused", false));
		}
		CHECK(selectors(sheet) == selectors(gnubase::test::readText(themesDir() / "gnubase" / "theme.rcss")));

		const nlohmann::json tokens = readJson(dir / "tokens.json");
		for (const char* color : {"background", "surface", "surfaceRaised", "border", "text", "textMuted", "accent",
				 "accent2", "danger", "warning", "success"})
			CHECK(tokens.contains(color));
		CHECK(tokens.at("trackPalette").size() == 8);
		CHECK(tokens.at("fontFamilies").contains("ui"));
		CHECK(tokens.at("fontFamilies").contains("counter"));
		// The time display window's digits at each of its sizes (docs/features/transport-bar.md), larger each time.
		const nlohmann::json& digits = tokens.at("timeDisplaySizes");
		REQUIRE(digits.size() == 5);
		for (size_t size = 0; size < digits.size(); ++size)
		{
			INFO("time display size " << size);
			CHECK(sheet.find(".time-display.size-" + std::to_string(size)) != std::string::npos);
			if (size > 0)
				CHECK(digits.at(size).get<int>() > digits.at(size - 1).get<int>());
		}
	}
}

TEST_CASE("every theme shows the keyboard focus on every widget that takes it, in each of its states", "[unit]")
{
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		const std::string sheet = gnubase::test::readText(themesDir() / theme / "theme.rcss");
		const std::vector<std::pair<std::string, std::string>> found = rules(sheet);
		std::vector<std::string> focused; // the selectors that style :focus-visible
		for (const auto& [selector, declarations] : found)
			if (selector.find(":focus-visible") != std::string::npos)
			{
				INFO(selector);
				// The focus ring, over the widget's own skin for that state; menu items light up as under the mouse.
				CHECK((declarations.find("focus-ring.svg") != std::string::npos ||
					selector.find("menuitem") != std::string::npos));
				std::string part;
				for (std::istringstream parts(selector); std::getline(parts, part, ',');)
				{
					const size_t first = part.find_first_not_of(" \t");
					focused.push_back(part.substr(first, part.find_last_not_of(" \t\r") + 1 - first));
				}
			}
		// Hovered and pressed ones keep looking so with the focus, also while the mouse presses them.
		for (const char* widget : {"button", "button:hover", "button:active", "button.primary", "button.primary:hover",
				 "button.primary:active", "button.checked", "button.toggle", "button.toggle:hover",
				 "button.toggle.checked", "button.toggle.mute.checked", "input.checkbox", "input.checkbox:checked",
				 "input.radio", "input.radio:checked", "listview", "valuefield", "knob", "fader", "slider",
				 "input.range", "menuitem"})
		{
			INFO("widget " << widget);
			CHECK(std::find(focused.begin(), focused.end(), std::string(widget) + ":focus-visible") != focused.end());
		}
	}
}

TEST_CASE("the high contrast theme's text, accents and borders stand out from its backgrounds", "[unit]")
{
	const nlohmann::json tokens = readJson(themesDir() / "high-contrast" / "tokens.json");
	for (const char* background : {"background", "surface", "surfaceRaised"})
	{
		INFO("on " << background);
		const std::string under = tokens.at(background).get<std::string>();
		// WCAG 2's enhanced contrast for text (1.4.6), and the contrast of controls' borders (1.4.11).
		for (const char* text : {"text", "textMuted", "accent", "accent2", "danger", "warning", "success"})
		{
			INFO(text);
			CHECK(contrast(tokens.at(text).get<std::string>(), under) >= 7.0);
		}
		CHECK(contrast(tokens.at("border").get<std::string>(), under) >= 3.0);
		for (const nlohmann::json& color : tokens.at("trackPalette"))
			CHECK(contrast(color.get<std::string>(), under) >= 7.0);
	}
	// Text on accent-colored fills, such as the primary button's.
	CHECK(contrast(tokens.at("background").get<std::string>(), tokens.at("accent").get<std::string>()) >= 7.0);
	// The focus ring is thicker than in the other themes, and each theme's focus-ring.svg is as wide as its tokens say.
	const auto ring = [](const std::string& theme)
	{ return readJson(themesDir() / theme / "tokens.json").at("borderWidths").at("focusRing").get<int>(); };
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		// The ring is the first rect, the line in the background color inside it the second.
		pugi::xml_document document;
		REQUIRE(document.load_string(gnubase::test::readText(themesDir() / theme / "focus-ring.svg").c_str()));
		CHECK(document.child("svg").child("rect").attribute("stroke-width").as_int() == ring(theme));
		if (theme != "high-contrast")
			CHECK(ring("high-contrast") > ring(theme));
	}
}

TEST_CASE("the high contrast theme's labels and marks stand out from the fills under them", "[unit]")
{
	const std::filesystem::path dir = themesDir() / "high-contrast";
	const std::vector<std::pair<std::string, std::string>> sheet = rules(gnubase::test::readText(dir / "theme.rcss"));
	// The color, or another color property, that the last rule for the selector gives, alone or among others.
	const auto color = [&sheet](const std::string& selector, const std::string& property = "color")
	{
		std::string found;
		for (const auto& [selectors, declarations] : sheet)
		{
			const size_t first = selectors.find_first_not_of(" \t");
			const std::string list =
				" " + selectors.substr(first, selectors.find_last_not_of(" \t\r") + 1 - first) + ",";
			const size_t at = declarations.find("\t" + property + ": #");
			if (at != std::string::npos && list.find(" " + selector + ",") != std::string::npos)
				found = declarations.substr(at + property.size() + 3, 7);
		}
		REQUIRE(!found.empty());
		return found;
	};
	// An attribute of the first element of a skin that matches the XPath query, such as its fill.
	const auto attribute = [&dir](const std::string& file, const char* query, const char* name)
	{
		pugi::xml_document document;
		REQUIRE(document.load_string(gnubase::test::readText(dir / file).c_str()));
		return std::string(document.select_node(query).node().attribute(name).value());
	};
	// WCAG 2's enhanced contrast for text (1.4.6): labels on buttons filled with the accent or a toggle's color.
	for (const auto& [selector, skin] :
		std::vector<std::pair<std::string, std::string>>{{"button.primary", "button-primary-normal.svg"},
			{"button.toggle.checked", "toggle-on.svg"}, {"button.toggle.mute.checked", "toggle-mute.svg"},
			{"button.toggle.solo.checked", "toggle-solo.svg"}, {"button.toggle.record.checked", "toggle-record.svg"},
			{"button.toggle.monitor.checked", "toggle-monitor.svg"},
			{"button.toggle.listen.checked", "toggle-monitor.svg"}})
	{
		INFO(selector);
		CHECK(contrast(color(selector), attribute(skin, "//rect", "fill")) >= 7.0);
	}
	// The icons of record and monitor buttons and of toggles of no kind, such as Edit In-Place, too.
	for (const auto& [kind, skin] : std::vector<std::pair<std::string, std::string>>{
			 {".record", "toggle-record.svg"}, {".monitor", "toggle-monitor.svg"}, {"", "toggle-on.svg"}})
	{
		INFO("toggle" << kind);
		CHECK(contrast(color("button.toggle" + kind + ".checked:not(:disabled) svg.icon", "image-color"),
				  attribute(skin, "//rect", "fill")) >= 7.0);
	}
	// The marks of checked and mixed check boxes on the accent.
	CHECK(contrast(attribute("checkbox-on.svg", "//polyline", "stroke"),
			  attribute("checkbox-on.svg", "//rect", "fill")) >= 7.0);
	CHECK(contrast(attribute("checkbox-mixed.svg", "//line", "stroke"),
			  attribute("checkbox-mixed.svg", "//rect", "fill")) >= 7.0);
}

TEST_CASE("every theme's piano keyboard has light white keys and dark black keys", "[unit]")
{
	// The key editor's keyboard (docs/features/key-editor.md): a light theme's too, though its text is the dark color.
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		std::string white;
		std::string black;
		for (const auto& [selector, declarations] : rules(gnubase::test::readText(themesDir() / theme / "theme.rcss")))
		{
			const size_t first = selector.find_first_not_of(" \t");
			const std::string name = selector.substr(first, selector.find_last_not_of(" \t\r") + 1 - first);
			const size_t at = declarations.find("background-color: #");
			if (at == std::string::npos)
				continue;
			const std::string color = declarations.substr(at + std::string_view("background-color: ").size(), 7);
			if (name == ".key-editor-key")
				white = color;
			else if (name == ".key-editor-key.black")
				black = color;
		}
		REQUIRE(white.size() == 7);
		REQUIRE(black.size() == 7);
		CHECK(luminance(white) > luminance(black));
		CHECK(contrast(white, black) >= 3.0);
	}
}

TEST_CASE("every theme's mixer strip names are legible on any track color", "[unit]")
{
	// A strip's name on its track's color (docs/features/mixer-window.md), which can be any color: light text, or
	// dark text where the mixer marks the color .bright, by the same luminance as ui::Mixer.
	const double brightFrom = 0.179; // where black and white text have the same contrast
	std::vector<std::string> colors;
	const char* digits = "0123456789abcdef";
	for (int r = 0; r < 256; r += 17)
		for (int g = 0; g < 256; g += 17)
			for (int b = 0; b < 256; b += 17)
				colors.push_back(std::string("#") + digits[r / 16] + digits[r % 16] + digits[g / 16] + digits[g % 16] +
					digits[b / 16] + digits[b % 16]);
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		std::string light;
		std::string dark;
		for (const auto& [selector, declarations] : rules(gnubase::test::readText(themesDir() / theme / "theme.rcss")))
		{
			const size_t first = selector.find_first_not_of(" \t");
			const std::string name = selector.substr(first, selector.find_last_not_of(" \t\r") + 1 - first);
			const size_t at = declarations.find("\tcolor: #"); // not background-color
			if (at == std::string::npos)
				continue;
			const std::string color = declarations.substr(at + std::string_view("\tcolor: ").size(), 7);
			if (name == ".mixer-channel-name.colored")
				light = color;
			else if (name == ".mixer-channel-name.colored.bright")
				dark = color;
		}
		REQUIRE(light.size() == 7);
		REQUIRE(dark.size() == 7);
		CHECK(luminance(light) > luminance(dark));
		const nlohmann::json tokens = readJson(themesDir() / theme / "tokens.json");
		std::vector<std::string> all = colors;
		for (const nlohmann::json& item : tokens.at("trackPalette"))
			all.push_back(item.get<std::string>());
		double least = 21.0;
		for (const std::string& color : all)
			least = std::min(least, contrast(color, luminance(color) > brightFrom ? dark : light));
		CHECK(least >= 4.5); // WCAG 2's contrast for text (1.4.3)
	}
}

TEST_CASE("every theme shows the chord pads' black section and modifier keys as such inside the remote range", "[unit]")
{
	// #1447: what the engine takes a key as wins over the remote range, on black keys too. The rules live in the
	// generated style sheets (tools/skin/generate.py), so a regeneration that drops them or puts them before the
	// black remote keys' rule fails here.
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		const std::vector<std::pair<std::string, std::string>> found =
			rules(gnubase::test::readText(themesDir() / theme / "theme.rcss"));
		// The index of the last rule that sets the background color of selector, or -1.
		const auto last = [&found](const std::string& selector)
		{
			int result = -1;
			for (size_t i = 0; i < found.size(); ++i)
			{
				std::string names = found.at(i).first;
				names.erase(
					std::remove_if(names.begin(), names.end(), [](unsigned char c) { return std::isspace(c) != 0; }),
					names.end());
				if (("," + names + ",").find("," + selector + ",") != std::string::npos &&
					found.at(i).second.find("background-color:") != std::string::npos)
					result = static_cast<int>(i);
			}
			return result;
		};
		const int remote = last(".chord-pads-key.black.remote");
		REQUIRE(remote >= 0);
		CHECK(last(".chord-pads-key.black.section") > remote);
		CHECK(last(".chord-pads-key.black.modifier") > remote);
	}
}

TEST_CASE("every theme dims the audio part editor's events that their part hides entirely", "[unit]")
{
	// The audio part editor (docs/features/audio-part-editor.md): an event out of its part doesn't play there, so it
	// is styled and looks dimmed, not like an event that plays.
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		std::string playing;
		std::string outside;
		for (const auto& [selector, declarations] : rules(gnubase::test::readText(themesDir() / theme / "theme.rcss")))
		{
			const size_t at = declarations.find("background-color: #");
			if (at == std::string::npos)
				continue;
			const std::string color = declarations.substr(at + std::string_view("background-color: ").size(), 7);
			const size_t first = selector.find_first_not_of(" \t");
			const std::string name = selector.substr(first, selector.find_last_not_of(" \t\r") + 1 - first);
			if (name == ".ape-event")
				playing = color;
			else if (name.find(".ape-event.outside") != std::string::npos)
				outside = color;
		}
		REQUIRE(playing.size() == 7);
		REQUIRE(outside.size() == 7);
		CHECK(outside != playing);
	}
}

TEST_CASE("the default theme's bars, tabs and inspector headers are flat, the other themes' framed as before", "[unit]")
{
	// #1328: the default theme follows the mockup; dark, light and high contrast keep their framed widgets, with the
	// same selectors as the default theme. Their drop-down button and disclosure skins are the plain button's, so the
	// rules that use them change nothing. The bars' rules reach the transport's sections, not its setup pane, which
	// is filled in with them (#1342; tests/uithemes_tests.cpp checks the cascade).
	const std::string header = ".inspector-section button.section-header";
	const std::string collapsed = ".inspector-section.collapsed button.section-header";
	// The selector of the buttons in bars with more classes or attributes and a state, as tools/skin/generate.py
	// writes it.
	const auto bars = [](const std::string& more, const std::string& state)
	{
		return ".toolbar:not(.transport) button" + more + ":not(.toggle):not(.primary)" + state +
			", .transport .transport-section button" + more + ":not(.toggle):not(.primary)" + state;
	};
	const std::string bar = bars("", "");
	const std::string menu = bars("[menu]", "");
	// A property's value in a rule's declarations, or empty.
	const auto value = [](const std::string& declarations, const std::string& property)
	{
		const size_t at = declarations.find("\t" + property + ": ");
		if (at == std::string::npos)
			return std::string();
		const size_t from = at + property.size() + 3;
		return declarations.substr(from, declarations.find(';', from) - from);
	};
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		const std::filesystem::path dir = themesDir() / theme;
		std::vector<std::pair<std::string, std::string>> found; // the last declarations of each selector
		for (const auto& [selector, declarations] : rules(gnubase::test::readText(dir / "theme.rcss")))
		{
			const size_t first = selector.find_first_not_of(" \t");
			const std::string name = selector.substr(first, selector.find_last_not_of(" \t\r") + 1 - first);
			// No rule reaches all the transport's buttons, its setup pane's too, only those of its sections or one by
			// its id, nor all a toolbar's, which the transport panel is; rules for buttons in bars leave toggle and
			// primary buttons their own skins.
			CHECK(name.find(".toolbar button") == std::string::npos);
			for (size_t at = name.find(".transport button"); at != std::string::npos;
				at = name.find(".transport button", at + 1))
				CHECK(name.compare(at, std::string_view(".transport button#").size(), ".transport button#") == 0);
			if (name.rfind(".toolbar:not(.transport) button", 0) == 0 ||
				name.rfind(".transport .transport-section button", 0) == 0)
			{
				INFO(name);
				CHECK(name.find(":not(.toggle):not(.primary)") != std::string::npos);
			}
			found.emplace_back(name, declarations);
		}
		const auto last = [&found](const std::string& name)
		{
			const auto at =
				std::find_if(found.rbegin(), found.rend(), [&name](const auto& rule) { return rule.first == name; });
			return at == found.rend() ? std::string() : at->second;
		};
		// A disabled menu button looks disabled also when checked, as every disabled bar button does (#1711): its rule
		// has the checked one's specificity and comes after it.
		const auto position = [&found](const std::string& name)
		{
			const auto at =
				std::find_if(found.rbegin(), found.rend(), [&name](const auto& rule) { return rule.first == name; });
			REQUIRE(at != found.rend());
			return std::distance(at, found.rend());
		};
		CHECK(position(bars("[menu]", ":disabled")) > position(bars(".checked[menu]", "")));
		const auto file = [&dir](const char* asset) { return gnubase::test::readText(dir / asset); };
		const nlohmann::json tokens = readJson(dir / "tokens.json");
		const auto token = [&tokens](const char* name) { return tokens.at(name).get<std::string>(); };
		if (std::string_view(theme) == "gnubase")
		{
			CHECK(last(bar).find("decorator: none") != std::string::npos);
			CHECK(last(bar).find("border-radius") != std::string::npos);
			CHECK(last(menu).find("dropdown-button-normal.svg") != std::string::npos);
			CHECK(last("tabset tab").find("decorator: none") != std::string::npos);
			CHECK(last(header).find("disclosure-open.svg") != std::string::npos);
			CHECK(last(collapsed).find("disclosure-closed.svg") != std::string::npos);
			// Bar buttons: a soft fill under the mouse, the accent when checked.
			CHECK(value(last(bars("", ":hover")), "decorator") == "none");
			CHECK(value(last(bars("", ":hover")), "background-color").rfind('#', 0) == 0);
			CHECK(value(last(bars(".checked", "")), "color") == token("accent"));
			// Start in the accent, filled while playing; Record in the danger color, filled while recording; the
			// primary time display bold and in the titles' size (#1342).
			CHECK(value(last(".transport #start svg.icon"), "image-color") == token("accent"));
			CHECK(value(last(".transport button#start.checked"), "background-color") == token("accent"));
			CHECK(value(last(".transport #record svg.icon"), "image-color") == token("danger"));
			CHECK(value(last(".transport button#record.checked"), "background-color") == token("danger"));
			CHECK(value(last(".transport .primary-time"), "font-weight") == "600");
			CHECK(value(last(".transport .primary-time"), "color") == token("text"));
			CHECK(value(last(".transport .primary-time"), "font-size") ==
				std::to_string(tokens.at("fontSizes").at("title").get<int>()) + "dp");
			// Zones as rounded panels on the surface.
			CHECK(!value(last(".docking [zone]"), "border-radius").empty());
			CHECK(value(last(".docking [zone]"), "background-color") == token("surface"));
			// The zone with the keyboard focus framed by a hairline in the accent, as round as the zone (#1342).
			CHECK(value(last(".docking .zone-frame"), "border-width") ==
				std::to_string(tokens.at("borderWidths").at("hairline").get<int>()) + "dp");
			CHECK(value(last(".docking .zone-frame"), "border-color").rfind(token("accent"), 0) == 0);
			CHECK(value(last(".docking .zone-frame"), "border-radius") ==
				value(last(".docking [zone]"), "border-radius"));
			// The tempo plain digits, as the time displays (#1342).
			CHECK(value(last(".transport .transport-section valuefield"), "decorator") == "none");
			// The fader's slot keeps its round ends in the nine-patch's corners, which don't stretch (#1342): from the
			// slot's top and bottom edges a radius inwards.
			const nlohmann::json slice = readJson(dir / "manifest.json").at("assets").at("fader-track").at("slice");
			const std::string slot = file("fader-track.svg");
			const size_t slotAt = slot.find("<rect", slot.find("<rect") + 1);
			REQUIRE(slotAt != std::string::npos);
			const auto attribute = [&slot, slotAt](const std::string& name)
			{
				const size_t at = slot.find(" " + name + "=\"", slotAt) + name.size() + 3;
				return std::stod(slot.substr(at, slot.find('"', at) - at));
			};
			const double end = attribute("y") + attribute("rx");
			CHECK(slice.at("top").get<double>() >= end);
			CHECK(slice.at("bottom").get<double>() >= end);
			// The selected track list row's rounded outline keeps its corners in the nine-patch's corners too, the
			// color strip at its left included (#1314).
			const nlohmann::json row = readJson(dir / "manifest.json").at("assets").at("track-row-selected");
			const std::string lit = file("track-row-selected.svg");
			const size_t litAt = lit.find("<rect");
			REQUIRE(litAt != std::string::npos);
			const auto litAttribute = [&lit, litAt](const std::string& name)
			{
				const size_t at = lit.find(" " + name + "=\"", litAt) + name.size() + 3;
				return std::stod(lit.substr(at, lit.find('"', at) - at));
			};
			const double radius = litAttribute("rx");
			CHECK(radius > 0);
			const nlohmann::json& rowSlice = row.at("slice");
			CHECK(rowSlice.at("left").get<double>() >= litAttribute("x") + radius);
			CHECK(rowSlice.at("top").get<double>() >= litAttribute("y") + radius);
			CHECK(rowSlice.at("right").get<double>() >=
				row.at("width").get<double>() - litAttribute("x") - litAttribute("width") + radius);
			CHECK(rowSlice.at("bottom").get<double>() >=
				row.at("height").get<double>() - litAttribute("y") - litAttribute("height") + radius);
			// Lanes and the track list on the panels' surface over the window's background, the selected row outlined.
			const std::string background = token("background");
			CHECK(value(last(".event-display"), "background-color") == token("surface"));
			CHECK(value(last(".track-list"), "background-color").rfind('#', 0) == 0);
			CHECK(last(".track-list").find("background-color: " + background) == std::string::npos);
			CHECK(last(".track-list listitem.selected").find("list-item-selected.svg") != std::string::npos);
			CHECK(file("list-item-selected.svg").find("stroke=") != std::string::npos);
			// Mixer channel strips as raised panels, the transport bar's time displays as plain digits.
			CHECK(last(".mixer-channel").find("border-radius") != std::string::npos);
			CHECK(last(".transport .display").find("decorator: none") != std::string::npos);
			// A light fader cap with grip lines; a knob without a fixed accent arc.
			CHECK(file("fader-cap.svg").find("linearGradient") != std::string::npos);
			CHECK(file("knob-base.svg").find(readJson(dir / "tokens.json").at("accent").get<std::string>()) ==
				std::string::npos);
			for (const char* arrow : {"dropdown-button-normal.svg", "disclosure-open.svg", "disclosure-closed.svg"})
			{
				INFO(arrow);
				CHECK(file(arrow).find("<polyline") != std::string::npos);
			}
		}
		else
		{
			CHECK(last(bar).find("button-normal.svg") != std::string::npos);
			CHECK(last("tabset tab").find("tab-normal.svg") != std::string::npos);
			CHECK(last(".toolbar").find("toolbar.svg") != std::string::npos);
			// The bar rules outrank the general button rules, so each state of a button in a bar, plain or a menu
			// button, gives what the general rule of that state gives: its skin (a drop-down button's skin is the
			// plain button's), and its color where it sets one. So do the inspector headers' states.
			const auto same = [&](const std::string& selector, const std::string& general)
			{
				INFO(selector << " as " << general);
				const std::string own = last(selector);
				REQUIRE(!own.empty());
				std::string skin = value(own, "decorator");
				if (const size_t at = skin.find("dropdown-button-"); at != std::string::npos)
					skin.erase(at, std::string_view("dropdown-").size());
				CHECK(skin == value(last(general), "decorator"));
				if (const std::string color = value(last(general), "color"); !color.empty())
					CHECK(value(own, "color") == color);
			};
			for (const char* state : {":hover", ":active", ":disabled"})
				same(bars("", state), std::string("button") + state);
			for (const char* state : {".checked", ".suspended", ".tapping"})
				same(bars(state, ""), std::string("button") + state);
			same(bars("[menu]", ":hover"), "button:hover");
			same(bars("[menu]", ":active"), "button:active");
			same(bars(".checked[menu]", ""), "button.checked");
			same(bars("[menu]", ":disabled"), "button:disabled");
			same(header + ":hover", "button:hover");
			same(header + ":active", "button:active");
			const std::string background = readJson(dir / "tokens.json").at("background").get<std::string>();
			// The lanes stay on the window's background.
			for (const auto& [name, declarations] : found)
				if (name == ".event-display" && declarations.find("background-color") != std::string::npos)
					CHECK(declarations.find("background-color: " + background) != std::string::npos);
			CHECK(file("dropdown-button-normal.svg") == file("button-normal.svg"));
			CHECK(file("dropdown-button-hover.svg") == file("button-hover.svg"));
			CHECK(file("dropdown-button-pressed.svg") == file("button-pressed.svg"));
			CHECK(file("dropdown-button-disabled.svg") == file("button-disabled.svg"));
			// The primary time display in the displays' size, as before.
			CHECK(value(last(".transport .primary-time"), "font-size").empty());
			// The tempo keeps the displays' skin.
			CHECK(value(last(".transport .transport-section valuefield"), "decorator") ==
				value(last(".display, valuefield"), "decorator"));
			CHECK(file("disclosure-open.svg") == file("button-normal.svg"));
			CHECK(file("disclosure-closed.svg") == file("button-normal.svg"));
		}
	}
}

TEST_CASE("every theme marks the arranger editor's row that is playing", "[unit]")
{
	// The arranger editor (docs/features/arranger.md): the row of the chain step or event playing carries .current and
	// the theme decides how that looks, since the document's own locate arrows show on every row and mark nothing. The
	// rule lives in the generated style sheets (tools/skin/generate.py), so a regeneration that drops it fails here.
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		bool marked = false;
		for (const auto& [selector, declarations] : rules(gnubase::test::readText(themesDir() / theme / "theme.rcss")))
		{
			const size_t first = selector.find_first_not_of(" \t");
			const std::string name = selector.substr(first, selector.find_last_not_of(" \t\r") + 1 - first);
			if (name == ".arranger-row.current")
				marked = declarations.find(':') != std::string::npos; // it says how the row looks
		}
		CHECK(marked);
	}
}

TEST_CASE("every theme draws the arranger event being drawn as an outline", "[unit]")
{
	// The arranger track's lane (docs/features/arranger.md) shows the event being drawn as an
	// .arranger-lane-event.drawing while the button is down: without a fill, so that it doesn't look like an event the
	// engine has (#1696). The rule lives in the generated style sheets (tools/skin/generate.py).
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		std::string drawing;
		for (const auto& [selector, declarations] : rules(gnubase::test::readText(themesDir() / theme / "theme.rcss")))
		{
			const size_t first = selector.find_first_not_of(" \t");
			const std::string name = selector.substr(first, selector.find_last_not_of(" \t\r") + 1 - first);
			if (name == ".arranger-lane-row .arranger-lane-event.drawing")
				drawing = declarations;
		}
		CHECK(drawing.find("background-color: transparent") != std::string::npos);
	}
}

TEST_CASE("every theme marks a checked toggle button of any kind", "[unit]")
{
	// The mixer strip's section switches and the inspector's toggles have no kind of their own, such as mute or solo:
	// a checked one has a skin of its own, from a rule after those of toggles that are off or under the mouse, which
	// have the same specificity (#2070).
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		std::map<std::string, std::pair<size_t, std::string>> found; // selector: its index and decorator
		const std::vector<std::pair<std::string, std::string>> sheet =
			rules(gnubase::test::readText(themesDir() / theme / "theme.rcss"));
		for (size_t i = 0; i < sheet.size(); ++i)
		{
			const auto& [selector, declarations] = sheet[i];
			const size_t first = selector.find_first_not_of(" \t");
			const size_t at = declarations.find("decorator:");
			if (at != std::string::npos)
				found[selector.substr(first, selector.find_last_not_of(" \t\r") + 1 - first)] = {
					i, declarations.substr(at, declarations.find(';', at) - at)};
		}
		REQUIRE(found.count("button.toggle.checked") == 1);
		const auto& [index, decorator] = found.at("button.toggle.checked");
		for (const char* other : {"button.checked", "button.toggle", "button.toggle:hover"})
		{
			INFO(other);
			REQUIRE(found.count(other) == 1);
			CHECK(found.at(other).first < index);
			CHECK(found.at(other).second != decorator);
		}
	}
}

TEST_CASE("every theme marks a selected arranger event", "[unit]")
{
	// A click selects an arranger event on the arranger track's lane (docs/features/arranger.md): .selected has a fill
	// of its own, unlike the events that aren't selected.
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		std::string plain;
		std::string selected;
		for (const auto& [selector, declarations] : rules(gnubase::test::readText(themesDir() / theme / "theme.rcss")))
		{
			const size_t first = selector.find_first_not_of(" \t");
			const std::string name = selector.substr(first, selector.find_last_not_of(" \t\r") + 1 - first);
			if (name == ".arranger-lane-row .arranger-lane-event")
				plain = declarations;
			if (name == ".arranger-lane-row .arranger-lane-event.selected")
				selected = declarations;
		}
		const auto fill = [](const std::string& declarations)
		{
			const size_t at = declarations.find("background-color:");
			return at == std::string::npos ? std::string() : declarations.substr(at, declarations.find(';', at) - at);
		};
		CHECK_FALSE(fill(selected).empty());
		CHECK(fill(selected) != fill(plain));
	}
}

TEST_CASE(
	"only the default theme tints events and keeps them off their lanes' edges; no theme pads waveforms", "[unit]")
{
	// #1312, #1332: dark, light and high contrast show events as before the tint: opaque in their colors, no tint over
	// their outlines and waveforms, at their lanes' full height. A <waveform> takes event-inset itself, so the padding
	// stays in the shared style sheet.
	for (const std::string& theme : themes())
	{
		INFO("theme " << theme);
		const bool tinted = std::string_view(theme) == "gnubase";
		std::map<std::string, std::set<std::string>> values; // by property, the values the theme's rules give it
		for (const auto& [selector, declarations] : rules(gnubase::test::readText(themesDir() / theme / "theme.rcss")))
		{
			for (const char* property : {"own-fill-opacity", "own-color-tint", "event-inset"})
				for (size_t at = declarations.find(std::string("\t") + property + ": "); at != std::string::npos;
					at = declarations.find(std::string("\t") + property + ": ", at + 1))
				{
					const size_t from = at + std::string_view(property).size() + 3;
					values[property].insert(declarations.substr(from, declarations.find(';', from) - from));
				}
			if (selector.find("waveform") != std::string::npos)
			{
				INFO(selector);
				CHECK(declarations.find("padding") == std::string::npos);
			}
		}
		const std::map<std::string, std::string> defaults{
			{"own-fill-opacity", "1"}, {"own-color-tint", "none"}, {"event-inset", "0dp"}};
		for (const auto& [property, value] : defaults)
		{
			INFO(property);
			REQUIRE(!values[property].empty());
			if (!tinted)
				CHECK(values[property] == std::set<std::string>{value});
		}
		if (tinted)
		{
			CHECK(values["own-fill-opacity"].count("1") == 0);
			CHECK(values["own-color-tint"].count("none") == 0);
			CHECK(values["event-inset"].count("0dp") == 0);
		}
	}
}

namespace
{
	// A style sheet's text without its comments, which may name a property they explain.
	std::string withoutComments(std::string text)
	{
		for (size_t begin = text.find("/*"); begin != std::string::npos; begin = text.find("/*", begin))
		{
			const size_t end = text.find("*/", begin + 2);
			text.erase(begin, end == std::string::npos ? std::string::npos : end + 2 - begin);
		}
		return text;
	}
}

TEST_CASE("the shared style sheets leave font sizes to the themes", "[unit]")
{
	// Shared sheets that set the size of a small label to fit its row (8 to 10 dp, in the key editor and the controller
	// lanes) and still name one. They belong in the themes, which own every font size (docs/features/themes.md); the
	// list holds them until they move there (#594), and may only shrink: a sheet that no longer names a font size
	// has to leave it, a new one never joins it.
	const std::set<std::string> sheetsWithFontSizes{"controllerlanes.rcss", "keyeditor.rcss"};
	std::set<std::string> found;
	std::set<std::string> checked;
	const std::filesystem::path styles = std::filesystem::path(GNUBASE_RESOURCES_DIR) / "styles";
	for (const auto& entry : std::filesystem::recursive_directory_iterator(styles))
	{
		if (entry.path().extension() != ".rcss")
			continue;
		const std::string name = entry.path().lexically_relative(styles).generic_string();
		checked.insert(name);
		const std::string text = withoutComments(gnubase::test::readText(entry.path()));
		INFO(name << (sheetsWithFontSizes.count(name) != 0 ? " is on the list of sheets with font sizes (#594)" : ""));
		if (text.find("font-size") != std::string::npos)
		{
			found.insert(name);
			CHECK(sheetsWithFontSizes.count(name) != 0);
		}
	}
	// Both the base sheet and the mixer's, which started this check (#560, #535), are among those read.
	CHECK(checked.count("base.rcss") != 0);
	CHECK(checked.count("mixer.rcss") != 0);
	CHECK(found == sheetsWithFontSizes);
}

TEST_CASE("rendering track appearance belongs to each theme", "[unit]")
{
	const std::string base =
		gnubase::test::readText(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "styles" / "base.rcss");
	CHECK(base.find("listitem.rendering") == std::string::npos);
	for (const std::string& theme : themes())
	{
		INFO(theme);
		const auto sheet = rules(gnubase::test::readText(themesDir() / theme / "theme.rcss"));
		const auto found = std::find_if(sheet.begin(), sheet.end(),
			[](const auto& rule) { return rule.first.find("listitem.rendering .track-name") != std::string::npos; });
		REQUIRE(found != sheet.end());
		CHECK(found->second.find("font-style: italic") != std::string::npos);
	}
}
