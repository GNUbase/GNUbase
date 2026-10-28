// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/audioengine.h"

#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/processors/builtin.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
#include <utility>

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

		Json transportSchema()
		{
			return Json::parse(R"({"type": "object",
			"properties": {"playing": {"type": "boolean"}, "position": {"type": "integer"},
				"left": {"type": "integer"}, "right": {"type": "integer"}, "cycle": {"type": "boolean"},
				"startMode": {"type": "string", "enum": ["cursor", "cycleStart"]},
				"returnToStart": {"type": "boolean"}},
			"required": ["playing", "position", "left", "right", "cycle", "startMode", "returnToStart"]})");
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

		Json toJson(const audio::DeviceInfo& info)
		{
			Json clockSource = nullptr;
			if (info.clockSource >= 0 && static_cast<size_t>(info.clockSource) < info.clockSources.size())
				clockSource = info.clockSources[static_cast<size_t>(info.clockSource)];
			return {{"id", info.id}, {"name", info.name}, {"inputs", info.inputs}, {"outputs", info.outputs},
				{"sampleRates", info.sampleRates}, {"blockSizes", info.blockSizes}, {"clockSources", info.clockSources},
				{"clockSource", clockSource}, {"controlPanel", info.controlPanel}};
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

	AudioEngine::AudioEngine(
		rt::SnapshotPublisher<RenderSnapshot>& snapshots, std::vector<std::unique_ptr<audio::Backend>> backends)
		: m_snapshots(snapshots), m_backends(std::move(backends)), m_generatorEvents(graph::g_maxEventsPerBlock),
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
		program.graph.process(block.frames, m_transport.advance(block.frames));
		for (const BusLink& link : program.outputs)
		{
			float* const* bus = program.graph.audio(link.node);
			for (int c = 0; c < link.channels && link.firstChannel + c < format.outputChannels; ++c)
				std::memcpy(block.outputs[link.firstChannel + c], bus[c], bytes);
		}
		m_programs->release();
	}

	audio::DummyDevice& AudioEngine::requireDummy()
	{
		require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
		require(m_dummy != nullptr, "only the dummy device renders offline and captures its output");
		return *m_dummy;
	}

	void AudioEngine::closeDevice()
	{
		if (m_device)
			m_device->stop();
		m_device.reset();
		m_dummy = nullptr;
		m_programs.reset();
	}

	void AudioEngine::setDevice(const Json& parameters)
	{
		const auto name = parameters.value("backend", std::string("dummy"));
		const auto backend = std::find_if(m_backends.begin(), m_backends.end(),
			[&name](const std::unique_ptr<audio::Backend>& candidate) { return candidate->name() == name; });
		if (backend == m_backends.end())
		{
			std::string names;
			for (const std::unique_ptr<audio::Backend>& candidate : m_backends)
				names.append(names.empty() ? "" : ", ").append(candidate->name());
			throw CommandError("no audio backend " + name + "; there are " + names);
		}

		if (name == "dummy")
		{
			openDummy(parameters);
		}
		else
		{
			for (const char* key : {"inputChannels", "outputChannels", "captureSeconds", "input"})
				require(!parameters.contains(key), std::string("only the dummy device takes ") + key);
			audio::DeviceConfig config;
			config.device = parameters.value("device", std::string());
			config.sampleRate = parameters.value("sampleRate", 0.0);
			const auto blockSize = parameters.value("blockSize", int64_t{0});
			require(blockSize >= 0 && blockSize <= 8192, "blockSize must be from 0 (the driver's choice) to 8192");
			config.blockSize = static_cast<uint32_t>(blockSize);
			config.clockSource = parameters.value("clockSource", std::string());

			// Drivers often allow one open device at a time: close the old one first.
			closeDevice();
			m_generator.reset();
			m_device = (*backend)->open(config);
		}
		m_backend = name;
		m_parameters = parameters;
		m_device->onReset(
			[engine = m_engine]
			{
				if (engine != nullptr)
					engine->post("audio.reset");
			});
		rebuild();
	}

	void AudioEngine::reset()
	{
		require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
		const bool running = m_device->running();
		logging::info("Resetting the audio device ({} {})", m_backend, m_device->info().id);
		const Json parameters = m_parameters;
		setDevice(parameters);
		if (running)
			m_device->start(*this);
	}

	Json AudioEngine::devices() const
	{
		Json backends = Json::array();
		for (const std::unique_ptr<audio::Backend>& backend : m_backends)
		{
			Json entry = {{"name", backend->name()}, {"devices", Json::array()}};
			try
			{
				for (const audio::DeviceInfo& info : backend->devices())
					entry["devices"].push_back(toJson(info));
			}
			catch (const std::exception& error)
			{
				entry["error"] = error.what();
			}
			backends.push_back(std::move(entry));
		}
		return {{"backends", std::move(backends)}};
	}

	void AudioEngine::openDummy(const Json& parameters)
	{
		if (const auto device = parameters.find("device"); device != parameters.end())
			require(
				device->get<std::string>() == "dummy", "the dummy backend has no device " + device->get<std::string>());
		if (const auto clock = parameters.find("clockSource"); clock != parameters.end())
			throw CommandError("the dummy device has no clock source " + clock->get<std::string>());

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
			generator->process(
				{nullptr, 0, 0, settings, m_generatorOut, graph::TransportBlock{}}); // applies the parameters
		}

		closeDevice();
		m_generator = std::move(generator);
		auto device = std::make_unique<audio::DummyDevice>(
			format, static_cast<size_t>(std::llround(seconds * format.sampleRate)));
		m_dummy = device.get();
		m_device = std::move(device);
		if (m_generator)
			m_dummy->setInput(
				[this](float* const* channels, int count, uint32_t frames, uint64_t /*position*/)
				{
					m_generatorOut.clear();
					m_generator->process(
						{channels, count, frames, m_generatorEvents, m_generatorOut, graph::TransportBlock{}});
				});
	}

	Json AudioEngine::status() const
	{
		if (!m_device)
			return {
				{"device", nullptr}, {"running", false}, {"position", 0}, {"capturedFrames", 0}, {"droppedFrames", 0}};
		const audio::DeviceFormat& format = m_device->format();
		const audio::DeviceLatency latency = m_device->latency();
		Json device = toJson(m_device->info());
		device.update({{"backend", m_backend}, {"sampleRate", format.sampleRate}, {"blockSize", format.blockSize},
			{"inputChannels", format.inputChannels}, {"outputChannels", format.outputChannels},
			{"inputLatency", latency.input}, {"outputLatency", latency.output}});
		return {{"device", std::move(device)}, {"running", m_device->running()}, {"position", m_device->position()},
			{"capturedFrames", m_dummy != nullptr ? m_dummy->capturedFrames() : 0},
			{"droppedFrames", m_dummy != nullptr ? m_dummy->droppedFrames() : 0}};
	}

	void AudioEngine::registerCommands(Engine& engine)
	{
		m_engine = &engine;
		engine.registerCommand(
			{"audio.devices", 1,
				"The audio backends of this build, the preferred one first, and the devices each has now: their ids, "
				"names, channel names, sample rates, block sizes, clock sources (and the current one), and whether "
				"the driver has a control panel. A backend that can't list its devices has an error instead.",
				noParameters(), Json::parse(R"({"type": "object", "properties": {"backends": {"type": "array"}},
					"required": ["backends"]})"),
				true},
			[this](CommandContext&) { return devices(); });
		engine.registerCommand(
			{"audio.setDevice", 1,
				"Replaces the audio device. backend: one from audio.devices, by default \"dummy\". device: its id, by "
				"default the backend's default device. sampleRate and blockSize: by default the driver's current "
				"rate and preferred size. clockSource: a name from the device's clockSources. The device starts "
				"stopped at position 0. Another backend's device is closed before the new one opens, since drivers "
				"often allow only one; if opening fails, there is no device.\n"
				"The dummy device has no hardware: its output is captured in memory (audio.capture) and it renders "
				"offline (audio.render). Its defaults: 48000 Hz, blocks of 256 frames, 2 inputs, 2 outputs "
				"(inputChannels, outputChannels), 60 s of capture (captureSeconds). Its input is silent, or a "
				"built-in generator (see processor.list) with parameters by name.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
					"backend": {"type": "string"}, "device": {"type": "string"}, "clockSource": {"type": "string"},
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
			{"audio.get", 1,
				"The audio device (its backend, id, name, format, channel names, latencies in frames, clock source "
				"and whether it has a control panel), whether it runs, and how many frames it has processed and, if "
				"it is the dummy device, captured.",
				noParameters(), statusSchema(), true},
			[this](CommandContext&) { return status(); });
		engine.registerCommand(
			{"audio.reset", 1,
				"Opens the audio device again with the same settings and restarts it if it ran. Drivers ask for this "
				"when their settings change, e.g. in their control panel; the engine then resets on its own.",
				noParameters(), statusSchema(), false},
			[this](CommandContext& context)
			{
				reset();
				Json result = status();
				context.emit("audio.changed", result);
				return result;
			});
		engine.registerCommand(
			{"audio.controlPanel", 1,
				"Opens the control panel of the audio device's driver. Changes made there may reset the device.",
				noParameters(), statusSchema(), false},
			[this](CommandContext&)
			{
				require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
				require(m_device->info().controlPanel, "the audio device has no control panel");
				m_device->openControlPanel();
				return status();
			});
		engine.registerCommand(
			{"audio.render", 1,
				"Offline mode: processes the given number of frames as fast as possible, then returns. Only the dummy "
				"device, and only while it is stopped.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["frames"],
					"properties": {"frames": {"type": "integer"}}})"),
				statusSchema(), false},
			[this](CommandContext& context)
			{
				const auto frames = context.parameters().at("frames").get<int64_t>();
				require(frames >= 0, "frames must not be negative");
				audio::DummyDevice& device = requireDummy();
				require(!device.running(), "the audio device is running");
				device.render(*this, static_cast<uint64_t>(frames));
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
				"Captured output samples of one output channel of the dummy device, from frame start, at most "
				"1048576 frames.",
				Json::parse(R"({"type": "object", "additionalProperties": false,
					"required": ["channel", "start", "frames"], "properties": {"channel": {"type": "integer"},
					"start": {"type": "integer"}, "frames": {"type": "integer"}}})"),
				Json::parse(R"({"type": "object", "properties": {"samples": {"type": "array"}},
					"required": ["samples"]})"),
				true},
			[this](CommandContext& context)
			{
				const audio::DummyDevice& device = requireDummy();
				const Json& parameters = context.parameters();
				const auto channel = parameters.at("channel").get<int64_t>();
				const auto start = parameters.at("start").get<int64_t>();
				const auto frames = parameters.at("frames").get<int64_t>();
				require(channel >= 0 && channel < device.format().outputChannels,
					"no output channel " + std::to_string(channel));
				require(start >= 0, "start must not be negative");
				require(frames >= 0 && frames <= g_maxCaptureRead, "frames must be from 0 to 1048576");
				return Json{{"samples",
					device.captured(
						static_cast<int>(channel), static_cast<size_t>(start), static_cast<size_t>(frames))}};
			});
		registerTransportCommands(engine);
	}

	void AudioEngine::sent(bool accepted)
	{
		require(accepted, "the transport is busy; try again");
		if (!m_device || !m_device->running())
			m_transport.update(); // no audio thread applies the request
	}

	Json AudioEngine::transportStatus() const
	{
		const TransportSettings& settings = m_transport.settings();
		return {{"playing", m_transport.playing()}, {"position", m_transport.position()}, {"left", settings.left},
			{"right", settings.right}, {"cycle", settings.cycle},
			{"startMode", settings.startMode == StartMode::CycleStart ? "cycleStart" : "cursor"},
			{"returnToStart", settings.returnToStart}};
	}

	time::Samples AudioEngine::projectEnd() const
	{
		const RenderSnapshot& snapshot = m_snapshots.latest();
		time::Samples end = 0;
		for (const RenderChannel& channel : snapshot.channels)
		{
			for (const RenderAudioEvent& event : channel.audioEvents)
			{
				// The event's length is in media frames; the project may run at another rate.
				double ratio = 1.0;
				if (event.media >= 0 && static_cast<size_t>(event.media) < snapshot.media.size())
					ratio = snapshot.sampleRate / snapshot.media[static_cast<size_t>(event.media)].sampleRate;
				end = std::max(end,
					event.start + static_cast<time::Samples>(std::llround(static_cast<double>(event.length) * ratio)));
			}
			for (const RenderNote& note : channel.notes)
				end = std::max(end, note.end);
			for (const RenderController& controller : channel.controllers)
				end = std::max(end, controller.position);
		}
		return end;
	}

	void AudioEngine::locate(const Json& parameters)
	{
		time::Samples position = 0;
		if (const auto value = parameters.find("position"); value != parameters.end())
		{
			position = value->get<time::Samples>();
			require(position >= 0, "position must not be negative");
		}
		else if (const auto target = parameters.find("target"); target != parameters.end())
		{
			const auto name = target->get<std::string>();
			if (name == "projectStart")
				position = 0;
			else if (name == "projectEnd")
				position = projectEnd();
			else if (name == "leftLocator")
				position = m_transport.settings().left;
			else if (name == "rightLocator")
				position = m_transport.settings().right;
			else
				throw CommandError("no target " + name + "; use projectStart, projectEnd, leftLocator or rightLocator");
		}
		else
		{
			throw CommandError("give a position or target");
		}
		sent(m_transport.locate(position));
	}

	void AudioEngine::setTransport(const Json& parameters)
	{
		TransportSettings settings = m_transport.settings();
		const auto locator = [this, &parameters](const std::string& name, time::Samples& value)
		{
			const auto found = parameters.find(name);
			if (found == parameters.end())
				return;
			if (found->is_string())
			{
				require(found->get<std::string>() == "cursor", name + " must be a position or \"cursor\"");
				value = m_transport.position();
				return;
			}
			value = found->get<time::Samples>();
			require(value >= 0, name + " must not be negative");
		};
		locator("left", settings.left);
		locator("right", settings.right);
		settings.cycle = parameters.value("cycle", settings.cycle);
		settings.returnToStart = parameters.value("returnToStart", settings.returnToStart);
		if (const auto mode = parameters.find("startMode"); mode != parameters.end())
		{
			const auto name = mode->get<std::string>();
			require(name == "cursor" || name == "cycleStart", "startMode must be cursor or cycleStart");
			settings.startMode = name == "cycleStart" ? StartMode::CycleStart : StartMode::Cursor;
		}
		sent(m_transport.set(settings));
	}

	void AudioEngine::registerTransportCommands(Engine& engine)
	{
		// Every transport command returns the transport's state and announces it with transport.changed. While the
		// device runs, the audio thread applies the request at its next block, so the position may not show it yet.
		const auto command = [this, &engine](const char* name, const char* description, const char* parameters,
								 std::function<void(const Json&)> handle)
		{
			engine.registerCommand(
				{name, 1, description, parameters != nullptr ? Json::parse(parameters) : noParameters(),
					transportSchema(), false},
				[this, handle = std::move(handle)](CommandContext& context)
				{
					handle(context.parameters());
					Json result = transportStatus();
					context.emit("transport.changed", result);
					return result;
				});
		};
		command("transport.play",
			"Starts playback from the project cursor, or from the left locator if startMode is cycleStart. Does "
			"nothing while playing.",
			nullptr, [this](const Json&) { sent(m_transport.play()); });
		command("transport.stop",
			"Stops playback. With returnToStart the cursor goes back to where playback started or was last located "
			"to.",
			nullptr, [this](const Json&) { sent(m_transport.stop()); });
		command("transport.toggle", "Stops playback if it plays, otherwise starts it.", nullptr,
			[this](const Json&) { sent(m_transport.playing() ? m_transport.stop() : m_transport.play()); });
		command("transport.locate",
			"Moves the project cursor to a position in samples, or to a target: projectStart, projectEnd (the end of "
			"the last event), leftLocator or rightLocator. While playing, playback continues from there.",
			R"({"type": "object", "additionalProperties": false, "properties": {"position": {"type": "integer"},
				"target": {"type": "string"}}})",
			[this](const Json& parameters) { locate(parameters); });
		command("transport.set",
			"Changes the given settings and keeps the rest. left and right: the locators, in samples, or \"cursor\" "
			"for the cursor position. cycle: cycle playback between the locators; with the right locator before the "
			"left one, playback skips the range instead. startMode: cursor or cycleStart (the left locator). "
			"returnToStart: on stop, return to where playback started.",
			R"({"type": "object", "additionalProperties": false, "properties": {
				"left": {"type": ["integer", "string"]}, "right": {"type": ["integer", "string"]},
				"cycle": {"type": "boolean"}, "startMode": {"type": "string"}, "returnToStart": {"type": "boolean"}}})",
			[this](const Json& parameters) { setTransport(parameters); });
		command("transport.exchangeLocators", "Swaps the left and right locators.", nullptr,
			[this](const Json&)
			{
				TransportSettings settings = m_transport.settings();
				std::swap(settings.left, settings.right);
				sent(m_transport.set(settings));
			});
		engine.registerCommand(
			{"transport.get", 1, "Whether playback runs, the project cursor in samples, the locators and the modes.",
				noParameters(), transportSchema(), true},
			[this](CommandContext&) { return transportStatus(); });
	}
}
