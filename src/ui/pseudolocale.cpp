// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/pseudolocale.h"

#include <array>
#include <cstddef>

namespace gnubase::ui
{
	namespace
	{
		// An accented look-alike for each ASCII letter, from the Latin blocks every bundled font has.
		constexpr std::array<const char*, 26> g_lower = {"å", "ƀ", "ç", "ð", "é", "ƒ", "ĝ", "ĥ", "î", "ĵ", "ķ", "ļ",
			"ɱ", "ñ", "ö", "þ", "ǫ", "ŕ", "š", "ŧ", "ü", "ṽ", "ŵ", "ẋ", "ý", "ž"};
		constexpr std::array<const char*, 26> g_upper = {"Å", "Ɓ", "Ç", "Ð", "É", "Ƒ", "Ĝ", "Ĥ", "Î", "Ĵ", "Ķ", "Ļ",
			"Ṁ", "Ñ", "Ö", "Þ", "Ǫ", "Ŕ", "Š", "Ŧ", "Ü", "Ṽ", "Ŵ", "Ẋ", "Ý", "Ž"};

		bool continuationByte(char byte)
		{
			return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
		}
	}

	std::string pseudoLocalize(std::string_view text)
	{
		if (text.empty())
			return {};
		std::string result = "[";
		size_t characters = 0;
		bool inPlaceholder = false;
		for (const char byte : text)
		{
			if (!continuationByte(byte))
				++characters;
			if (byte == '{')
				inPlaceholder = true;
			else if (byte == '}')
				inPlaceholder = false;
			if (!inPlaceholder && byte >= 'a' && byte <= 'z')
				result += g_lower.at(static_cast<size_t>(byte - 'a'));
			else if (!inPlaceholder && byte >= 'A' && byte <= 'Z')
				result += g_upper.at(static_cast<size_t>(byte - 'A'));
			else
				result += byte;
		}
		// A third longer, rounded up, as translations into many languages are.
		result += ' ';
		result.append(((characters * 3) + 9) / 10, '~');
		result += ']';
		return result;
	}
}
