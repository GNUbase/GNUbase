// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

// Checks what every audio backend promises, for each of its devices (tests/device_tests.cpp). With skipUnavailable,
// devices that don't open or start are skipped with a warning: real hardware may be unplugged.
void checkDeviceContract(gnubase::audio::Backend& backend, bool skipUnavailable = false);
