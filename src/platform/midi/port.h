// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"
#include "platform/hosttime.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// MIDI ports and their backends. A backend is a kind of MIDI driver (dummy, Windows MIDI, CoreMIDI, ALSA); it lists
// its ports and opens them. Messages are MIDI 1.0 channel messages (audio::MidiMessage) stamped with host time;
// backends that speak MIDI 2.0 (Universal MIDI Packets) translate.
namespace gnubase::midi
{
	struct PortInfo
	{
		std::string id; // what Backend::openInput and openOutput take; stays the same across runs where the OS allows.
						// Unique among the backend's inputs and among its outputs: a device's input and output may
						// share it.
		std::string name;   // the system name
		bool input = true;  // into GNUbase; false: an output
		bool midi2 = false; // the OS speaks MIDI 2.0 with the port
	};

	// Gets each message of an input port with the host time it arrived. Called on a driver thread; must not block.
	using Receiver = std::function<void(const audio::MidiMessage& message, HostTime time)>;

	// An open input port. Destroying it closes the port; its receiver isn't called after that.
	class Input
	{
	public:
		Input() = default;
		virtual ~Input() = default;
		Input(const Input&) = delete;
		Input& operator=(const Input&) = delete;
	};

	// An open output port. Destroying it closes the port.
	class Output
	{
	public:
		Output() = default;
		virtual ~Output() = default;
		Output(const Output&) = delete;
		Output& operator=(const Output&) = delete;

		// Sends a message at a host time, or now if that time has passed. Not on audio threads.
		virtual void send(const audio::MidiMessage& message, HostTime time) = 0;
	};

	// A kind of MIDI driver. It lists its ports and opens them.
	class Backend
	{
	public:
		Backend() = default;
		virtual ~Backend() = default;
		Backend(const Backend&) = delete;
		Backend& operator=(const Backend&) = delete;

		virtual std::string name() const = 0; // e.g. "dummy", "winmm"

		// The ports there are now. Throws std::runtime_error if the driver can't list them.
		virtual std::vector<PortInfo> ports() = 0;

		// Open a port. Throw std::runtime_error if they can't, e.g. for an unknown port.
		virtual std::unique_ptr<Input> openInput(const std::string& id, Receiver receiver) = 0;
		virtual std::unique_ptr<Output> openOutput(const std::string& id) = 0;

		// Called when ports appear or disappear (hot-plug), on any thread but an audio thread. Set it before opening
		// ports.
		void onPortsChanged(std::function<void()> handler) { m_portsChanged = std::move(handler); }

	protected:
		void portsChanged() const
		{
			if (m_portsChanged)
				m_portsChanged();
		}

	private:
		std::function<void()> m_portsChanged;
	};

	// The MIDI backends this build has, the dummy backend last.
	std::vector<std::unique_ptr<Backend>> makeBackends();
}
