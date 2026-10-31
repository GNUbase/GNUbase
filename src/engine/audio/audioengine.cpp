// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/audioengine.h"

#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/processors/builtin.h"
#include "engine/settings/usersettings.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		constexpr double g_maxCaptureSeconds = 600.0;
		constexpr double g_cueSeconds = 2.0; // how far ahead the cue thread positions disk streams
		constexpr auto g_cueInterval = std::chrono::milliseconds(5);
		constexpr auto g_startWait = std::chrono::seconds(2);    // for the streams before playback starts or locates
		constexpr auto g_offlineWait = std::chrono::seconds(30); // for the streams before an offline block
		constexpr int64_t g_maxCaptureRead = int64_t{1} << 20;   // frames one audio.capture returns at most
		constexpr int64_t g_maxThreads = 64;
		constexpr int64_t g_maxLatency = 65536; // frames: the dummy device's latencies and the record shift
		// Frames: the largest record offset, the latency each way and the record shift at their limits. The audio
		// thread keeps the played positions for it, so a new offset applies without resizing them.
		constexpr int64_t g_maxRecordOffset = 3 * g_maxLatency;
		constexpr auto g_reconnectInterval = std::chrono::milliseconds(500); // between tries to open a lost device
		// While a fallback plays, between tries of a lost device that its backend lists as unavailable without knowing
		// whether it is there (Backend::listsPresence): each try interrupts the fallback.
		constexpr auto g_uncertainReconnectInterval = std::chrono::seconds(3);

		Json statusSchema()
		{
			return Json::parse(R"({"type": "object",
			"properties": {"device": {"type": ["object", "null"]}, "running": {"type": "boolean"},
				"position": {"type": "integer"}, "capturedFrames": {"type": "integer"},
				"droppedFrames": {"type": "integer"}, "playbackDropouts": {"type": "integer"},
				"processing": {"type": "object", "properties": {"multiProcessing": {"type": "boolean"},
					"threads": {"type": "integer"}, "activeThreads": {"type": "integer"},
					"limitedBy": {"type": ["string", "null"],
						"enum": [null, "multiProcessing", "hardware", "realtime", "system"]}},
					"required": ["multiProcessing", "threads", "activeThreads", "limitedBy"]},
				"lost": {"type": ["object", "null"], "properties": {"backend": {"type": "string"},
					"device": {"type": "string"}, "reason": {"type": "string"}, "restart": {"type": "boolean"}},
					"required": ["backend", "device", "reason", "restart"]}},
			"required": ["device", "running", "position", "capturedFrames", "droppedFrames", "playbackDropouts",
				"processing", "lost"]})");
		}

		bool supports(const audio::DeviceInfo& info, double sampleRate)
		{
			return std::find(info.sampleRates.begin(), info.sampleRates.end(), sampleRate) != info.sampleRates.end();
		}

		Json transportSchema()
		{
			return Json::parse(R"({"type": "object",
			"properties": {"playing": {"type": "boolean"}, "recording": {"type": "boolean"},
				"position": {"type": "integer"},
				"left": {"type": "integer"}, "right": {"type": "integer"}, "cycle": {"type": "boolean"},
				"startMode": {"type": "string", "enum": ["cursor", "cycleStart"]},
				"returnToStart": {"type": "boolean"}},
			"required": ["playing", "recording", "position", "left", "right", "cycle", "startMode",
				"returnToStart"]})");
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
				{"clockSource", clockSource}, {"controlPanel", info.controlPanel},
				{"available", info.unavailableReason.empty()},
				{"unavailableReason", info.unavailableReason.empty() ? Json() : Json(info.unavailableReason)},
				{"canMonitorDirectly", info.canMonitorDirectly}};
		}

		// A device's port names: its channel names, "Input 3" or "Output 3" where it has none, " #2" and so on after
		// names that repeat, so that each one names one port.
		std::vector<std::string> portNames(std::vector<std::string> names, int channels, const std::string& generic)
		{
			names.resize(static_cast<size_t>(std::max(channels, 0)));
			for (size_t i = 0; i < names.size(); ++i)
			{
				if (names[i].empty())
					names[i] = generic + std::to_string(i + 1);
				const auto before = names.begin() + static_cast<std::ptrdiff_t>(i);
				const std::string base = names[i];
				for (int n = 2; std::find(names.begin(), before, names[i]) != before; ++n)
					names[i] = base + " #" + std::to_string(n);
			}
			return names;
		}

		// The device channel each channel of each bus uses, or -1: ports by name; a bus without ports the next ones
		// in bus order, as many as the busses of its direction before it have channels are skipped; a child bus its
		// parent's.
		std::vector<std::vector<int>> devicePorts(const std::vector<RenderBus>& busses,
			const std::vector<std::string>& inputs, const std::vector<std::string>& outputs)
		{
			std::vector<std::vector<int>> result(busses.size());
			std::array<int, 2> next{}; // per direction, the first port of the next bus without ports
			for (size_t b = 0; b < busses.size(); ++b)
			{
				const RenderBus& bus = busses[b];
				if (bus.parent >= 0)
					continue;
				const bool input = bus.direction == BusDirection::Input;
				const std::vector<std::string>& names = input ? inputs : outputs;
				int& first = next[input ? 0 : 1];
				const int channels = channelCount(bus.format);
				for (int c = 0; c < channels; ++c)
				{
					int port = first + c < static_cast<int>(names.size()) ? first + c : -1;
					if (!bus.ports.empty())
					{
						const auto found = std::find(names.begin(), names.end(), bus.ports[static_cast<size_t>(c)]);
						port = found != names.end() ? static_cast<int>(found - names.begin()) : -1;
					}
					result[b].push_back(port);
				}
				first += channels;
			}
			for (size_t b = 0; b < busses.size(); ++b)
				if (busses[b].parent >= 0)
					for (const int channel : busses[b].parentChannels)
						result[b].push_back(
							result[static_cast<size_t>(busses[b].parent)][static_cast<size_t>(channel)]);
			return result;
		}

		const processors::BuiltinInfo& generatorInfo(const std::string& id)
		{
			const processors::BuiltinInfo* info = processors::findBuiltin(id);
			require(info != nullptr, "no built-in processor " + id);
			require(!info->instrument, id + " is not a generator");
			return *info;
		}
	}

	AudioEngine::AudioEngine(rt::SnapshotPublisher<RenderSnapshot>& snapshots, const settings::Store& settings,
		std::vector<std::unique_ptr<audio::Backend>> backends,
		std::vector<std::unique_ptr<gnubase::midi::Backend>> midiBackends)
		: m_snapshots(snapshots), m_settings(settings), m_backends(std::move(backends)),
		  m_midi(std::move(midiBackends)), m_midiInput(graph::g_maxEventsPerBlock),
		  m_generatorEvents(graph::g_maxEventsPerBlock), m_generatorOut(graph::g_maxEventsPerBlock)
	{
	}

	AudioEngine::~AudioEngine()
	{
		{
			const std::scoped_lock lock(m_reconnectMutex);
			m_stopReconnecting = true;
		}
		m_reconnectWake.notify_all();
		if (m_reconnectThread.joinable())
			m_reconnectThread.join();
		stopDevice();
	}

	void AudioEngine::waitForDevice()
	{
		m_waitingForDevice = true;
		if (m_reconnectThread.joinable())
			return;
		// ponytail: polls every backend the same way; a backend that announces returning devices could post at once.
		m_reconnectThread = std::thread(
			[this]
			{
				std::unique_lock lock(m_reconnectMutex);
				while (!m_reconnectWake.wait_for(lock, g_reconnectInterval, [this] { return m_stopReconnecting; }))
					if (m_waitingForDevice && m_engine != nullptr && !m_reconnecting.exchange(true))
						m_engine->post(
							"audio.reconnect", Json::object(), [this](const Result&) { m_reconnecting = false; });
			});
	}

	void AudioEngine::stopDevice()
	{
		if (m_device)
			m_device->stop();
		stopCueing();
	}

	void AudioEngine::cue(time::Samples position, bool playing, time::Samples window, bool wait)
	{
		Playback::Cue cue;
		cue.position = position;
		cue.playing = playing;
		cue.settings = m_transport.settings();
		cue.window = window;
		m_playback.cue(cue);
		if (wait)
			m_playback.waitUntilBuffered(g_startWait);
	}

	void AudioEngine::startCueing()
	{
		m_cueing = true;
		m_cueThread = std::thread(
			[this]
			{
				while (m_cueing)
				{
					Playback::Cue cue;
					{
						const std::scoped_lock lock(m_cueMutex);
						cue.playing = m_cueState.playing;
						cue.settings = m_cueState.settings;
					}
					cue.position = m_transport.position();
					cue.window = static_cast<time::Samples>(g_cueSeconds * m_device->format().sampleRate);
					m_playback.cue(cue);
					std::this_thread::sleep_for(g_cueInterval);
				}
			});
	}

	void AudioEngine::startDevice()
	{
		m_midi.clear(); // input that waited while no device ran is stale
		startCueing();
		try
		{
			m_device->start(*this);
		}
		catch (...)
		{
			stopCueing(); // a later start would start a second cue thread
			throw;
		}
	}

	void AudioEngine::stopCueing()
	{
		m_cueing = false;
		if (m_cueThread.joinable())
			m_cueThread.join();
	}

	void AudioEngine::render(audio::DummyDevice& device, uint64_t frames)
	{
		// Block by block, each after its streams are positioned and buffered: the same output every time. Between
		// blocks nothing reads the streams, so the cue is as for a stopped transport: exactly at the cursor.
		const uint32_t blockSize = device.format().blockSize;
		for (uint64_t done = 0; done < frames;)
		{
			const auto block = static_cast<uint32_t>(std::min<uint64_t>(blockSize, frames - done));
			cue(m_transport.position(), false, block, false);
			m_playback.waitUntilBuffered(g_offlineWait);
			device.render(*this, block);
			done += block;
		}
	}

	void AudioEngine::sync()
	{
		// A new project sample rate: the device changes to it if it can, otherwise it plays faster or slower.
		const double rate = m_snapshots.latest().sampleRate;
		if (rate != m_projectRate)
		{
			m_projectRate = rate;
			if (m_lost)
				m_lost->parameters.erase("sampleRate"); // it returns at the new rate if it supports it
			if (m_device && m_device->format().sampleRate != rate && supports(m_device->info(), rate))
			{
				if (!m_lost)
					m_parameters["sampleRate"] = rate;
				reset(); // a fallback opens again at the new rate, after the lost device is tried
				if (m_engine != nullptr)
					m_engine->broadcast("audio.changed", status());
			}
		}
		if (m_device && (m_snapshots.latest().revision != m_revision || autoMonitoring() != m_autoMonitoring))
			rebuild();
	}

	AutoMonitoring AudioEngine::autoMonitoring() const
	{
		const std::string mode = m_settings.get(settings::g_audioAutoMonitoring);
		if (mode == "manual")
			return AutoMonitoring::Manual;
		if (mode == "whileRecordEnabled")
			return AutoMonitoring::WhileRecordEnabled;
		if (mode == "whileRecording")
			return AutoMonitoring::WhileRecording;
		return AutoMonitoring::TapeMachine;
	}

	void AudioEngine::rebuild()
	{
		const RenderSnapshot& snapshot = m_snapshots.latest();
		m_autoMonitoring = autoMonitoring();
		const audio::DeviceFormat& format = m_device->format();
		graph::GraphDesc desc = graph::describe(snapshot);

		// Audio tracks play their playlists.
		std::vector<Playback::Track> tracks;
		std::map<ObjectId, std::string> paths;
		for (const RenderMedia& media : snapshot.media)
			paths[media.id] = media.path;
		for (const RenderChannel& channel : snapshot.channels)
		{
			if (channel.type != TrackType::Audio)
				continue;
			auto playlist = std::make_shared<const Playlist>(makePlaylist(snapshot, channel));
			for (graph::NodeDesc& node : desc.nodes)
				if (node.kind == graph::NodeKind::Channel && node.object == channel.id)
					node.processor = std::make_shared<TrackPlayer>(playlist, channel.id, m_playback);
			tracks.push_back({channel.id, std::move(playlist)});
		}
		m_playback.setTracks(std::move(tracks), std::move(paths), snapshot.sampleRate);
		const uint64_t generation = ++m_generation;
		attachMidi(snapshot, desc, generation);
		attachStrips(snapshot, desc);
		std::vector<std::vector<int>> ports = devicePorts(snapshot.busses, m_inputPorts, m_outputPorts);
		if (m_directMonitoring)
			monitorDirectly(snapshot, desc, ports);

		auto program = std::make_unique<Program>(desc, format.sampleRate, format.blockSize);
		program->tempo = snapshot.tempo;
		program->sampleRate = snapshot.sampleRate;
		program->generation = generation;
		program->releases = releases();
		program->sequencers.assign(m_sequencers.begin(), m_sequencers.end()); // ordered by channel id
		for (const auto& [channel, instrument] : m_instruments)
			program->instruments.emplace_back(channel, instrument.channel);
		for (const auto& [channel, sequencer] : program->sequencers)
		{
			uint64_t destination = 0; // its own instrument, or the one it is routed to
			for (const auto& [id, instrument] : m_instruments)
				if (id == channel || instrument.sources.count(channel) != 0)
					destination = instrument.channel->serial();
			program->destinations.push_back(destination);
		}
		// Bus nodes come first, in bus order (graph::describe); child busses reach the device through their parent.
		for (size_t b = 0; b < snapshot.busses.size(); ++b)
			if (snapshot.busses[b].parent < 0)
				(snapshot.busses[b].direction == BusDirection::Input ? program->inputs : program->outputs)
					.push_back({static_cast<int>(b), std::move(ports[b])});
		m_revision = snapshot.revision;
		if (m_programs)
			m_programs->publish(std::move(program));
		else
			m_programs = std::make_unique<rt::SnapshotPublisher<Program>>(std::move(program));
	}

	void AudioEngine::attachMidi(const RenderSnapshot& snapshot, graph::GraphDesc& desc, uint64_t generation)
	{
		std::map<ObjectId, std::set<ObjectId>> sources;  // per instrument channel, the MIDI channels routed to it
		std::map<ObjectId, const RenderChannel*> routed; // those MIDI channels
		for (const RenderChannel& channel : snapshot.channels)
			if (channel.type == TrackType::Midi && channel.output.kind == RenderTarget::Kind::Channel)
			{
				sources[snapshot.channels[static_cast<size_t>(channel.output.index)].id].insert(channel.id);
				routed[channel.id] = &channel;
			}

		std::map<ObjectId, std::shared_ptr<MidiSequencer>> sequencers;
		std::map<ObjectId, Instrument> instruments;
		for (const RenderChannel& channel : snapshot.channels)
		{
			if (channel.type != TrackType::Midi && channel.type != TrackType::Instrument)
				continue;
			const auto kept = m_sequencers.find(channel.id);
			const std::shared_ptr<MidiSequencer> sequencer =
				kept != m_sequencers.end() ? kept->second : std::make_shared<MidiSequencer>();
			auto sequence = std::make_unique<MidiSequence>();
			sequence->notes = channel.notes;
			sequence->controllers = channel.controllers;
			// An instrument chases the controller events of the MIDI tracks routed to it with its own, so the latest of
			// each kind wins, whichever track it is on.
			if (routed.count(channel.id) == 0)
				sequence->chase.push_back(makeChaseTrack(channel.controllers, channel.midiParts));
			if (channel.type == TrackType::Instrument)
				for (const ObjectId source : sources[channel.id])
					sequence->chase.push_back(
						makeChaseTrack(routed.at(source)->controllers, routed.at(source)->midiParts));
			sequencer->setSequence(std::move(sequence));
			sequencer->setChase(m_chase);
			sequencers[channel.id] = sequencer;

			std::shared_ptr<graph::Processor> processor = sequencer;
			if (channel.type == TrackType::Instrument)
			{
				const std::set<ObjectId>& from = sources[channel.id];
				Instrument instrument;
				const auto old = m_instruments.find(channel.id);
				if (old != m_instruments.end() && old->second.id == channel.instrument)
				{
					instrument = old->second;
					// A MIDI track that left can't end its notes here any more: the audio thread ends them when it
					// starts the new program, after the last block in which the track could still start notes.
					for (const ObjectId source : instrument.sources)
						if (from.count(source) == 0)
							m_releases.push_back(
								{channel.id, source, instrument.channel, m_sequencers.at(source), generation});
				}
				else
				{
					// Unknown ids play nothing; projects are checked when they load (projectFromJson).
					instrument.id = channel.instrument;
					instrument.channel = std::make_shared<InstrumentChannel>(sequencer,
						processors::isInstrument(channel.instrument) ? processors::createBuiltin(channel.instrument)
																	 : nullptr,
						++m_instrumentSerial);
				}
				instrument.sources = from;
				processor = instrument.channel;
				instruments[channel.id] = instrument;
			}
			for (graph::NodeDesc& node : desc.nodes)
				if (node.kind == graph::NodeKind::Channel && node.object == channel.id)
					node.processor = processor;
		}
		m_sequencers = std::move(sequencers);
		m_instruments = std::move(instruments);
	}

	void AudioEngine::attachStrips(const RenderSnapshot& snapshot, graph::GraphDesc& desc)
	{
		std::map<ObjectId, std::shared_ptr<ChannelState>> states;
		for (const RenderChannel& channel : snapshot.channels)
		{
			const auto kept = m_channelStates.find(channel.id);
			const std::shared_ptr<ChannelState> state =
				kept != m_channelStates.end() ? kept->second : std::make_shared<ChannelState>();
			std::unique_ptr<ChannelAutomation> automation = channelAutomation(channel, snapshot.maxVolume);
			if (kept != m_channelStates.end())
			{
				// A fader set to another value glides there.
				for (PlayedLane& lane : automation->lanes)
					for (const PlayedLane& old : state->automation.latest().lanes)
						lane.smooth = lane.smooth ||
							(lane.kind == PlayedLane::Kind::Fader && lane.target == old.target && lane.points.empty() &&
								old.points.empty() && lane.staticValue != old.staticValue);
			}
			if (kept == m_channelStates.end() || !automation->sameContent(state->automation.latest()))
				state->automation.setAutomation(std::move(automation));
			states[channel.id] = state;
			const Panning panning{channel.panner, channel.stereoPanner, channel.panBypass, snapshot.panLaw};
			std::optional<Monitoring> monitoring;
			if (channel.input >= 0)
				monitoring = Monitoring{channel.monitor, channel.recordEnable, m_autoMonitoring};
			for (graph::NodeDesc& node : desc.nodes)
				if (node.kind == graph::NodeKind::Channel && node.object == channel.id)
					node.processor = std::make_shared<ChannelStrip>(state, node.processor, panning, monitoring);
		}
		m_channelStates = std::move(states);
	}

	namespace
	{
		// The tracks that direct monitoring is for: groups aren't on the device, so what plays into one is
		// monitored through the graph.
		bool monitoredDirectly(const RenderChannel& channel)
		{
			return channel.type == TrackType::Audio && channel.input >= 0 &&
				channel.output.kind == RenderTarget::Kind::Bus;
		}
	}

	void AudioEngine::monitorDirectly(
		const RenderSnapshot& snapshot, graph::GraphDesc& desc, const std::vector<std::vector<int>>& ports)
	{
		const int firstChannel = static_cast<int>(snapshot.busses.size());
		for (size_t i = 0; i < snapshot.channels.size(); ++i)
		{
			const RenderChannel& channel = snapshot.channels[i];
			if (!monitoredDirectly(channel))
				continue;
			const int node = firstChannel + static_cast<int>(i);
			desc.audio.erase(std::remove_if(desc.audio.begin(), desc.audio.end(),
								 [&channel, node](const graph::Connection& connection)
								 { return connection.from == channel.input && connection.to == node; }),
				desc.audio.end());
		}
		routeDirectly(snapshot, ports);
	}

	void AudioEngine::routeDirectly(const RenderSnapshot& snapshot, const std::vector<std::vector<int>>& ports)
	{
		// Each input port plays on one output, the first monitoring track's that uses it; a mono input on a wider bus
		// is panned to the bus's first port and the one after it, other inputs play on the port of the same channel.
		std::map<int, audio::InputMonitor> monitors;
		for (const RenderChannel& channel : snapshot.channels)
		{
			const Monitoring monitoring{channel.monitor, channel.recordEnable, m_autoMonitoring};
			if (!monitoredDirectly(channel) || !monitoring.active(m_transport.playing(), m_transport.recording()))
				continue;
			const std::vector<int>& inputs = ports[static_cast<size_t>(channel.input)];
			const std::vector<int>& outputs = ports[static_cast<size_t>(channel.output.index)];
			const bool panned = inputs.size() == 1 && outputs.size() > 1;
			for (size_t c = 0; c < inputs.size() && !outputs.empty(); ++c)
			{
				const int output = outputs[std::min(c, outputs.size() - 1)];
				if (inputs[c] >= 0 && output >= 0 && monitors.count(inputs[c]) == 0)
					monitors[inputs[c]] = {inputs[c], output, true, channel.volume, panned ? channel.pan : -1.0};
			}
		}

		// Those that stop first, as an input may move to another output.
		for (const auto& [input, monitor] : m_monitors)
			if (monitors.count(input) == 0)
				m_device->monitorInput({monitor.input, monitor.output, false, monitor.gain, monitor.pan});
		for (const auto& [input, monitor] : monitors)
		{
			const auto old = m_monitors.find(input);
			if (old == m_monitors.end() || old->second.output != monitor.output || old->second.gain != monitor.gain ||
				old->second.pan != monitor.pan)
				m_device->monitorInput(monitor);
		}
		m_monitors = std::move(monitors);
	}

	void AudioEngine::stopMonitoringDirectly()
	{
		for (const auto& [input, monitor] : m_monitors)
			m_device->monitorInput({monitor.input, monitor.output, false, monitor.gain, monitor.pan});
		m_monitors.clear();
	}

	std::vector<AudioEngine::Release> AudioEngine::releases()
	{
		// Done once the audio thread started a program that has them; not needed if the instrument is gone or
		// replaced, or the MIDI track came back.
		const uint64_t started = m_started.load(std::memory_order_acquire);
		const auto done = [this, started](const PendingRelease& pending)
		{
			const auto instrument = m_instruments.find(pending.instrument);
			return pending.generation <= started || instrument == m_instruments.end() ||
				instrument->second.channel != pending.channel || instrument->second.sources.count(pending.source) != 0;
		};
		m_releases.erase(std::remove_if(m_releases.begin(), m_releases.end(), done), m_releases.end());

		std::vector<Release> result;
		result.reserve(m_releases.size());
		for (const PendingRelease& pending : m_releases)
		{
			Release release{pending.channel, pending.departed, {}};
			for (const ObjectId source : m_instruments.at(pending.instrument).sources)
				release.staying.push_back(m_sequencers.at(source));
			result.push_back(std::move(release));
		}
		return result;
	}

	void AudioEngine::process(const audio::DeviceBlock& block) noexcept
	{
		const auto start = std::chrono::steady_clock::now();
		if (m_resetPerformance.exchange(false, std::memory_order_acq_rel))
			m_meter.reset();
		const Program& program = m_programs->acquire();
		if (program.generation != m_running)
		{
			m_running = program.generation;
			for (const Release& release : program.releases)
				release.channel->releaseNotesOf(*release.departed, release.staying);
			for (size_t i = 0; i < program.sequencers.size(); ++i)
				program.sequencers[i].second->playOn(program.destinations[i]);
			m_started.store(program.generation, std::memory_order_release);
		}
		const audio::DeviceFormat& format = m_device->format();
		m_midiInput.clear();
		m_midi.take(block, format.sampleRate, m_midiInput);
		const size_t bytes = block.frames * sizeof(float);
		for (const BusLink& link : program.inputs)
		{
			float* const* bus = program.graph.audio(link.node);
			for (size_t c = 0; c < link.ports.size(); ++c)
			{
				const int port = link.ports[c];
				if (port >= 0 && port < format.inputChannels)
					std::memcpy(bus[c], block.inputs[port], bytes);
				else
					std::memset(bus[c], 0, bytes);
			}
		}
		m_playback.beginBlock();
		graph::TransportBlock transport = m_transport.advance(block.frames);
		m_played.add(block.position, block.frames, transport);
		captureInput(block);
		transport.tempo = program.tempo.bpmAt(program.tempo.toTicks(transport.position, program.sampleRate));
		m_tap.deliver(block.position, block.frames, program.sequencers);
		program.graph.process(block.frames, transport, m_workers.get());
		m_tap.capture(block.position, program.instruments);
		m_playback.endBlock();
		// Outputs start silent; busses that share a port add up there, in bus order.
		for (const BusLink& link : program.outputs)
		{
			float* const* bus = program.graph.audio(link.node);
			for (size_t c = 0; c < link.ports.size(); ++c)
			{
				const int port = link.ports[c];
				if (port < 0 || port >= format.outputChannels)
					continue;
				float* out = block.outputs[port];
				const float* in = bus[c];
				for (uint32_t i = 0; i < block.frames; ++i)
					out[i] += in[i];
			}
		}

		const std::chrono::duration<double> busy = std::chrono::steady_clock::now() - start;
		std::array<double, PerformanceData::g_maxThreads> threads{};
		const size_t count = std::min(m_workers ? m_workers->workers() + 1 : size_t{1}, threads.size());
		for (size_t t = 0; t < count; ++t)
			threads[t] = static_cast<double>(program.graph.busyNanoseconds(t)) * 1e-9;
		m_meter.cycle(busy.count(), block.frames / format.sampleRate, threads.data(), count);
		publishPerformance(m_meter.data());
		m_programs->release();
	}

	void AudioEngine::captureInput(const audio::DeviceBlock& block) noexcept
	{
		if (m_capturedInput.empty() || m_capturedInput[0].empty())
			return;
		const auto frames = static_cast<int64_t>(m_capturedInput[0].size());
		const int64_t offset = m_recordOffset.load(std::memory_order_relaxed); // for the whole block
		const size_t channels = std::min(m_capturedInput.size(), static_cast<size_t>(m_device->format().inputChannels));
		for (uint32_t i = 0; i < block.frames; ++i)
		{
			const time::Samples position = m_played.at(static_cast<int64_t>(block.position + i) - offset);
			if (position < 0 || position >= frames)
				continue;
			for (size_t c = 0; c < channels; ++c)
				m_capturedInput[c][static_cast<size_t>(position)] = block.inputs[c][i];
		}
	}

	audio::DummyDevice& AudioEngine::requireDummy()
	{
		require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
		require(m_dummy != nullptr, "only the dummy device renders offline and captures its output");
		return *m_dummy;
	}

	void AudioEngine::closeDevice()
	{
		if (m_device || m_lost)
			++m_closed;
		stopDevice();
		if (m_device)
			stopMonitoringDirectly(); // the hardware would go on monitoring
		m_workers.reset();
		m_device.reset();
		clearPerformance(); // the old device's numbers would stay up until the new one processes a block
		m_dummy = nullptr;
		m_inputPorts.clear();
		m_outputPorts.clear();
		m_programs.reset();
		m_directMonitoring = false;
		m_played = PlayedPositions();
		m_capturedInput.clear();
		m_tap.clear(); // the next device starts at position 0
		m_lost.reset();
		m_waitingForDevice = false;
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

		const auto recordShift = parameters.value("recordShift", int64_t{0});
		require(
			recordShift >= -g_maxLatency && recordShift <= g_maxLatency, "recordShift must be from -65536 to 65536");
		const bool directMonitoring = parameters.value("directMonitoring", false);
		const double captureSeconds = parameters.value("captureSeconds", name == "dummy" ? 60.0 : 0.0);
		require(captureSeconds >= 0.0 && captureSeconds <= g_maxCaptureSeconds, "captureSeconds must be from 0 to 600");
		if (name == "dummy")
		{
			openDummy(parameters);
		}
		else
		{
			for (const char* key :
				{"inputChannels", "outputChannels", "input", "inputLatency", "outputLatency", "loopback"})
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
			m_generator = nullptr;
			m_device = (*backend)->open(config);
		}
		const audio::DeviceInfo info = m_device->info();
		if (directMonitoring && !info.canMonitorDirectly)
		{
			const std::string device = info.name;
			closeDevice();
			throw CommandError("the driver of " + device + " can't monitor directly");
		}
		// Without a sample rate asked for, the device runs at the project's if it can.
		m_projectRate = m_snapshots.latest().sampleRate;
		if (!parameters.contains("sampleRate") && m_device->format().sampleRate != m_projectRate &&
			supports(info, m_projectRate))
		{
			Json atProjectRate = parameters;
			atProjectRate["sampleRate"] = m_projectRate;
			setDevice(atProjectRate);
			return;
		}
		m_backend = name;
		m_parameters = parameters;
		m_directMonitoring = directMonitoring;
		m_recordShift = recordShift;
		// Before the latency is read, so that a change the driver reports in between isn't lost.
		m_device->onLatencyChange(
			[engine = m_engine]
			{
				if (engine != nullptr)
					engine->post("audio.updateLatency");
			});
		m_recordOffset = recordOffset();
		m_played = PlayedPositions(static_cast<size_t>(g_maxRecordOffset) + m_device->format().blockSize);
		m_capturedInput.assign(static_cast<size_t>(m_device->format().inputChannels),
			std::vector<float>(static_cast<size_t>(std::llround(captureSeconds * m_device->format().sampleRate))));
		m_inputPorts = portNames(info.inputs, m_device->format().inputChannels, "Input ");
		m_outputPorts = portNames(info.outputs, m_device->format().outputChannels, "Output ");
		m_device->onReset(
			[engine = m_engine]
			{
				if (engine != nullptr)
					engine->post("audio.reset");
			});
		startWorkersOrFallBack();
		rebuild();
	}

	bool AudioEngine::reset()
	{
		require(m_device != nullptr || m_lost, "there is no audio device; set one with audio.setDevice");
		// While a device is lost, it is the one to open: a fallback that plays gives way to it.
		const std::optional<Lost> lost = m_lost;
		const bool fellBack = lost && m_device;
		const bool running = m_device ? m_device->running() : lost && lost->restart;
		const std::string backend = lost ? lost->backend : m_backend;
		const Json parameters = lost ? lost->parameters : m_parameters;
		const std::string id = lost ? lost->device : m_device->info().id;
		if (!lost)
			logging::info("Resetting the audio device ({} {})", backend, id);
		// The measurement goes on across the restart. Only a restart for a dropout is one (the reference manual,
		// p. 1589), and backends report that dropout before they ask for the reset; the restart itself adds nothing.
		std::optional<PerformanceData> measured;
		uint64_t dropouts = 0;
		if (m_device)
		{
			stopDevice(); // the audio thread leaves the meter
			if (m_resetPerformance.exchange(false, std::memory_order_acq_rel))
				m_meter.reset();
			measured = m_meter.data();
			dropouts = m_deviceOverloadsKept + (m_device->overloads() - m_deviceOverloadsBefore);
		}
		try
		{
			setDevice(parameters);
			if (measured)
			{
				m_meter.resume(*measured);
				m_deviceOverloadsKept = dropouts;
				publishPerformance(m_meter.data());
			}
		}
		catch (const std::exception& error)
		{
			// E.g. unplugged: the engine goes on without it, or on the fallback, and tries again until it returns.
			closeDevice();
			if (!lost)
				logging::warning(
					"The audio device ({} {}) is lost, trying again until it returns: {}", backend, id, error.what());
			m_lost = Lost{backend, parameters, id, error.what(), running};
			failOver();
			waitForDevice();
			return !lost || fellBack || m_device != nullptr;
		}
		if (lost)
			logging::info("The audio device ({} {}) is back", backend, id);
		if (running)
			startDevice();
		return true;
	}

	bool AudioEngine::reconnect()
	{
		if (!m_lost)
			return false;
		if (m_device) // a fallback plays, which every try closes first
		{
			const Listed listed = lostDeviceListed();
			if (listed == Listed::No ||
				(listed == Listed::Maybe && std::chrono::steady_clock::now() < m_nextUncertainTry))
				return false;
		}
		return reset();
	}

	AudioEngine::Listed AudioEngine::lostDeviceListed() const
	{
		if (!m_lost)
			return Listed::No;
		if (m_lost->backend == "dummy")
			return m_dummyUnplugged ? Listed::No : Listed::Yes;
		for (const std::unique_ptr<audio::Backend>& backend : m_backends)
		{
			if (backend->name() != m_lost->backend)
				continue;
			try
			{
				const std::vector<audio::DeviceInfo> devices = backend->devices();
				const auto found = std::find_if(devices.begin(), devices.end(),
					[this](const audio::DeviceInfo& info) { return info.id == m_lost->device; });
				if (found == devices.end())
					return Listed::No;
				if (found->unavailableReason.empty())
					return Listed::Yes;
				return backend->listsPresence() ? Listed::No : Listed::Maybe;
			}
			catch (const std::exception&)
			{
				return Listed::No;
			}
		}
		return Listed::No;
	}

	void AudioEngine::failOver()
	{
		const std::string fallback = m_settings.get(settings::g_audioFallbackDevice);
		if (fallback == "none" || !m_lost)
			return;
		const Lost lost = *m_lost;
		// The dummy device without capture memory, which it would only fill with what can't be heard.
		std::vector<Json> candidates;
		if (fallback == "default" && lost.backend != "dummy")
			candidates.push_back({{"backend", lost.backend}});
		candidates.push_back({{"backend", "dummy"}, {"captureSeconds", 0}});
		for (const Json& parameters : candidates)
		{
			try
			{
				m_failingOver = true;
				setDevice(parameters); // which ends the loss
				m_failingOver = false;
				m_lost = lost;
				m_nextUncertainTry = std::chrono::steady_clock::now() + g_uncertainReconnectInterval;
				if (lost.restart)
					startDevice();
				logging::info("Playing on {} {} until the audio device returns", m_backend, m_device->info().id);
				return;
			}
			catch (const std::exception& error)
			{
				m_failingOver = false;
				closeDevice();
				logging::warning("Cannot play on the fallback device ({}): {}",
					parameters.at("backend").get<std::string>(), error.what());
			}
		}
		m_lost = lost;
	}

	void AudioEngine::reopen(const Json& parameters, bool running)
	{
		setDevice(parameters);
		if (running)
			startDevice();
	}

	void AudioEngine::set(const Json& changes)
	{
		require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
		require(!m_lost,
			"the audio device is lost and a fallback plays (lost in audio.get); audio.setDevice picks "
			"another one");
		const bool running = m_device->running();
		const Json old = m_parameters;
		Json parameters = old;
		parameters.update(changes);
		try
		{
			reopen(parameters, running);
		}
		catch (const std::exception&)
		{
			try
			{
				reopen(old, running);
			}
			catch (const std::exception& error)
			{
				logging::warning("Cannot open the audio device again with its old settings: {}", error.what());
			}
			throw;
		}
	}

	int64_t AudioEngine::recordOffset() const
	{
		const audio::DeviceLatency latency = m_device->latency();
		return std::clamp<int64_t>(int64_t{latency.input} + latency.output - m_recordShift, 0, g_maxRecordOffset);
	}

	bool AudioEngine::updateLatency(const Json& parameters)
	{
		require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
		const bool set = parameters.contains("inputLatency") || parameters.contains("outputLatency");
		if (set)
		{
			require(m_dummy != nullptr, "only the dummy device takes inputLatency and outputLatency");
			require(!m_device->running(), "the audio device is running");
			const audio::DeviceLatency current = m_dummy->latency();
			const auto input = parameters.value("inputLatency", int64_t{current.input});
			const auto output = parameters.value("outputLatency", int64_t{current.output});
			require(input >= 0 && input <= g_maxLatency && output >= 0 && output <= g_maxLatency,
				"inputLatency and outputLatency must be from 0 to 65536");
			const bool loopback = m_parameters.value("loopback", false);
			require(!loopback || input + output >= m_device->format().blockSize,
				"the dummy device's loopback needs inputLatency + outputLatency of at least a block");
			m_dummy->setLatency({static_cast<uint32_t>(input), static_cast<uint32_t>(output)}, loopback);
			m_parameters["inputLatency"] = input; // for audio.reset
			m_parameters["outputLatency"] = output;
		}
		const int64_t offset = recordOffset();
		if (offset == m_recordOffset.load(std::memory_order_relaxed))
			return set;
		// The audio thread takes it at its next block and keeps the positions it has, which cover any offset.
		m_recordOffset.store(offset, std::memory_order_relaxed);
		return true;
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

	Json AudioEngine::ports() const
	{
		const RenderSnapshot& snapshot = m_snapshots.latest();
		const std::vector<std::vector<int>> used = devicePorts(snapshot.busses, m_inputPorts, m_outputPorts);
		const auto list = [this](const std::vector<std::string>& names, bool input)
		{
			Json ports = Json::array();
			for (const std::string& name : names)
			{
				const auto found = m_portSettings.find(portKey(input, name));
				const PortSettings settings = found != m_portSettings.end() ? found->second : PortSettings{};
				ports.push_back({{"name", name}, {"showAs", settings.showAs.empty() ? name : settings.showAs},
					{"visible", settings.visible}, {"busses", Json::array()}});
			}
			return ports;
		};
		Json inputs = list(m_inputPorts, true);
		Json outputs = list(m_outputPorts, false);
		Json busses = Json::array();
		Json missing = Json::array();
		Json beyond = Json::array();
		for (size_t b = 0; b < snapshot.busses.size(); ++b)
		{
			const RenderBus& bus = snapshot.busses[b];
			const bool input = bus.direction == BusDirection::Input;
			const std::vector<std::string>& names = input ? m_inputPorts : m_outputPorts;
			Json ports = Json::array();
			for (const int port : used[b])
				ports.push_back(port >= 0 ? Json(names[static_cast<size_t>(port)]) : Json(nullptr));
			busses.push_back({{"bus", bus.id}, {"ports", std::move(ports)}});
			if (bus.parent >= 0)
				continue; // child busses use their parent's
			for (const int port : used[b])
			{
				if (port < 0)
					continue;
				Json& users = (input ? inputs : outputs)[static_cast<size_t>(port)]["busses"];
				if (users.empty() || users.back() != bus.id)
					users.push_back(bus.id);
			}
			// A bus on consecutive ports has no names that could be missing: its channels past the device's ports.
			Json silent = Json::array();
			for (size_t c = 0; bus.ports.empty() && c < used[b].size(); ++c)
				if (used[b][c] < 0)
					silent.push_back(c);
			if (!silent.empty())
				beyond.push_back(
					{{"bus", bus.id}, {"direction", input ? "input" : "output"}, {"channels", std::move(silent)}});
			for (const std::string& name : bus.ports)
			{
				if (name.empty() || std::find(names.begin(), names.end(), name) != names.end())
					continue;
				const auto entry = std::find_if(missing.begin(), missing.end(), [&name, input](const Json& port)
					{ return port.at("name") == name && (port.at("direction") == "input") == input; });
				if (entry == missing.end())
					missing.push_back(
						{{"name", name}, {"direction", input ? "input" : "output"}, {"busses", {bus.id}}});
				else if (entry->at("busses").back() != bus.id)
					entry->at("busses").push_back(bus.id);
			}
		}
		return {{"inputs", std::move(inputs)}, {"outputs", std::move(outputs)}, {"busses", std::move(busses)},
			{"missing", std::move(missing)}, {"beyondDevice", std::move(beyond)}};
	}

	std::string AudioEngine::portKey(bool input, const std::string& name) const
	{
		return m_backend + '/' + m_device->info().id + (input ? "/in/" : "/out/") + name;
	}

	Json AudioEngine::setPort(const Json& parameters)
	{
		require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
		const bool input = parameters.at("input").get<bool>();
		const auto name = parameters.at("name").get<std::string>();
		const std::vector<std::string>& names = input ? m_inputPorts : m_outputPorts;
		const auto found = std::find(names.begin(), names.end(), name);
		require(found != names.end(), std::string(input ? "no input port " : "no output port ") + name);
		PortSettings& settings = m_portSettings[portKey(input, name)];
		settings.showAs = parameters.value("showAs", settings.showAs);
		settings.visible = parameters.value("visible", settings.visible);
		return ports().at(input ? "inputs" : "outputs").at(static_cast<size_t>(found - names.begin()));
	}

	void AudioEngine::openDummy(const Json& parameters)
	{
		if (const auto device = parameters.find("device"); device != parameters.end())
			require(
				device->get<std::string>() == "dummy", "the dummy backend has no device " + device->get<std::string>());
		if (const auto clock = parameters.find("clockSource"); clock != parameters.end())
			throw CommandError("the dummy device has no clock source " + clock->get<std::string>());
		if (m_dummyUnplugged && !m_failingOver)
			throw audio::DeviceError("the dummy device is unplugged (audio.simulateDeviceLoss)");

		audio::DeviceFormat format;
		format.sampleRate = parameters.value("sampleRate", format.sampleRate);
		const auto blockSize = parameters.value("blockSize", int64_t{format.blockSize});
		const auto inputs = parameters.value("inputChannels", int64_t{format.inputChannels});
		const auto outputs = parameters.value("outputChannels", int64_t{format.outputChannels});
		const double seconds = parameters.value("captureSeconds", 60.0); // checked by setDevice
		const auto inputLatency = parameters.value("inputLatency", int64_t{0});
		const auto outputLatency = parameters.value("outputLatency", int64_t{0});
		const bool loopback = parameters.value("loopback", false);
		require(format.sampleRate >= 8000.0 && format.sampleRate <= 384000.0, "sampleRate must be from 8000 to 384000");
		require(blockSize >= 1 && blockSize <= 8192, "blockSize must be from 1 to 8192");
		require(inputs >= 0 && inputs <= 64, "inputChannels must be from 0 to 64");
		require(outputs >= 0 && outputs <= 64, "outputChannels must be from 0 to 64");
		require(
			inputLatency >= 0 && inputLatency <= g_maxLatency && outputLatency >= 0 && outputLatency <= g_maxLatency,
			"inputLatency and outputLatency must be from 0 to 65536");
		require(!loopback || inputLatency + outputLatency >= blockSize,
			"the dummy device's loopback needs inputLatency + outputLatency of at least a block");
		require(!loopback || !parameters.contains("input"), "the dummy device's input is its loopback or a generator");
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
		const auto captureFrames = static_cast<size_t>(std::llround(seconds * format.sampleRate));
		auto device = std::make_unique<audio::DummyDevice>(format, captureFrames);
		device->setLatency({static_cast<uint32_t>(inputLatency), static_cast<uint32_t>(outputLatency)}, loopback);
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

	void AudioEngine::setProcessing(const Json& parameters)
	{
		const bool multiProcessing = parameters.value("multiProcessing", m_multiProcessing);
		const auto threads = parameters.value("threads", static_cast<int64_t>(m_threads));
		require(threads >= 1 && threads <= g_maxThreads, "threads must be from 1 to 64");
		require(!m_device || !m_device->running(), "the audio device is running");
		const bool previousMultiProcessing = m_multiProcessing;
		const size_t previousThreads = m_threads;
		m_multiProcessing = multiProcessing;
		m_threads = static_cast<size_t>(threads);
		clearPerformance(); // threads that are dropped and added again would resume their old averages
		if (!m_device)
			return;
		try
		{
			startWorkers();
		}
		catch (const std::system_error& error)
		{
			// Back to the settings that worked, as far as the system still lets them.
			m_multiProcessing = previousMultiProcessing;
			m_threads = previousThreads;
			startWorkersOrFallBack();
			throw CommandError(std::string("could not start the processing threads: ") + error.what());
		}
	}

	void AudioEngine::startWorkers()
	{
		m_workers.reset();
		m_limitedBy = Limit::None;
		const size_t wanted = plannedThreads() - 1;
		if (wanted == 0)
			return;
		const audio::DeviceFormat& format = m_device->format();
		const double period = format.blockSize / format.sampleRate;
		auto workers = std::make_unique<rt::WorkerPool>(wanted, period, m_device->workgroup());
		if (!workers->realtime())
		{
			// The audio thread waits for nodes that workers hold: a worker without real-time priority that the OS
			// preempts would make it miss its deadline. Only workers that got it help, if any.
			const size_t realtime = workers->realtimeWorkers();
			workers.reset();
			if (realtime > 0)
				workers = std::make_unique<rt::WorkerPool>(realtime, period, m_device->workgroup());
			if (workers && !workers->realtime())
				workers.reset();
			m_limitedBy = Limit::Realtime;
			logging::warning("{} of {} processing threads got real-time priority; processing with {}", realtime, wanted,
				workers ? workers->workers() + 1 : 1);
		}
		m_workers = std::move(workers);
	}

	void AudioEngine::startWorkersOrFallBack()
	{
		try
		{
			startWorkers();
		}
		catch (const std::system_error& error)
		{
			m_workers.reset();
			m_limitedBy = Limit::System;
			logging::warning(
				"Could not start the processing threads ({}); processing on the audio thread only", error.what());
		}
	}

	size_t AudioEngine::plannedThreads() const
	{
		// More threads than cores only wait for each other: a worker that holds a node loses its core to the ones
		// spinning for the next.
		return m_multiProcessing ? std::min(m_threads, rt::hardwareThreads()) : 1;
	}

	size_t AudioEngine::defaultThreads()
	{
		// About one per physical core: the second hardware thread of a core adds little to DSP code, and the disk
		// streams, the command thread, the UI and the plugin host processes need cores too.
		return std::max<size_t>(rt::hardwareThreads() / 2, 1);
	}

	Json AudioEngine::processingStatus() const
	{
		const size_t active = m_device ? 1 + (m_workers ? m_workers->workers() : 0) : plannedThreads();
		Json limitedBy = nullptr;
		if (active < m_threads)
		{
			if (!m_multiProcessing)
				limitedBy = "multiProcessing";
			else if (m_device && m_limitedBy == Limit::Realtime)
				limitedBy = "realtime";
			else if (m_device && m_limitedBy == Limit::System)
				limitedBy = "system";
			else
				limitedBy = "hardware";
		}
		return {{"multiProcessing", m_multiProcessing}, {"threads", m_threads}, {"activeThreads", active},
			{"limitedBy", std::move(limitedBy)}};
	}

	Json AudioEngine::status() const
	{
		const Json processing = processingStatus();
		Json lost = nullptr;
		if (m_lost)
			lost = {{"backend", m_lost->backend}, {"device", m_lost->device}, {"reason", m_lost->reason},
				{"restart", m_lost->restart}};
		if (!m_device)
		{
			return {{"device", nullptr}, {"running", false}, {"position", 0}, {"capturedFrames", 0},
				{"droppedFrames", 0}, {"playbackDropouts", m_playback.dropouts()}, {"processing", processing},
				{"lost", std::move(lost)}};
		}
		const audio::DeviceFormat& format = m_device->format();
		const audio::DeviceLatency latency = m_device->latency();
		const double projectRate = m_snapshots.latest().sampleRate;
		Json device = toJson(m_device->info());
		device.update({{"backend", m_backend}, {"sampleRate", format.sampleRate}, {"blockSize", format.blockSize},
			{"inputChannels", format.inputChannels}, {"outputChannels", format.outputChannels},
			{"inputLatency", latency.input}, {"outputLatency", latency.output}, {"recordShift", m_recordShift},
			{"recordOffset", m_recordOffset.load(std::memory_order_relaxed)}, {"directMonitoring", m_directMonitoring},
			{"projectSampleRate", projectRate}, {"sampleRateMismatch", format.sampleRate != projectRate}});
		return {{"device", std::move(device)}, {"running", m_device->running()}, {"position", m_device->position()},
			{"capturedFrames", m_dummy != nullptr ? m_dummy->capturedFrames() : 0},
			{"droppedFrames", m_dummy != nullptr ? m_dummy->droppedFrames() : 0},
			{"playbackDropouts", m_playback.dropouts()}, {"processing", processing}, {"lost", std::move(lost)}};
	}

	void AudioEngine::publishPerformance(const PerformanceData& data)
	{
		m_performance.writeBuffer() = data;
		m_performance.publish();
	}

	void AudioEngine::clearPerformance()
	{
		m_meter = PerformanceMeter();
		m_resetPerformance.store(false, std::memory_order_release);
		m_deviceOverloadsBefore = m_device ? m_device->overloads() : 0;
		m_deviceOverloadsKept = 0;
		publishPerformance(PerformanceData{});
	}

	Json AudioEngine::resetPerformance()
	{
		// Read once, so that a dropout reported meanwhile counts in the measurement that ends or in the new one.
		const uint64_t deviceOverloads = m_device ? m_device->overloads() : 0;
		Json ended = performance(deviceOverloads);
		m_deviceOverloadsBefore = deviceOverloads;
		m_deviceOverloadsKept = 0;
		if (m_device && m_device->running())
		{
			m_resetPerformance.store(true, std::memory_order_release); // the audio thread owns the meter
		}
		else
		{
			m_resetPerformance.store(false, std::memory_order_release);
			m_meter.reset();
			publishPerformance(m_meter.data());
		}
		return ended;
	}

	Json AudioEngine::performance()
	{
		return performance(m_device ? m_device->overloads() : 0);
	}

	Json AudioEngine::performance(uint64_t deviceOverloads)
	{
		m_performance.update();
		const PerformanceData& data = m_performance.readBuffer();
		Json threads = Json::array();
		for (size_t t = 0; t < data.threads; ++t)
			threads.push_back(data.threadLoad[t]);
		const uint64_t dropouts = m_deviceOverloadsKept + (deviceOverloads - m_deviceOverloadsBefore);
		return {{"load", data.load}, {"peak", data.peak}, {"overloads", data.overloads + dropouts},
			{"deviceOverloads", dropouts}, {"cycles", data.cycles}, {"threads", threads},
			{"diskLoad", m_playback.diskLoad()}, {"diskDropouts", m_playback.dropouts()},
			{"blockSize", m_device ? m_device->format().blockSize : 0}};
	}

	void AudioEngine::registerCommands(Engine& engine)
	{
		m_engine = &engine;
		// A failed command's events go nowhere: if it closed the device or opened it again before it failed, clients
		// still learn the status it left.
		const auto announcingFailure = [this](const std::function<void()>& change)
		{
			const uint64_t closed = m_closed;
			try
			{
				change();
			}
			catch (...)
			{
				if (m_closed != closed)
					m_engine->broadcast("audio.changed", status());
				throw;
			}
		};
		engine.registerCommand(
			{"audio.devices", 1,
				"The audio backends of this build, the preferred one first, and the devices each has now: their ids, "
				"names, channel names, sample rates, block sizes, clock sources (and the current one), whether the "
				"driver has a control panel, whether it can monitor inputs directly (canMonitorDirectly), and whether "
				"the device is available. An unavailable device, e.g. one whose hardware isn't connected, has an "
				"unavailableReason; audio.setDevice still tries it. Some backends only learn that by opening or "
				"starting the device (ASIO). A backend that can't list its devices has an error instead.",
				noParameters(), Json::parse(R"({"type": "object", "properties": {"backends": {"type": "array"}},
					"required": ["backends"]})"),
				true},
			[this](CommandContext&) { return devices(); });
		const Json deviceSchema = Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
					"backend": {"type": "string"}, "device": {"type": "string"}, "clockSource": {"type": "string"},
					"sampleRate": {"type": "number"}, "blockSize": {"type": "integer"},
					"recordShift": {"type": "integer"}, "directMonitoring": {"type": "boolean"},
					"inputChannels": {"type": "integer"}, "outputChannels": {"type": "integer"},
					"captureSeconds": {"type": "number"}, "inputLatency": {"type": "integer"},
					"outputLatency": {"type": "integer"}, "loopback": {"type": "boolean"},
					"input": {"type": "object", "additionalProperties": false, "required": ["processor"],
						"properties": {"processor": {"type": "string"}, "parameters": {"type": "object"}}}}})");
		engine.registerCommand(
			{"audio.setDevice", 1,
				"Replaces the audio device. backend: one from audio.devices, by default \"dummy\". device: its id, by "
				"default the backend's default device. sampleRate: by default the project's if the device supports "
				"it, otherwise the driver's current rate; when the project's sample rate changes, the device follows "
				"it where it can. blockSize: by default the driver's preferred size. clockSource: a name from the "
				"device's clockSources. The device starts "
				"stopped at position 0. Another backend's device is closed before the new one opens, since drivers "
				"often allow only one; if opening fails, there is no device.\n"
				"Recorded input goes where the transport played the device's input + output latency before it "
				"arrived, so what is played along to or looped back lines up with what played (recordOffset in "
				"audio.get). recordShift (frames, -65536 to 65536) moves recordings that much later, for drivers that "
				"report their latency wrong. captureSeconds (0 to 600, by default 0, or 60 for the dummy device) keeps "
				"that much of what the inputs record in memory for audio.captureInput, e.g. for a loopback test. "
				"directMonitoring: the driver monitors audio tracks' inputs itself, "
				"without the engine's latency, for tracks that play into an output bus (canMonitorDirectly in "
				"audio.devices says which can); their fader and pan set its level and position, automation doesn't "
				"reach it.\n"
				"The dummy device has no hardware: its output is captured in memory (audio.capture) and it renders "
				"offline (audio.render). It supports 44100, 48000, 88200, 96000, 176400 and 192000 Hz "
				"and opens with any rate from 8000 Hz. Its defaults: 48000 Hz, blocks of 256 frames, 2 inputs, 2 "
				"outputs "
				"(inputChannels, outputChannels), 60 s of capture (captureSeconds, also of its output). Its input is "
				"silent, or a "
				"built-in generator (see processor.list) with parameters by name. It reports inputLatency and "
				"outputLatency (frames, 0 to 65536, default 0); with loopback its inputs carry its outputs back that "
				"much later, as a cable from each output to the input of the same number would, which needs at least "
				"a block. It monitors directly by adding inputs to its outputs.\n"
				"Sends audio.changed, also when it fails after closing the old device.",
				deviceSchema, statusSchema(), false},
			[this, announcingFailure](CommandContext& context)
			{
				announcingFailure([this, &context] { setDevice(context.parameters()); });
				Json result = status();
				context.emit("audio.changed", result);
				return result;
			});
		Json settingsSchema = deviceSchema;
		settingsSchema.at("properties").erase("backend");
		settingsSchema.at("properties").erase("device");
		engine.registerCommand(
			{"audio.set", 1,
				"Changes the audio device's settings and keeps the others: the parameters of audio.setDevice except "
				"backend and device, e.g. sampleRate, blockSize, clockSource, recordShift or directMonitoring. The "
				"device opens again with them, as for audio.reset, and runs on if it ran. If it can't open with them, "
				"it opens again with the settings it had and the command fails; if those fail too, there is no device. "
				"Fails while a fallback plays for a lost device (see audio.reset). "
				"Sends audio.changed, also when it fails after the device closed: with the device opened again with "
				"its old settings, at position 0, or with none.",
				settingsSchema, statusSchema(), false},
			[this, announcingFailure](CommandContext& context)
			{
				announcingFailure([this, &context] { set(context.parameters()); });
				Json result = status();
				context.emit("audio.changed", result);
				return result;
			});
		engine.registerCommand(
			{"audio.ports", 1,
				"The audio device's ports by name, in order, each with the name it is shown as and whether it is "
				"visible (audio.setPort), and the busses that use it (the bus ids); each "
				"bus's ports on this device, null where it has none (a child bus's are its parent's); and the missing "
				"ports: port names that busses use but the device doesn't have, with those busses. bus.remapPorts "
				"re-routes them. beyondDevice: busses without ports of their own (consecutive ports in bus order) "
				"whose channels run past the device's ports, with those channels, which are silent; bus.set gives "
				"them ports. Without a device there are no ports.",
				noParameters(), Json::parse(R"({"type": "object", "properties": {"inputs": {"type": "array"},
					"outputs": {"type": "array"}, "busses": {"type": "array"}, "missing": {"type": "array"},
					"beyondDevice": {"type": "array"}},
					"required": ["inputs", "outputs", "busses", "missing", "beyondDevice"]})"),
				true},
			[this](CommandContext&) { return ports(); });
		engine.registerCommand(
			{"audio.setPort", 1,
				"Changes how a port of the audio device is shown, given by whether it is an input and its system name "
				"(name in audio.ports). showAs: the name routing menus show; empty shows the system name. visible: "
				"whether routing menus offer it; busses on a hidden port keep it. The settings are kept per device "
				"while GNUbase runs. Returns the port as audio.ports lists it, and sends audio.ports as the event "
				"audio.portsChanged.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["input", "name"],
					"properties": {"input": {"type": "boolean"}, "name": {"type": "string"},
					"showAs": {"type": "string"}, "visible": {"type": "boolean"}}})"),
				Json::parse(R"({"type": "object", "properties": {"name": {"type": "string"},
					"showAs": {"type": "string"}, "visible": {"type": "boolean"}, "busses": {"type": "array"}},
					"required": ["name", "showAs", "visible", "busses"]})"),
				false},
			[this](CommandContext& context)
			{
				Json port = setPort(context.parameters());
				context.emit("audio.portsChanged", ports());
				return port;
			});
		engine.registerCommand(
			{"audio.setProcessing", 1,
				"Multi-processing: threads is how many threads process the graph, the audio thread included (1 to 64; "
				"default: half the hardware threads, about one per core). Independent tracks then run in parallel; "
				"the output is the same for any number of threads. The device must be stopped. activeThreads in the "
				"result is how many process, and limitedBy why that is fewer: multiProcessing is off, the hardware has "
				"fewer threads, worker threads didn't get real-time priority (on Linux: no real-time privileges and no "
				"rtkit), or the system refused to start them (system). Fails, keeping the previous settings, if "
				"threads can't be started.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
					"multiProcessing": {"type": "boolean"}, "threads": {"type": "integer"}}})"),
				statusSchema(), false},
			[this](CommandContext& context)
			{
				setProcessing(context.parameters());
				Json result = status();
				context.emit("audio.changed", result);
				return result;
			});
		const Json performanceSchema = Json::parse(R"({"type": "object", "properties": {"load": {"type": "number"},
				"peak": {"type": "number"}, "overloads": {"type": "integer"}, "deviceOverloads": {"type": "integer"},
				"cycles": {"type": "integer"}, "threads": {"type": "array", "items": {"type": "number"}},
				"diskLoad": {"type": "number"}, "diskDropouts": {"type": "integer"}, "blockSize": {"type": "integer"}},
				"required": ["load", "peak", "overloads", "deviceOverloads", "cycles", "threads", "diskLoad",
					"diskDropouts", "blockSize"]})");
		engine.registerCommand(
			{"audio.performance", 1,
				"Performance data for the performance monitor. Loads are the share of a block's duration spent "
				"processing it, so above 1 the device drops out. load: averaged over about 0.3 s; peak: the highest "
				"block; overloads: blocks above 1 plus deviceOverloads, the dropouts the driver reported, also those "
				"it recovers from with a reset (a block that was both counts twice; a reset after a settings change "
				"counts nothing); cycles: blocks measured; "
				"threads: the load of each processing thread, the audio thread first. Peak, overloads and cycles "
				"count since the last audio.resetPerformance, also across audio.reset; everything starts over with "
				"a new device or new processing threads. "
				"diskLoad: how far the disk stream furthest behind has run empty, 0 to 1; diskDropouts: frames of "
				"audio track playback that were silent because a disk stream was late. blockSize: the device's "
				"block size in frames, 0 without a device.",
				noParameters(), performanceSchema, true},
			[this](CommandContext&) { return performance(); });
		engine.registerCommand(
			{"audio.simulateOverload", 1,
				"Only the dummy device: simulates its driver reporting a dropout, which audio.performance counts in "
				"deviceOverloads and overloads. Returns the performance data, as audio.performance does.",
				noParameters(), performanceSchema, false},
			[this](CommandContext&)
			{
				require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
				require(m_dummy != nullptr, "only the dummy device simulates overloads");
				m_dummy->reportOverload();
				return performance();
			});
		engine.registerCommand(
			{"audio.resetPerformance", 1,
				"Starts a new performance measurement, like clicking the overload indicator: clears the peak, the "
				"overloads and the cycles; the averaged loads go on. Immediately while the device is stopped, "
				"otherwise with its next block. Returns the measurement that ended, as audio.performance does, and "
				"sends it as the event audio.performanceReset.",
				noParameters(), performanceSchema, false},
			[this](CommandContext& context)
			{
				Json result = resetPerformance();
				context.emit("audio.performanceReset", result);
				return result;
			});
		engine.registerCommand(
			{"audio.get", 1,
				"The audio device (its backend, id, name, format, channel names, latencies in frames, clock source, "
				"whether it has a control panel, whether it can monitor directly and does (directMonitoring), the "
				"record shift and the record offset: inputLatency + outputLatency - recordShift, from 0 to 196608), "
				"whether it runs, how many frames it has processed and, if it is the dummy device, captured, how many "
				"frames "
				"of audio track playback were silent because a disk stream "
				"was late, and the processing threads (see audio.setProcessing). projectSampleRate is the project's "
				"sample rate; with sampleRateMismatch the device runs at another one, as it doesn't support the "
				"project's or was asked for another, and plays faster or slower. lost: the device that went away "
				"(its backend, device id, the reason it can't be opened and whether it ran and restarts), while the "
				"engine waits for it to return (see audio.reset); otherwise null. While a device is lost, device is "
				"the fallback that plays meanwhile (the user setting audio.fallbackDevice), or null.",
				noParameters(), statusSchema(), true},
			[this](CommandContext&) { return status(); });
		engine.registerCommand(
			{"audio.reset", 1,
				"Opens the audio device again with the same settings and restarts it if it ran. Drivers ask for this "
				"when their settings change, e.g. in their control panel, or when their device goes away; the engine "
				"then resets on its own. A device that can't be opened, e.g. because it was unplugged, is lost (lost "
				"in audio.get): the engine goes on without it and tries again about every half second until it "
				"opens (audio.reconnect), then restarts it if it ran, so playback goes on where it stopped; "
				"audio.start and audio.stop meanwhile decide whether it does. With the user setting "
				"audio.fallbackDevice, a fallback device plays meanwhile and the engine switches back once the lost "
				"device opens; a reset while one plays tries the lost device and opens the fallback again if that "
				"fails. audio.setDevice ends the wait. The "
				"performance measurement goes on across a reset, which itself counts nothing: a driver that resets "
				"because of a dropout reports the dropout (see audio.performance). A reset that loses the device ends "
				"the measurement. Sends audio.changed when the device closes, opens or is lost, also when it fails "
				"after the device opened again.",
				noParameters(), statusSchema(), false},
			[this, announcingFailure](CommandContext& context)
			{
				bool changed = false;
				announcingFailure([this, &changed] { changed = reset(); });
				Json result = status();
				if (changed)
					context.emit("audio.changed", result);
				return result;
			});
		engine.registerCommand(
			{"audio.reconnect", 1,
				"Tries once to open the lost audio device again (lost in audio.get), as audio.reset does; does nothing "
				"unless a device is lost. While one is, the engine runs it on its own about every half second, one "
				"try at a time. While a fallback device plays, it tries only once the backend lists the lost device "
				"as available again, so that the fallback isn't interrupted, or every few seconds if the backend "
				"learns only by opening it whether it is there (ASIO). Tries that find the device still gone "
				"change nothing and are left out of engine.commandLog. Sends audio.changed when the device opens.",
				noParameters(), statusSchema(), false},
			[this, announcingFailure](CommandContext& context)
			{
				bool changed = false;
				announcingFailure([this, &changed] { changed = reconnect(); });
				Json result = status();
				if (changed)
					context.emit("audio.changed", result);
				else
					context.leaveOutOfLog();
				return result;
			});
		engine.registerCommand(
			{"audio.simulateDeviceLoss", 1,
				"Only the dummy device: simulates its hardware being unplugged (connected false) and its driver "
				"asking for a reset, after which the device is lost (see audio.reset), or plugged in again (connected "
				"true), after which the engine opens it again on its own. While it is unplugged, audio.setDevice "
				"can't open it either. The dummy device as a fallback (audio.fallbackDevice) can't be unplugged.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["connected"],
					"properties": {"connected": {"type": "boolean"}}})"),
				statusSchema(), false},
			[this](CommandContext& context)
			{
				if (context.parameters().at("connected").get<bool>())
				{
					m_dummyUnplugged = false; // always, even after another backend replaced the lost dummy device
					return status();
				}
				require(m_dummy != nullptr || (m_lost && m_lost->backend == "dummy"),
					"only the dummy device simulates device loss");
				m_dummyUnplugged = true;
				if (m_dummy != nullptr && !m_lost)
					m_dummy->requestReset(); // as a driver does whose device went away; a fallback can't be unplugged
				return status();
			});
		engine.registerCommand(
			{"audio.updateLatency", 1,
				"Reads the audio device's latencies again and places recordings by the new record offset (see "
				"audio.get); a running device keeps running and switches at its next block. Drivers that learn their "
				"latency after the device opens, or whose latency changes with the graph, report the change; the "
				"engine then runs this on its own. Only for the dummy device, and only while it is stopped: "
				"inputLatency and "
				"outputLatency (default: as they are) first change what it reports, and with loopback its round "
				"trip, as such a driver's would. Sends audio.changed if anything changed.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
					"inputLatency": {"type": "integer"}, "outputLatency": {"type": "integer"}}})"),
				statusSchema(), false},
			[this](CommandContext& context)
			{
				const bool changed = updateLatency(context.parameters());
				Json result = status();
				if (changed)
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
				render(device, static_cast<uint64_t>(frames));
				Json result = status();
				context.emit("audio.changed", result);
				return result;
			});
		engine.registerCommand(
			{"audio.start", 1,
				"Starts the audio device; the dummy device processes one block per block duration (simulated real "
				"time) until stopped. While the device is lost (see audio.reset), it starts once it returns (restart "
				"in lost), and audio.changed announces that.",
				noParameters(), statusSchema(), false},
			[this](CommandContext& context)
			{
				if (m_lost && !m_device)
				{
					if (!m_lost->restart)
					{
						m_lost->restart = true;
						context.emit("audio.changed", status());
					}
					return status();
				}
				require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
				require(!m_device->running(), "the audio device is already running");
				startDevice();
				if (m_lost)
					m_lost->restart = true; // a fallback runs
				Json result = status();
				context.emit("audio.changed", result);
				return result;
			});
		engine.registerCommand(
			{"audio.stop", 1,
				"Stops the audio device. Does nothing if it isn't running. While the device is lost (see audio.reset), "
				"it stays stopped once it returns (restart in lost), and audio.changed announces that.",
				noParameters(), statusSchema(), false},
			[this](CommandContext& context)
			{
				if (m_device && m_device->running())
				{
					stopDevice();
					if (m_lost)
						m_lost->restart = false;
					context.emit("audio.changed", status());
				}
				else if (m_lost && m_lost->restart)
				{
					m_lost->restart = false;
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
		engine.registerCommand(
			{"audio.captureInput", 1,
				"What an input channel of the audio device recorded at project positions from start, at most 1048576 "
				"frames: its input goes where the transport played the record offset before it arrived (see "
				"audio.get), only while the transport plays, and the last cycle lap over the ones before. Up to the "
				"device's captureSeconds (audio.setDevice) from position 0; positions not recorded are silent. Only "
				"while the device stands.",
				Json::parse(R"({"type": "object", "additionalProperties": false,
					"required": ["channel", "start", "frames"], "properties": {"channel": {"type": "integer"},
					"start": {"type": "integer"}, "frames": {"type": "integer"}}})"),
				Json::parse(R"({"type": "object", "properties": {"samples": {"type": "array"}},
					"required": ["samples"]})"),
				true},
			[this](CommandContext& context)
			{
				require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
				require(!m_device->running(), "the audio device is running");
				const Json& parameters = context.parameters();
				const auto channel = parameters.at("channel").get<int64_t>();
				const auto start = parameters.at("start").get<int64_t>();
				const auto frames = parameters.at("frames").get<int64_t>();
				require(channel >= 0 && channel < m_device->format().inputChannels,
					"no input channel " + std::to_string(channel));
				require(start >= 0, "start must not be negative");
				require(frames >= 0 && frames <= g_maxCaptureRead, "frames must be from 0 to 1048576");
				const std::vector<float>& samples = m_capturedInput[static_cast<size_t>(channel)];
				const auto begin = static_cast<size_t>(std::min<int64_t>(start, static_cast<int64_t>(samples.size())));
				const size_t end = std::min(samples.size(), begin + static_cast<size_t>(frames));
				return Json{{"samples",
					std::vector<float>(samples.begin() + static_cast<std::ptrdiff_t>(begin),
						samples.begin() + static_cast<std::ptrdiff_t>(end))}};
			});
		registerAnalysisCommands(engine);
		registerTransportCommands(engine);
		m_midi.registerCommands(engine);
		registerMidiTapCommands(engine);
	}

	void AudioEngine::sent(bool accepted)
	{
		require(accepted, "the transport is busy; try again");
		if (!m_device || !m_device->running())
			m_transport.update(); // no audio thread applies the request
		const std::scoped_lock lock(m_cueMutex);
		m_cueState.playing = m_transport.playing();
		m_cueState.settings = m_transport.settings();
	}

	void AudioEngine::play()
	{
		if (m_device && !m_transport.playing())
		{
			const TransportSettings& settings = m_transport.settings();
			const time::Samples start =
				settings.startMode == StartMode::CycleStart ? settings.left : m_transport.position();
			cue(start, false, m_device->format().blockSize, true);
		}
		sent(m_transport.play());
	}

	Json AudioEngine::transportStatus() const
	{
		const TransportSettings& settings = m_transport.settings();
		return {{"playing", m_transport.playing()}, {"recording", m_transport.recording()},
			{"position", m_transport.position()}, {"left", settings.left}, {"right", settings.right},
			{"cycle", settings.cycle},
			{"startMode", settings.startMode == StartMode::CycleStart ? "cycleStart" : "cursor"},
			{"returnToStart", settings.returnToStart}};
	}

	void AudioEngine::setChase(const MidiChase& chase)
	{
		m_chase = chase;
		for (const auto& [id, sequencer] : m_sequencers)
			sequencer->setChase(chase);
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
		if (m_device)
			cue(position, false, m_device->format().blockSize, true); // exactly there, like a start
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
		// device runs, the audio thread applies the request at its next block; until then the position is where a
		// locate goes.
		const auto command = [this, &engine](const char* name, const char* description, const char* parameters,
								 std::function<void(const Json&)> handle)
		{
			engine.registerCommand(
				{name, 1, description, parameters != nullptr ? Json::parse(parameters) : noParameters(),
					transportSchema(), false},
				[this, handle = std::move(handle)](CommandContext& context)
				{
					handle(context.parameters());
					if (m_device && m_directMonitoring)
						routeDirectly(m_snapshots.latest(),
							devicePorts(m_snapshots.latest().busses, m_inputPorts, m_outputPorts));
					Json result = transportStatus();
					context.emit("transport.changed", result);
					return result;
				});
		};
		command("transport.play",
			"Starts playback from the project cursor, or from the left locator if startMode is cycleStart. Does "
			"nothing while playing.",
			nullptr, [this](const Json&) { play(); });
		command("transport.stop",
			"Stops playback and recording. With returnToStart the cursor goes back to where playback started or was "
			"last located to.",
			nullptr, [this](const Json&) { sent(m_transport.stop()); });
		command("transport.record",
			"Starts recording (record true, the default): playback starts as with transport.play if it stood, or "
			"recording punches in where it plays. record false punches out and playback goes on. Record-enabled "
			"tracks may monitor their input meanwhile, by the user setting audio.autoMonitoring (see channel.set).",
			R"({"type": "object", "additionalProperties": false, "properties": {"record": {"type": "boolean"}}})",
			[this](const Json& parameters)
			{
				const bool record = parameters.value("record", true);
				if (record && !m_transport.playing())
					play();
				sent(m_transport.record(record));
			});
		command("transport.toggle", "Stops playback if it plays, otherwise starts it.", nullptr,
			[this](const Json&)
			{
				if (m_transport.playing())
					sent(m_transport.stop());
				else
					play();
			});
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
			{"transport.get", 1,
				"Whether playback and recording run, the project cursor in samples, the locators and the modes. "
				"After a locate the cursor is where it goes, even before the audio thread has applied it.",
				noParameters(), transportSchema(), true},
			[this](CommandContext&) { return transportStatus(); });
	}
}
