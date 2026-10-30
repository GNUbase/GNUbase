// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/fonts.h"

#include "platform/files.h"
#include "ui/rmlui.h"

#include <RmlUi/Core/StringUtilities.h>
#include <ft2build.h>
#include <nlohmann/json.hpp>
#include FT_FREETYPE_H
#include FT_ADVANCES_H

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	struct FontFile::Face
	{
		std::string data; // FreeType reads the font from memory, so that paths with any characters work everywhere
		FT_Library library = nullptr;
		FT_Face face = nullptr;

		Face() = default;
		Face(const Face&) = delete;
		Face& operator=(const Face&) = delete;
		Face(Face&&) = delete;
		Face& operator=(Face&&) = delete;
		~Face()
		{
			if (face != nullptr)
				FT_Done_Face(face);
			if (library != nullptr)
				FT_Done_FreeType(library);
		}
	};

	FontFile::FontFile(const std::filesystem::path& file) : m_face(std::make_unique<Face>())
	{
		std::optional<std::string> data = files::readFile(file);
		if (!data)
			throw std::runtime_error("cannot read the font " + file.u8string());
		m_face->data = std::move(*data);
		if (FT_Init_FreeType(&m_face->library) != 0 ||
			FT_New_Memory_Face(m_face->library, reinterpret_cast<const FT_Byte*>(m_face->data.data()),
				static_cast<FT_Long>(m_face->data.size()), 0, &m_face->face) != 0)
			throw std::runtime_error(file.u8string() + " is not a font that FreeType can read");
	}

	FontFile::~FontFile() = default;
	FontFile::FontFile(FontFile&&) noexcept = default;
	FontFile& FontFile::operator=(FontFile&&) noexcept = default;

	std::string FontFile::family() const
	{
		return m_face->face->family_name != nullptr ? m_face->face->family_name : "";
	}

	bool FontFile::has(char32_t character) const
	{
		return FT_Get_Char_Index(m_face->face, character) != 0;
	}

	long FontFile::advance(char32_t character) const
	{
		const FT_UInt glyph = FT_Get_Char_Index(m_face->face, character);
		FT_Fixed advance = 0;
		if (glyph == 0 || FT_Get_Advance(m_face->face, glyph, FT_LOAD_NO_SCALE, &advance) != 0)
			return 0;
		return advance;
	}

	namespace
	{
		std::filesystem::path fontPath(const std::filesystem::path& folder, const nlohmann::json& entry)
		{
			return folder / std::filesystem::u8path(entry.get<std::string>());
		}

		// The font, which must exist and be one FreeType can read.
		FontFile open(const std::filesystem::path& manifest, const std::filesystem::path& font)
		{
			if (!std::filesystem::is_regular_file(font))
				throw std::runtime_error(manifest.u8string() + " lists " + font.u8string() + ", which doesn't exist");
			return FontFile(font);
		}
	}

	Fonts::Fonts(const std::filesystem::path& folder)
	{
		const std::filesystem::path manifest = folder / "fonts.json";
		const std::optional<std::string> text = files::readFile(manifest);
		if (!text)
			throw std::runtime_error("cannot read the font manifest " + manifest.u8string());
		try
		{
			const nlohmann::json json = nlohmann::json::parse(*text);
			for (const nlohmann::json& entry : json.at("families"))
			{
				FontFamily family{entry.at("name").get<std::string>(), entry.at("role").get<std::string>(), {}};
				for (const nlohmann::json& face : entry.at("faces"))
					family.faces.push_back(fontPath(folder, face));
				m_families.push_back(std::move(family));
			}
			for (const nlohmann::json& face : json.value("fallback", nlohmann::json::array()))
				m_fallback.push_back(fontPath(folder, face));
		}
		catch (const nlohmann::json::exception& error)
		{
			throw std::runtime_error("the font manifest " + manifest.u8string() + " is invalid: " + error.what());
		}
		if (m_families.empty())
			throw std::runtime_error("the font manifest " + manifest.u8string() + " lists no font families");

		for (const FontFamily& family : m_families)
		{
			for (const std::filesystem::path& face : family.faces)
			{
				const std::string actual = open(manifest, face).family();
				if (actual != family.name)
					throw std::runtime_error(face.u8string() + " is the font family " + actual + ", not " +
						family.name + " as " + manifest.u8string() + " says");
			}
		}
		for (const std::filesystem::path& face : m_fallback)
			open(manifest, face);
	}

	const FontFamily* Fonts::withRole(std::string_view role) const
	{
		const auto found = std::find_if(
			m_families.begin(), m_families.end(), [role](const FontFamily& family) { return family.role == role; });
		return found != m_families.end() ? &*found : nullptr;
	}

	void Fonts::load() const
	{
		const auto loadFont = [](const std::filesystem::path& face, bool fallback)
		{
			if (!RmlUi::loadFont(face, fallback))
				throw std::runtime_error("RmlUi cannot load the font " + face.u8string());
		};
		for (const FontFamily& family : m_families)
			for (const std::filesystem::path& face : family.faces)
				loadFont(face, false);
		for (const std::filesystem::path& face : m_fallback)
			loadFont(face, true);
	}

	std::u32string Fonts::missing(const FontFamily& family, std::string_view text) const
	{
		std::vector<FontFile> faces;
		for (const std::filesystem::path& face : family.faces)
			faces.emplace_back(face);
		std::vector<FontFile> fallback;
		for (const std::filesystem::path& face : m_fallback)
			fallback.emplace_back(face);

		std::u32string result;
		for (Rml::StringIteratorU8 it{Rml::StringView(text.data(), text.data() + text.size())}; it; ++it)
		{
			const auto character = static_cast<char32_t>(*it);
			if (result.find(character) != std::u32string::npos)
				continue;
			const bool inEveryFace = std::all_of(
				faces.begin(), faces.end(), [character](const FontFile& face) { return face.has(character); });
			const bool inFallback = std::any_of(
				fallback.begin(), fallback.end(), [character](const FontFile& face) { return face.has(character); });
			if (!inEveryFace && !inFallback)
				result += character;
		}
		return result;
	}
}
