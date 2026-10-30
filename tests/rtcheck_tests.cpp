// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/transport.h"
#include "engine/audiofile/diskstream.h"
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "engine/plugins/hostmanager.h"
#include "engine/plugins/pluginslot.h"
#include "engine/processors/builtin.h"
#include "engine/processors/builtinplugin.h"
#include "ipc/connection.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"
#include "platform/rt/snapshotpublisher.h"
#include "platform/rt/workerpool.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"
#include "plugin/remote/proxy.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef GNUBASE_PLUGINHOST
#define GNUBASE_PLUGINHOST "gnubase-pluginhost"
#endif

// gnubase-tests links the RT checker (gnubase-rtcheck), which reports allocations made in real-time code.
using namespace gnubase::rt;

namespace
{
	std::atomic<int> g_violations{0};

	// Optimizers may remove allocations whose memory is never used; publishing the pointer keeps them.
	void* volatile g_keepAlive = nullptr;

	void countViolation(const char* /*what*/) noexcept
	{
		++g_violations;
	}

	// Counts violations instead of aborting, for the lifetime of the object.
	class RecordViolations
	{
	public:
		RecordViolations() : m_previous(setViolationHandler(&countViolation)) { g_violations = 0; }
		~RecordViolations() { setViolationHandler(m_previous); }
		RecordViolations(const RecordViolations&) = delete;
		RecordViolations& operator=(const RecordViolations&) = delete;

	private:
		ViolationHandler m_previous;
	};
}

TEST_CASE("the RT checker reports an allocation in real-time code", "[unit]")
{
	const RecordViolations record;
	std::vector<int> values;
	{
		const ScopedRealtimeContext realtime;
		values.reserve(16);
		g_keepAlive = values.data();
	}
	CHECK(g_violations == 1);
}

TEST_CASE("the RT checker reports a deallocation in real-time code", "[unit]")
{
	const RecordViolations record;
	auto value = std::make_unique<int>(42);
	g_keepAlive = value.get();
	{
		const ScopedRealtimeContext realtime;
		value.reset();
	}
	CHECK(g_violations == 1);
}

#if (defined(_MSC_VER) && defined(_DEBUG)) || defined(GNUBASE_RTCHECK_MALLOC)
// The RT checker sees the C heap too, for malloc in third-party code such as Rubber Band Library: MSVC debug builds,
// and Linux (not with sanitizers).
// Called through volatile pointers: compilers know the C heap functions touch no memory of the program, so they may
// move direct calls out of a real-time scope.
namespace
{
	void* (*volatile g_malloc)(std::size_t) = std::malloc;
	void* (*volatile g_realloc)(void*, std::size_t) = std::realloc;
	void* (*volatile g_calloc)(std::size_t, std::size_t) = std::calloc;
	void (*volatile g_free)(void*) = std::free;
}

TEST_CASE("the RT checker reports malloc and free in real-time code", "[unit]")
{
	const RecordViolations record;
	void* memory = nullptr;
	{
		const ScopedRealtimeContext realtime;
		memory = g_malloc(64);
		g_keepAlive = memory;
	}
	CHECK(g_violations == 1);
	void* grown = nullptr;
	{
		const ScopedRealtimeContext realtime;
		grown = g_realloc(memory, 128);
		g_keepAlive = grown;
	}
	CHECK(g_violations == 2);
	REQUIRE(grown != nullptr);
	memory = grown;
	{
		const ScopedRealtimeContext realtime;
		g_free(memory);
		g_free(nullptr); // frees nothing
	}
	CHECK(g_violations == 3);
	{
		const ScopedRealtimeContext realtime;
		memory = g_calloc(4, 16);
		g_keepAlive = memory;
	}
	CHECK(g_violations == 4);
	g_free(memory);
	// operator new reports once, not again for the malloc inside it.
	{
		const ScopedRealtimeContext realtime;
		const auto value = std::make_unique<int>(42);
		g_keepAlive = value.get();
	}
	CHECK(g_violations == 6);
	// Outside real-time code, all of them are fine.
	memory = g_malloc(64);
	g_keepAlive = memory;
	g_free(memory);
	CHECK(g_violations == 6);
}
#endif

#ifdef GNUBASE_RTCHECK_MALLOC
TEST_CASE("the RT checker reports aligned C allocations in real-time code", "[unit]")
{
	const RecordViolations record;
	void* memory = nullptr;
	int result = -1;
	{
		const ScopedRealtimeContext realtime;
		result = posix_memalign(&memory, 64, 256); // Rubber Band Library allocates this way on Linux
		g_keepAlive = memory;
	}
	REQUIRE(result == 0);
	CHECK(g_violations == 1);
	CHECK(reinterpret_cast<std::uintptr_t>(memory) % 64 == 0);
	{
		const ScopedRealtimeContext realtime;
		std::free(memory);
	}
	CHECK(g_violations == 2);
}
#endif

TEST_CASE("allocations outside real-time code are fine", "[unit]")
{
	const RecordViolations record;
	const auto value = std::make_unique<int>(42);
	CHECK(*value == 42);
	CHECK(g_violations == 0);
}

TEST_CASE("assertNotRealtime reports only inside real-time code", "[unit]")
{
	const RecordViolations record;
	assertNotRealtime("blocking call");
	CHECK(g_violations == 0);
	{
		const ScopedRealtimeContext realtime;
		assertNotRealtime("blocking call");
	}
	CHECK(g_violations == 1);
}

TEST_CASE("real-time contexts nest", "[unit]")
{
	const RecordViolations record;
	std::vector<int> values;
	{
		const ScopedRealtimeContext outer;
		{
			const ScopedRealtimeContext inner;
		}
		CHECK(isRealtimeContext());
		values.reserve(16);
		g_keepAlive = values.data();
	}
	CHECK_FALSE(isRealtimeContext());
	CHECK(g_violations == 1);
}

TEST_CASE("logging from real-time code passes the RT checker", "[unit]")
{
	const gnubase::test::TempDir dir;
	const gnubase::logging::Session session(dir.path() / "test.log");
	{
		const RecordViolations violations;
		{
			const ScopedRealtimeContext realtime;
			gnubase::logging::info("block {} took {} ms in {} ({})", 42, 0.75, "Delay", "true");
		}
		CHECK(g_violations == 0);
	}
	gnubase::logging::flush();
	CHECK_THAT(gnubase::test::readText(dir.path() / "test.log"),
		Catch::Matchers::ContainsSubstring("block 42 took 0.75 ms in Delay (true)"));
}

TEST_CASE("SnapshotPublisher readers never allocate or free", "[unit]")
{
	SnapshotPublisher<std::vector<int>> publisher(std::make_unique<std::vector<int>>(64, 1));
	const RecordViolations record;
	publisher.publish(std::make_unique<std::vector<int>>(64, 2));
	int seen = 0;
	{
		const ScopedRealtimeContext realtime;
		seen = publisher.acquire().back();
		publisher.release();
	}
	publisher.publish(std::make_unique<std::vector<int>>(64, 3)); // frees the others outside real-time code
	CHECK(seen == 2);
	CHECK(g_violations == 0);
}

TEST_CASE("processing a processing graph and swapping in a new one never allocate or free", "[unit]")
{
	using namespace gnubase::engine::graph;
	// Every node kind, audio and event connections, and a processor that writes events.
	class Echo : public Processor
	{
	public:
		void process(const ProcessContext& context) noexcept override
		{
			for (const ProcessEvent& event : context.eventsIn)
				context.eventsOut.add(event);
		}
	};
	GraphDesc desc;
	desc.nodes = {{NodeKind::InputBus, 1, gnubase::engine::ChannelFormat::Stereo, nullptr},
		{NodeKind::Channel, 2, gnubase::engine::ChannelFormat::None, nullptr},
		{NodeKind::PluginSlot, 0, gnubase::engine::ChannelFormat::Stereo, std::make_shared<Echo>()},
		{NodeKind::Builtin, 0, gnubase::engine::ChannelFormat::Mono, std::make_shared<Echo>()},
		{NodeKind::OutputBus, 3, gnubase::engine::ChannelFormat::Stereo, nullptr}};
	desc.audio = {{0, 2}, {2, 3}, {2, 4}, {3, 4}};
	desc.events = {{1, 2}, {2, 3}};
	SnapshotPublisher<CompiledGraph> graphs(std::make_unique<CompiledGraph>(desc, 48000.0, 256));

	const RecordViolations record;
	const auto cycle = [&graphs]
	{
		const ScopedRealtimeContext realtime;
		const CompiledGraph& graph = graphs.acquire();
		ProcessEvent event;
		for (uint32_t offset = 0; offset < 256; offset += 16)
		{
			event.offset = offset;
			graph.events(1).add(event);
		}
		graph.process(256);
		graphs.release();
	};
	cycle();
	graphs.publish(std::make_unique<CompiledGraph>(desc, 48000.0, 256)); // built outside real-time code
	cycle();
	cycle();
	CHECK(g_violations == 0);
}

TEST_CASE("processing a graph on a worker pool never allocates or frees, on any thread", "[unit]")
{
	using namespace gnubase::engine::graph;
	using namespace gnubase::engine::processors;
	GraphDesc desc;
	for (int i = 0; i < 16; ++i)
		desc.nodes.push_back({NodeKind::Builtin, 0, gnubase::engine::ChannelFormat::Stereo,
			createBuiltin(i % 2 == 0 ? "testSynth" : "noise")});
	desc.nodes.push_back({NodeKind::Channel, 0, gnubase::engine::ChannelFormat::Stereo, nullptr});
	desc.nodes.push_back({NodeKind::OutputBus, 0, gnubase::engine::ChannelFormat::Stereo, nullptr});
	for (int i = 0; i < 16; ++i)
		desc.audio.push_back({i, i < 8 ? 16 : 17});
	desc.audio.push_back({16, 17});
	WorkerPool pool(3);
	const CompiledGraph graph(desc, 48000.0, 256);

	const RecordViolations record;
	for (int block = 0; block < 50; ++block)
	{
		ProcessEvent note;
		note.type = ProcessEvent::Type::NoteOn;
		note.key = static_cast<uint8_t>(40 + block);
		note.velocity = 0x8000;
		graph.events(0).add(note);
		const ScopedRealtimeContext realtime;
		graph.process(256, TransportBlock{}, &pool);
	}
	CHECK(g_violations == 0);
}

TEST_CASE("the transport applies requests and computes cycle segments without allocating", "[unit]")
{
	using namespace gnubase::engine;
	Transport transport(256);
	TransportSettings settings;
	settings.left = 100;
	settings.right = 101; // a jump every sample: the most segments a block can have
	settings.cycle = true;
	settings.returnToStart = true;
	REQUIRE(transport.set(settings));
	REQUIRE(transport.locate(100));
	REQUIRE(transport.play());

	const RecordViolations record;
	size_t segments = 0;
	for (int block = 0; block < 4; ++block)
	{
		const ScopedRealtimeContext realtime;
		segments = transport.advance(256).segmentCount;
	}
	CHECK(segments == 256);
	REQUIRE(transport.stop());
	{
		const ScopedRealtimeContext realtime;
		transport.advance(256);
	}
	CHECK(g_violations == 0);
}

TEST_CASE("every built-in processor processes notes and parameter changes without allocating", "[unit]")
{
	using namespace gnubase::engine::graph;
	using namespace gnubase::engine::processors;
	constexpr uint32_t frames = 256;
	std::vector<float> samples(size_t{2} * frames);
	float* const channels[] = {samples.data(), samples.data() + frames};
	EventBuffer eventsIn(64);
	EventBuffer eventsOut(64);

	const RecordViolations record;
	for (const BuiltinInfo& info : builtins())
	{
		const std::shared_ptr<Processor> processor = createBuiltin(info.id);
		processor->prepareFor(48000.0, frames);
		for (int block = 0; block < 4; ++block)
		{
			eventsIn.clear();
			for (const ParameterInfo& parameter : info.parameters)
			{
				ProcessEvent event;
				event.type = ProcessEvent::Type::Parameter;
				event.offset = 10;
				event.parameter = parameter.id;
				event.parameterValue = parameter.maximum;
				eventsIn.add(event);
			}
			ProcessEvent note;
			note.type = block < 2 ? ProcessEvent::Type::NoteOn : ProcessEvent::Type::NoteOff;
			note.offset = 20;
			note.key = 60;
			note.velocity = 0x8000;
			eventsIn.add(note);
			const ScopedRealtimeContext realtime;
			processor->process({channels, 2, frames, eventsIn, eventsOut, TransportBlock{}});
		}
	}
	CHECK(g_violations == 0);
}

TEST_CASE("every built-in processor processes signals, parameter sweeps and resets without allocating", "[unit]")
{
	using namespace gnubase::engine::graph;
	using namespace gnubase::engine::processors;
	constexpr uint32_t frames = 256;
	EventBuffer eventsIn(256);
	EventBuffer eventsOut(64);
	TransportBlock transport;
	transport.tempo = 97.0;

	const RecordViolations record;
	for (const int channels : {1, 2, 6})
	{
		std::vector<float> samples(static_cast<size_t>(channels) * frames);
		std::vector<float*> pointers;
		pointers.reserve(static_cast<size_t>(channels));
		for (int c = 0; c < channels; ++c)
			pointers.push_back(samples.data() + (static_cast<size_t>(c) * frames));
		for (const BuiltinInfo& info : builtins())
		{
			const std::shared_ptr<Processor> processor = createBuiltin(info.id);
			processor->prepareFor(48000.0, frames);
			for (int block = 0; block < 6; ++block)
			{
				// A loud signal, every parameter at its minimum, maximum and default in turn, and a reset.
				for (size_t i = 0; i < samples.size(); ++i)
					samples[i] = (i % 7 == 0 ? 3.0f : -0.5f) * static_cast<float>(block % 3);
				eventsIn.clear();
				for (const ParameterInfo& parameter : info.parameters)
				{
					ProcessEvent event;
					event.type = ProcessEvent::Type::Parameter;
					event.offset = static_cast<uint32_t>(block * 40);
					event.parameter = parameter.id;
					event.parameterValue = parameter.defaultValue;
					if (block % 3 == 0)
						event.parameterValue = parameter.minimum;
					else if (block % 3 == 1)
						event.parameterValue = parameter.maximum;
					eventsIn.add(event);
				}
				const ScopedRealtimeContext realtime;
				processor->process({pointers.data(), channels, frames, eventsIn, eventsOut, transport});
				if (block == 3)
					(*processor).reset();
			}
		}
	}
	CHECK(g_violations == 0);
}

TEST_CASE("the built-in plugins process, flush parameters and reset without allocating", "[unit]")
{
	using namespace gnubase;
	using namespace gnubase::plugin;
	class NullHost final : public Host
	{
	public:
		void requestRestart() override {}
		void requestProcess() override {}
		void requestCallback() override {}
		void* extension(std::string_view /*id*/) override { return nullptr; }
	};
	constexpr uint32_t frames = 128;
	std::vector<float> in(size_t{2} * frames, 0.25f);
	std::vector<float> out(size_t{2} * frames, 0.0f);
	float* const inputs[] = {in.data(), in.data() + frames};
	float* const outputs[] = {out.data(), out.data() + frames};
	EventList eventsIn(64);
	EventList eventsOut(64);
	TransportSegment segment;
	segment.transport.flags = Transport::Playing | Transport::TempoValid;

	NullHost host;
	engine::processors::BuiltinFactory factory;
	const RecordViolations record;
	for (size_t i = 0; i < factory.count(); ++i)
	{
		const std::unique_ptr<Plugin> instance = factory.create(factory.descriptor(i).id, host);
		REQUIRE(instance->init());
		REQUIRE(instance->activate(48000.0, 1, frames));
		auto* params = extension<Params>(*instance);
		const bool instrument = extension<AudioPorts>(*instance)->audioPortCount(true) == 0;
		REQUIRE(instance->startProcessing());
		for (int block = 0; block < 4; ++block)
		{
			eventsIn.clear();
			ParamInfo info;
			for (uint32_t p = 0; p < params->paramCount(); ++p)
				if (params->paramInfo(p, info))
				{
					Event event;
					event.offset = static_cast<uint32_t>(block * 8);
					event.type = EventType::ParamValue;
					event.param = info.id;
					event.value = block % 2 == 0 ? info.maximum : info.defaultValue;
					eventsIn.push(event);
				}
			const AudioBuffer input{inputs, 2, 0};
			AudioBuffer output{outputs, 2, 0};
			// The event lists are references, so they come first; the rest is set by name.
			Process process{0, -1, ProcessMode::Realtime, nullptr, 0, nullptr, 0, eventsIn, eventsOut, nullptr, 0};
			process.frames = frames;
			process.steadyTime = int64_t{block} * frames;
			process.audioInputs = &input;
			process.audioInputCount = instrument ? 0U : 1U;
			process.audioOutputs = &output;
			process.audioOutputCount = 1;
			process.transport = &segment;
			process.transportCount = 1;
			const ScopedRealtimeContext realtime;
			CHECK(instance->process(process) == ProcessStatus::Continue);
			eventsOut.clear();
			params->flushParams(eventsIn, eventsOut);
			(*instance).reset();
		}
		instance->stopProcessing();
		instance->deactivate();
	}
	CHECK(g_violations == 0);
}

TEST_CASE("rendering a project through the dummy device passes the RT checker", "[unit]")
{
	using gnubase::engine::Json;
	gnubase::engine::Engine engine;
	gnubase::engine::Client client(engine);
	const auto run = [&client](const std::string& command, const Json& parameters)
	{
		const gnubase::engine::Result result = client.call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
	};
	run("track.add", {{"type", "group"}});
	run("track.add", {{"type", "audio"}});
	run("track.add", {{"type", "audio"}, {"format", "mono"}}); // formats converted where they reach the output
	run("track.add", {{"type", "audio"}, {"format", "5.1"}});
	run("audio.setDevice", {{"blockSize", 64}, {"captureSeconds", 1}, {"input", {{"processor", "noise"}}}});

	const RecordViolations record;
	run("audio.render", {{"frames", 4800}});
	run("track.add", {{"type", "audio"}}); // a new graph, swapped in between blocks
	run("audio.render", {{"frames", 4800}});
	CHECK(g_violations == 0);
}

TEST_CASE("playing audio events, with overlaps and cycle jumps, passes the RT checker", "[integration]")
{
	using gnubase::engine::Json;
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "clip.wav").u8string();
	gnubase::test::Audio audio;
	audio.channels = 2;
	audio.samples.assign(size_t{2} * 20000, 0.25f);
	gnubase::test::writeWav(path, audio);

	gnubase::engine::Engine engine;
	gnubase::engine::Client client(engine);
	const auto run = [&client](const std::string& command, const Json& parameters)
	{
		const gnubase::engine::Result result = client.call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
		return result.value;
	};
	const Json track = run("track.add", {{"type", "audio"}, {"timeBase", "linear"}});
	const Json media = run("media.add", {{"path", path}});
	const Json clip = run("clip.add", {{"media", media.at("media").at("id")}});
	for (const int position : {0, 700, 1500})
		run("audioEvent.add",
			{{"track", track.at("track").at("id")}, {"clip", clip.at("clip").at("id")}, {"position", position},
				{"length", 1000}});
	run("audio.setDevice", {{"blockSize", 64}, {"captureSeconds", 1}});
	run("transport.set", {{"left", 500}, {"right", 2000}, {"cycle", true}});
	run("transport.play", Json::object());

	const RecordViolations record;
	run("audio.render", {{"frames", 9600}});
	CHECK(g_violations == 0);
}

TEST_CASE("playing fades, crossfades and auto fades passes the RT checker", "[integration]")
{
	using gnubase::engine::Json;
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "clip.wav").u8string();
	gnubase::test::Audio audio;
	audio.channels = 2;
	audio.samples.assign(size_t{2} * 20000, 0.25f);
	gnubase::test::writeWav(path, audio);

	gnubase::engine::Engine engine;
	gnubase::engine::Client client(engine);
	const auto run = [&client](const std::string& command, const Json& parameters)
	{
		const gnubase::engine::Result result = client.call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
		return result.value;
	};
	const Json track = run("track.add", {{"type", "audio"}, {"timeBase", "linear"}});
	const Json media = run("media.add", {{"path", path}});
	const Json clip = run("clip.add", {{"media", media.at("media").at("id")}});
	std::vector<Json> events;
	for (const int position : {0, 700, 1500, 3000})
		events.push_back(run("audioEvent.add",
			{{"track", track.at("track").at("id")}, {"clip", clip.at("clip").at("id")}, {"position", position},
				{"length", 1000}}));
	const auto id = [&events](size_t i) { return events[i].at("event").at("id"); };
	run("audioEvent.setFades", {{"event", id(0)}, {"fadeIn", {{"length", 300}, {"shape", "sCurve"}}}});
	run("crossfade.set", {{"from", id(0)}, {"to", id(1)}, {"mode", "equalPower"}});
	run("clip.setFades", {{"clip", clip.at("clip").at("id")}, {"fadeOut", {{"length", 5000}}}});
	run("autoFades.set", {{"fadeIn", true}, {"fadeOut", true}, {"crossfades", true}});
	run("audio.setDevice", {{"blockSize", 64}, {"captureSeconds", 1}});
	run("transport.set", {{"left", 500}, {"right", 3500}, {"cycle", true}});
	run("transport.play", Json::object());

	const RecordViolations record;
	run("audio.render", {{"frames", 9600}});
	CHECK(g_violations == 0);
}

TEST_CASE("the audio thread reads disk streams without allocating or locking", "[integration]")
{
	using namespace gnubase::engine;
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "stream.wav").u8string();
	{
		WriteOptions options;
		options.sampleRate = 44100.0; // played at 48 kHz, so the disk thread resamples too
		const auto writer = AudioFileWriter::create(path, options);
		const std::vector<float> silence(size_t{2} * 44100);
		writer->write(silence.data(), 44100);
		writer->close();
	}
	DiskStreamer streamer;
	const auto stream = streamer.open(AudioFileReader::open(path), 48000.0);
	for (int i = 0; i < 10000 && !stream->ready(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	REQUIRE(stream->ready());

	std::vector<float> buffer(size_t{2} * 64);
	const RecordViolations record;
	{
		const ScopedRealtimeContext realtime;
		for (int i = 0; i < 1000; ++i)
			stream->read(buffer.data(), 64);
	}
	CHECK(g_violations == 0);
}

TEST_CASE("playing MIDI parts through an instrument, with edits, stops and cycle jumps, passes the RT checker",
	"[integration]")
{
	using gnubase::engine::Json;
	gnubase::engine::Engine engine;
	gnubase::engine::Client client(engine);
	const auto run = [&client](const std::string& command, const Json& parameters)
	{
		const gnubase::engine::Result result = client.call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
		return result.value;
	};
	const auto channelOf = [&](const Json& track)
	{
		const Json project = run("project.get", Json::object());
		for (const Json& t : project.at("tracks"))
			if (t.at("id") == track.at("track").at("id"))
				return t.at("channel");
		return Json();
	};
	constexpr int64_t quarter = 1720320; // ticks per 24000 samples at 120 BPM and 48 kHz
	const Json instrument = run("track.add", {{"type", "instrument"}});
	const Json midi = run("track.add", {{"type", "midi"}});
	run("channel.set", {{"channel", channelOf(instrument)}, {"instrument", "testSynth"}});
	run("channel.set", {{"channel", channelOf(midi)}, {"output", channelOf(instrument)}});
	std::vector<Json> parts;
	for (const Json& track : {instrument, midi})
		parts.push_back(
			run("midiPart.add", {{"track", track.at("track").at("id")}, {"position", 0}, {"length", quarter}})
				.at("part")
				.at("id"));
	for (const Json& part : parts)
	{
		Json notes = Json::array();
		for (int i = 0; i < 16; ++i)
			notes.push_back(
				{{"position", i * quarter / 32}, {"length", quarter / 8}, {"pitch", 48 + i}, {"velocity", 0.5}});
		run("note.add", {{"part", part}, {"notes", notes}});
		run("controller.add",
			{{"part", part},
				{"controllers",
					{{{"position", 100}, {"number", 7}, {"value", 0.5}},
						{{"position", 200}, {"type", "pitchBend"}, {"value", 0.75}},
						{{"position", 300}, {"type", "channelPressure"}, {"value", 0.25}},
						{{"position", 400}, {"type", "polyPressure"}, {"number", 50}, {"value", 1.0}},
						{{"position", 500}, {"type", "program"}, {"number", 3}},
						{{"position", 600}, {"type", "sysEx"}, {"data", {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7}}}}}});
	}
	run("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	run("transport.set", {{"left", 1000}, {"right", 9000}, {"cycle", true}});
	run("transport.play", Json::object());

	const RecordViolations record;
	run("audio.render", {{"frames", 9600}});
	run("note.add", // a new sequence, swapped in between blocks
		{{"part", parts[1]},
			{"notes", {{{"position", 0}, {"length", quarter / 2}, {"pitch", 72}, {"velocity", 1.0}}}}});
	run("audio.render", {{"frames", 4800}});
	run("transport.locate", {{"position", 3000}}); // a locate ends the sounding notes and chases the held ones
	run("audio.render", {{"frames", 4800}});
	run("transport.stop", Json::object());
	run("audio.render", {{"frames", 960}});
	run("settings.set", {{"key", "midi.chaseBeyondParts"}, {"value", false}});
	run("transport.locate", {{"position", 2000}}); // chases the controllers while stopped
	run("audio.render", {{"frames", 960}});
	run("transport.play", Json::object());
	run("audio.render", {{"frames", 960}});
	CHECK(g_violations == 0);
}

TEST_CASE(
	"playing automation of volume, pan, instrument parameters and controllers passes the RT checker", "[integration]")
{
	using gnubase::engine::Json;
	gnubase::engine::Engine engine;
	gnubase::engine::Client client(engine);
	const auto run = [&client](const std::string& command, const Json& parameters)
	{
		const gnubase::engine::Result result = client.call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
		return result.value;
	};
	const auto points = [](std::initializer_list<std::pair<int64_t, double>> list, const char* shape)
	{
		Json result = Json::array();
		for (const auto& [position, value] : list) // samples, in ticks at 120 BPM and 48 kHz
			result.push_back({{"position", position * 1792 / 25}, {"value", value}, {"shape", shape}});
		return result;
	};
	const auto lane = [&run](const Json& track, const Json& target)
	{
		return run("automationLane.add", {{"track", track.at("track").at("id")}, {"target", target}})
			.at("lane")
			.at("id");
	};
	run("audio.setDevice",
		{{"blockSize", 64}, {"captureSeconds", 1},
			{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 441.0}, {"Level", 0.5}}}}}});
	const Json audio = run("track.add", {{"type", "audio"}});
	const Json instrument = run("track.add", {{"type", "instrument"}});
	run("channel.set", {{"channel", instrument.at("channel").at("id")}, {"instrument", "testSynth"}});
	run("automationPoint.add",
		{{"lane", lane(audio, {{"type", "volume"}})},
			{"points", points({{0, -30.0}, {2000, 0.0}, {5000, -6.0}, {9000, -60.0}}, "linear")}});
	run("automationPoint.add",
		{{"lane", lane(audio, {{"type", "pan"}})}, {"points", points({{0, -1.0}, {4000, 1.0}, {8000, 0.0}}, "curve")}});
	run("automationPoint.add",
		{{"lane", lane(instrument, {{"type", "instrument"}, {"parameter", 0}})},
			{"points", points({{0, 0.0}, {3000, 1.0}, {6000, 0.5}}, "linear")}});
	const Json controller = lane(instrument, {{"type", "controller"}, {"number", 7}, {"channel", 0}});
	run("automationPoint.add",
		{{"lane", controller}, {"points", points({{0, 0.0}, {1000, 1.0}, {7000, 0.25}}, "curve")}});
	run("transport.set", {{"left", 1000}, {"right", 9000}, {"cycle", true}});
	run("transport.play", Json::object());

	const RecordViolations record;
	run("audio.render", {{"frames", 9600}});
	run("automationPoint.add", // new curves, swapped in between blocks
		{{"lane", controller}, {"points", points({{1500, 0.5}, {2500, 0.0}}, "step")}});
	run("audio.render", {{"frames", 4800}});
	run("transport.locate", {{"position", 3000}}); // mid-ramp
	run("audio.render", {{"frames", 4800}});
	run("transport.stop", Json::object());
	run("audio.render", {{"frames", 960}});
	run("transport.locate", {{"position", 5000}}); // while stopped
	run("audio.render", {{"frames", 960}});
	CHECK(g_violations == 0);
}

TEST_CASE("sending blocks to a plugin host and waiting for its answers passes the RT checker", "[unit]")
{
	using namespace gnubase::ipc;
	Config config;
	config.maxChannels = 2;
	config.maxFrames = 64;
	config.eventRingSize = 1024;
	config.controlRingSize = 1024;
	Connection engine = Connection::create(config);
	Connection host = Connection::open(engine.name());
	static constexpr int blocks = 200;

	const RecordViolations record;
	// The host's audio thread answers every block, as a plugin host process would.
	std::thread hostThread(
		[&host]
		{
			const ScopedRealtimeContext realtime;
			std::array<std::byte, 64> event{};
			MessageHeader header;
			for (int i = 0; i < blocks; ++i)
			{
				AudioBlock input;
				AudioBlock output;
				if (host.waitForBlock(std::chrono::seconds(10)) != WaitResult::Ready ||
					host.receiveBlock(input) != ReadStatus::Ok ||
					host.receiveEvent(header, event.data(), event.size()) != ReadStatus::Ok || !host.beginBlock(output))
					return;
				output.sequence = input.sequence;
				output.frames = input.frames;
				output.channels = 1;
				output.channel(0)[0] = input.channel(0)[0];
				host.releaseBlock();
				host.sendBlock(output);
			}
		});
	int answered = 0;
	{
		const ScopedRealtimeContext realtime;
		for (int i = 0; i < blocks; ++i)
		{
			// No Catch2 assertions in here: they may allocate.
			AudioBlock block;
			if (!engine.beginBlock(block))
				break;
			block.sequence = static_cast<uint64_t>(i);
			block.frames = 64;
			block.channels = 2;
			block.channel(0)[0] = static_cast<float>(i);
			block.eventCount = 1;
			if (!engine.sendEvent(1, 1, &i, sizeof(i)))
				break;
			engine.sendBlock(block);
			AudioBlock answer;
			if (engine.waitForAnswer(static_cast<uint64_t>(i), std::chrono::seconds(10)) != WaitResult::Ready ||
				engine.receiveBlock(answer) != ReadStatus::Ok)
				break;
			if (answer.channel(0)[0] == static_cast<float>(i))
				++answered;
			engine.releaseBlock();
		}
	}
	hostThread.join();
	CHECK(answered == blocks);
	CHECK(g_violations == 0);
}

#ifndef GNUBASE_NO_TEST_PLUGINS
TEST_CASE("a plugin in a plugin host process processes blocks without allocating or locking", "[integration]")
{
	using namespace gnubase;
	engine::plugins::HostManager::Options options;
	options.executable = GNUBASE_PLUGINHOST;
	engine::plugins::HostManager manager(options);
	std::unique_ptr<plugin::remote::RemotePlugin> remote = manager.create({"test:gain", "", "GNUbase"});
	REQUIRE(remote->init());
	engine::plugins::PluginSlot slot(std::move(remote));
	slot.setProcessMode(plugin::ProcessMode::Offline);
	slot.prepareFor(48000.0, 128);

	std::vector<float> left(128, 0.5f);
	std::vector<float> right(128, -0.5f);
	const std::array<float*, 2> audio{left.data(), right.data()};
	engine::graph::EventBuffer eventsIn(4);
	engine::graph::EventBuffer eventsOut(4);
	const engine::graph::TransportBlock transport;
	engine::graph::ProcessEvent change;
	change.type = engine::graph::ProcessEvent::Type::Parameter;
	change.offset = 10;
	change.parameter = 1; // test:gain's Gain
	change.parameterValue = -6.0;
	eventsIn.add(change);

	const RecordViolations record;
	{
		const ScopedRealtimeContext realtime;
		for (int i = 0; i < 100; ++i)
		{
			slot.process({audio.data(), 2, 128, eventsIn, eventsOut, transport});
			eventsOut.clear();
		}
	}
	CHECK(g_violations == 0);
	CHECK(!slot.failed());
	CHECK(left[0] != 0.0f);
}

TEST_CASE("late blocks to a plugin host, and blocks it can't take yet, neither allocate nor lock", "[integration]")
{
	using namespace gnubase;
	engine::plugins::HostManager::Options options;
	options.executable = GNUBASE_PLUGINHOST;
	options.deadlines.realtimeMinimum = std::chrono::milliseconds(5);
	options.deadlines.realtimeMaximum = std::chrono::milliseconds(5);
	options.deadlines.hang = std::chrono::seconds(10);
	engine::plugins::HostManager manager(options);
	std::unique_ptr<plugin::remote::RemotePlugin> remote = manager.create({"test:stall", "", "GNUbase"});
	REQUIRE(remote->init());
	const plugin::remote::RemotePlugin& plugin = *remote;
	engine::plugins::PluginSlot slot(std::move(remote));
	slot.setProcessMode(plugin::ProcessMode::Realtime);
	slot.prepareFor(48000.0, 128);

	std::vector<float> left(128, 0.5f);
	std::vector<float> right(128, -0.5f);
	const std::array<float*, 2> audio{left.data(), right.data()};
	engine::graph::EventBuffer eventsIn(4);
	engine::graph::EventBuffer eventsOut(4);
	const engine::graph::TransportBlock transport;
	engine::graph::ProcessEvent stall;
	stall.type = engine::graph::ProcessEvent::Type::Parameter;
	stall.parameter = 1; // test:stall's Stall, in ms
	stall.parameterValue = 100.0;

	// The block that sets Stall is late, the next one too, and the ones after don't fit until it catches up.
	int failed = 0;
	const auto start = std::chrono::steady_clock::now();
	const RecordViolations record;
	{
		const ScopedRealtimeContext realtime;
		eventsIn.add(stall);
		while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(400))
		{
			std::fill(left.begin(), left.end(), 0.5f);
			std::fill(right.begin(), right.end(), -0.5f);
			slot.process({audio.data(), 2, 128, eventsIn, eventsOut, transport});
			failed += slot.failed() ? 1 : 0;
			eventsIn.clear();
			eventsOut.clear();
		}
	}
	CHECK(g_violations == 0);
	CHECK(failed >= 2);
	CHECK(!slot.failed());
	CHECK(left[0] == 0.5f);
	CHECK(plugin.failure() == plugin::remote::Failure::None);
}
#endif
