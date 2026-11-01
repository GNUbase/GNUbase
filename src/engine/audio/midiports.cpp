// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/midiports.h"

#include "engine/engine.h"
#include "platform/log.h"

#include <algorithm>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		constexpr size_t g_monitorSize = 4096; // messages midi.monitor keeps between two reads

		Json portsSchema()
		{
			return Json::parse(R"({"type": "object", "properties": {"ports": {"type": "array"}},
				"required": ["ports"]})");
		}
	}

	MidiPorts::MidiPorts(std::vector<std::unique_ptr<gnubase::midi::Backend>> backends)
		: m_backends(std::move(backends)), m_monitor(g_monitorSize)
	{
	}

	MidiPorts::~MidiPorts()
	{
		for (Port& port : m_ports)
			port.input.reset(); // before the queue their receivers push into
	}

	void MidiPorts::open(Port& port)
	{
		if (port.slot < 0)
		{
			if (m_nextSlot >= static_cast<int>(g_maxInputs))
			{
				logging::warning("Cannot open the MIDI input {}: {} inputs are open", port.info.name, g_maxInputs);
				return;
			}
			port.slot = m_nextSlot++;
		}
		const int slot = port.slot;
		m_allInputs[static_cast<size_t>(slot)].store(port.inAllInputs, std::memory_order_relaxed);
		try
		{
			port.input = port.backend->openInput(port.info.id,
				[this, slot](const audio::MidiMessage& message, HostTime time) { m_queue.push(slot, message, time); });
		}
		catch (const std::exception& error)
		{
			logging::warning("Cannot open the MIDI input {}: {}", port.info.name, error.what());
		}
	}

	void MidiPorts::listOnce()
	{
		if (!m_listed)
			refresh();
	}

	bool MidiPorts::refresh()
	{
		m_listed = true;
		bool changed = false;
		std::vector<bool> seen(m_ports.size(), false);
		for (const std::unique_ptr<gnubase::midi::Backend>& backend : m_backends)
		{
			std::vector<gnubase::midi::PortInfo> infos;
			try
			{
				infos = backend->ports();
			}
			catch (const std::exception& error)
			{
				logging::warning("Cannot list the {} MIDI ports: {}", backend->name(), error.what());
				for (size_t i = 0; i < m_ports.size(); ++i)
					seen[i] = seen[i] || (m_ports[i].backend == backend.get() && m_ports[i].present); // as they were
				continue;
			}
			for (const gnubase::midi::PortInfo& info : infos)
			{
				const auto found = std::find_if(m_ports.begin(), m_ports.end(),
					[&backend, &info](const Port& port)
					{
						return port.backend == backend.get() && port.info.id == info.id &&
							port.info.input == info.input;
					});
				if (found == m_ports.end())
				{
					Port port;
					port.backend = backend.get();
					port.info = info;
					m_ports.push_back(std::move(port));
					seen.push_back(true);
					changed = true;
				}
				else
				{
					found->info = info;
					seen[static_cast<size_t>(found - m_ports.begin())] = true;
				}
			}
		}
		for (size_t i = 0; i < m_ports.size(); ++i)
		{
			Port& port = m_ports[i];
			changed = changed || port.present != seen[i];
			port.present = seen[i];
			const bool wanted = port.present && port.visible && port.info.input;
			if (wanted && !port.input)
			{
				open(port);
				changed = true;
			}
			else if (!wanted && port.input)
			{
				port.input.reset();
				changed = true;
			}
		}
		return changed;
	}

	void MidiPorts::take(const audio::DeviceBlock& block, double sampleRate, audio::MidiBuffer& allInputs) noexcept
	{
		m_queue.take(block.time, sampleRate, block.frames,
			[this, &block, &allInputs](int slot, const audio::MidiMessage& message)
			{
				if (m_allInputs[static_cast<size_t>(slot)].load(std::memory_order_relaxed))
					allInputs.add(message);
				if (!m_monitor.tryPush(Received{slot, block.position + message.offset, message}))
					m_monitorDropped.fetch_add(1, std::memory_order_relaxed);
			});
	}

	Json MidiPorts::toJson(const Port& port)
	{
		return {{"backend", port.backend->name()}, {"id", port.info.id}, {"input", port.info.input},
			{"name", port.info.name}, {"showAs", port.showAs.empty() ? port.info.name : port.showAs},
			{"visible", port.visible}, {"inAllInputs", port.info.input && port.inAllInputs}, {"present", port.present},
			{"open", port.input != nullptr}, {"midi2", port.info.midi2}};
	}

	Json MidiPorts::list() const
	{
		Json ports = Json::array();
		for (const Port& port : m_ports)
			ports.push_back(toJson(port));
		return {{"ports", std::move(ports)}};
	}

	MidiPorts::Port& MidiPorts::find(const Json& parameters)
	{
		const auto id = parameters.at("id").get<std::string>();
		const bool input = parameters.at("input").get<bool>();
		const auto backend = parameters.value("backend", std::string());
		const auto found = std::find_if(m_ports.begin(), m_ports.end(),
			[&id, input, &backend](const Port& port)
			{
				return port.info.id == id && port.info.input == input &&
					(backend.empty() || port.backend->name() == backend);
			});
		if (found == m_ports.end())
			throw CommandError(std::string(input ? "no MIDI input " : "no MIDI output ") + id);
		return *found;
	}

	Json MidiPorts::setPort(const Json& parameters)
	{
		listOnce();
		Port& port = find(parameters);
		if (parameters.contains("inAllInputs") && !port.info.input)
			throw CommandError("only inputs belong to all MIDI inputs");
		port.showAs = parameters.value("showAs", port.showAs);
		port.visible = parameters.value("visible", port.visible);
		port.inAllInputs = parameters.value("inAllInputs", port.inAllInputs);
		if (port.slot >= 0)
			m_allInputs[static_cast<size_t>(port.slot)].store(port.inAllInputs, std::memory_order_relaxed);
		refresh(); // opens or closes it
		return toJson(port);
	}

	void MidiPorts::registerCommands(Engine& engine)
	{
		for (const std::unique_ptr<gnubase::midi::Backend>& backend : m_backends)
			backend->onPortsChanged([&engine] { engine.post("midi.refresh"); });

		engine.registerCommand(
			{"midi.ports", 1,
				"The MIDI ports of every MIDI backend, inputs and outputs: backend, id, system name, the name they are "
				"shown as (showAs), whether they are visible, whether an input belongs to all MIDI inputs, whether "
				"the port is present now and open, and whether the system speaks MIDI 2.0 with it. Ports that were "
				"unplugged stay listed with their settings, not present.",
				CommandInfo{}.parameters, portsSchema(), true},
			[this](CommandContext&)
			{
				listOnce();
				return list();
			});
		engine.registerCommand(
			{"midi.setPort", 1,
				"Changes a MIDI port's settings. The port is given by its id, whether it is an input, and its backend "
				"if ids repeat. showAs: the name it is shown as; empty shows the system name. visible: hidden inputs "
				"are closed. inAllInputs, for inputs: whether tracks set to all MIDI inputs receive it. Returns the "
				"port.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["id", "input"],
					"properties": {"backend": {"type": "string"}, "id": {"type": "string"},
					"input": {"type": "boolean"}, "showAs": {"type": "string"}, "visible": {"type": "boolean"},
					"inAllInputs": {"type": "boolean"}}})"),
				Json::parse(R"({"type": "object"})"), false},
			[this](CommandContext& context)
			{
				Json port = setPort(context.parameters());
				context.emit("midi.changed", list());
				return port;
			});
		engine.registerCommand(
			{"midi.refresh", 1,
				"Lists the MIDI ports again: opens new and returning visible inputs and marks unplugged ports as not "
				"present. Runs on its own when a backend reports that its ports changed.",
				CommandInfo{}.parameters, portsSchema(), false},
			[this](CommandContext& context)
			{
				if (refresh())
					context.emit("midi.changed", list());
				return list();
			});
		engine.registerCommand(
			{"midi.monitor", 1,
				"The MIDI input received since the last call from every open input, at most 4096 messages, each with "
				"its port, its frame position on the audio device and its bytes. dropped counts what didn't fit: "
				"while nobody reads, newer messages are dropped. start: true starts afresh; it drops what was "
				"received so far and returns nothing. Each call empties what it returns. Input reaches the monitor "
				"while an audio device processes blocks, which places it in time.",
				Json::parse(R"({"type": "object", "additionalProperties": false,
					"properties": {"start": {"type": "boolean"}}})"),
				Json::parse(R"({"type": "object", "properties": {"received": {"type": "array"},
					"dropped": {"type": "integer"}}, "required": ["received", "dropped"]})"),
				true},
			[this](CommandContext& context)
			{
				listOnce();
				const bool start = context.parameters().value("start", false);
				Json received = Json::array();
				Received entry;
				while (m_monitor.tryPop(entry))
				{
					if (start)
						continue;
					const auto port = std::find_if(m_ports.begin(), m_ports.end(),
						[&entry](const Port& candidate) { return candidate.slot == entry.slot; });
					const std::vector<uint8_t> bytes(
						entry.message.bytes.begin(), entry.message.bytes.begin() + entry.message.size);
					received.push_back({{"backend", port->backend->name()}, {"id", port->info.id},
						{"position", entry.position}, {"bytes", bytes}});
				}
				const size_t dropped = m_monitorDropped.exchange(0, std::memory_order_relaxed);
				return Json{{"received", std::move(received)}, {"dropped", start ? 0 : dropped}};
			});
	}
}
