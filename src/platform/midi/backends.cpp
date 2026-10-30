// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/midi/dummyport.h"
#include "platform/midi/port.h"

#ifdef _WIN32
#include "platform/midi/winmm.h"
#endif
#ifdef __APPLE__
#include "platform/midi/coremidi.h"
#endif

namespace gnubase::midi
{
	std::vector<std::unique_ptr<Backend>> makeBackends()
	{
		std::vector<std::unique_ptr<Backend>> backends;
#ifdef _WIN32
		backends.push_back(makeWinmmBackend());
#endif
#ifdef __APPLE__
		backends.push_back(makeCoreMidiBackend());
#endif
		backends.push_back(std::make_unique<DummyBackend>());
		return backends;
	}
}
