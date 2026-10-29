// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/graph/graph.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using namespace gnubase::engine::graph;
using Catch::Matchers::ContainsSubstring;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr uint32_t g_maxFrames = 64;

	// Writes the same value to every sample of every channel.
	class Constant : public Processor
	{
	public:
		explicit Constant(float value) : m_value(value) {}

		void process(const ProcessContext& context) noexcept override
		{
			for (int c = 0; c < context.channels; ++c)
				for (uint32_t i = 0; i < context.frames; ++i)
					context.audio[c][i] = m_value;
		}

	private:
		float m_value;
	};

	// Writes a 1 at the offset of every note-on and leaves the rest silent.
	class ImpulseOnNote : public Processor
	{
	public:
		void process(const ProcessContext& context) noexcept override
		{
			for (int c = 0; c < context.channels; ++c)
				std::memset(context.audio[c], 0, context.frames * sizeof(float));
			for (const ProcessEvent& event : context.eventsIn)
				if (event.type == ProcessEvent::Type::NoteOn)
					for (int c = 0; c < context.channels; ++c)
						context.audio[c][event.offset] = 1.0f;
		}
	};

	// Multiplies its input by parameter 0, which parameter events change from their offset on.
	class Gain : public Processor
	{
	public:
		void process(const ProcessContext& context) noexcept override
		{
			size_t next = 0;
			for (uint32_t i = 0; i < context.frames; ++i)
			{
				for (; next < context.eventsIn.size() && context.eventsIn[next].offset <= i; ++next)
				{
					const ProcessEvent& event = context.eventsIn[next];
					if (event.type == ProcessEvent::Type::Parameter && event.parameter == 0)
						m_gain = static_cast<float>(event.parameterValue);
				}
				for (int c = 0; c < context.channels; ++c)
					context.audio[c][i] *= m_gain;
			}
		}

	private:
		float m_gain = 1.0f;
	};

	// Counts up by one per sample, continuing where the last block stopped.
	class Ramp : public Processor
	{
	public:
		void process(const ProcessContext& context) noexcept override
		{
			for (uint32_t i = 0; i < context.frames; ++i)
			{
				for (int c = 0; c < context.channels; ++c)
					context.audio[c][i] = m_next;
				m_next += 1.0f;
			}
		}

		int prepared = 0;

	protected:
		void prepare(double /*sampleRate*/, uint32_t /*maxFrames*/) override { ++prepared; }

	private:
		float m_next = 0.0f;
	};

	// Passes audio through and reports a fixed latency.
	class Delayed : public Processor
	{
	public:
		explicit Delayed(uint32_t latency) : m_latency(latency) {}

		void process(const ProcessContext& /*context*/) noexcept override {}
		uint32_t latency() const override { return m_latency; }

	private:
		uint32_t m_latency;
	};

	ProcessEvent noteOn(uint32_t offset, uint8_t pitch = 60)
	{
		ProcessEvent event;
		event.offset = offset;
		event.type = ProcessEvent::Type::NoteOn;
		event.key = pitch;
		event.velocity = 0xffff;
		return event;
	}

	ProcessEvent parameter(uint32_t offset, uint32_t id, double value)
	{
		ProcessEvent event;
		event.offset = offset;
		event.type = ProcessEvent::Type::Parameter;
		event.parameter = id;
		event.parameterValue = value;
		return event;
	}

	NodeDesc node(NodeKind kind, int channels, std::shared_ptr<Processor> processor = nullptr, ObjectId object = 0)
	{
		return {kind, object, channels, std::move(processor)};
	}

	std::vector<float> channel(const CompiledGraph& graph, int node, int index, uint32_t frames)
	{
		const float* samples = graph.audio(node)[index];
		return {samples, samples + frames};
	}
}

TEST_CASE("every node is processed after the nodes it receives from, whatever the connection order", "[unit]")
{
	GraphDesc graph;
	for (int i = 0; i < 5; ++i)
		graph.nodes.push_back(node(NodeKind::Channel, 2));
	// 3 -> 1 -> 0, 4 -> 0, and 2 feeds events to 3.
	graph.audio = {{1, 0}, {4, 0}, {3, 1}};
	graph.events = {{2, 3}};
	CHECK(processingOrder(graph) == std::vector<int>{2, 3, 1, 4, 0});

	std::swap(graph.audio[0], graph.audio[2]);
	CHECK(processingOrder(graph) == std::vector<int>{2, 3, 1, 4, 0});
}

TEST_CASE("a processing graph with a cycle is rejected", "[unit]")
{
	GraphDesc graph;
	for (int i = 0; i < 3; ++i)
		graph.nodes.push_back(node(NodeKind::Channel, 2));
	graph.audio = {{0, 1}, {1, 2}};
	graph.events = {{2, 0}};
	CHECK_THROWS_WITH(processingOrder(graph), ContainsSubstring("cycle"));

	graph.events = {{0, 7}};
	CHECK_THROWS_WITH(processingOrder(graph), ContainsSubstring("no node 7"));
}

TEST_CASE("a node sums its inputs in a fixed order, so the output is bit-identical", "[unit]")
{
	// In float, (1e8 + 1) - 1e8 is 0 but (1e8 - 1e8) + 1 is 1: the order of the sum shows in the result.
	GraphDesc graph;
	graph.nodes.push_back(node(NodeKind::Builtin, 1, std::make_shared<Constant>(1e8f)));
	graph.nodes.push_back(node(NodeKind::Builtin, 1, std::make_shared<Constant>(1.0f)));
	graph.nodes.push_back(node(NodeKind::Builtin, 1, std::make_shared<Constant>(-1e8f)));
	graph.nodes.push_back(node(NodeKind::OutputBus, 1));
	graph.audio = {{2, 3}, {0, 3}, {1, 3}};

	const CompiledGraph first(graph, g_sampleRate, g_maxFrames);
	first.process(g_maxFrames);
	CHECK(channel(first, 3, 0, g_maxFrames) == std::vector<float>(g_maxFrames, 0.0f)); // 1e8 + 1 - 1e8

	graph.audio = {{1, 3}, {2, 3}, {0, 3}};
	const CompiledGraph second(graph, g_sampleRate, g_maxFrames);
	second.process(g_maxFrames);
	CHECK(channel(second, 3, 0, g_maxFrames) == channel(first, 3, 0, g_maxFrames));
}

TEST_CASE("a mono input feeds every channel of a node, other inputs map channel by channel", "[unit]")
{
	GraphDesc graph;
	graph.nodes.push_back(node(NodeKind::InputBus, 1));
	graph.nodes.push_back(node(NodeKind::InputBus, 2));
	graph.nodes.push_back(node(NodeKind::OutputBus, 2));
	graph.audio = {{0, 2}, {1, 2}};
	const CompiledGraph compiled(graph, g_sampleRate, g_maxFrames);

	constexpr uint32_t frames = 8;
	for (uint32_t i = 0; i < frames; ++i)
	{
		compiled.audio(0)[0][i] = 1.0f;
		compiled.audio(1)[0][i] = 10.0f;
		compiled.audio(1)[1][i] = 20.0f;
	}
	compiled.process(frames);
	CHECK(channel(compiled, 2, 0, frames) == std::vector<float>(frames, 11.0f));
	CHECK(channel(compiled, 2, 1, frames) == std::vector<float>(frames, 21.0f));
}

TEST_CASE("note events reach a processor at their sample offsets", "[unit]")
{
	GraphDesc graph;
	graph.nodes.push_back(node(NodeKind::Builtin, 2, std::make_shared<ImpulseOnNote>()));
	const CompiledGraph compiled(graph, g_sampleRate, g_maxFrames);

	// Added out of order: the buffer sorts them by offset.
	for (const uint32_t offset : {63u, 0u, 17u})
		REQUIRE(compiled.events(0).add(noteOn(offset)));
	compiled.process(g_maxFrames);

	std::vector<float> expected(g_maxFrames, 0.0f);
	expected[0] = expected[17] = expected[63] = 1.0f;
	CHECK(channel(compiled, 0, 0, g_maxFrames) == expected);
	CHECK(channel(compiled, 0, 1, g_maxFrames) == expected);

	// Events are used up by the block they were added for.
	compiled.process(g_maxFrames);
	CHECK(channel(compiled, 0, 0, g_maxFrames) == std::vector<float>(g_maxFrames, 0.0f));
}

TEST_CASE("event connections carry MIDI from a MIDI channel to an instrument", "[unit]")
{
	GraphDesc graph;
	graph.nodes.push_back(node(NodeKind::Channel, 0)); // MIDI track: no audio channels
	graph.nodes.push_back(node(NodeKind::PluginSlot, 2, std::make_shared<ImpulseOnNote>()));
	graph.nodes.push_back(node(NodeKind::OutputBus, 2));
	graph.events = {{0, 1}};
	graph.audio = {{1, 2}};
	const CompiledGraph compiled(graph, g_sampleRate, g_maxFrames);

	REQUIRE(compiled.events(0).add(noteOn(5)));
	REQUIRE(compiled.events(0).add(noteOn(40, 64)));
	compiled.process(g_maxFrames);

	std::vector<float> expected(g_maxFrames, 0.0f);
	expected[5] = expected[40] = 1.0f;
	CHECK(channel(compiled, 2, 0, g_maxFrames) == expected);
}

TEST_CASE("parameter events change a processor's parameter from their sample offset on", "[unit]")
{
	GraphDesc graph;
	graph.nodes.push_back(node(NodeKind::InputBus, 1));
	graph.nodes.push_back(node(NodeKind::Builtin, 1, std::make_shared<Gain>()));
	graph.audio = {{0, 1}};
	const CompiledGraph compiled(graph, g_sampleRate, g_maxFrames);

	constexpr uint32_t frames = 16;
	for (uint32_t i = 0; i < frames; ++i)
		compiled.audio(0)[0][i] = 1.0f;
	REQUIRE(compiled.events(1).add(parameter(10, 0, 0.5)));
	compiled.process(frames);

	std::vector<float> expected(frames, 1.0f);
	for (uint32_t i = 10; i < frames; ++i)
		expected[i] = 0.5f;
	CHECK(channel(compiled, 1, 0, frames) == expected);
}

TEST_CASE("an event buffer keeps events in offset order and drops what doesn't fit", "[unit]")
{
	EventBuffer events(3);
	CHECK(events.add(noteOn(8, 1)));
	CHECK(events.add(noteOn(2, 2)));
	CHECK(events.add(noteOn(8, 3))); // same offset: after the earlier one
	CHECK_FALSE(events.add(noteOn(0, 4)));

	REQUIRE(events.size() == 3);
	CHECK(events[0].key == 2);
	CHECK(events[1].key == 1);
	CHECK(events[2].key == 3);
	events.clear();
	CHECK(events.empty());
	CHECK(events.capacity() == 3);
}

namespace
{
	ProcessEvent event(uint32_t offset, ProcessEvent::Type type, uint8_t key)
	{
		ProcessEvent result;
		result.offset = offset;
		result.type = type;
		result.key = key;
		return result;
	}

	std::vector<int> keys(const EventBuffer& events)
	{
		std::vector<int> result;
		for (const ProcessEvent& e : events)
			result.push_back(e.key);
		return result;
	}
}

TEST_CASE("at one offset an event buffer puts note-offs first, then other events, then note-ons", "[unit]")
{
	// Whichever source adds its events first, e.g. a MIDI track whose note starts where the instrument track's own
	// note of the same pitch ends: the note-off must not end the new note. Here every event comes from a source of
	// its own.
	using Type = ProcessEvent::Type;
	EventBuffer events(8);
	for (const ProcessEvent& e : {event(4, Type::NoteOn, 1), event(4, Type::Controller, 2), event(4, Type::NoteOff, 3),
			 event(2, Type::NoteOn, 4), event(4, Type::NoteOff, 5), event(4, Type::Parameter, 6),
			 event(4, Type::NoteOn, 7), event(6, Type::NoteOff, 8)})
	{
		events.nextSource();
		CHECK(events.add(e));
	}
	CHECK(keys(events) == std::vector<int>{4, 3, 5, 2, 6, 1, 7, 8});
}

TEST_CASE("an event buffer keeps the order of one source's events at one offset", "[unit]")
{
	// A note of no length, a note-on followed by pressure on it, from live MIDI input or a plugin. Another source's
	// note-off still goes before the note-ons.
	using Type = ProcessEvent::Type;
	EventBuffer events(8);
	for (const ProcessEvent& e : {event(4, Type::NoteOn, 1), event(4, Type::NoteOff, 1), event(4, Type::NoteOn, 2),
			 event(4, Type::PolyPressure, 2), event(6, Type::NoteOn, 3), event(6, Type::NoteOff, 3)})
		CHECK(events.add(e));
	events.nextSource();
	CHECK(events.add(event(4, Type::NoteOff, 2)));
	CHECK(keys(events) == std::vector<int>{2, 1, 1, 2, 2, 3, 3});
	CHECK(events[0].type == Type::NoteOff);
	CHECK(events[1].type == Type::NoteOn);
	CHECK(events[2].type == Type::NoteOff);
}

TEST_CASE("merging into a full event buffer drops other events before note-offs", "[unit]")
{
	using Type = ProcessEvent::Type;
	EventBuffer events(3);
	for (const ProcessEvent& e : {event(1, Type::NoteOn, 1), event(2, Type::Controller, 2), event(3, Type::NoteOff, 3)})
		REQUIRE(events.add(e));
	EventBuffer source(3);
	for (const ProcessEvent& e : {event(0, Type::NoteOff, 4), event(0, Type::NoteOn, 5), event(4, Type::NoteOff, 6)})
		REQUIRE(source.add(e));
	// The note-offs take the places of the latest other events; the note-on finds no room.
	events.merge(source);
	CHECK(keys(events) == std::vector<int>{4, 3, 6});
	// A buffer of note-offs keeps them.
	source.clear();
	REQUIRE(source.add(event(0, Type::NoteOff, 7)));
	events.merge(source);
	CHECK(keys(events) == std::vector<int>{4, 3, 6});
}

TEST_CASE("processors keep their state when the graph is rebuilt", "[unit]")
{
	auto ramp = std::make_shared<Ramp>();
	GraphDesc graph;
	graph.nodes.push_back(node(NodeKind::Builtin, 1, ramp));
	const CompiledGraph before(graph, g_sampleRate, g_maxFrames);
	before.process(4);
	CHECK(channel(before, 0, 0, 4) == std::vector<float>{0, 1, 2, 3});

	// A new graph with another node, built while the old one is still in use, continues the ramp.
	graph.nodes.push_back(node(NodeKind::OutputBus, 1));
	graph.audio = {{0, 1}};
	const CompiledGraph after(graph, g_sampleRate, g_maxFrames);
	after.process(4);
	CHECK(channel(after, 1, 0, 4) == std::vector<float>{4, 5, 6, 7});

	// Prepared once for the format; only a new sample rate or block size prepares it again.
	CHECK(ramp->prepared == 1);
	const CompiledGraph larger(graph, g_sampleRate, g_maxFrames * 2);
	CHECK(ramp->prepared == 2);
}

TEST_CASE("the path latency adds up the processors' latencies along the longest path", "[unit]")
{
	GraphDesc graph;
	graph.nodes.push_back(node(NodeKind::InputBus, 2));
	graph.nodes.push_back(node(NodeKind::PluginSlot, 2, std::make_shared<Delayed>(64)));
	graph.nodes.push_back(node(NodeKind::PluginSlot, 2, std::make_shared<Delayed>(100)));
	graph.nodes.push_back(node(NodeKind::PluginSlot, 2, std::make_shared<Delayed>(32)));
	graph.nodes.push_back(node(NodeKind::OutputBus, 2));
	graph.audio = {{0, 1}, {1, 2}, {0, 3}, {2, 4}, {3, 4}};
	const CompiledGraph compiled(graph, g_sampleRate, g_maxFrames);

	CHECK(compiled.pathLatency(0) == 0);
	CHECK(compiled.pathLatency(2) == 164);
	CHECK(compiled.pathLatency(3) == 32);
	CHECK(compiled.pathLatency(4) == 164);
}

TEST_CASE("the graph of a render snapshot routes channels to busses, groups and instruments", "[unit]")
{
	// Busses: stereo in, stereo out. Channels: audio track -> group -> out, MIDI track -> instrument -> out.
	RenderSnapshot snapshot;
	snapshot.busses = {{1, BusDirection::Input, 2}, {2, BusDirection::Output, 2}};
	RenderChannel audio;
	audio.id = 10;
	audio.input = 0;
	audio.output = {RenderTarget::Kind::Channel, 1};
	RenderChannel group;
	group.id = 11;
	group.type = TrackType::Group;
	group.output = {RenderTarget::Kind::Bus, 1};
	RenderChannel midi;
	midi.id = 12;
	midi.type = TrackType::Midi;
	midi.channels = 0;
	midi.output = {RenderTarget::Kind::Channel, 3};
	RenderChannel instrument;
	instrument.id = 13;
	instrument.type = TrackType::Instrument;
	instrument.output = {RenderTarget::Kind::Bus, 1};
	snapshot.channels = {audio, group, midi, instrument};

	const GraphDesc graph = describe(snapshot);
	REQUIRE(graph.nodes.size() == 6);
	CHECK(graph.nodes[0].kind == NodeKind::InputBus);
	CHECK(graph.nodes[1].kind == NodeKind::OutputBus);
	CHECK(graph.nodes[2].kind == NodeKind::Channel);
	CHECK(graph.nodes[2].object == 10);
	CHECK(graph.nodes[4].channels == 0);
	CHECK(graph.audio == std::vector<Connection>{{0, 2}, {2, 3}, {3, 1}, {5, 1}});
	CHECK(graph.events == std::vector<Connection>{{4, 5}});

	// The input reaches the output through the audio track and the group.
	const CompiledGraph compiled(graph, g_sampleRate, g_maxFrames);
	CHECK(compiled.find(13) == 5);
	CHECK(compiled.find(99) == -1);
	for (uint32_t i = 0; i < g_maxFrames; ++i)
	{
		compiled.audio(0)[0][i] = 0.25f;
		compiled.audio(0)[1][i] = -0.5f;
	}
	compiled.process(g_maxFrames);
	CHECK(channel(compiled, 1, 0, g_maxFrames) == std::vector<float>(g_maxFrames, 0.25f));
	CHECK(channel(compiled, 1, 1, g_maxFrames) == std::vector<float>(g_maxFrames, -0.5f));
}

TEST_CASE("graph.get describes the project's processing graph", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Result added = client.call("track.add", {{"type", "audio"}});
	REQUIRE(added.ok());

	const Result result = client.call("graph.get");
	REQUIRE(result.ok());
	const Json& nodes = result.value.at("nodes");
	REQUIRE(nodes.size() == 3); // stereo in, stereo out, the track's channel
	CHECK(nodes[0].at("kind") == "inputBus");
	CHECK(nodes[1].at("kind") == "outputBus");
	CHECK(nodes[2].at("kind") == "channel");
	CHECK(nodes[1].at("audioInputs") == Json::array({2}));
	CHECK(nodes[2].at("audioInputs") == Json::array({0})); // audio tracks record from the main input
	CHECK(nodes[2].at("eventInputs") == Json::array());
	CHECK(result.value.at("order") == Json::array({0, 2, 1}));
}
