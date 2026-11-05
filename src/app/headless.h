// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "app/options.h"

#include <csignal>
#include <iosfwd>

namespace gnubase::engine
{
	class Engine;
}

namespace gnubase::app
{
	// Sets up the engine as the options say, for headless runs and tests: --audio's backend's default device or the
	// dummy device, stopped, so that audio.render renders offline and every run renders the same; then the project
	// if there is one. Problems go to err. False if either failed.
	bool setUpEngine(engine::Engine& engine, const Options& options, std::ostream& err);

	// Sets up the engine of the windowed app: opens the audio device the user chose last (the user settings
	// audio.backend and audio.device, which Studio Setup saves), or --audio's backend's default device, and starts it,
	// so Play moves the cursor and the metronome sounds right away. A device that can't be opened or started leaves
	// the app on the dummy device: it says why on err and in audio.get's failed, for Studio Setup to show. Then loads
	// the project if there is one. False if the project couldn't be loaded, or not even the dummy device opened.
	bool setUpWindowedEngine(engine::Engine& engine, const Options& options, std::ostream& err);

	// Stops a headless run that serves MCP over HTTP. Set by main's signal handler (Ctrl+C, SIGTERM).
	extern volatile std::sig_atomic_t g_stopRequested;

	// Runs GNUbase without UI (gnubase --headless): sets up the audio device, loads the project and serves MCP. Over
	// stdio it answers the requests from in on out until in ends; over HTTP it prints the URL and the access token on
	// out and serves until g_stopRequested. Problems go to err, so out stays MCP's. Returns an exit code.
	int runHeadless(const Options& options, std::istream& in, std::ostream& out, std::ostream& err);
}
