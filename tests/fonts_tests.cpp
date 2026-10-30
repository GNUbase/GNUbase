// SPDX-License-Identifier: GPL-3.0-or-later
// GNUbase's bundled fonts (#207, docs/features/fonts.md): the manifest in resources/fonts and the licenses next to the
// fonts, the characters the fonts cover, pseudo-localized text included, the counter font's digits, and loading them
// into RmlUi. tests/window_tests.cpp draws them and compares the screenshots with golden images.
#include "testutils/fakeui.h"
#include "testutils/testutils.h"
#include "ui/fonts.h"
#include "ui/pseudolocale.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/StringUtilities.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;

namespace
{
	std::filesystem::path fontsFolder()
	{
		return std::filesystem::path(GNUBASE_RESOURCES_DIR) / "fonts";
	}

	std::string utf8(const std::u32string& text)
	{
		std::string result;
		for (const char32_t character : text)
			result += Rml::StringUtilities::ToUTF8(static_cast<Rml::Character>(character));
		return result;
	}

	void write(const std::filesystem::path& file, const std::string& content)
	{
		std::ofstream out(file, std::ios::binary);
		out << content;
	}

	// Text of the user interface, with the symbols a DAW shows.
	std::vector<std::string> uiTexts()
	{
		return {
			"Open Project…",
			"Save As…",
			"Audio Connections",
			"Sample rate: 48.000 kHz · Bit depth: 24 bit",
			"Volume −6.0 dB, pan 25% L, −∞ dB",
			"Key: C♯ major / B♭ minor / F♮",
			"Bars → Seconds ← ↑ ↓ ± ½ × ° € “Quoted” ‘single’ «guillemets»",
		};
	}

	// The alphabets of languages GNUbase may be translated into, as pangrams. Chinese, Japanese and Korean need
	// fallback fonts of their own, which come with the translations (#172).
	std::vector<std::string> translations()
	{
		return {
			"Falsches Üben von Xylophonmusik quält jeden größeren Zwerg.",
			"Portez ce vieux whisky au juge blond qui fume : dès Noël où un zéphyr haï me vêt de glaçons würmiens,",
			"je dîne d’exquis rôtis de bœuf au kir à l’aÿ d’âge mûr & cætera !",
			"El veloz murciélago hindú comía feliz cardillo y kiwi. ¿La cigüeña tocaba el saxofón detrás del palenque?",
			"Luís argüia à Júlia que «brações, fé, chá, óxido, pôr, zângão» eram palavras do português.",
			"Pchnąć w tę łódź jeża lub ośm skrzyń fig.",
			"Příliš žluťoučký kůň úpěl ďábelské ódy.",
			"Árvíztűrő tükörfúrógép.",
			"Fumegând hipnotic sașiul azvârle mreje în bălți.",
			"Pijamalı hasta yağız şoföre çabucak güvendi.",
			"Flygande bäckasiner söka hwila på mjuka tuvor. Høj bly gom vandt fræk sexquiz på wc.",
			"Ξεσκεπάζω την ψυχοφθόρα βδελυγμία.",
			"Съешь же ещё этих мягких французских булок, да выпей чаю.",
			"Чуєш їх, доцю, га? Кумедна ж ти, прощайся без ґольфів!",
			"Tôi có thể ăn thủy tinh mà không hại gì.",
		};
	}

	// What counters and time displays show.
	std::vector<std::string> counterTexts()
	{
		return {
			"0001.01.01.000",
			"00:01:23:12",
			"00:01:23.456",
			"−12.5 dB +3.0 dB −∞",
			"120.000 BPM 4/4",
		};
	}
}

TEST_CASE("GNUbase's fonts are all in their manifest, with their licenses next to them", "[unit]")
{
	const ui::Fonts fonts(fontsFolder());
	REQUIRE(fonts.withRole("ui") != nullptr);
	REQUIRE(fonts.withRole("counter") != nullptr);
	CHECK(fonts.withRole("logo") == nullptr);
	CHECK_FALSE(fonts.fallback().empty());

	std::vector<std::filesystem::path> files = fonts.fallback();
	for (const ui::FontFamily& family : fonts.families())
	{
		CHECK_FALSE(family.faces.empty());
		for (const std::filesystem::path& face : family.faces)
		{
			INFO(face);
			CHECK(ui::FontFile(face).family() == family.name);
			files.push_back(face);
		}
	}
	for (const std::filesystem::path& file : files)
	{
		INFO(file);
		CHECK(std::filesystem::is_regular_file(file));
		CHECK(std::filesystem::is_regular_file(file.parent_path() / "OFL.txt"));
	}
	for (const auto& entry : std::filesystem::recursive_directory_iterator(fontsFolder()))
	{
		const std::filesystem::path extension = entry.path().extension();
		if (extension != ".ttf" && extension != ".otf")
			continue;
		INFO(entry.path());
		CHECK(std::count(files.begin(), files.end(), entry.path()) == 1);
	}
}

TEST_CASE("a font manifest that can't be read, or names a missing font or the wrong family, is an error", "[unit]")
{
	const test::TempDir temp;
	CHECK_THROWS_WITH(ui::Fonts(temp.path()), ContainsSubstring("fonts.json"));

	write(temp.path() / "fonts.json", "{ not json");
	CHECK_THROWS_WITH(ui::Fonts(temp.path()), ContainsSubstring("fonts.json"));

	write(temp.path() / "fonts.json", R"({"families": [{"name": "Inter", "role": "ui", "faces": ["Missing.ttf"]}],
		"fallback": []})");
	CHECK_THROWS_WITH(ui::Fonts(temp.path()), ContainsSubstring("Missing.ttf"));

	std::filesystem::copy_file(fontsFolder() / "Inter" / "Inter-Regular.ttf", temp.path() / "Inter-Regular.ttf");
	write(temp.path() / "fonts.json", R"({"families": [{"name": "Lato", "role": "ui", "faces": ["Inter-Regular.ttf"]}],
		"fallback": []})");
	CHECK_THROWS_WITH(ui::Fonts(temp.path()), ContainsSubstring("Inter") && ContainsSubstring("Lato"));

	write(temp.path() / "fonts.json", "{}");
	CHECK_THROWS_WITH(ui::Fonts(temp.path()), ContainsSubstring("fonts.json"));
}

TEST_CASE(
	"the fonts have every character of GNUbase's text, pseudo-localized and in the translations' alphabets", "[unit]")
{
	const ui::Fonts fonts(fontsFolder());
	const ui::FontFamily& text = *fonts.withRole("ui");
	const ui::FontFamily& counter = *fonts.withRole("counter");
	for (const std::string& line : uiTexts())
	{
		INFO(line);
		CHECK(utf8(fonts.missing(text, line)).empty());
		INFO(ui::pseudoLocalize(line));
		CHECK(utf8(fonts.missing(text, ui::pseudoLocalize(line))).empty());
	}
	for (const std::string& line : translations())
	{
		INFO(line);
		CHECK(utf8(fonts.missing(text, line)).empty());
	}
	for (const std::string& line : counterTexts())
	{
		INFO(line);
		CHECK(utf8(fonts.missing(counter, line)).empty());
	}

	// The check finds what is missing, each character once: here the Japanese, but not the sharp, which the fallback
	// font has.
	CHECK(utf8(fonts.missing(text, "音楽 C♯ 楽")) == "音楽");
}

TEST_CASE("the counter font's digits are all equally wide, so running numbers don't jitter", "[unit]")
{
	const ui::Fonts fonts(fontsFolder());
	for (const std::filesystem::path& face : fonts.withRole("counter")->faces)
	{
		INFO(face);
		const ui::FontFile font(face);
		for (char32_t digit = U'1'; digit <= U'9'; ++digit)
			CHECK(font.advance(digit) == font.advance(U'0'));
	}

	// Unlike the user interface's font, which has proportional digits.
	const ui::FontFile text(fonts.withRole("ui")->faces.front());
	CHECK(text.advance(U'1') < text.advance(U'0'));
}

TEST_CASE("the fonts load into RmlUi in every weight and style the manifest lists", "[unit]")
{
	const test::FakeUi fake{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
	const ui::Fonts fonts(fontsFolder());
	fonts.load();

	// RmlUi looks families up by their names in lowercase.
	Rml::FontEngineInterface* engine = Rml::GetFontEngineInterface();
	REQUIRE(engine != nullptr);
	using Rml::Style::FontStyle;
	using Rml::Style::FontWeight;
	// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange): FontWeight takes any CSS weight, not only its names
	for (const auto weight : {FontWeight::Normal, FontWeight(500), FontWeight(600), FontWeight::Bold})
		CHECK(engine->GetFontFaceHandle("inter", FontStyle::Normal, weight, 13) != 0);
	// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
	CHECK(engine->GetFontFaceHandle("inter", FontStyle::Italic, FontWeight::Normal, 13) != 0);
	CHECK(engine->GetFontFaceHandle("jetbrains mono", FontStyle::Normal, FontWeight::Normal, 13) != 0);
	CHECK(engine->GetFontFaceHandle("jetbrains mono", FontStyle::Normal, FontWeight::Bold, 13) != 0);
	CHECK(engine->GetFontFaceHandle("noto sans symbols", FontStyle::Normal, FontWeight::Normal, 13) != 0);
}

TEST_CASE("pseudo-localization accents every letter, keeps placeholders and lengthens the text by a third", "[unit]")
{
	CHECK(ui::pseudoLocalize("Save") == "[Šåṽé ~~]");
	CHECK(ui::pseudoLocalize("Open {name}?") == "[Öþéñ {name}? ~~~~]");
	CHECK(ui::pseudoLocalize("Größe") == "[Ĝŕößé ~~]");
	CHECK(ui::pseudoLocalize("abcdefghijklmnopqrstuvwxyz") == "[åƀçðéƒĝĥîĵķļɱñöþǫŕšŧüṽŵẋýž ~~~~~~~~]");
	CHECK(ui::pseudoLocalize("ABCDEFGHIJKLMNOPQRSTUVWXYZ") == "[ÅƁÇÐÉƑĜĤÎĴĶĻṀÑÖÞǪŔŠŦÜṼŴẊÝŽ ~~~~~~~~]");
	CHECK(ui::pseudoLocalize("") == "");
	// An unclosed brace keeps the rest as it is.
	CHECK(ui::pseudoLocalize("a {b") == "[å {b ~~]");
}
