// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/options.h"

#include <iosfwd>

namespace gnubase::app
{
	// Runs GNUbase with its UI: sets up the engine as runHeadless does, opens the main window and runs the UI until
	// it is closed. Problems go to err. Returns an exit code; g_exitSetup if this machine can't show the UI.
	int runWindowed(const Options& options, std::ostream& err);
}
