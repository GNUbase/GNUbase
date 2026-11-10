// SPDX-License-Identifier: GPL-3.0-or-later
// Every run of the tests gets a temp folder of its own, removed when the run ends. An engine without a settings folder
// keeps the recordings and frozen files of an unsaved project in the temp folder, and a shared one fills up on a
// machine that runs the tests all day.
#include "testutils/testutils.h"

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdlib>
#include <filesystem>
#include <memory>

namespace
{
	void setTempFolder(const std::filesystem::path& folder)
	{
#ifdef _WIN32
		_wputenv_s(L"TMP", folder.c_str());
		_wputenv_s(L"TEMP", folder.c_str());
#else
		setenv("TMPDIR", folder.c_str(), 1);
#endif
	}

	class TempFolderOfTheRun final : public Catch::EventListenerBase
	{
	public:
		using Catch::EventListenerBase::EventListenerBase;

		void testRunStarting(const Catch::TestRunInfo& /*info*/) override
		{
			m_shared = std::filesystem::temp_directory_path();
			m_folder = std::make_unique<gnubase::test::TempDir>();
			setTempFolder(m_folder->path());
		}

		void testRunEnded(const Catch::TestRunStats& /*stats*/) override
		{
			setTempFolder(m_shared);
			m_folder.reset();
		}

	private:
		std::filesystem::path m_shared;
		std::unique_ptr<gnubase::test::TempDir> m_folder;
	};
}

CATCH_REGISTER_LISTENER(TempFolderOfTheRun)
