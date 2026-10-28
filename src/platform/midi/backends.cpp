// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/midi/dummyport.h"
#include "platform/midi/port.h"

namespace gnubase::midi
{
	std::vector<std::unique_ptr<Backend>> makeBackends()
	{
		std::vector<std::unique_ptr<Backend>> backends;
		backends.push_back(std::make_unique<DummyBackend>());
		return backends;
	}
}
