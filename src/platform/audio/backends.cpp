// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/device.h"
#include "platform/audio/dummydevice.h"

#ifdef _WIN32
#include "platform/audio/wasapi.h"
#endif

namespace gnubase::audio
{
	std::vector<std::unique_ptr<Backend>> makeBackends()
	{
		std::vector<std::unique_ptr<Backend>> backends;
#ifdef _WIN32
		backends.push_back(makeWasapiBackend(false));
		backends.push_back(makeWasapiBackend(true));
#endif
		backends.push_back(std::make_unique<DummyBackend>());
		return backends;
	}
}
