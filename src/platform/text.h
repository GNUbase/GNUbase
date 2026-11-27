// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <string>

// Text helpers for the engine and the UI, which keep text in UTF-8.
namespace gnubase::text
{
	// text with its ASCII letters in lower case, for comparing names, file extensions and searches without regard to
	// case. Every other byte stays as it is, so UTF-8 stays valid and "Ü" doesn't fold to "ü"; unlike std::tolower, it
	// doesn't depend on the locale.
	inline std::string asciiLower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(),
			[](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
		return text;
	}
}
