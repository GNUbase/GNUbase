// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/midi/port.h"

#include <memory>
#include <string>
#include <vector>

namespace gnubase::midi
{
	// MIDI ports without hardware, for tests and headless runs. It starts without ports. A loopback is an output and
	// an input that share a name and an id: what is sent to the output arrives at the input at once, stamped with the
	// time it was sent at. Adding and removing loopbacks simulates plugging devices in and out.
	class DummyBackend : public Backend
	{
	public:
		DummyBackend();
		~DummyBackend() override;

		std::string name() const override { return "dummy"; }
		std::vector<PortInfo> ports() override;
		std::unique_ptr<Input> openInput(const std::string& id, Receiver receiver) override;
		std::unique_ptr<Output> openOutput(const std::string& id) override;

		// The loopback's id is its name. Open ports of a removed loopback stay silent.
		void addLoopback(const std::string& name);
		void removeLoopback(const std::string& name);

		struct State;

	private:
		std::shared_ptr<State> m_state;
	};
}
