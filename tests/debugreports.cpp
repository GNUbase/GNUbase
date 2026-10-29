// SPDX-License-Identifier: GPL-3.0-or-later
// MSVC's debug runtime shows a dialog for a failed assertion, such as a vector index out of range, and waits for a
// click: an unattended test run hangs. The test executable sends those reports to stderr instead, and the failure
// ends the run.
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#include <cstdlib>

namespace
{
	const bool g_reportsToStderr = []
	{
		const int types[] = {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT};
		for (const int type : types)
		{
			_CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
			_CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
		}
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
		return true;
	}();
}
#endif
