// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/transport.h"
#include "engine/audiofile/diskstream.h"
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "engine/processors/builtin.h"
#include "ipc/connection.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"
#include "platform/rt/snapshotpublisher.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
	desc.nodes = {{NodeKind::InputBus, 1, 2, nullptr}, {NodeKind::Channel, 2, 0, nullptr},
		{NodeKind::PluginSlot, 0, 2, std::make_shared<Echo>()}, {NodeKind::Builtin, 0, 1, std::make_shared<Echo>()},
		{NodeKind::OutputBus, 3, 2, nullptr}};
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
			if (engine.waitForBlock(std::chrono::seconds(10)) != WaitResult::Ready ||
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
