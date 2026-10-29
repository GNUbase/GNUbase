// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Windows only: between Windows' UTF-16 strings and GNUbase's UTF-8.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace gnubase
{
	inline std::string utf8(const wchar_t* text)
	{
		const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
		if (size <= 1)
			return {};
		std::string result(static_cast<size_t>(size - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
		return result;
	}

	inline std::wstring wide(const std::string& text)
	{
		const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
		if (size <= 1)
			return {};
		std::wstring result(static_cast<size_t>(size - 1), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
		return result;
	}
}
