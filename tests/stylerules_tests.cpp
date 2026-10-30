// SPDX-License-Identifier: GPL-3.0-or-later
// The UI's styling rules (CLAUDE.md, "UI conventions"; #53): C++ and Lua code never set a style property such as a
// color, font, border or spacing. They change an element's classes and pseudo-classes, and the style sheets decide how
// each state looks. Code may set only geometry that comes from data, such as a position, a size or a transform. Nor do
// documents and the shared style sheets name a theme: ui::Themes gives them the active one (docs/features/themes.md).
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <filesystem>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace
{
	// The properties code may set: where an element is and how big, from data.
	const std::set<std::string>& geometry()
	{
		static const std::set<std::string> properties = {"left", "top", "right", "bottom", "width", "height",
			"transform", "transform-origin", "transform-origin-x", "transform-origin-y"};
		return properties;
	}

	// RmlUi's property id, e.g. TransformOriginX, as the RCSS property name, transform-origin-x.
	std::string propertyName(const std::string& id)
	{
		std::string name;
		for (const char c : id)
		{
			if (std::isupper(static_cast<unsigned char>(c)) != 0 && !name.empty())
				name += '-';
			name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		}
		return name;
	}

	// Where the code sets a style property other than geometry, as "line N: <what>". Code in C++ calls
	// Element::SetProperty with a property name or id, or sets the style attribute; Lua scripts use RmlUi's Lua
	// API, element.style.<name>, element.style["<name>"], SetProperty and SetAttribute.
	std::vector<std::string> styleViolations(const std::string& code)
	{
		static const std::regex setProperty(
			R"re(SetProperty\s*\(\s*(?:"([a-zA-Z-]+)"|(?:Rml::)?PropertyId::(\w+)|([^,)]*)))re");
		static const std::regex luaStyle(
			R"re(\.style\s*(?:\.\s*([a-zA-Z_]+)|\[\s*["']([a-zA-Z-]+)["']\s*\])\s*=[^=])re");
		static const std::regex styleAttribute(R"re(SetAttribute\s*\(\s*["']style["'])re");

		std::vector<std::string> found;
		size_t lineNumber = 0;
		size_t start = 0;
		while (start <= code.size())
		{
			size_t end = code.find('\n', start);
			if (end == std::string::npos)
				end = code.size();
			const std::string line = code.substr(start, end - start);
			++lineNumber;
			const auto report = [&](const std::string& what)
			{ found.push_back("line " + std::to_string(lineNumber) + ": " + what); };

			for (std::sregex_iterator it(line.begin(), line.end(), setProperty), last; it != last; ++it)
			{
				const std::smatch& match = *it;
				if (match[1].matched || match[2].matched)
				{
					const std::string property = match[1].matched ? match[1].str() : propertyName(match[2].str());
					if (geometry().count(property) == 0)
						report("sets the style property " + property);
				}
				else
				{
					report("sets a style property it doesn't name: " + match[3].str());
				}
			}
			for (std::sregex_iterator it(line.begin(), line.end(), luaStyle), last; it != last; ++it)
			{
				std::string property = (*it)[1].matched ? (*it)[1].str() : (*it)[2].str();
				for (char& c : property)
					if (c == '_')
						c = '-';
				if (geometry().count(property) == 0)
					report("sets the style property " + property);
			}
			if (std::regex_search(line, styleAttribute))
				report("sets the style attribute");
			start = end + 1;
		}
		return found;
	}

	// The files in the folder and below with one of the extensions.
	std::vector<std::filesystem::path> files(
		const std::filesystem::path& folder, const std::set<std::string>& extensions)
	{
		std::vector<std::filesystem::path> result;
		for (const auto& entry : std::filesystem::recursive_directory_iterator(folder))
			if (entry.is_regular_file() && extensions.count(entry.path().extension().string()) != 0)
				result.push_back(entry.path());
		return result;
	}

	std::filesystem::path sourceTree()
	{
		return std::filesystem::path(GNUBASE_RESOURCES_DIR).parent_path();
	}
}

TEST_CASE("the style check finds code that sets a style property, but allows geometry from data", "[unit]")
{
	CHECK(styleViolations(R"code(element->SetProperty("color", "#ff0000");)code") ==
		std::vector<std::string>{"line 1: sets the style property color"});
	CHECK(styleViolations("\n\tbox->SetProperty(Rml::PropertyId::BorderTopWidth, Rml::Property(1, Rml::Unit::PX));") ==
		std::vector<std::string>{"line 2: sets the style property border-top-width"});
	CHECK(styleViolations(R"code(e->SetProperty(name, value);)code") ==
		std::vector<std::string>{"line 1: sets a style property it doesn't name: name"});
	CHECK(styleViolations(R"code(e->SetAttribute("style", "padding: 4dp");)code") ==
		std::vector<std::string>{"line 1: sets the style attribute"});
	CHECK(styleViolations("element.style.font_size = '13dp'") ==
		std::vector<std::string>{"line 1: sets the style property font-size"});
	CHECK(styleViolations(R"code(element.style["background-color"] = "red")code") ==
		std::vector<std::string>{"line 1: sets the style property background-color"});
	CHECK(styleViolations(R"code(element:SetProperty("margin-left", "4dp"))code") ==
		std::vector<std::string>{"line 1: sets the style property margin-left"});

	// Geometry from data, reading styles, and changing classes and pseudo-classes are fine.
	CHECK(styleViolations(R"code(event->SetProperty(Rml::PropertyId::Left, Rml::Property(x, Rml::Unit::DP));)code")
			.empty());
	CHECK(styleViolations(R"code(fader->SetProperty("transform", "translateY(12dp)");)code").empty());
	CHECK(styleViolations("knob.style.transform = 'rotate(45deg)'").empty());
	CHECK(styleViolations("if element.style.color == 'red' then end").empty());
	CHECK(styleViolations(R"code(element->SetClass("muted", true); element->SetPseudoClass("checked", true);)code")
			.empty());
}

TEST_CASE("GNUbase's C++ and Lua code style elements through classes, never through style properties", "[unit]")
{
	std::vector<std::filesystem::path> code = files(sourceTree() / "src", {".cpp", ".h", ".lua"});
	for (const std::filesystem::path& file : files(sourceTree() / "resources", {".lua"}))
		code.push_back(file);
	for (const std::filesystem::path& file : files(sourceTree() / "tests" / "lua", {".lua"}))
		code.push_back(file);
	CHECK(code.size() > 100);
	for (const std::filesystem::path& file : code)
	{
		INFO(file);
		CHECK(styleViolations(gnubase::test::readText(file)).empty());
	}
}

TEST_CASE("documents and the shared style sheets never name a theme", "[unit]")
{
	const std::filesystem::path resources(GNUBASE_RESOURCES_DIR);
	for (const std::filesystem::path& file : files(resources, {".rml", ".rcss"}))
	{
		if (file.parent_path().parent_path() == resources / "themes")
			continue; // a theme's own style sheet
		INFO(file);
		const std::string text = gnubase::test::readText(file);
		CHECK(text.find("themes/") == std::string::npos);
		CHECK(text.find("(theme:") == std::string::npos);
	}
}
