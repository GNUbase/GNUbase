// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/device.h"
#include "platform/audio/dummydevice.h"

namespace gnubase::audio
{
	std::vector<std::unique_ptr<Backend>> makeBackends()
	{
		std::vector<std::unique_ptr<Backend>> backends;
		backends.push_back(std::make_unique<DummyBackend>());
		return backends;
	}
}
