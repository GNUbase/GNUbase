// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/device.h"
#include "platform/audio/dummydevice.h"

#ifdef _WIN32
#include "platform/audio/asiohost.h"
#include "platform/audio/wasapi.h"
#endif
#ifdef __APPLE__
#include "platform/audio/coreaudio.h"
#endif
#ifdef __linux__
#include "platform/audio/jack.h"
#include "platform/audio/pipewire.h"
#endif

namespace gnubase::audio
{
	std::vector<std::unique_ptr<Backend>> makeBackends()
	{
		std::vector<std::unique_ptr<Backend>> backends;
#ifdef _WIN32
		backends.push_back(makeAsioBackend());
		backends.push_back(makeWasapiBackend(false));
		backends.push_back(makeWasapiBackend(true));
#endif
#ifdef __APPLE__
		backends.push_back(makeCoreAudioBackend());
#endif
#ifdef __linux__
		backends.push_back(makePipeWireBackend());
		backends.push_back(makeJackBackend());
#endif
		backends.push_back(std::make_unique<DummyBackend>());
		return backends;
	}
}
