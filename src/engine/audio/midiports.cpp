// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/midiports.h"

#include "engine/audio/miditap.h"
#include "engine/engine.h"
#include "engine/settings/usersettings.h"
#include "platform/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		constexpr size_t g_monitorSize = 4096; // messages midi.monitor keeps between two reads
		constexpr size_t g_activitySize = 256; // messages midi.activity keeps between two runs

		Json portsSchema()
		{
			return Json::parse(R"({"type": "object", "properties": {"ports": {"type": "array"}},
				"required": ["ports"]})");
		}
	}

	MidiPorts::MidiPorts(const settings::Store& settings, std::vector<std::unique_ptr<gnubase::midi::Backend>> backends)
		: m_settings(settings), m_backends(std::move(backends)), m_mpeInputs(g_maxInputs), m_monitor(g_monitorSize),
		  m_activity(g_activitySize)
	{
	}

	MidiPorts::~MidiPorts()
	{
		if (m_sender.joinable())
		{
			m_stopSending.store(true, std::memory_order_relaxed);
			m_sendWake.release();
			m_sender.join();
		}
		m_outputs.clear(); // before their backends
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
		m_mpeSetups[static_cast<size_t>(slot)].store(mpe(port).pack(), std::memory_order_relaxed);
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

	int MidiPorts::outputSlot(const std::string& key)
	{
		listOnce();
		if (const auto found = m_outputSlots.find(key); found != m_outputSlots.end())
			return found->second;
		// ponytail: an output stays open, also when its device goes and comes back; reopen it on hot-plug when a
		// backend needs that.
		for (const Port& port : m_ports)
		{
			if (port.info.input || !port.present || MidiPorts::key(port) != key)
				continue;
			std::unique_ptr<gnubase::midi::Output> output;
			try
			{
				output = port.backend->openOutput(port.info.id);
			}
			catch (const std::runtime_error& error)
			{
				logging::warning("Cannot open the MIDI output {}: {}", port.info.name, error.what());
				return -1;
			}
			const std::scoped_lock lock(m_outputMutex);
			const auto slot = static_cast<int>(m_outputs.size());
			m_outputs.push_back(std::move(output));
			m_outputSlots[key] = slot;
			if (!m_sender.joinable())
				m_sender = std::thread([this] { sendQueued(); });
			return slot;
		}
		return -1;
	}

	void MidiPorts::send(
		int slot, const graph::EventBuffer& events, HostTime time, double sampleRate, HeldNotes& held) noexcept
	{
		std::array<audio::MidiMessage, 3> messages;
		bool queued = false;
		for (const graph::ProcessEvent& event : events)
		{
			const HostTime due = time + static_cast<HostTime>(std::llround(event.offset * 1e9 / sampleRate));
			const size_t count = MidiTap::toMidi1(event, messages);
			for (size_t i = 0; i < count; ++i)
			{
				const audio::MidiMessage& message = messages[i];
				if (!m_outgoing.tryPush(Outgoing{slot, message, due}))
					continue;
				queued = true;
				const auto status = message.bytes[0] & 0xF0;
				const size_t note = ((message.bytes[0] & 0x0Fu) * 128u) + (message.bytes[1] & 0x7Fu);
				if (status == 0x90 && message.bytes[2] != 0)
					held.set(note);
				else if (status == 0x80 || status == 0x90)
					held.reset(note);
			}
		}
		if (queued)
			m_sendWake.release();
	}

	void MidiPorts::release(int slot, HeldNotes& held, const HeldNotes& staying, HostTime time) noexcept
	{
		const HeldNotes ending = held & ~staying;
		held.reset();
		bool queued = false;
		for (size_t note = 0; note < ending.size(); ++note)
		{
			if (!ending.test(note))
				continue;
			audio::MidiMessage off;
			off.size = 3;
			off.bytes = {static_cast<uint8_t>(0x80 | (note / 128)), static_cast<uint8_t>(note % 128), 64};
			if (m_outgoing.tryPush(Outgoing{slot, off, time}))
				queued = true;
		}
		if (queued)
			m_sendWake.release();
	}

	void MidiPorts::sendQueued()
	{
		while (!m_stopSending.load(std::memory_order_relaxed))
		{
			m_sendWake.wait();
			const std::scoped_lock lock(m_outputMutex);
			Outgoing next;
			while (m_outgoing.tryPop(next))
				try
				{
					m_outputs.at(static_cast<size_t>(next.slot))->send(next.message, next.time);
				}
				catch (const std::exception& error)
				{
					logging::warning("A MIDI output didn't take a message: {}", error.what());
				}
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
			{
				m_allInputs[static_cast<size_t>(port.slot)].store(inAllInputs(port), std::memory_order_relaxed);
				m_mpeSetups[static_cast<size_t>(port.slot)].store(mpe(port).pack(), std::memory_order_relaxed);
			}
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

	void MidiPorts::addInput(int slot, const audio::MidiMessage& message, graph::EventBuffer& allInputs) noexcept
	{
		if (message.size != gnubase::midi::messageSize(message.bytes[0]) || message.bytes[0] < 0x80 ||
			message.bytes[0] >= 0xF0)
			return; // not a channel message
		const graph::ProcessEvent event = MidiTap::toEvent(message, message.offset);
		if (slot < 0)
		{
			if (graph::endsNotes(event))
				allInputs.addEnd(event);
			else
				allInputs.add(event);
			return;
		}
		const auto index = static_cast<size_t>(slot);
		const uint64_t setup = m_mpeSetups[index].load(std::memory_order_relaxed);
		if (setup != m_mpeApplied[index])
		{
			m_mpeApplied[index] = setup;
			m_mpeInputs[index].reset();
		}
		m_mpeInputs[index].process(midi::MpeSetup::unpack(setup), event, allInputs);
	}

	void MidiPorts::take(const audio::DeviceBlock& block, double sampleRate, graph::EventBuffer& allInputs,
		PadBuffers& pads, const ChordPadKeys& padKeys) noexcept
	{
		// The keys down on the remote's input before another became it go up with it (#1447), and what they hold ends:
		// their note-offs come from an input that is no longer the remote's (#1571).
		if (padKeys.portHash != m_sectionHeldPort)
		{
			m_sectionHeldPort = padKeys.portHash;
			m_sectionHeld.fill(0);
			m_sectionPadDown = false;
			for (size_t key = 0; key < midi::PadVoices::g_triggers; ++key)
			{
				for (size_t player = 0; player < m_padVoices.size(); ++player)
				{
					m_padVoices.at(player).off(key,
						[&buffer = pads.at(player)](bool, uint8_t pitch, uint8_t, uint8_t channel)
						{
							audio::MidiMessage note;
							note.size = 3;
							note.bytes = {static_cast<uint8_t>(0x80 | channel), pitch, 0};
							buffer.add(note);
						});
				}
				// The pad each key lit goes off, once (#1652, #1708), also with no notes held (#1740); a section key
				// lights none.
				tellReleased(key);
			}
			m_sectionPadOf.fill(0);
		}
		m_queue.take(block.time, sampleRate, block.frames,
			[this, &block, &allInputs, &pads, &padKeys](int slot, const audio::MidiMessage& message)
			{
				if (slot == g_keyboardSlot)
				{
					addInput(slot, message, allInputs); // not a port, so not for the monitor
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
				// The remote keys come from all MIDI inputs, or only from the chosen one, which may be outside them.
				const uint64_t hash = m_slotHashes[static_cast<size_t>(slot)].load(std::memory_order_relaxed);
				const bool remote = padKeys.portHash == 0 ? inAll : hash == padKeys.portHash;
				if (m_padListen.load(std::memory_order_relaxed) && message.size == 3 &&
					(status == 0x90 || status == 0x80))
				{
					const bool on = status == 0x90 && message.bytes[2] > 0;
					m_padEvents.tryPush(PadEvent{PadEvent::Kind::Note, static_cast<uint8_t>(message.bytes[1] & 0x7F),
						static_cast<uint8_t>(on ? message.bytes[2] : 0)});
					// A remote key that goes up meanwhile ends what it played and is no section key down (#1447).
					if (!on && remote)
						playPad(message, padKeys, pads);
					if (inAll)
						addInput(slot, message, allInputs);
				}
				else if (m_padListen.load(std::memory_order_relaxed) &&
					((status == 0xD0 && message.size >= 2) ||
						(message.size == 3 && (status == 0xE0 || status == 0xB0))))
				{
					// MIDI Learn of what moves all pads: the control plays on as it is.
					uint8_t source = 3; // a controller
					if (status == 0xD0)
						source = 1;
					else if (status == 0xE0)
						source = 2;
					m_padEvents.tryPush(PadEvent{PadEvent::Kind::ControlInput, source,
						static_cast<uint8_t>(status == 0xB0 ? message.bytes[1] & 0x7F : 0)});
					if (inAll)
						addInput(slot, message, allInputs);
				}
				else if (!(remote && playPad(message, padKeys, pads)) && inAll)
				{
					addInput(slot, message, allInputs);
				}
				const Received received{slot, block.position + message.offset, message};
				// Only while someone listens, and without system real-time messages (clock, active sensing), which
				// some devices send all the time.
				if (m_activityListen.load(std::memory_order_relaxed) && message.bytes[0] < 0xF8)
				{
					if (!m_activity.tryPush(received))
						m_activityDropped.fetch_add(1, std::memory_order_relaxed);
					m_activityCount.fetch_add(1, std::memory_order_relaxed);
				}
				// After the count: a thread that midi.monitor gave the message sees it counted (#1889).
				if (!m_monitor.tryPush(received))
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
		const auto holds = [this](size_t which)
		{
			return std::any_of(m_padVoices.begin(), m_padVoices.end(),
				[which](const midi::PadVoices& voices) { return voices.holds(which); });
		};
		const auto tell = [this, key](PadEvent::Kind kind, uint8_t velocity)
		{ m_padEvents.tryPush(PadEvent{kind, static_cast<uint8_t>(key), velocity}); };
		const bool off = status == 0x80 || message.bytes[2] == 0;
		if (off)
			m_sectionHeld.at(key) = 0; // also after the player mode changed
		// Select/Mute Player's keys play nothing; the command thread selects or mutes (p. 1389).
		if (keys.active && std::find(keys.playerKeys.begin(), keys.playerKeys.end(), key) != keys.playerKeys.end())
		{
			// A pad key that became one while it was down ends what it holds and puts out its pad (#1812).
			if (off)
			{
				offAll();
				tellReleased(key);
			}
			else
			{
				tell(PadEvent::Kind::Player, message.bytes[2]);
			}
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
				if (!off)
					m_sectionHeld.at(key) = message.bytes[2];
				if (off)
				{
					offAll();
					tellReleased(key); // a pad it lit before the player mode changed
				}
				else
				{
					playSection(message, keys, pads);
				}
				return true;
			}
			if (keys.counts.at(keys.selected).at(key) > 0 || (off && static_cast<int>(key) == m_sectionPad))
			{
				// A chord the key held or latched before the player mode changed ends. A pad key holds no notes here:
				// it puts out its pad with its note-off (#1740), also when another pad key came meanwhile.
				if (off)
				{
					offAll();
					tellReleased(key);
				}
				if (!off)
				{
					m_sectionPad = static_cast<int>(key);
					m_sectionPadDown = true;
					tellTriggered(key, static_cast<uint8_t>(key - keys.start), message.bytes[2]);
					// Combination: section keys pressed first start with the pad key (p. 1384).
					if (keys.sectionPlay == 2)
						for (size_t held = 0; held < m_sectionHeld.size(); ++held)
							if (m_sectionHeld.at(held) > 0)
							{
								for (size_t player = 0; player < m_padVoices.size(); ++player)
									m_padVoices.at(player).off(held, emitTo(player));
								audio::MidiMessage press = message;
								press.bytes = {0x90, static_cast<uint8_t>(held), m_sectionHeld.at(held)};
								playSection(press, keys, pads);
							}
				}
				else if (static_cast<int>(key) == m_sectionPad)
				{
					m_sectionPadDown = false;
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
			if (off)
			{
				offAll(); // as a Select/Mute Player key's (#1812)
				tellReleased(key);
			}
			tell(PadEvent::Kind::Modifier, off ? 0 : message.bytes[2]);
			return true;
		}
		// A remote key that Filter Notes keeps from MIDI thru, its note-offs too.
		const bool filtered = keys.active && keys.filter && key >= keys.start && key <= keys.end;
		if (off)
		{
			if (!holds(key))
			{
				tellReleased(key); // a pad it lit with nothing to play, such as with the selected player muted (#1740)
				return filtered;
			}
			if (!keys.latch)
			{
				offAll();
				tellReleased(key);
			}
			return true;
		}
		// Before the pad check: a key that latched a chord and plays no pad now (moved range, emptied pad, a section
		// key after the player mode changed) still ends it.
		if (keys.latch && holds(key))
		{
			offAll();
			tellReleased(key);
			return true;
		}
		if (!keys.active || keys.counts.at(keys.selected).at(key) == 0)
			return filtered;
		m_sectionPadOf.at(key) = 0;
		for (size_t player = 0; player < m_padVoices.size(); ++player)
			if (plays(player) && keys.counts.at(player).at(key) > 0)
				m_padVoices.at(player).on(key, keys.pitches.at(player).at(key).data(), keys.counts.at(player).at(key),
					message.bytes[2], keys.overlap, emitTo(player));
		// With Latch Chords, the pads of the other keys whose chords this one ended (Legato, Stop First) go out, as
		// their note-offs reported nothing (#1812).
		if (keys.latch)
			for (size_t other = 0; other < m_padLit.size(); ++other)
				if (other != key && m_padLit.at(other) != 0 && !holds(other))
					tellReleased(other);
		tellTriggered(key, static_cast<uint8_t>(key - keys.start), message.bytes[2]);
		return true;
	}

	void MidiPorts::tellTriggered(size_t key, uint8_t pad, uint8_t velocity) noexcept
	{
		m_padLit.at(key) = static_cast<uint8_t>(pad + 1);
		m_padEvents.tryPush(PadEvent{PadEvent::Kind::Triggered, static_cast<uint8_t>(key), velocity, pad});
	}

	void MidiPorts::tellReleased(size_t key) noexcept
	{
		const uint8_t lit = m_padLit.at(key);
		if (lit == 0)
			return;
		m_padLit.at(key) = 0;
		m_padEvents.tryPush(
			PadEvent{PadEvent::Kind::Released, static_cast<uint8_t>(key), 0, static_cast<uint8_t>(lit - 1)});
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
		// sections and combination need the pad key down (or latched), chordPads takes the pad triggered last.
		const bool padPlays = m_sectionPad >= 0 && (m_sectionPadDown || keys.latchPads || keys.sectionPlay == 1);
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
				// Overlaps is for another chord's notes (p. 1382): the sections of one pad play together (p. 1383).
				m_padVoices.at(player).on(key, notes.data(), n, velocity, keys.overlap, emit,
					[this, pad](size_t other) { return m_sectionPadOf.at(other) == pad + 1; });
			}
		}
		m_sectionPadOf.at(key) = static_cast<uint8_t>(pad + 1);
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

	midi::MpeSetup MidiPorts::preset(const Port& port)
	{
		const midi::MpeDevice* device = port.info.input ? midi::detectMpeDevice(port.info.name) : nullptr;
		return device != nullptr ? device->setup : midi::MpeSetup();
	}

	midi::MpeSetup MidiPorts::mpe(const Port& port) const
	{
		if (!port.info.input)
			return {};
		return midi::MpeSetup::fromJson(setting(port).value("mpe", Json::object()), preset(port));
	}

	Json MidiPorts::toJson(const Port& port) const
	{
		const std::string showAs = setting(port).value("showAs", std::string());
		Json json = {{"backend", port.backend->name()}, {"id", port.info.id}, {"input", port.info.input},
			{"name", port.info.name}, {"showAs", showAs.empty() ? port.info.name : showAs}, {"visible", visible(port)},
			{"inAllInputs", inAllInputs(port)}, {"present", port.present}, {"open", port.input != nullptr},
			{"midi2", port.info.midi2}};
		if (port.info.input)
			json["mpe"] = mpe(port).toJson();
		if (const midi::MpeDevice* device = port.info.input ? midi::detectMpeDevice(port.info.name) : nullptr)
			json["mpeDevice"] = device->name;
		return json;
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
		if (changes.contains("mpe") && !port.info.input)
			throw CommandError("only inputs have MPE input");
		for (const char* name : {"backend", "id", "input"})
			changes.erase(name);
		if (changes.contains("mpe"))
		{
			// Tuning for X is Use for Tuning, as on the page: X keeps its own assignment underneath (#1749).
			Json& call = changes.at("mpe");
			if (call.value("xExpression", std::string()) == "tuning")
			{
				if (!call.value("tuning", true))
					throw CommandError("xExpression tuning is Use for Tuning, which tuning false turns off");
				call["tuning"] = true;
				call.erase("xExpression");
			}
			// What the call leaves out stays, or is the detected device's preset; members of a later GNUbase's stay
			// too.
			Json mpe = setting(port).value("mpe", preset(port).toJson());
			mpe.update(changes.at("mpe"));
			mpe.update(midi::MpeSetup::fromJson(mpe, preset(port)).toJson());
			changes["mpe"] = std::move(mpe);
		}
		// The engine applies the changed setting (applySettings), which opens or closes the port and sends
		// midi.changed.
		engine.modifySetting(context, "midi.portSettings",
			[entry = key(port), &changes, defaults = preset(port).toJson()](const Json& value)
			{
				return settings::changeEntry(value, entry, changes,
					{{"showAs", ""}, {"visible", true}, {"inAllInputs", true}, {"mpe", defaults}});
			});
		return toJson(port);
	}

	void MidiPorts::registerCommands(Engine& engine)
	{
		for (const std::unique_ptr<gnubase::midi::Backend>& backend : m_backends)
			backend->onPortsChanged([&engine] { engine.post("midi.refresh"); });

		engine.registerCommand(
			{"midi.padListen", 1,
				"Whether every MIDI input port's notes and controls (aftertouch, pitch bend and controllers) also go "
				"to the chord pads as they play (chordPads.listen, which uses this, and chordPads.remoteInput), with "
				"the pads' remote keys left alone meanwhile, but for their note-offs, which end what they play.",
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
			{"midi.activityListen", 1,
				"Whether what the open MIDI inputs receive goes to midi.activity, which the engine then runs itself "
				"as input arrives: the studio setup dialog turns it on while it is open, for its Note Expression "
				"Input page (#1749). listener names who listens, such as the dialog's studioSetup; without it, the "
				"calls share one. It is on while any listener has it on, so turning it off for one leaves the "
				"others' (#1832). Off by default; turning it on while no listener has it on starts afresh, without "
				"what "
				"came before, and a second listener's on keeps what the first one still reads. post names a command "
				"that each run of midi.activity with input then posts with it as {received}, each message also with "
				"inAllInputs, whether its port belongs to all MIDI inputs, while the listener has it on: the key "
				"editors' step and MIDI input take the notes played so (editor.played, #679).",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["on"],
					"properties": {"on": {"type": "boolean"}, "listener": {"type": "string"},
					"post": {"type": "string"}}})"),
				Json::object(), false},
			[this](CommandContext& context)
			{
				listOnce();
				const bool was = !m_activityListeners.empty();
				const std::string listener = context.parameters().value("listener", std::string());
				if (context.parameters().at("on").get<bool>())
					m_activityListeners[listener] = context.parameters().value("post", std::string());
				else
					m_activityListeners.erase(listener);
				if (!was && !m_activityListeners.empty())
					dropActivity();
				m_activityListen.store(!m_activityListeners.empty(), std::memory_order_relaxed);
				return Json::object();
			});

		engine.registerCommand(
			{"midi.ports", 1,
				"The MIDI ports of every MIDI backend, inputs and outputs: backend, id, system name, the name they are "
				"shown as (showAs), whether they are visible, whether an input belongs to all MIDI inputs, an "
				"input's MPE input (mpe, as midi.setPort takes it), for an input that is a known MPE device the "
				"device's name (mpeDevice), whether the port is present now and open, and whether the system speaks "
				"MIDI 2.0 with it. Ports that were unplugged stay listed with their "
				"settings, not present.",
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
				"are closed. inAllInputs, for inputs: whether tracks set to all MIDI inputs receive it. mpe, for "
				"inputs: its MPE input (reference manual pp. 1295-1298), whose members the call leaves out stay as "
				"they were: mode off, lower (MPE's lower zone: master channel 1, the notes on channels 2 and up), "
				"upper (its upper zone: master channel 16, the notes on 15 and down), rotation (channel rotation: "
				"the notes on the used channels from firstChannel on, up to 16, no master channel) or both (both "
				"zones at once); channels, the notes' channels (a zone has at most 15), with both the lower zone's; "
				"upperChannels, with both the upper zone's (at most 13; the lower zone gets at most the rest of 14); "
				"pitchRange, the semitones of the notes' pitch bend; tuning "
				"(Use for Tuning), whether the notes' pitch bend is their tuning; initialX and initialY (Send Initial "
				"Value), what each note's X and Y start at: off, the channel's latest value, or center, min or max (Y "
				"only); xMessage, pressureMessage and yMessage (MIDI Message), the message of each dimension: "
				"pitchBend, aftertouch (channel pressure) or cc0 to cc127, by default pitchBend, aftertouch and cc74; "
				"xExpression, pressureExpression and yExpression (Note Expression Assignment), the note expression "
				"each plays (volume, pan, tuning, vibrato, expression, brightness, pressure), polyPressure (MIDI "
				"PolyPressure: the notes' poly pressure) or none, which passes the message as it is, by default "
				"none (X), pressure and brightness; with tuning X plays tuning "
				"over its own assignment, which it plays again without it, and xExpression tuning turns tuning "
				"on (with tuning false it is rejected), keeping X's own. On the notes' channels the dimensions' "
				"messages are the expression of the notes that sound there, as per-note expression, live and in "
				"recordings (docs/features/note-expression.md). An input whose name is a known MPE device's "
				"(mpeDevice in midi.ports) has the device's preset settings until a call changes them. The settings "
				"are kept in the user setting "
				"midi.portSettings, so they come back with the port, also after a restart. Returns the port. If that "
				"changes the setting, sends midi.ports as the event midi.changed, as settings.set and settings.reset "
				"on midi.portSettings do.",
				[]
				{
					Json schema = Json::parse(R"({"type": "object", "additionalProperties": false,
						"required": ["id", "input"], "properties": {"backend": {"type": "string"},
						"id": {"type": "string"}, "input": {"type": "boolean"}, "showAs": {"type": "string"},
						"visible": {"type": "boolean"}, "inAllInputs": {"type": "boolean"}}})");
					schema["properties"]["mpe"] = midi::mpeSchema();
					return schema;
				}(),
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
				true, true},
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
		engine.registerCommand(
			{"midi.activity", 1,
				"Takes the MIDI input that the open inputs received since the last run while midi.activityListen "
				"was on, at most 256 messages without system real-time messages (0xF8 to 0xFF), each with its port "
				"(backend and id) and its bytes, and announces it as the event midi.activity unless there is none: "
				"the Note Expression Input page's MIDI Activity display (reference manual p. 1298) shows it. The "
				"engine runs it itself, at most ten times a second, while input arrives and an audio device "
				"processes blocks; its runs stay out of engine.commandLog. What was received before a device "
				"started is dropped then. dropped counts what didn't fit. "
				"midi.monitor, which keeps the input's positions, takes its own copy.",
				CommandInfo{}.parameters,
				Json::parse(R"({"type": "object", "properties": {"received": {"type": "array"},
					"dropped": {"type": "integer"}}, "required": ["received", "dropped"]})"),
				false},
			[this, &engine](CommandContext& context)
			{
				listOnce();
				context.leaveOutOfLog();
				Json received = Json::array();
				Json posted = Json::array(); // for the listeners' post: also whether the port is in all MIDI inputs
				Received entry;
				while (m_activity.tryPop(entry))
				{
					const auto port = std::find_if(m_ports.begin(), m_ports.end(),
						[&entry](const Port& candidate) { return candidate.slot == entry.slot; });
					if (port == m_ports.end())
						continue;
					const std::vector<uint8_t> bytes(
						entry.message.bytes.begin(), entry.message.bytes.begin() + entry.message.size);
					received.push_back({{"backend", port->backend->name()}, {"id", port->info.id}, {"bytes", bytes}});
					posted.push_back(received.back());
					posted.back()["inAllInputs"] = inAllInputs(*port);
				}
				Json result = {{"received", std::move(received)},
					{"dropped", m_activityDropped.exchange(0, std::memory_order_relaxed)}};
				if (!result.at("received").empty())
				{
					context.emit("midi.activity", result);
					for (const auto& listener : m_activityListeners)
						if (!listener.second.empty())
							engine.post(listener.second, {{"received", posted}});
				}
				return result;
			});
	}
}
