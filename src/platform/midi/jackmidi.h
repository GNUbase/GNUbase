// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/midi/port.h"

#include <memory>
#include <string>

namespace gnubase::midi
{
	// JACK MIDI (backend "jack"): the MIDI ports of a running JACK server, jackd2's or PipeWire's JACK layer. Other
	// clients' MIDI outputs are inputs, their MIDI inputs outputs; a port's id and name are its full JACK name
	// ("system:midi_capture_1"). Ports go through a JACK client "gnubase-midi" of their own, which starts with the
	// first listing or port: an open port is a port of that client, connected to the one it was opened for. Without
	// libjack or a running server the backend has no ports. Input is stamped with the host time of its frame in the
	// cycle it arrived in; outputs place what is sent at its frame in the cycle it is due in. Ports registered and
	// unregistered on the server, and the server going away, are reported as changed ports.
	//
	// libjack is loaded when the backend is first used; library is the one to load, e.g. PipeWire's.
	std::unique_ptr<Backend> makeJackMidiBackend(std::string library = "libjack.so.0");
}
