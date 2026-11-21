// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/tempfolder.h"

#include <array>
#include <cstdlib>

namespace gnubase::test
{
	namespace
	{
#ifdef _WIN32
		constexpr std::array<const wchar_t*, 2> g_tempVariables{L"TMP", L"TEMP"};
#else
		constexpr std::array<const char*, 1> g_tempVariables{"TMPDIR"};
#endif

		void setTempVariable(size_t index, const std::optional<std::filesystem::path::string_type>& value)
		{
#ifdef _WIN32
			_wputenv_s(g_tempVariables.at(index), value ? value->c_str() : L""); // an empty value removes it
#else
			if (value)
				setenv(g_tempVariables.at(index), value->c_str(), 1);
			else
				unsetenv(g_tempVariables.at(index));
#endif
		}
	}

	TempFolderAt::TempFolderAt(const std::filesystem::path& folder)
	{
		for (size_t i = 0; i < g_tempVariables.size(); ++i)
		{
#ifdef _WIN32
			wchar_t* value = nullptr;
			size_t length = 0;
			if (_wdupenv_s(&value, &length, g_tempVariables.at(i)) == 0 && value != nullptr)
				m_previous.emplace_back(value);
			else
				m_previous.emplace_back(std::nullopt);
			std::free(value);
#else
			const char* value = std::getenv(g_tempVariables.at(i));
			m_previous.push_back(value != nullptr ? std::optional<std::string>(value) : std::nullopt);
#endif
			setTempVariable(i, folder.native());
		}
	}

	TempFolderAt::~TempFolderAt()
	{
		for (size_t i = 0; i < m_previous.size(); ++i)
			setTempVariable(i, m_previous[i]);
	}
}
