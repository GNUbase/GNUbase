// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct IASIO; // the driver interface of the ASIO SDK (iasiodrv.h)

namespace gnubase::audio
{
	// Where ASIO drivers come from: by default the installed ones, listed in the registry and created through COM.
	// Tests give fakes.
	struct AsioDrivers
	{
		std::function<std::vector<std::string>()> list; // driver names
		// A new driver with one reference, or null. Called on the driver thread.
		std::function<IASIO*(const std::string& name)> create;
	};

	// ASIO (Windows, backend "asio"), through the ASIO SDK under its GPLv3 option. A device is an installed driver,
	// its id the driver's name. Only one driver can be open at a time, as ASIO's callbacks have no context. Every call
	// into a driver runs on a thread of its own in a COM single-threaded apartment that handles window messages, since
	// drivers expect the thread that created them and some show their control panel there. Inputs and outputs use all
	// of the driver's channels; block sizes are those the driver allows, the preferred one by default. Reset, resync
	// and buffer size change requests and sample rate changes ask the engine for a reset.
	std::unique_ptr<Backend> makeAsioBackend(AsioDrivers drivers = {});
}
