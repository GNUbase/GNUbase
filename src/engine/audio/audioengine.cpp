// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/audioengine.h"

#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/processors/builtin.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace gnubase::engine
{
	namespace
	{
		constexpr double g_maxCaptureSeconds = 600.0;
		constexpr int64_t g_maxCaptureRead = int64_t{1} << 20; // frames one audio.capture returns at most

		Json statusSchema()
		{
			return Json::parse(R"({"type": "object",
			"properties": {"device": {"type": ["object", "null"]}, "running": {"type": "boolean"},
				"position": {"type": "integer"}, "capturedFrames": {"type": "integer"},
				"droppedFrames": {"type": "integer"}},
			"required": ["device", "running", "position", "capturedFrames", "droppedFrames"]})");
		}

		Json noParameters()
		{
			return CommandInfo{}.parameters;
		}

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		const processors::BuiltinInfo& generatorInfo(const std::string& id)
		{
			for (const processors::BuiltinInfo& info : processors::builtins())
				if (info.id == id)
				{
					require(!info.instrument, id + " is not a generator");
					return info;
				}
			throw CommandError("no built-in processor " + id);
		}
	}

	AudioEngine::AudioEngine(rt::SnapshotPublisher<RenderSnapshot>& snapshots)
		: m_snapshots(snapshots), m_generatorEvents(graph::g_maxEventsPerBlock),
		  m_generatorOut(graph::g_maxEventsPerBlock)
	{
	}

	AudioEngine::~AudioEngine()
	{
		if (m_device)
			m_device->stop();
	}

	void AudioEngine::sync()
	{
		if (m_device && m_snapshots.latest().revision != m_revision)
			rebuild();
	}

	void AudioEngine::rebuild()
	{
		const RenderSnapshot& snapshot = m_snapshots.latest();
		const audio::DeviceFormat& format = m_device->format();
		const graph::GraphDesc desc = graph::describe(snapshot);
		auto program = std::make_unique<Program>(desc, format.sampleRate, format.blockSize);
		int input = 0;
		int output = 0;
		for (size_t i = 0; i < desc.nodes.size(); ++i)
		{
			const graph::NodeDesc& node = desc.nodes[i];
			if (node.kind == graph::NodeKind::InputBus)
			{
				program->inputs.push_back({static_cast<int>(i), input, node.channels});
				input += node.channels;
			}
			else if (node.kind == graph::NodeKind::OutputBus)
			{
				program->outputs.push_back({static_cast<int>(i), output, node.channels});
				output += node.channels;
			}
		}
		m_revision = snapshot.revision;
		if (m_programs)
			m_programs->publish(std::move(program));
		else
			m_programs = std::make_unique<rt::SnapshotPublisher<Program>>(std::move(program));
	}

	void AudioEngine::process(const audio::DeviceBlock& block) noexcept
	{
		const Program& program = m_programs->acquire();
		const audio::DeviceFormat& format = m_device->format();
		const size_t bytes = block.frames * sizeof(float);
		for (const BusLink& link : program.inputs)
		{
			float* const* bus = program.graph.audio(link.node);
			for (int c = 0; c < link.channels; ++c)
			{
				const int device = link.firstChannel + c;
				if (device < format.inputChannels)
					std::memcpy(bus[c], block.inputs[device], bytes);
				else
					std::memset(bus[c], 0, bytes);
			}
		}
		program.graph.process(block.frames);
		for (const BusLink& link : program.outputs)
		{
			float* const* bus = program.graph.audio(link.node);
			for (int c = 0; c < link.channels && link.firstChannel + c < format.outputChannels; ++c)
				std::memcpy(block.outputs[link.firstChannel + c], bus[c], bytes);
		}
		m_programs->release();
	}

	audio::DummyDevice& AudioEngine::requireStopped()
	{
		require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
		require(!m_device->running(), "the audio device is running");
		return *m_device;
	}

	void AudioEngine::setDevice(const Json& parameters)
	{
		audio::DeviceFormat format;
		format.sampleRate = parameters.value("sampleRate", format.sampleRate);
		const auto blockSize = parameters.value("blockSize", int64_t{format.blockSize});
		const auto inputs = parameters.value("inputChannels", int64_t{format.inputChannels});
		const auto outputs = parameters.value("outputChannels", int64_t{format.outputChannels});
		const double seconds = parameters.value("captureSeconds", 60.0);
		require(format.sampleRate >= 8000.0 && format.sampleRate <= 384000.0, "sampleRate must be from 8000 to 384000");
		require(blockSize >= 1 && blockSize <= 8192, "blockSize must be from 1 to 8192");
		require(inputs >= 0 && inputs <= 64, "inputChannels must be from 0 to 64");
		require(outputs >= 0 && outputs <= 64, "outputChannels must be from 0 to 64");
		require(seconds >= 0.0 && seconds <= g_maxCaptureSeconds, "captureSeconds must be from 0 to 600");
		format.blockSize = static_cast<uint32_t>(blockSize);
		format.inputChannels = static_cast<int>(inputs);
		format.outputChannels = static_cast<int>(outputs);

		// Check the input before anything changes.
		std::shared_ptr<graph::Processor> generator;
		graph::EventBuffer settings(graph::g_maxEventsPerBlock);
		if (const auto input = parameters.find("input"); input != parameters.end())
		{
			const auto id = input->at("processor").get<std::string>();
			const processors::BuiltinInfo& info = generatorInfo(id);
			const Json values = input->value("parameters", Json::object()); // items() needs it to outlive the loop
			for (const auto& [name, value] : values.items())
			{
				const auto parameter = std::find_if(info.parameters.begin(), info.parameters.end(),
					[&name = name](const processors::ParameterInfo& p) { return p.name == name; });
				require(parameter != info.parameters.end(), std::string(id).append(" has no parameter ").append(name));
				require(value.is_number(), "parameter " + name + " must be a number");
				graph::ProcessEvent event;
				event.type = graph::ProcessEvent::Type::Parameter;
				event.parameter = parameter->id;
				event.parameterValue = std::clamp(value.get<double>(), parameter->minimum, parameter->maximum);
				settings.add(event);
			}
			generator = processors::createBuiltin(id);
			generator->prepareFor(format.sampleRate, format.blockSize);
			m_generatorOut.clear();
			generator->process({nullptr, 0, 0, settings, m_generatorOut}); // applies the parameters
		}

		if (m_device)
			m_device->stop();
		m_device.reset();
		m_programs.reset();
		m_generator = std::move(generator);
		m_device = std::make_unique<audio::DummyDevice>(
			format, static_cast<size_t>(std::llround(seconds * format.sampleRate)));
		if (m_generator)
			m_device->setInput(
				[this](float* const* channels, int count, uint32_t frames, uint64_t /*position*/)
				{
					m_generatorOut.clear();
					m_generator->process({channels, count, frames, m_generatorEvents, m_generatorOut});
				});
		rebuild();
	}

	Json AudioEngine::status() const
	{
		if (!m_device)
			return {
				{"device", nullptr}, {"running", false}, {"position", 0}, {"capturedFrames", 0}, {"droppedFrames", 0}};
		const audio::DeviceFormat& format = m_device->format();
		return {{"device",
					{{"type", "dummy"}, {"sampleRate", format.sampleRate}, {"blockSize", format.blockSize},
						{"inputChannels", format.inputChannels}, {"outputChannels", format.outputChannels}}},
			{"running", m_device->running()}, {"position", m_device->position()},
			{"capturedFrames", m_device->capturedFrames()}, {"droppedFrames", m_device->droppedFrames()}};
	}

	void AudioEngine::registerCommands(Engine& engine)
	{
		engine.registerCommand(
			{"audio.setDevice", 1,
				"Replaces the audio device with a dummy device: no hardware, its output captured in memory "
				"(audio.capture). Defaults: 48000 Hz, blocks of 256 frames, 2 inputs, 2 outputs, 60 s of capture. "
				"The input is silent, or a built-in generator (see processor.list) with parameters by name. Starts "
				"stopped at position 0.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
					"sampleRate": {"type": "number"}, "blockSize": {"type": "integer"},
					"inputChannels": {"type": "integer"}, "outputChannels": {"type": "integer"},
					"captureSeconds": {"type": "number"},
					"input": {"type": "object", "additionalProperties": false, "required": ["processor"],
						"properties": {"processor": {"type": "string"}, "parameters": {"type": "object"}}}}})"),
				statusSchema(), false},
			[this](CommandContext& context)
			{
				setDevice(context.parameters());
				Json result = status();
				context.emit("audio.changed", result);
				return result;
			});
		engine.registerCommand(
			{"audio.get", 1, "The audio device, whether it runs, and how many frames it has processed and captured.",
				noParameters(), statusSchema(), true},
			[this](CommandContext&) { return status(); });
		engine.registerCommand(
			{"audio.render", 1,
				"Offline mode: processes the given number of frames as fast as possible, then returns. The device "
				"must be stopped.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["frames"],
					"properties": {"frames": {"type": "integer"}}})"),
				statusSchema(), false},
			[this](CommandContext& context)
			{
				const auto frames = context.parameters().at("frames").get<int64_t>();
				require(frames >= 0, "frames must not be negative");
				requireStopped().render(*this, static_cast<uint64_t>(frames));
				Json result = status();
				context.emit("audio.changed", result);
				return result;
			});
		engine.registerCommand(
			{"audio.start", 1, "Simulated real time: the device processes one block per block duration until stopped.",
				noParameters(), statusSchema(), false},
			[this](CommandContext& context)
			{
				require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
				require(!m_device->running(), "the audio device is already running");
				m_device->start(*this);
				Json result = status();
				context.emit("audio.changed", result);
				return result;
			});
		engine.registerCommand({"audio.stop", 1, "Stops simulated real time. Does nothing if the device isn't running.",
								   noParameters(), statusSchema(), false},
			[this](CommandContext& context)
			{
				if (m_device && m_device->running())
				{
					m_device->stop();
					context.emit("audio.changed", status());
				}
				return status();
			});
		engine.registerCommand(
			{"audio.capture", 1,
				"Captured output samples of one device output channel, from frame start, at most 1048576 frames.",
				Json::parse(R"({"type": "object", "additionalProperties": false,
					"required": ["channel", "start", "frames"], "properties": {"channel": {"type": "integer"},
					"start": {"type": "integer"}, "frames": {"type": "integer"}}})"),
				Json::parse(R"({"type": "object", "properties": {"samples": {"type": "array"}},
					"required": ["samples"]})"),
				true},
			[this](CommandContext& context)
			{
				require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
				const Json& parameters = context.parameters();
				const auto channel = parameters.at("channel").get<int64_t>();
				const auto start = parameters.at("start").get<int64_t>();
				const auto frames = parameters.at("frames").get<int64_t>();
				require(channel >= 0 && channel < m_device->format().outputChannels,
					"no output channel " + std::to_string(channel));
				require(start >= 0, "start must not be negative");
				require(frames >= 0 && frames <= g_maxCaptureRead, "frames must be from 0 to 1048576");
				return Json{{"samples",
					m_device->captured(
						static_cast<int>(channel), static_cast<size_t>(start), static_cast<size_t>(frames))}};
			});
	}
}
