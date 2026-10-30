// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/midiports.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"
#include "platform/midi/inputqueue.h"
#include "platform/midi/port.h"
#ifdef _WIN32
#include "platform/midi/winmm.h"
#endif
#ifdef __APPLE__
#include "platform/midi/coremidi.h"

#include <CoreMIDI/CoreMIDI.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase::midi;
using gnubase::HostTime;
using gnubase::hostTime;
using gnubase::audio::DeviceBlock;
using gnubase::audio::MidiBuffer;
using gnubase::audio::MidiMessage;

namespace
{
	constexpr HostTime g_ms = 1000000;

	MidiMessage noteOn(uint8_t pitch)
	{
		MidiMessage message;
		message.size = 3;
		message.bytes = {0x90, pitch, 100};
		return message;
	}

	// Collects what an input port receives.
	class Inbox
	{
	public:
		Receiver receiver()
		{
			return [this](const MidiMessage& message, HostTime time)
			{
				const std::scoped_lock lock(m_mutex);
				m_messages.emplace_back(message, time);
			};
		}

		std::vector<std::pair<MidiMessage, HostTime>> messages()
		{
			const std::scoped_lock lock(m_mutex);
			return m_messages;
		}

		// False if fewer than count messages arrived within a second.
		bool waitFor(size_t count)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
			while (messages().size() < count)
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			return true;
		}

	private:
		std::mutex m_mutex;
		std::vector<std::pair<MidiMessage, HostTime>> m_messages;
	};

	// What every MIDI backend promises: the same suite runs against the dummy backend in CI and against the hardware
	// backends locally. With a loopback, an output named sendTo wired to an input named receiveFrom, it also sends and
	// receives.
	void checkContract(Backend& backend, const std::string& sendTo, const std::string& receiveFrom)
	{
		INFO("backend " << backend.name());
		const std::vector<PortInfo> ports = backend.ports();
		std::set<std::pair<std::string, bool>> ids;
		for (const PortInfo& port : ports)
		{
			CHECK(!port.id.empty());
			CHECK(!port.name.empty());
			CHECK(ids.insert({port.id, port.input}).second);
		}
		Inbox inbox;
		CHECK_THROWS_AS(backend.openInput("no such port", inbox.receiver()), std::runtime_error);
		CHECK_THROWS_AS(backend.openOutput("no such port"), std::runtime_error);
		for (const PortInfo& port : ports)
		{
			INFO("port " << port.id);
			if (port.input)
				CHECK(backend.openInput(port.id, inbox.receiver()) != nullptr);
			else
				CHECK(backend.openOutput(port.id) != nullptr);
		}
		if (sendTo.empty())
			return;

		INFO("loopback from " << sendTo << " to " << receiveFrom);
		const auto named = [&ports, &sendTo, &receiveFrom](bool input)
		{
			return std::find_if(ports.begin(), ports.end(),
				[&name = input ? receiveFrom : sendTo, input](const PortInfo& port)
				{ return port.input == input && port.name == name; });
		};
		REQUIRE(named(true) != ports.end());
		REQUIRE(named(false) != ports.end());
		auto input = backend.openInput(named(true)->id, inbox.receiver());
		const auto output = backend.openOutput(named(false)->id);

		// Sent now and scheduled 50 ms ahead: each arrives with a host time no earlier than it was sent at.
		const HostTime sent = hostTime();
		output->send(noteOn(60), sent);
		const HostTime scheduled = hostTime() + (50 * g_ms);
		output->send(noteOn(62), scheduled);
		REQUIRE(inbox.waitFor(2));
		const auto received = inbox.messages();
		CHECK(received[0].first.bytes == noteOn(60).bytes);
		CHECK(received[0].second >= sent - g_ms);
		CHECK(received[0].second <= hostTime());
		CHECK(received[1].first.bytes == noteOn(62).bytes);
		CHECK(received[1].second >= scheduled - g_ms);

		// A closed input receives nothing.
		input.reset();
		output->send(noteOn(64), hostTime());
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		CHECK(inbox.messages().size() == 2);
	}

#ifdef __APPLE__
	// A loopback of virtual ports: a virtual destination that passes what it receives on to a virtual source of the
	// same name, timestamps and all.
	class VirtualLoopback
	{
	public:
		explicit VirtualLoopback(const char* name)
		{
			const CFStringRef text = CFStringCreateWithCString(nullptr, name, kCFStringEncodingUTF8);
			REQUIRE(MIDIClientCreateWithBlock(text, &m_client, nullptr) == noErr);
			REQUIRE(MIDISourceCreateWithProtocol(m_client, text, kMIDIProtocol_1_0, &m_source) == noErr);
			const MIDIEndpointRef source = m_source;
			REQUIRE(MIDIDestinationCreateWithProtocol(m_client, text, kMIDIProtocol_1_0, &m_destination,
						^(const MIDIEventList* list, void* /*refCon*/) {
						  MIDIReceivedEventList(source, list);
						}) == noErr);
			CFRelease(text);
		}

		// The client stays: CoreMIDI advises against disposing clients.
		~VirtualLoopback()
		{
			MIDIEndpointDispose(m_destination);
			MIDIEndpointDispose(m_source);
		}

		VirtualLoopback(const VirtualLoopback&) = delete;
		VirtualLoopback& operator=(const VirtualLoopback&) = delete;

	private:
		MIDIClientRef m_client = 0;
		MIDIEndpointRef m_source = 0;
		MIDIEndpointRef m_destination = 0;
	};

	bool hasPort(Backend& backend, const std::string& name)
	{
		const std::vector<PortInfo> ports = backend.ports();
		return std::any_of(ports.begin(), ports.end(), [&name](const PortInfo& port) { return port.name == name; });
	}

	// False if the condition isn't true within two seconds.
	template <typename Condition> bool eventually(Condition condition)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!condition())
		{
			if (std::chrono::steady_clock::now() > deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return true;
	}
#endif

	struct Taken
	{
		int port = 0;
		MidiMessage message;
	};

	std::vector<Taken> take(InputQueue& queue, HostTime blockTime)
	{
		std::vector<Taken> taken;
		queue.take(blockTime, 48000.0, 480, // 10 ms
			[&taken](int port, const MidiMessage& message) { taken.push_back({port, message}); });
		return taken;
	}
}

TEST_CASE("MIDI input lands in the block after it arrived, at the offset of its arrival", "[unit]")
{
	const HostTime block = 1000 * g_ms;
	InputQueue queue(16);
	queue.push(0, noteOn(60), block - (10 * g_ms)); // when the block before started
	queue.push(1, noteOn(61), block - (5 * g_ms));
	queue.push(0, noteOn(62), block - 1);
	queue.push(0, noteOn(63), block + g_ms); // for the next block

	std::vector<Taken> taken = take(queue, block);
	REQUIRE(taken.size() == 3);
	CHECK(taken[0].port == 0);
	CHECK(taken[0].message.offset == 0);
	CHECK(taken[1].port == 1);
	CHECK(taken[1].message.offset == 240);
	CHECK(taken[2].message.offset == 479);
	CHECK(taken[2].message.bytes == noteOn(62).bytes);

	taken = take(queue, block + (10 * g_ms));
	REQUIRE(taken.size() == 1);
	CHECK(taken[0].message.offset == 48);
	CHECK(take(queue, block + (20 * g_ms)).empty());
}

TEST_CASE("late MIDI input goes to the start of its block, and offsets never decrease", "[unit]")
{
	const HostTime block = 1000 * g_ms;
	InputQueue queue(16);
	queue.push(0, noteOn(60), block - (50 * g_ms)); // the block came late
	queue.push(0, noteOn(61), block - (2 * g_ms));
	queue.push(1, noteOn(62), block - (5 * g_ms)); // another port's driver was slower to deliver
	const std::vector<Taken> taken = take(queue, block);
	REQUIRE(taken.size() == 3);
	CHECK(taken[0].message.offset == 0);
	CHECK(taken[1].message.offset == 384);
	CHECK(taken[2].message.offset == 384);
}

TEST_CASE("a full MIDI input queue drops and counts, and clear empties it", "[unit]")
{
	const HostTime block = 1000 * g_ms;
	InputQueue queue(2);
	CHECK(queue.push(0, noteOn(60), block + g_ms)); // waits for a later block
	CHECK(queue.push(0, noteOn(61), block));
	CHECK(!queue.push(0, noteOn(62), block));
	CHECK(queue.dropped() == 1);
	CHECK(take(queue, block).empty());
	queue.clear();
	CHECK(take(queue, block + (100 * g_ms)).empty());
}

TEST_CASE("the dummy MIDI backend keeps the port contract and reports loopbacks plugged in and out", "[unit]")
{
	DummyBackend backend;
	int changes = 0;
	backend.onPortsChanged([&changes] { ++changes; });
	CHECK(backend.ports().empty());
	checkContract(backend, "", "");

	backend.addLoopback("Loop");
	CHECK(changes == 1);
	const std::vector<PortInfo> ports = backend.ports();
	REQUIRE(ports.size() == 2);
	CHECK(ports[0].input);
	CHECK(!ports[1].input);
	CHECK(ports[1].id == "Loop");
	checkContract(backend, "Loop", "Loop");

	Inbox inbox;
	const auto input = backend.openInput("Loop", inbox.receiver());
	const auto output = backend.openOutput("Loop");
	backend.removeLoopback("Loop");
	CHECK(changes == 2);
	CHECK(backend.ports().empty());
	output->send(noteOn(60), hostTime()); // the ports of an unplugged device are silent
	CHECK(inbox.messages().empty());
	CHECK_THROWS_AS(backend.openInput("Loop", inbox.receiver()), std::runtime_error);
}

// Hidden: opens every MIDI port of the machine. Run it on a machine with MIDI ports: gnubase-tests "[hardware]".
// With GNUBASE_TEST_MIDI_LOOPBACK set to a loopback port's name (an output wired to the input of the same name, as
// loopMIDI makes them), or to "output>input" (as Windows MIDI Services' "Loopback A>Loopback B"), it also sends and
// receives through it.
TEST_CASE("every hardware MIDI backend keeps the port contract", "[.hardware]")
{
	const char* variable = std::getenv("GNUBASE_TEST_MIDI_LOOPBACK");
	const std::string loopback = variable != nullptr ? variable : "";
	const size_t arrow = loopback.find('>');
	const std::string sendTo = loopback.substr(0, arrow);
	const std::string receiveFrom = arrow == std::string::npos ? loopback : loopback.substr(arrow + 1);
	for (const std::unique_ptr<Backend>& backend : makeBackends())
		if (backend->name() != "dummy")
			checkContract(*backend, sendTo, receiveFrom);
}

#ifdef _WIN32
// Only lists ports; the contract suite above opens them ("[hardware]").
TEST_CASE("the WinMM backend lists its ports and refuses unknown ones", "[unit]")
{
	const std::unique_ptr<Backend> backend = makeWinmmBackend();
	std::set<std::pair<std::string, bool>> ids;
	for (const PortInfo& port : backend->ports())
	{
		CHECK(!port.name.empty());
		CHECK(ids.insert({port.id, port.input}).second);
	}
	Inbox inbox;
	CHECK_THROWS_AS(backend->openInput("no such port", inbox.receiver()), std::runtime_error);
	CHECK_THROWS_AS(backend->openOutput("no such port"), std::runtime_error);
}
#endif

#ifdef __APPLE__
// Opens every CoreMIDI port of the machine and loops back through virtual ports it creates itself, and through the
// IAC driver's first bus when that is enabled.
TEST_CASE("the CoreMIDI backend keeps the port contract and notices virtual ports coming and going", "[integration]")
{
	const std::unique_ptr<Backend> backend = makeCoreMidiBackend();
	CHECK(backend->name() == "coremidi");
	std::atomic<int> changes{0};
	backend->onPortsChanged([&changes] { ++changes; });
	const std::string name = "GNUbase test loopback";
	CHECK(!hasPort(*backend, name));
	{
		const VirtualLoopback loopback(name.c_str());
		REQUIRE(eventually([&] { return hasPort(*backend, name); }));
		checkContract(*backend, name, name);
	}
	CHECK(eventually([&] { return !hasPort(*backend, name); }));
	CHECK(eventually([&changes] { return changes > 0; })); // notifications come after the listing changed

	const std::string iac = "IAC Driver Bus 1";
	if (hasPort(*backend, iac))
		checkContract(*backend, iac, iac);
	else
		WARN("the IAC driver is off: its loopback isn't checked");
}

TEST_CASE("the CoreMIDI backend receives MIDI 2.0 sources as MIDI 1.0 messages", "[integration]")
{
	const std::unique_ptr<Backend> backend = makeCoreMidiBackend();
	const CFStringRef text = CFSTR("GNUbase test MIDI 2.0");
	MIDIClientRef client = 0;
	MIDIEndpointRef source = 0;
	REQUIRE(MIDIClientCreateWithBlock(text, &client, nullptr) == noErr);
	REQUIRE(MIDISourceCreateWithProtocol(client, text, kMIDIProtocol_2_0, &source) == noErr);
	const auto ports = backend->ports();
	const auto port = std::find_if(ports.begin(), ports.end(),
		[](const PortInfo& info) { return info.input && info.name == "GNUbase test MIDI 2.0"; });
	REQUIRE(port != ports.end());
	CHECK(port->midi2);

	Inbox inbox;
	const auto input = backend->openInput(port->id, inbox.receiver());
	// A MIDI 2.0 note on, group 0, channel 2, pitch 60, full velocity.
	const std::array<UInt32, 2> words{0x40913c00, 0xffff0000};
	MIDIEventList list{};
	MIDIEventListAdd(&list, sizeof(list), MIDIEventListInit(&list, kMIDIProtocol_2_0), 0, words.size(), words.data());
	REQUIRE(MIDIReceivedEventList(source, &list) == noErr);
	REQUIRE(inbox.waitFor(1));
	const MidiMessage received = inbox.messages()[0].first;
	CHECK(received.size == 3);
	CHECK(received.bytes == std::array<uint8_t, 3>{0x91, 60, 127});
	MIDIEndpointDispose(source);
}
#endif

TEST_CASE("MIDI status bytes tell how long their message is", "[unit]")
{
	CHECK(messageSize(0x90) == 3); // note on
	CHECK(messageSize(0xb5) == 3); // controller
	CHECK(messageSize(0xc0) == 2); // program change
	CHECK(messageSize(0xdf) == 2); // channel pressure
	CHECK(messageSize(0xe0) == 3); // pitch bend
	CHECK(messageSize(0xf1) == 2); // time code quarter frame
	CHECK(messageSize(0xf2) == 3); // song position
	CHECK(messageSize(0xf3) == 2); // song select
	CHECK(messageSize(0xf8) == 1); // clock
	CHECK(messageSize(0xfe) == 1); // active sensing
	CHECK(messageSize(0xf0) == 0); // SysEx
	CHECK(messageSize(0x40) == 0); // a data byte
}

TEST_CASE("the MIDI backends of this build come with the dummy backend last", "[unit]")
{
	const std::vector<std::unique_ptr<Backend>> backends = makeBackends();
	REQUIRE(!backends.empty());
	CHECK(backends.back()->name() == "dummy");
}

TEST_CASE("only inputs that belong to all MIDI inputs reach them", "[unit]")
{
	auto dummy = std::make_unique<DummyBackend>();
	DummyBackend& backend = *dummy;
	backend.addLoopback("Keys");
	backend.addLoopback("Pads");
	std::vector<std::unique_ptr<Backend>> backends;
	backends.push_back(std::move(dummy));
	gnubase::engine::MidiPorts ports(std::move(backends));
	CHECK(ports.refresh());
	CHECK(!ports.refresh()); // nothing changed
	ports.setPort({{"id", "Pads"}, {"input", true}, {"inAllInputs", false}});

	const auto keys = backend.openOutput("Keys");
	const auto pads = backend.openOutput("Pads");
	const HostTime now = hostTime();
	keys->send(noteOn(60), now - g_ms);
	pads->send(noteOn(61), now - g_ms);
	MidiBuffer midiIn(8);
	MidiBuffer midiOut(8);
	MidiBuffer allInputs(8);
	const DeviceBlock block{nullptr, nullptr, 480, 1000, now, midiIn, midiOut};
	ports.take(block, 48000.0, allInputs);
	REQUIRE(allInputs.size() == 1);
	CHECK(allInputs[0].bytes[1] == 60);
	CHECK(allInputs[0].offset == 432); // 9 ms into the 10 ms before the block
}
