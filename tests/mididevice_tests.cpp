// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/midiports.h"
#include "engine/settings/usersettings.h"
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
#ifdef __linux__
#include "platform/audio/jack.h"
#include "platform/jacklibrary.h"
#include "platform/midi/alsaseq.h"
#include "platform/midi/jackmidi.h"

#include <alsa/asoundlib.h>
#include <poll.h>
#endif

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
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
		// Room for what tests send: receivers may run in real-time code, which doesn't allocate.
		Inbox() { m_messages.reserve(64); }

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
#endif

#if defined(__APPLE__) || defined(__linux__)
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

#ifdef __linux__
	bool required(const char* variable)
	{
		const char* value = std::getenv(variable);
		return value != nullptr && std::string(value) == "1";
	}

	// A sequencer client with a port that passes what it receives on to its subscribers, as the kernel's Midi Through
	// port does, without snd-seq-dummy.
	class SeqLoopback
	{
	public:
		explicit SeqLoopback(const char* name)
		{
			REQUIRE(snd_seq_open(&m_seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK) >= 0);
			snd_seq_set_client_name(m_seq, name);
			m_port = snd_seq_create_simple_port(m_seq, name,
				SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_WRITE |
					SND_SEQ_PORT_CAP_SUBS_WRITE,
				SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
			REQUIRE(m_port >= 0);
			m_thread = std::thread([this] { pass(); });
		}

		~SeqLoopback()
		{
			m_stopping = true;
			m_thread.join();
			snd_seq_close(m_seq);
		}

		SeqLoopback(const SeqLoopback&) = delete;
		SeqLoopback& operator=(const SeqLoopback&) = delete;

	private:
		void pass()
		{
			std::vector<pollfd> descriptors(static_cast<size_t>(snd_seq_poll_descriptors_count(m_seq, POLLIN)));
			snd_seq_poll_descriptors(m_seq, descriptors.data(), static_cast<unsigned>(descriptors.size()), POLLIN);
			while (!m_stopping)
			{
				poll(descriptors.data(), descriptors.size(), 10);
				snd_seq_event_t* event = nullptr;
				while (snd_seq_event_input(m_seq, &event) >= 0 && event != nullptr)
				{
					snd_seq_ev_set_source(event, m_port);
					snd_seq_ev_set_subs(event);
					snd_seq_ev_set_direct(event);
					snd_seq_event_output_direct(m_seq, event);
				}
			}
		}

		snd_seq_t* m_seq = nullptr;
		int m_port = 0;
		std::atomic<bool> m_stopping{false};
		std::thread m_thread;
	};

	// A JACK client "gnubase-test-midi" that passes what its MIDI input "in" receives on to its MIDI output "out", in
	// the same cycle.
	class JackLoopback
	{
	public:
		// Null if libjack is missing or no JACK server runs.
		static std::unique_ptr<JackLoopback> start()
		{
			const gnubase::JackLibrary* jack = nullptr;
			try
			{
				jack = &gnubase::JackLibrary::get("libjack.so.0");
			}
			catch (const std::exception&)
			{
				return nullptr;
			}
			jack_client_t* client = jack->clientOpen("gnubase-test-midi", JackNoStartServer, nullptr);
			return client != nullptr ? std::make_unique<JackLoopback>(*jack, client) : nullptr;
		}

		JackLoopback(const gnubase::JackLibrary& jack, jack_client_t* client) : m_jack(jack), m_client(client)
		{
			m_in = jack.portRegister(m_client, "in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
			m_out = jack.portRegister(m_client, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);
			REQUIRE(m_in != nullptr);
			REQUIRE(m_out != nullptr);
			jack.setProcessCallback(m_client, &JackLoopback::process, this);
			REQUIRE(jack.activate(m_client) == 0);
		}

		~JackLoopback() { m_jack.clientClose(m_client); } // removes its ports

		JackLoopback(const JackLoopback&) = delete;
		JackLoopback& operator=(const JackLoopback&) = delete;

	private:
		static int process(jack_nframes_t frames, void* data)
		{
			const auto& self = *static_cast<JackLoopback*>(data);
			void* in = self.m_jack.portGetBuffer(self.m_in, frames);
			void* out = self.m_jack.portGetBuffer(self.m_out, frames);
			self.m_jack.midiClearBuffer(out);
			const uint32_t count = self.m_jack.midiGetEventCount(in);
			for (uint32_t i = 0; i < count; ++i)
			{
				jack_midi_event_t event{};
				if (self.m_jack.midiEventGet(&event, in, i) == 0)
					self.m_jack.midiEventWrite(out, event.time, event.buffer, event.size);
			}
			return 0;
		}

		const gnubase::JackLibrary& m_jack;
		jack_client_t* m_client = nullptr;
		jack_port_t* m_in = nullptr;
		jack_port_t* m_out = nullptr;
	};
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
	backend->ports(); // the backend's client comes first, as in the application, so notifications reach it
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

#ifdef __linux__
// Loops back through a sequencer client the test creates itself, and through the kernel's Midi Through port when
// snd-seq-dummy is loaded. Skipped without a sequencer (/dev/snd/seq), as in WSL and containers, unless
// GNUBASE_REQUIRE_ALSA_SEQ=1: CI loads the sequencer's modules and requires it.
TEST_CASE("the ALSA sequencer backend keeps the port contract and notices ports coming and going", "[integration]")
{
	if (!std::filesystem::exists("/dev/snd/seq"))
	{
		if (required("GNUBASE_REQUIRE_ALSA_SEQ"))
			FAIL("there is no ALSA sequencer (/dev/snd/seq)");
		SKIP("there is no ALSA sequencer here (/dev/snd/seq): sudo modprobe snd-seq-dummy");
	}
	const std::unique_ptr<Backend> backend = makeAlsaSeqBackend();
	CHECK(backend->name() == "alsa");
	std::atomic<int> changes{0};
	backend->onPortsChanged([&changes] { ++changes; });
	const std::string name = "GNUbase test loopback";
	CHECK(!hasPort(*backend, name));
	{
		const SeqLoopback loopback(name.c_str());
		REQUIRE(eventually([&] { return hasPort(*backend, name); }));
		CHECK(eventually([&changes] { return changes > 0; }));
		const std::vector<PortInfo> ports = backend->ports();
		const auto port =
			std::find_if(ports.begin(), ports.end(), [&name](const PortInfo& info) { return info.name == name; });
		REQUIRE(port != ports.end());
		CHECK(port->id == name + ":" + name);
		checkContract(*backend, name, name);
	}
	CHECK(eventually([&] { return !hasPort(*backend, name); }));

	const std::string through = "Midi Through Port-0";
	if (hasPort(*backend, through))
		checkContract(*backend, through, through);
	else
		WARN("snd-seq-dummy isn't loaded: the Midi Through loopback isn't checked");
}

// Against jackd; skipped where no JACK server runs, unless GNUBASE_REQUIRE_JACK=1 (CI starts jackd with its dummy
// driver, tools/start-jack.sh).
TEST_CASE("the JACK MIDI backend keeps the port contract and notices ports coming and going", "[integration]")
{
	std::unique_ptr<JackLoopback> loopback = JackLoopback::start();
	if (!loopback)
	{
		if (required("GNUBASE_REQUIRE_JACK"))
			FAIL("no JACK server runs here");
		SKIP("no JACK server runs here");
	}
	const std::unique_ptr<gnubase::audio::Device> device = gnubase::audio::makeJackBackend()->open({});
	const std::unique_ptr<Backend> backend = makeJackMidiBackend();
	CHECK(backend->name() == "jack");
	std::atomic<int> changes{0};
	backend->onPortsChanged([&changes] { ++changes; });
	const std::string in = "gnubase-test-midi:in";
	const std::string out = "gnubase-test-midi:out";
	REQUIRE(hasPort(*backend, in));
	loopback.reset(); // before GNUbase's client has ports of its own, which would report changes too
	CHECK(eventually([&] { return !hasPort(*backend, in); }));
	CHECK(eventually([&changes] { return changes > 0; }));

	loopback = JackLoopback::start();
	REQUIRE(loopback);
	REQUIRE(eventually([&] { return hasPort(*backend, in) && hasPort(*backend, out); }));
	checkContract(*backend, in, out);
}

// Where libjack is PipeWire's, a server always answers, and its MIDI ports are the ALSA backend's too: JACK MIDI is on
// only while a JACK audio device is open.
TEST_CASE("the JACK MIDI backend has ports only while a JACK audio device is open", "[integration]")
{
	const std::unique_ptr<JackLoopback> loopback = JackLoopback::start();
	if (!loopback)
	{
		if (required("GNUBASE_REQUIRE_JACK"))
			FAIL("no JACK server runs here");
		SKIP("no JACK server runs here");
	}
	const std::unique_ptr<Backend> backend = makeJackMidiBackend();
	std::atomic<int> changes{0};
	backend->onPortsChanged([&changes] { ++changes; });
	const std::string in = "gnubase-test-midi:in";
	CHECK(backend->ports().empty());
	Inbox inbox;
	CHECK_THROWS_AS(backend->openInput(in, inbox.receiver()), std::runtime_error);
	CHECK_THROWS_AS(backend->openOutput(in), std::runtime_error);
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	CHECK(changes == 0);

	std::unique_ptr<gnubase::audio::Device> device = gnubase::audio::makeJackBackend()->open({});
	CHECK(hasPort(*backend, in));
	CHECK(eventually([&changes] { return changes > 0; }));

	const int before = changes;
	device.reset();
	CHECK(backend->ports().empty());
	CHECK(eventually([&] { return changes > before; }));

	// Reopening the device, as audio.setDevice and audio.reset do, keeps the client and its open ports, and the refresh
	// the change report causes runs once the device is open again: GNUbase's own ports never come back as listed ones.
	device = gnubase::audio::makeJackBackend()->open({});
	const auto input = backend->openInput("gnubase-test-midi:out", inbox.receiver());
	const auto output = backend->openOutput(in);
	std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the ports' registrations are reported
	const int opened = changes;
	device.reset();
	REQUIRE(eventually([&] { return changes > opened; }));
	std::this_thread::sleep_for(std::chrono::milliseconds(50)); // the watcher has seen no device open
	device = gnubase::audio::makeJackBackend()->open({});
	for (const PortInfo& port : backend->ports())
		CHECK(port.id.rfind("gnubase-midi", 0) != 0);
	output->send(noteOn(60), hostTime());
	CHECK(inbox.waitFor(1));
}

TEST_CASE("the JACK MIDI backend has no ports without libjack", "[unit]")
{
	const std::unique_ptr<Backend> backend = makeJackMidiBackend("libjack-that-isnt-there.so");
	CHECK(backend->ports().empty());
	Inbox inbox;
	CHECK_THROWS_AS(backend->openInput("system:midi_capture_1", inbox.receiver()), std::runtime_error);
	CHECK_THROWS_AS(backend->openOutput("system:midi_playback_1"), std::runtime_error);
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
#ifdef __linux__
	REQUIRE(backends.size() == 3);
	CHECK(backends[0]->name() == "alsa");
	CHECK(backends[1]->name() == "jack");
#endif
}

TEST_CASE("only inputs that belong to all MIDI inputs reach them", "[unit]")
{
	auto dummy = std::make_unique<DummyBackend>();
	DummyBackend& backend = *dummy;
	backend.addLoopback("Keys");
	backend.addLoopback("Pads");
	std::vector<std::unique_ptr<Backend>> backends;
	backends.push_back(std::move(dummy));
	gnubase::engine::settings::Store settings(gnubase::engine::settings::userFormat());
	settings.set("midi.portSettings", gnubase::engine::Json{{"dummy/in/Pads", {{"inAllInputs", false}}}});
	gnubase::engine::MidiPorts ports(settings, std::move(backends));
	CHECK(ports.refresh()); // a single listing opens the inputs with their all MIDI inputs flags

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

TEST_CASE("a hidden input that becomes visible reaches all MIDI inputs", "[unit]")
{
	auto dummy = std::make_unique<DummyBackend>();
	DummyBackend& backend = *dummy;
	backend.addLoopback("Keys");
	std::vector<std::unique_ptr<Backend>> backends;
	backends.push_back(std::move(dummy));
	gnubase::engine::settings::Store settings(gnubase::engine::settings::userFormat());
	settings.set("midi.portSettings", gnubase::engine::Json{{"dummy/in/Keys", {{"visible", false}}}});
	gnubase::engine::MidiPorts ports(settings, std::move(backends));
	CHECK(ports.refresh()); // lists the hidden input, doesn't open it

	settings.set("midi.portSettings", gnubase::engine::Json::object());
	ports.applySettings(); // opens the input for the first time, no further listing

	const auto keys = backend.openOutput("Keys");
	const HostTime now = hostTime();
	keys->send(noteOn(60), now - g_ms);
	MidiBuffer midiIn(8);
	MidiBuffer midiOut(8);
	MidiBuffer allInputs(8);
	const DeviceBlock block{nullptr, nullptr, 480, 1000, now, midiIn, midiOut};
	ports.take(block, 48000.0, allInputs);
	REQUIRE(allInputs.size() == 1);
	CHECK(allInputs[0].bytes[1] == 60);
}
