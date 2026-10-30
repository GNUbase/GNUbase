// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
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
	for (const char* theme : {"gnubase", "dark", "light"})
	{
		INFO("theme " << theme);
		const std::filesystem::path dir = themesDir() / theme;
		const nlohmann::json assets = readJson(dir / "manifest.json").at("assets");
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
	}
}
