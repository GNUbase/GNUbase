// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/midi/dummyport.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace gnubase::midi
{
	struct DummyBackend::State
	{
		std::mutex mutex; // guards everything below; receivers are called with it held
		std::vector<std::string> loopbacks;
		std::map<int, std::pair<std::string, Receiver>> inputs; // the open inputs: their loopback and receiver
		int nextInput = 0;

		bool has(const std::string& name) const
		{
			return std::find(loopbacks.begin(), loopbacks.end(), name) != loopbacks.end();
		}
	};

	namespace
	{
		class DummyInput : public Input
		{
		public:
			DummyInput(std::shared_ptr<DummyBackend::State> state, int number)
				: m_state(std::move(state)), m_number(number)
			{
			}

			~DummyInput() override
			{
				const std::scoped_lock lock(m_state->mutex);
				m_state->inputs.erase(m_number);
			}

		private:
			std::shared_ptr<DummyBackend::State> m_state;
			int m_number;
		};

		class DummyOutput : public Output
		{
		public:
			DummyOutput(std::shared_ptr<DummyBackend::State> state, std::string name)
				: m_state(std::move(state)), m_name(std::move(name))
			{
			}

			void send(const audio::MidiMessage& message, HostTime time) override
			{
				const std::scoped_lock lock(m_state->mutex);
				if (!m_state->has(m_name))
					return;
				for (const auto& [number, input] : m_state->inputs)
					if (input.first == m_name)
						input.second(message, time);
			}

		private:
			std::shared_ptr<DummyBackend::State> m_state;
			std::string m_name;
		};
	}

	DummyBackend::DummyBackend() : m_state(std::make_shared<State>())
	{
	}

	DummyBackend::~DummyBackend() = default;

	std::vector<PortInfo> DummyBackend::ports()
	{
		const std::scoped_lock lock(m_state->mutex);
		std::vector<PortInfo> ports;
		for (const std::string& name : m_state->loopbacks)
		{
			ports.push_back({name, name, true, false});
			ports.push_back({name, name, false, false});
		}
		return ports;
	}

	std::unique_ptr<Input> DummyBackend::openInput(const std::string& id, Receiver receiver)
	{
		const std::scoped_lock lock(m_state->mutex);
		if (!m_state->has(id))
			throw std::runtime_error("no MIDI input " + id);
		const int number = m_state->nextInput++;
		m_state->inputs[number] = {id, std::move(receiver)};
		return std::make_unique<DummyInput>(m_state, number);
	}

	std::unique_ptr<Output> DummyBackend::openOutput(const std::string& id)
	{
		const std::scoped_lock lock(m_state->mutex);
		if (!m_state->has(id))
			throw std::runtime_error("no MIDI output " + id);
		return std::make_unique<DummyOutput>(m_state, id);
	}

	void DummyBackend::addLoopback(const std::string& name)
	{
		{
			const std::scoped_lock lock(m_state->mutex);
			if (m_state->has(name))
				return;
			m_state->loopbacks.push_back(name);
		}
		portsChanged();
	}

	void DummyBackend::removeLoopback(const std::string& name)
	{
		{
			const std::scoped_lock lock(m_state->mutex);
			auto& loopbacks = m_state->loopbacks;
			const auto found = std::find(loopbacks.begin(), loopbacks.end(), name);
			if (found == loopbacks.end())
				return;
			loopbacks.erase(found);
		}
		portsChanged();
	}
}
