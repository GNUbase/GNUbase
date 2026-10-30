// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

// Checks what every audio backend promises, for each of its devices (tests/device_tests.cpp): every device listed as
// available opens and starts. Devices listed as unavailable are skipped with a warning, as real hardware may be
// unplugged. With openFirst, every device is opened and started once before, since some backends learn only that way
// that a device is unavailable (ASIO).
void checkDeviceContract(gnubase::audio::Backend& backend, bool openFirst = false);
