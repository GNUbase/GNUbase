// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/midiports.h"

#include "engine/engine.h"
#include "engine/settings/usersettings.h"
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

	MidiPorts::MidiPorts(const settings::Store& settings, std::vector<std::unique_ptr<gnubase::midi::Backend>> backends)
		: m_settings(settings), m_backends(std::move(backends)), m_monitor(g_monitorSize)
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
		m_allInputs[static_cast<size_t>(slot)].store(inAllInputs(port), std::memory_order_relaxed); // first open too
		m_slotHashes[static_cast<size_t>(slot)].store(chordPadPortHash(key(port)), std::memory_order_relaxed);
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
		const bool listed = listHardware();
		const bool followed = followSettings();
		return listed || followed;
	}

	bool MidiPorts::listHardware()
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
			changed = changed || m_ports[i].present != seen[i];
			m_ports[i].present = seen[i];
		}
		return changed;
	}

	bool MidiPorts::followSettings()
	{
		bool changed = false;
		for (Port& port : m_ports)
		{
			if (port.slot >= 0)
				m_allInputs[static_cast<size_t>(port.slot)].store(inAllInputs(port), std::memory_order_relaxed);
			const bool wanted = port.present && port.info.input && visible(port);
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

	void MidiPorts::take(const audio::DeviceBlock& block, double sampleRate, audio::MidiBuffer& allInputs,
		PadBuffers& pads, const ChordPadKeys& padKeys) noexcept
	{
		m_queue.take(block.time, sampleRate, block.frames,
			[this, &block, &allInputs, &pads, &padKeys](int slot, const audio::MidiMessage& message)
			{
				if (slot == g_keyboardSlot)
				{
					allInputs.add(message); // not a port, so not for the monitor
					return;
				}
				if (slot < 0)
				{
					const auto player = static_cast<size_t>(g_padsSlot - slot);
					if (player < pads.size())
						pads.at(player).add(message);
					return;
				}
				const bool inAll = m_allInputs[static_cast<size_t>(slot)].load(std::memory_order_relaxed);
				const int status = message.bytes[0] & 0xF0;
				if (m_padListen.load(std::memory_order_relaxed) && message.size == 3 &&
					(status == 0x90 || status == 0x80))
				{
					const bool on = status == 0x90 && message.bytes[2] > 0;
					m_padEvents.tryPush(PadEvent{PadEvent::Kind::Note, static_cast<uint8_t>(message.bytes[1] & 0x7F),
						static_cast<uint8_t>(on ? message.bytes[2] : 0)});
					if (inAll)
						allInputs.add(message);
				}
				else
				{
					// The remote keys come from all MIDI inputs, or only from the chosen one, which may be outside
					// them.
					const uint64_t hash = m_slotHashes[static_cast<size_t>(slot)].load(std::memory_order_relaxed);
					const bool remote = padKeys.portHash == 0 ? inAll : hash == padKeys.portHash;
					if (!(remote && playPad(message, padKeys, pads)) && inAll)
						allInputs.add(message);
				}
				if (!m_monitor.tryPush(Received{slot, block.position + message.offset, message}))
					m_monitorDropped.fetch_add(1, std::memory_order_relaxed);
			});
	}

	bool MidiPorts::playPad(const audio::MidiMessage& message, const ChordPadKeys& keys, PadBuffers& pads) noexcept
	{
		const int status = message.bytes[0] & 0xF0;
		// Modify All Pad Voicings and Tensions and Transpose All Pads (p. 1387): their source plays nothing.
		for (size_t which = 0; keys.active && which < keys.controlSources.size(); ++which)
		{
			const uint8_t source = keys.controlSources.at(which);
			uint8_t value = 0;
			if (source == 1 && status == 0xD0 && message.size >= 2)
				value = message.bytes[1] & 0x7F;
			else if (message.size == 3 &&
				((source == 2 && status == 0xE0) ||
					(source == 3 && status == 0xB0 && message.bytes[1] == keys.controlNumbers.at(which))))
				value = message.bytes[2] & 0x7F; // the pitch bend's most significant bits, or the controller's value
			else
				continue;
			m_padEvents.tryPush(PadEvent{PadEvent::Kind::Control, static_cast<uint8_t>(which), value});
			return true;
		}
		if (message.size != 3 || (status != 0x90 && status != 0x80))
			return false;
		const size_t key = message.bytes[1] & 0x7F;
		// Each player that plays voices the pad its way into its own buffer (#1089).
		const auto emitTo = [&message, &pads](size_t player)
		{
			return [&message, &buffer = pads.at(player)](bool on, uint8_t pitch, uint8_t velocity, uint8_t channel)
			{
				audio::MidiMessage note = message;
				note.bytes[0] = static_cast<uint8_t>((on ? 0x90 : 0x80) | channel);
				note.bytes[1] = pitch;
				note.bytes[2] = velocity;
				buffer.add(note);
			};
		};
		const auto plays = [&keys](size_t player) { return (keys.players & (1U << player)) != 0; };
		const auto offAll = [this, &emitTo, key]
		{
			for (size_t player = 0; player < m_padVoices.size(); ++player)
				m_padVoices.at(player).off(key, emitTo(player));
		};
		const auto holds = [this, key]
		{
			return std::any_of(m_padVoices.begin(), m_padVoices.end(),
				[key](const midi::PadVoices& voices) { return voices.holds(key); });
		};
		const auto tell = [this, key](PadEvent::Kind kind, uint8_t velocity)
		{ m_padEvents.tryPush(PadEvent{kind, static_cast<uint8_t>(key), velocity}); };
		const bool off = status == 0x80 || message.bytes[2] == 0;
		// Select/Mute Player's keys play nothing; the command thread selects or mutes (p. 1389).
		if (keys.active && std::find(keys.playerKeys.begin(), keys.playerKeys.end(), key) != keys.playerKeys.end())
		{
			if (!off)
				tell(PadEvent::Kind::Player, message.bytes[2]);
			return true;
		}
		// The sections player mode (pp. 1381-1385): a pad key chooses the chord, its section keys play its sections.
		if (keys.active && keys.sections)
		{
			const bool section =
				std::find(keys.sectionKeys.begin(), keys.sectionKeys.end(), key) != keys.sectionKeys.end() ||
				std::find(keys.subsectionKeys.begin(), keys.subsectionKeys.end(), key) != keys.subsectionKeys.end();
			if (section)
			{
				if (off)
					offAll();
				else
					playSection(message, keys, pads);
				return true;
			}
			if (keys.counts.at(keys.selected).at(key) > 0 || (off && static_cast<int>(key) == m_sectionPad))
			{
				if (!off)
				{
					m_sectionPad = static_cast<int>(key);
					m_sectionPadDown = true;
					tell(PadEvent::Kind::Triggered, message.bytes[2]);
				}
				else if (static_cast<int>(key) == m_sectionPad)
				{
					m_sectionPadDown = false;
					tell(PadEvent::Kind::Released, 0);
					// Without Latch Chord Pads, the sections end with the pad key.
					if (!keys.latchPads)
						for (const uint8_t each : keys.sectionKeys)
							for (size_t player = 0; player < m_padVoices.size(); ++player)
								m_padVoices.at(player).off(each, emitTo(player));
					if (!keys.latchPads)
						for (const uint8_t each : keys.subsectionKeys)
							for (size_t player = 0; player < m_padVoices.size(); ++player)
								m_padVoices.at(player).off(each, emitTo(player));
				}
				return true;
			}
		}
		// A chord modifier's key plays nothing; the command thread changes the pads.
		if (keys.active && keys.modifiers &&
			std::find(keys.modifierKeys.begin(), keys.modifierKeys.end(), key) != keys.modifierKeys.end())
		{
			tell(PadEvent::Kind::Modifier, off ? 0 : message.bytes[2]);
			return true;
		}
		// A remote key that Filter Notes keeps from MIDI thru, its note-offs too.
		const bool filtered = keys.active && keys.filter && key >= keys.start && key <= keys.end;
		if (off)
		{
			if (!holds())
				return filtered;
			if (!keys.latch)
			{
				offAll();
				tell(PadEvent::Kind::Released, 0);
			}
			return true;
		}
		if (!keys.active || keys.counts.at(keys.selected).at(key) == 0)
			return filtered;
		if (keys.latch && holds())
		{
			offAll();
			tell(PadEvent::Kind::Released, 0);
			return true;
		}
		for (size_t player = 0; player < m_padVoices.size(); ++player)
			if (plays(player) && keys.counts.at(player).at(key) > 0)
				m_padVoices.at(player).on(key, keys.pitches.at(player).at(key).data(), keys.counts.at(player).at(key),
					message.bytes[2], keys.overlap, emitTo(player));
		tell(PadEvent::Kind::Triggered, message.bytes[2]);
		return true;
	}

	bool MidiPorts::playSection(const audio::MidiMessage& message, const ChordPadKeys& keys, PadBuffers& pads) noexcept
	{
		const size_t key = message.bytes[1] & 0x7F;
		const uint8_t velocity = message.bytes[2];
		// The section of a section key, or of a subsection key with its offset.
		int section = -1;
		int offset = 0;
		for (size_t i = 0; i < keys.sectionKeys.size(); ++i)
			if (keys.sectionKeys.at(i) == key)
				section = static_cast<int>(i);
		for (size_t i = 0; i < keys.subsectionKeys.size() && section < 0; ++i)
			if (keys.subsectionKeys.at(i) == key)
			{
				section = keys.subsectionSections.at(i);
				offset = keys.subsectionOffsets.at(i);
			}
		// sections needs the pad key down (or latched), chordPads and combination take the pad triggered last.
		const bool padPlays = m_sectionPad >= 0 && (m_sectionPadDown || keys.latchPads || keys.sectionPlay != 0);
		if (section < 0 || section >= 5 || keys.mutedSections.at(static_cast<size_t>(section)) || !padPlays)
			return false;
		const auto pad = static_cast<size_t>(m_sectionPad);
		for (size_t player = 0; player < m_padVoices.size(); ++player)
		{
			if ((keys.players & (1U << player)) == 0)
				continue;
			const uint8_t count = keys.counts.at(player).at(pad);
			const auto& chord = keys.pitches.at(player).at(pad);
			// The chord's notes from the lowest up over five sections: one each, and the rest to the top section, or
			// with spread as evenly as they go.
			std::array<uint8_t, ChordPadKeys::g_maxPitches> notes{};
			size_t n = 0;
			for (size_t i = 0; i < count; ++i)
			{
				size_t of = 0;
				if (count <= 5)
					of = i;
				else if (keys.spread)
					of = i * 5 / count;
				else
					of = std::min<size_t>(i, 4);
				if (static_cast<int>(of) == section)
					notes.at(n++) = static_cast<uint8_t>(std::clamp(chord.at(i) + offset, 0, 127));
			}
			if (n > 0)
			{
				const auto emit = [&pads, &message, player](bool on, uint8_t pitch, uint8_t level, uint8_t channel)
				{
					audio::MidiMessage note = message;
					note.bytes = {static_cast<uint8_t>((on ? 0x90 : 0x80) | channel), pitch, level};
					pads.at(player).add(note);
				};
				m_padVoices.at(player).on(key, notes.data(), n, velocity, midi::PadOverlap::HoldFirst, emit);
			}
		}
		return true;
	}

	std::string MidiPorts::key(const Port& port)
	{
		return port.backend->name() + (port.info.input ? "/in/" : "/out/") + port.info.id;
	}

	Json MidiPorts::setting(const Port& port) const
	{
		return m_settings.value("midi.portSettings").value(key(port), Json::object());
	}

	Json MidiPorts::toJson(const Port& port) const
	{
		const std::string showAs = setting(port).value("showAs", std::string());
		return {{"backend", port.backend->name()}, {"id", port.info.id}, {"input", port.info.input},
			{"name", port.info.name}, {"showAs", showAs.empty() ? port.info.name : showAs}, {"visible", visible(port)},
			{"inAllInputs", inAllInputs(port)}, {"present", port.present}, {"open", port.input != nullptr},
			{"midi2", port.info.midi2}};
	}

	Json MidiPorts::list() const
	{
		Json ports = Json::array();
		for (const Port& port : m_ports)
			ports.push_back(toJson(port));
		return {{"ports", std::move(ports)}};
	}

	size_t MidiPorts::find(const Json& parameters) const
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
		return static_cast<size_t>(found - m_ports.begin());
	}

	void MidiPorts::applySettings(CommandContext& context)
	{
		if (!m_listed)
			return; // the first midi command lists the ports with their settings
		followSettings();
		context.emit("midi.changed", list()); // also for names, which change nothing else
	}

	Json MidiPorts::setPort(Engine& engine, CommandContext& context)
	{
		listOnce();
		Json changes = context.parameters();
		const Port& port = m_ports[find(changes)];
		if (changes.contains("inAllInputs") && !port.info.input)
			throw CommandError("only inputs belong to all MIDI inputs");
		for (const char* name : {"backend", "id", "input"})
			changes.erase(name);
		// The engine applies the changed setting (applySettings), which opens or closes the port and sends
		// midi.changed.
		engine.modifySetting(context, "midi.portSettings",
			[entry = key(port), &changes](const Json& value)
			{
				return settings::changeEntry(
					value, entry, changes, {{"showAs", ""}, {"visible", true}, {"inAllInputs", true}});
			});
		return toJson(port);
	}

	void MidiPorts::registerCommands(Engine& engine)
	{
		for (const std::unique_ptr<gnubase::midi::Backend>& backend : m_backends)
			backend->onPortsChanged([&engine] { engine.post("midi.refresh"); });

		engine.registerCommand(
			{"midi.padListen", 1,
				"Whether every MIDI input port's notes also go to the chord pads as they play (chordPads.listen, "
				"which uses this, and chordPads.remoteInput), with the pads' remote keys left alone meanwhile.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["on"],
					"properties": {"on": {"type": "boolean"}}})"),
				Json::object(), false},
			[this](CommandContext& context)
			{
				listOnce();
				listenForPads(context.parameters().at("on").get<bool>());
				return Json::object();
			});

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
				"are closed. inAllInputs, for inputs: whether tracks set to all MIDI inputs receive it. The settings "
				"are kept in the user setting midi.portSettings, so they come back with the port, also after a "
				"restart. Returns the port. If that changes the setting, sends midi.ports as the event midi.changed, "
				"as settings.set and settings.reset on midi.portSettings do.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["id", "input"],
					"properties": {"backend": {"type": "string"}, "id": {"type": "string"},
					"input": {"type": "boolean"}, "showAs": {"type": "string"}, "visible": {"type": "boolean"},
					"inAllInputs": {"type": "boolean"}}})"),
				Json::parse(R"({"type": "object"})"), false},
			[this, &engine](CommandContext& context) { return setPort(engine, context); });
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
