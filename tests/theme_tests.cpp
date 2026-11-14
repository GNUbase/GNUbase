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

	constexpr std::array<const char*, 4> g_themes = {"gnubase", "dark", "light", "high-contrast"};

	std::filesystem::path themesDir()
	{
		return std::filesystem::path(GNUBASE_RESOURCES_DIR) / "themes";
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
	for (const char* theme : g_themes)
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

		// The theme's style sheet uses every skin, and styles the same widgets and states as every other theme.
		const std::string sheet = gnubase::test::readText(dir / "theme.rcss");
		CHECK(sheet.find("@media (theme: " + std::string(theme) + ")") != std::string::npos);
		for (const auto& [name, asset] : assets.items())
		{
			INFO("asset " << name);
			const std::string file = asset.at("file").get<std::string>();
			const bool used =
				sheet.find("(" + file + ")") != std::string::npos || sheet.find("(" + file + ",") != std::string::npos;
			CHECK(used);
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
	for (const char* theme : g_themes)
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
		for (const char* widget : {"button", "button.primary", "button.checked", "button.toggle",
				 "button.toggle.mute.checked", "input.checkbox", "input.checkbox:checked", "input.radio",
				 "input.radio:checked", "listview", "valuefield", "knob", "fader", "slider", "input.range", "menuitem"})
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
	// The focus ring is thicker than in the other themes.
	CHECK(gnubase::test::readText(themesDir() / "high-contrast" / "focus-ring.svg").find("stroke-width=\"2\"") !=
		std::string::npos);
}

TEST_CASE("every theme's piano keyboard has light white keys and dark black keys", "[unit]")
{
	// The key editor's keyboard (docs/features/key-editor.md): a light theme's too, though its text is the dark color.
	for (const char* theme : g_themes)
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
	for (const char* theme : g_themes)
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
	for (const char* theme : g_themes)
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
	for (const char* theme : g_themes)
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

TEST_CASE("every theme marks the arranger editor's row that is playing", "[unit]")
{
	// The arranger editor (docs/features/arranger.md): the row of the chain step or event playing carries .current and
	// the theme decides how that looks, since the document's own locate arrows show on every row and mark nothing. The
	// rule lives in the generated style sheets (tools/skin/generate.py), so a regeneration that drops it fails here.
	for (const char* theme : g_themes)
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

TEST_CASE("the shared style sheets leave font sizes to the themes", "[unit]")
{
	const std::string base =
		gnubase::test::readText(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "styles" / "base.rcss");
	CHECK(base.find("font-size") == std::string::npos);
}
