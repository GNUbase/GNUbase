// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// GNUbase's bundled fonts (#207): the font families in resources/fonts and the fallback fonts that supply the
// characters a family lacks, as the manifest fonts.json there lists them. docs/features/fonts.md has the reasoning.
namespace gnubase::ui
{
	// A font file, read with FreeType: its family and the characters it has.
	class FontFile
	{
	public:
		// Throws std::runtime_error if the file can't be read or isn't a font.
		explicit FontFile(const std::filesystem::path& file);
		~FontFile();
		FontFile(const FontFile&) = delete;
		FontFile& operator=(const FontFile&) = delete;
		FontFile(FontFile&&) noexcept;
		FontFile& operator=(FontFile&&) noexcept;

		// The family name as RmlUi sees it: the typographic family, e.g. "Inter" for Inter Medium.
		std::string family() const;
		bool has(char32_t character) const;
		// The character's advance width in font units; 0 if the font lacks it.
		long advance(char32_t character) const;

	private:
		struct Face;
		std::unique_ptr<Face> m_face;
	};

	struct FontFamily
	{
		std::string name; // the family name in the font files, which style sheets use
		std::string role; // what it is for: "ui" for text, "counter" for time and counter displays
		std::vector<std::filesystem::path> faces;
	};

	class Fonts
	{
	public:
		// Reads the manifest fonts.json in the folder. Throws std::runtime_error if it can't be read, or if a font it
		// lists is missing or belongs to another family than the manifest says.
		explicit Fonts(const std::filesystem::path& folder);

		const std::vector<FontFamily>& families() const { return m_families; }
		// The fallback fonts, in the order in which they are asked for a character.
		const std::vector<std::filesystem::path>& fallback() const { return m_fallback; }
		// The family with the role; null if none has it.
		const FontFamily* withRole(std::string_view role) const;

		// Loads every font into RmlUi, the fallback fonts last and in their order. RmlUi must be initialised (RmlUi in
		// ui/rmlui.h). Throws std::runtime_error naming a font that RmlUi can't load.
		void load() const;

		// The characters of the UTF-8 text that one of the family's faces lacks and no fallback font has, each once, in
		// the order they first appear.
		std::u32string missing(const FontFamily& family, std::string_view text) const;

	private:
		std::vector<FontFamily> m_families;
		std::vector<std::filesystem::path> m_fallback;
	};
}
