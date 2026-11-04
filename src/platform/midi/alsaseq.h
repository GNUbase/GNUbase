// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/midi/port.h"

#include <memory>

namespace gnubase::midi
{
	// Linux MIDI through the ALSA sequencer (backend "alsa"): every exported port of the other sequencer clients, the
	// kernel's sound card ports as well as applications' ones, PipeWire's MIDI bridge among them. Ports others can read
	// from are inputs, ports others can write to outputs. A port's name is the sequencer's ("Midi Through Port-0"), its
	// id "client name:port name", numbered from the second port of the same names ("USB MIDI:Port 1 #2"), since client
	// numbers change when devices come and go. The backend is a sequencer client "GNUbase", which starts with the first
	// listing or port; an open port is a port of that client, subscribed to the one it was opened for. Input is
	// stamped with the host time it arrives at; outputs leave what is sent for later to a sequencer queue. The
	// sequencer's announcements report ports coming and going. Open ports close before their backend.
	std::unique_ptr<Backend> makeAlsaSeqBackend();
}
