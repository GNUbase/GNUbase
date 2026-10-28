// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

// Checks what every audio backend promises, for each of its devices (tests/device_tests.cpp).
void checkDeviceContract(gnubase::audio::Backend& backend);
