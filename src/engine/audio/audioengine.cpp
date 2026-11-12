// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/audioengine.h"

#include "engine/document/rendersnapshot.h"
#include "engine/document/stripparameters.h"
#include "engine/engine.h"
#include "engine/plugins/slotinstances.h"
#include "engine/processors/builtin.h"
#include "engine/processors/sampler.h"
#include "engine/settings/usersettings.h"
#include "platform/hosttime.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
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
		// The longest pre-roll and post-roll: an hour at 768 kHz.
		constexpr time::Samples g_maxRollAmount = time::Samples{3600} * 768000;
		constexpr auto g_startWait = std::chrono::seconds(2);    // for the streams before playback starts or locates
		constexpr auto g_offlineWait = std::chrono::seconds(30); // for the streams before an offline block
		constexpr size_t g_offlineMidi = 1024;                   // MIDI messages per block another device renders
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
				"position": {"type": "integer"}, "projectSampleRate": {"type": "number"},
				"recordBitDepth": {"type": "string"},
				"capturedFrames": {"type": "integer"}, "droppedFrames": {"type": "integer"},
				"playbackDropouts": {"type": "integer"},
				"processing": {"type": "object", "properties": {"multiProcessing": {"type": "boolean"},
					"threads": {"type": "integer"}, "activeThreads": {"type": "integer"},
					"limitedBy": {"type": ["string", "null"],
						"enum": [null, "multiProcessing", "hardware", "realtime", "system"]}},
					"required": ["multiProcessing", "threads", "activeThreads", "limitedBy"]},
				"lost": {"type": ["object", "null"], "properties": {"backend": {"type": "string"},
					"device": {"type": "string"}, "name": {"type": "string"}, "reason": {"type": "string"},
					"restart": {"type": "boolean"}},
					"required": ["backend", "device", "name", "reason", "restart"]},
				"failed": {"type": ["object", "null"], "properties": {"backend": {"type": "string"},
					"device": {"type": "string"}, "reason": {"type": "string"}},
					"required": ["backend", "device", "reason"]}},
			"required": ["device", "running", "position", "projectSampleRate", "recordBitDepth", "capturedFrames",
				"droppedFrames", "playbackDropouts", "processing", "lost", "failed"]})");
		}

		bool supports(const audio::DeviceInfo& info, double sampleRate)
		{
			return std::find(info.sampleRates.begin(), info.sampleRates.end(), sampleRate) != info.sampleRates.end();
		}

		Json transportSchema()
		{
			return Json::parse(R"({"type": "object",
			"properties": {"playing": {"type": "boolean"}, "recording": {"type": "boolean"},
				"countingIn": {"type": "boolean"}, "position": {"type": "integer"}, "start": {"type": "integer"},
				"left": {"type": "integer"}, "right": {"type": "integer"}, "cycle": {"type": "boolean"},
				"startMode": {"type": "string", "enum": ["cursor", "cycleStart"]},
				"returnToStart": {"type": "boolean"}, "punchIn": {"type": "boolean"},
				"punchOut": {"type": "boolean"}, "preRoll": {"type": "boolean"}, "preRollAmount": {"type": "integer"},
				"postRoll": {"type": "boolean"}, "postRollAmount": {"type": "integer"},
				"metronome": {"type": "boolean"},
				"audioRecordMode": {"type": "string", "enum": ["keepHistory", "cycleHistoryReplace", "replace"]},
				"midiRecordMode": {"type": "string", "enum": ["newParts", "merge", "replace"]},
				"midiCycleRecordMode": {"type": "string",
					"enum": ["mix", "overwrite", "keepLast", "stacked", "mixStacked"]},
				"lockRecord": {"type": "boolean"},
				"arranger": {"type": "object", "properties": {"step": {"type": "integer"},
					"repeat": {"type": "integer"}, "event": {"type": "integer"}}}},
			"required": ["playing", "recording", "countingIn", "position", "start", "left", "right", "cycle", "startMode",
				"returnToStart", "punchIn", "punchOut", "preRoll", "preRollAmount", "postRoll", "postRollAmount",
				"metronome", "audioRecordMode", "midiRecordMode", "midiCycleRecordMode", "lockRecord", "arranger"]})");
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
		  m_recorder(
			  [this](const std::string& name, const Json& data)
			  {
				  if (m_engine == nullptr)
					  return;
				  if (name == "recording.finished")
					  m_engine->post("recording.add", data); // one undo step that adds the takes
				  else
					  m_engine->broadcast(name, data);
			  }),
		  m_midi(settings, std::move(midiBackends)), m_midiInput(graph::g_maxEventsPerBlock),
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
		// Before the members its jobs use, such as m_updatingPlugins.
		m_slots.reset();
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
		m_deviceRunning = false;
		if (m_slots)
			m_slots->wake(); // no graph runs any more
		stopCueing();
	}

	void AudioEngine::cue(time::Samples position, bool playing, time::Samples window, bool wait, time::Samples enter,
		time::Samples playsOn)
	{
		Playback::Cue cue;
		cue.position = position;
		cue.playing = playing;
		cue.settings = m_transport.settings();
		cue.window = window;
		const std::shared_ptr<const ArrangerPlan> plan = m_snapshots.latest().arranger;
		cue.arranger = plan.get();
		cue.arrangerState = m_transport.arranger();
		cue.enter = enter;
		cue.playsOn = playsOn;
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
				uint64_t changes = m_transport.changes();
				while (m_cueing)
				{
					// The audio thread changed what it plays or records, maybe on its own: the command thread syncs.
					if (const uint64_t now = m_transport.changes(); now != changes && m_engine != nullptr)
					{
						changes = now;
						m_engine->post("transport.update");
					}
					Playback::Cue cue;
					std::shared_ptr<const ArrangerPlan> plan;
					{
						const std::scoped_lock lock(m_cueMutex);
						cue.playing = m_cueState.playing;
						cue.settings = m_cueState.settings;
						plan = m_cueState.arranger;
					}
					cue.arranger = plan.get();
					cue.arrangerState = m_transport.arranger();
					// While counting in, where playback will start, as for a stopped transport.
					const time::Samples countInStart = m_transport.countInStart();
					cue.position = countInStart >= 0 ? countInStart : m_transport.position();
					cue.playing = cue.playing && countInStart < 0;
					cue.window = static_cast<time::Samples>(g_cueSeconds * m_device->format().sampleRate);
					m_playback.cue(cue);
					m_audition.cue(cue.window, true);
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
			m_deviceRunning = true;
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

	void AudioEngine::render(uint64_t frames, const std::function<void(uint32_t)>& rendered)
	{
		// Block by block, each after its streams are positioned and buffered: the same output every time. Between
		// blocks nothing reads the streams, so the cue is as for a stopped transport: exactly at the cursor.
		const audio::DeviceFormat& format = m_device->format();
		// Another device's blocks: silent inputs and outputs of its format, at the positions it would play next.
		const auto channels =
			m_dummy != nullptr ? size_t{0} : static_cast<size_t>(format.inputChannels + format.outputChannels);
		std::vector<float> samples(channels * format.blockSize);
		std::vector<float*> buffers(channels);
		for (size_t c = 0; c < channels; ++c)
			buffers[c] = samples.data() + (c * format.blockSize);
		const audio::MidiBuffer midiIn;
		audio::MidiBuffer midiOut(g_offlineMidi);
		uint64_t position = m_device->position();
		// Every sampler plays its sample from the first block, however long its file takes to read (#1180).
		while (!waitForSamples(g_offlineWait))
			logging::warning("The render still waits for the samplers' samples to be read");
		for (uint64_t done = 0; done < frames;)
		{
			const auto block = static_cast<uint32_t>(std::min<uint64_t>(format.blockSize, frames - done));
			const time::Samples countInStart = m_transport.countInStart(); // playback starts there after it
			cue(countInStart >= 0 ? countInStart : m_transport.position(), false, block, false);
			m_audition.cue(block, false);
			m_playback.waitUntilBuffered(g_offlineWait);
			m_audition.waitUntilBuffered(g_offlineWait);
			if (m_dummy != nullptr)
			{
				m_dummy->render(*this, block);
			}
			else
			{
				std::fill(samples.begin(), samples.end(), 0.0f);
				midiOut.clear();
				const rt::ScopedRealtimeContext realtime;
				process({buffers.data(), buffers.data() + format.inputChannels, block, position, hostTime(), midiIn,
					midiOut});
				position += block;
			}
			done += block;
			if (rendered)
				rendered(block);
		}
	}

	void AudioEngine::sync()
	{
		// A new record bit depth is only announced, for the status line's record format.
		const std::string depth = m_snapshots.latest().recordBitDepth;
		bool announce = !m_recordBitDepth.empty() && depth != m_recordBitDepth;
		bool reopened = false;
		m_recordBitDepth = depth;
		// A new project sample rate: the device changes to it if it can, otherwise it plays faster or slower.
		const double rate = m_snapshots.latest().sampleRate;
		if (rate != m_projectRate)
		{
			const bool known = m_projectRate != 0.0; // not the first sync, which only learns the rate
			m_projectRate = rate;
			if (m_lost)
				m_lost->parameters.erase("sampleRate"); // it returns at the new rate if it supports it
			if (m_device && m_device->format().sampleRate != rate && supports(m_device->info(), rate))
			{
				if (!m_lost)
					m_parameters["sampleRate"] = rate;
				reset(); // retry the lost device, or reopen the fallback at the new rate
				reopened = true;
			}
			else if (m_lost)
			{
				m_parameters.erase("sampleRate"); // it returns at the new rate if it supports it
			}
			// Also when the device stays as it is: the status line shows the project's rate and a mismatch.
			announce = announce || known;
		}
		if (announce && m_engine != nullptr)
			m_engine->broadcast("audio.changed", status());
		if (reopened)
			announceSlots(nullptr);
		if (m_device &&
			(m_snapshots.latest().revision != m_revision || autoMonitoring() != m_autoMonitoring ||
				listenGains() != m_listenGains || m_settings.get(settings::g_audioMutePreSends) != m_mutePreSends))
			rebuild();
		updateRecorder();
	}

	void AudioEngine::updateRecorder()
	{
		if (!m_device)
			return;
		RecordSetup setup;
		if (m_describeRecording)
			setup = m_describeRecording();
		const double deviceRate = m_device->format().sampleRate;
		setup.mode = m_transport.settings().audioRecordMode;
		setup.preRecordFrames =
			std::llround(std::clamp(m_settings.get(settings::g_audioPreRecordSeconds), 0.0, 60.0) * deviceRate);
		setup.midiMode = m_transport.settings().midiRecordMode;
		setup.midiCycleMode = m_transport.settings().midiCycleRecordMode;
		setup.midiFilter = MidiRecordFilter::fromJson(m_settings.value("midi.recordFilter"));
		setup.snapToBars = m_settings.get(settings::g_midiSnapPartsToBars);
		setup.catchFrames = std::llround(
			std::clamp(m_settings.get(settings::g_midiRecordCatchRange), 0.0, 1000.0) * deviceRate / 1000.0);
		setup.retrospective =
			static_cast<size_t>(std::max<int64_t>(m_settings.get(settings::g_midiRetrospectiveEvents), 0));
		setup.pauseFrames = std::llround(30.0 * deviceRate); // reference manual p. 354
		// Each channel of a track's file records the channel of its input bus with the same number; a bus with fewer
		// channels gives the others its last one, so a mono input fills a stereo track.
		const RenderSnapshot& snapshot = m_snapshots.latest();
		const std::vector<std::vector<int>> ports = devicePorts(snapshot.busses, m_inputPorts, m_outputPorts);
		std::vector<RecordTrack> tracks;
		for (RecordTrack& track : setup.tracks)
		{
			const auto channel = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
				[&track](const RenderChannel& candidate) { return candidate.track == track.track; });
			if (channel == snapshot.channels.end() || channel->input < 0)
				continue;
			const std::vector<int>& bus = ports.at(static_cast<size_t>(channel->input));
			for (int c = 0; c < channelCount(channel->format); ++c)
				track.ports.push_back(bus.empty() ? -1 : bus[std::min(static_cast<size_t>(c), bus.size() - 1)]);
			tracks.push_back(std::move(track));
		}
		setup.tracks = std::move(tracks);
		if (!(setup == m_recordSetup))
		{
			m_recordSetup = setup;
			m_recorder.setup(std::move(setup));
		}
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

	AudioEngine::ListenGains AudioEngine::listenGains() const
	{
		return {static_cast<float>(volumeGain(m_settings.get(settings::g_audioListenDim))),
			static_cast<float>(volumeGain(m_settings.get(settings::g_audioListenLevel)))};
	}

	void AudioEngine::rebuild()
	{
		const RenderSnapshot& snapshot = m_snapshots.latest();
		m_autoMonitoring = autoMonitoring();
		m_listenGains = listenGains();
		m_mutePreSends = m_settings.get(settings::g_audioMutePreSends);
		const audio::DeviceFormat& format = m_device->format();
		graph::GraphDesc desc = graph::describe(snapshot);
		m_sourceNodes.clear();
		for (size_t n = 0; n < desc.nodes.size(); ++n)
		{
			const graph::NodeDesc& node = desc.nodes[n];
			if (node.kind == graph::NodeKind::ChannelSource)
				m_sourceNodes[node.object] = static_cast<int>(n);
			else if (node.kind == graph::NodeKind::Channel)
				m_sourceNodes.emplace(node.object, static_cast<int>(n)); // unless it has a source node
		}

		// Audio tracks play their playlists.
		std::vector<Playback::Track> tracks;
		std::map<ObjectId, std::string> paths;
		std::map<ObjectId, Playback::Warp> warps;
		for (const RenderMedia& media : snapshot.media)
		{
			paths[media.id] = media.path;
			if (!media.warp.empty())
				warps[media.id] = {media.warp, media.preset};
		}
		m_mediaPaths = paths;
		m_auditionTracks.clear();
		for (const RenderChannel& channel : snapshot.channels)
		{
			if (channel.type != TrackType::Audio)
				continue;
			auto playlist = std::make_shared<const Playlist>(makePlaylist(snapshot, channel));
			const int source = sourceNode(channel.id);
			if (source >= 0)
				desc.nodes[static_cast<size_t>(source)].processor =
					std::make_shared<TrackPlayer>(playlist, channel.id, m_playback);
			m_auditionTracks[channel.track] = {channel.id, playlist};
			tracks.push_back({channel.id, std::move(playlist)});
		}
		m_playback.setTracks(std::move(tracks), std::move(paths), snapshot.sampleRate, std::move(warps));
		const uint64_t generation = ++m_generation;
		attachMidi(snapshot, desc, generation);
		attachSlots(snapshot, desc);
		attachStrips(snapshot, desc);
		std::vector<std::vector<int>> ports = devicePorts(snapshot.busses, m_inputPorts, m_outputPorts);
		if (m_directMonitoring)
			monitorDirectly(snapshot, desc, ports);
		attachMetronome(snapshot, desc);

		auto program = std::make_unique<Program>(desc, format.sampleRate, format.blockSize);
		program->tempo = snapshot.tempo;
		program->arranger = snapshot.arranger;
		{
			const std::scoped_lock lock(m_cueMutex);
			m_cueState.arranger = snapshot.arranger;
		}
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
			const auto found = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
				[channel = channel](const RenderChannel& candidate) { return candidate.id == channel; });
			program->thru.push_back(static_cast<uint8_t>(found != snapshot.channels.end() && found->midiThru));
			const auto transformer = m_inputTransformers.find(channel);
			program->inputTransformers.push_back(
				{transformer != m_inputTransformers.end() ? transformer->second : nullptr,
					found != snapshot.channels.end() ? found->inputTransformer : std::vector<midi::LogicalRules>()});
		}
		program->chordPadKeys = snapshot.chordPadKeys;
		// Bus nodes come first, in bus order (graph::describe); child busses reach the device through their parent.
		for (size_t b = 0; b < snapshot.busses.size(); ++b)
			if (snapshot.busses[b].mainMix)
				program->mainPorts = ports[b];
		for (size_t b = 0; b < snapshot.busses.size(); ++b)
		{
			if (snapshot.busses[b].parent >= 0)
				continue;
			if (snapshot.busses[b].mainMix)
				program->mainOutput = static_cast<int>(program->outputs.size());
			(snapshot.busses[b].direction == BusDirection::Input ? program->inputs : program->outputs)
				.push_back({static_cast<int>(b), std::move(ports[b])});
		}
		for (size_t n = 0; n < desc.nodes.size(); ++n)
			if (desc.nodes[n].kind == graph::NodeKind::ListenBus)
				program->listenNode = static_cast<int>(n);
		program->listen = m_listenGains;
		m_revision = snapshot.revision;
		if (m_programs)
			m_programs->publish(std::move(program));
		else
			m_programs = std::make_unique<rt::SnapshotPublisher<Program>>(std::move(program));
		if (m_slots)
			m_slots->retire(generation);
	}

	int AudioEngine::sourceNode(ObjectId channel) const
	{
		const auto found = m_sourceNodes.find(channel);
		return found != m_sourceNodes.end() ? found->second : -1;
	}

	void AudioEngine::announceSlots(CommandContext* context)
	{
		const auto announce = [this, context](Json data)
		{
			if (context != nullptr)
				context->emit("plugin.changed", std::move(data));
			else if (m_engine != nullptr)
				m_engine->broadcast("plugin.changed", std::move(data));
		};
		// A new slot's command reported it loading, or noDevice without a device. Every command that opens or closes
		// the device announces, so the device the slots were announced with last is the one the command saw.
		const std::string reported = m_slotsHadDevice ? "loading" : "noDevice";
		std::map<ObjectId, std::string> statuses;
		for (const RenderChannel& channel : m_snapshots.latest().channels)
			for (const RenderSlot& slot : channel.slots)
			{
				std::string status = slotStatus(slot.id);
				const auto announced = m_slotStatuses.find(slot.id);
				if (status != (announced != m_slotStatuses.end() ? announced->second : reported) && m_describeSlot)
				{
					Json described = m_describeSlot(slot.id);
					if (!described.is_null())
						announce({{"slot", std::move(described)}});
				}
				statuses[slot.id] = std::move(status);
			}
		m_slotStatuses = std::move(statuses);
		m_slotsHadDevice = m_device != nullptr;
	}

	std::string AudioEngine::slotStatus(ObjectId slot) const
	{
		if (!m_device || !m_slots)
			return "noDevice";
		const std::string status = m_slots->status(slot);
		return status.empty() ? "loading" : status; // the graph is built again after the command
	}

	void AudioEngine::attachSlots(const RenderSnapshot& snapshot, graph::GraphDesc& desc)
	{
		if (!m_slots)
			return;
		std::vector<plugins::SlotInstances::Wanted> wanted;
		for (const RenderChannel& channel : snapshot.channels)
			for (const RenderSlot& slot : channel.slots)
				wanted.push_back({&slot, static_cast<uint32_t>(channelCount(channel.format))});
		const audio::DeviceFormat& format = m_device->format();
		m_slots->update(wanted, format.sampleRate, format.blockSize);
		for (graph::NodeDesc& node : desc.nodes)
			if (node.kind == graph::NodeKind::PluginSlot)
				node.processor = m_slots->processor(node.object);
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

		// A MIDI track's chase is told by its automation when the track is heard again after playback moved while it
		// was silent (AutomationPlayer::heardAgain). The channel's state is there from the graph before, which had
		// the track while it was silent.
		const auto chaseTrack = [this](const RenderChannel& channel)
		{
			ChaseTrack track = makeChaseTrack(channel.controllers, channel.midiParts);
			if (const auto state = m_channelStates.find(channel.id);
				channel.type == TrackType::Midi && state != m_channelStates.end())
			{
				track.heardAgain = state->second->automation.heardAgain();
				track.channelKinds = channelChaseKinds(channel);
			}
			return track;
		};
		std::map<ObjectId, std::shared_ptr<MidiSequencer>> sequencers;
		std::map<ObjectId, std::shared_ptr<processors::InputTransformerProcessor>> transformers;
		std::map<ObjectId, Instrument> instruments;
		const bool replaced = snapshot.opened != m_opened; // another project
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
			sequence->modifiers = channel.modifiers;
			sequence->liveInput = channel.liveInput;
			sequence->articulations = channel.articulations;
			sequence->drumInput = channel.drumInput;
			sequence->inserts = channel.midiInserts;
			sequence->sends = channel.midiSends;
			sequence->preNotes = channel.preNotes;
			for (size_t s = 0; s < channel.midiSendOutputs.size(); ++s)
				if (const int output = channel.midiSendOutputs[s]; output >= 0)
					sequence->sendOutputs[s] = snapshot.channels[static_cast<size_t>(output)].id;
			// An instrument chases the controller events of the MIDI tracks routed to it with its own, so the latest of
			// each kind wins, whichever track it is on; not those of a track that its mute button mutes (a mute lane's
			// track it chases, not knowing where the lane mutes). A routed track chases its own into its strip alone
			// (MidiSequence::chaseIntoStrip), which learns its pedals from it and, muted by the button, keeps the
			// values for unmuting (ChannelStrip::chaseFrom). Both are told when the track is heard again, so that its
			// strip learns the pedals that the instrument's chase presses then.
			sequence->chase.push_back(chaseTrack(channel));
			sequence->chaseIntoStrip = routed.count(channel.id) != 0;
			if (channel.type == TrackType::Instrument)
				for (const ObjectId source : sources[channel.id])
					if (!routed.at(source)->muted)
						sequence->chase.push_back(chaseTrack(*routed.at(source)));
			sequencer->setSequence(std::move(sequence));
			sequencer->setChase(m_chase);
			sequencers[channel.id] = sequencer;
			// A track keeps its input transformer once it had rules, so the notes they changed end where they sound.
			if (const auto old = m_inputTransformers.find(channel.id); old != m_inputTransformers.end())
				transformers[channel.id] = old->second;
			else if (!channel.inputTransformer.empty())
				transformers[channel.id] =
					std::make_shared<processors::InputTransformerProcessor>(sequencer->input().capacity());

			std::shared_ptr<graph::Processor> processor = sequencer;
			if (channel.type == TrackType::Instrument)
			{
				const std::set<ObjectId>& from = sources[channel.id];
				Instrument instrument;
				const auto old = m_instruments.find(channel.id);
				if (old != m_instruments.end() && old->second.id == channel.instrument)
				{
					instrument = old->second;
					instrument.sends.clear(); // those that stay come back below
					// A MIDI track that left can't end its notes here any more: the audio thread ends them when it
					// starts the new program, after the last block in which the track could still start notes.
					for (const ObjectId source : instrument.sources)
						if (from.count(source) == 0)
							m_releases.push_back({channel.id, source, instrument.channel, m_sequencers.at(source),
								nullptr, 0, generation});
				}
				else
				{
					// Unknown ids play nothing; projects are checked when they load (projectFromJson).
					instrument.id = channel.instrument;
					std::shared_ptr<graph::Processor> played;
					if (channel.instrument == "sampler")
						played = instrument.sampler = std::make_shared<processors::Sampler>();
					else if (processors::isInstrument(channel.instrument))
						played = processors::createBuiltin(channel.instrument);
					instrument.channel =
						std::make_shared<InstrumentChannel>(sequencer, std::move(played), ++m_instrumentSerial);
				}
				// The sampler swaps its sample while it plays (sample hot swap). The sample loader reads a new one on
				// its own thread, and the sampler plays the one before, or nothing, until updateSamples hands it over
				// (#1122). A sampler of another project, which took over the instrument of a channel with the same id,
				// plays nothing until then: the sample before is the other project's. The sampler lets go of a sample
				// once the audio thread has moved on, by the next update.
				if (instrument.sampler)
				{
					instrument.sampler->collect();
					if (channel.sample != instrument.loaded && !instrument.loaded.empty() &&
						(channel.sample.empty() || replaced))
					{
						instrument.sampler->setSample(std::make_shared<const processors::Sample>());
						instrument.loaded.clear();
					}
					if (channel.sample != instrument.loaded)
						m_sampleLoader.request(channel.sample);
				}
				instrument.sample = channel.sample;
				instrument.sources = from;
				processor = instrument.channel;
				instruments[channel.id] = instrument;
			}
			const int source = sourceNode(channel.id);
			if (source >= 0)
				desc.nodes[static_cast<size_t>(source)].processor = processor;
		}

		// The MIDI sends to other instrument tracks, in the order of their nodes (graph::describe). A send keeps its
		// way while it plays to the same instrument; one that goes leaves its notes to end there.
		size_t sendNode = 0;
		for (const RenderChannel& channel : snapshot.channels)
			for (size_t s = 0; s < channel.midiSendOutputs.size(); ++s)
			{
				if (channel.midiSendOutputs[s] < 0)
					continue;
				while (desc.nodes[sendNode].kind != graph::NodeKind::MidiSend)
					++sendNode;
				const ObjectId target = snapshot.channels[static_cast<size_t>(channel.midiSendOutputs[s])].id;
				Instrument& instrument = instruments.at(target);
				const std::pair<ObjectId, size_t> key{channel.id, s};
				const auto old = m_instruments.find(target);
				std::shared_ptr<MidiSendTap> tap;
				if (old != m_instruments.end() && old->second.channel == instrument.channel)
					if (const auto kept = old->second.sends.find(key); kept != old->second.sends.end())
						tap = kept->second;
				if (!tap)
					tap = std::make_shared<MidiSendTap>(sequencers.at(channel.id), s);
				instrument.sends[key] = tap;
				desc.nodes[sendNode++].processor = tap;
			}
		for (const auto& [id, old] : m_instruments)
		{
			const auto now = instruments.find(id);
			if (now == instruments.end() || now->second.channel != old.channel)
				continue;
			for (const auto& [key, tap] : old.sends)
				if (now->second.sends.count(key) == 0)
					m_releases.push_back({id, key.first, old.channel, nullptr, tap, key.second, generation});
		}
		m_sequencers = std::move(sequencers);
		m_inputTransformers = std::move(transformers);
		m_instruments = std::move(instruments);
		m_opened = snapshot.opened;
		updateSamples();
	}

	std::set<std::string> AudioEngine::updateSamples()
	{
		std::set<std::string> waiting;
		for (auto& entry : m_instruments)
		{
			Instrument& instrument = entry.second;
			if (!instrument.sampler || instrument.loaded == instrument.sample)
				continue;
			if (std::shared_ptr<const processors::Sample> sample = m_sampleLoader.sample(instrument.sample))
			{
				instrument.sampler->collect();
				instrument.sampler->setSample(std::move(sample));
				instrument.loaded = instrument.sample;
			}
			else
			{
				waiting.insert(instrument.sample);
			}
		}
		m_sampleLoader.keepOnly(waiting);
		return waiting;
	}

	void AudioEngine::setSampleReader(SampleLoader::Read read)
	{
		m_sampleLoader.setRead(std::move(read));
	}

	bool AudioEngine::waitForSamples(std::chrono::milliseconds timeout)
	{
		// Only for the files that samplers wait for, not for one being read that none wants any more.
		const bool read = m_sampleLoader.waitUntilRead(timeout, updateSamples());
		updateSamples();
		return read;
	}

	void AudioEngine::postSampleUpdate()
	{
		if (m_engine != nullptr && !m_updatingSamples.exchange(true))
			m_engine->post("audio.updateSamples");
	}

	void AudioEngine::attachStrips(const RenderSnapshot& snapshot, graph::GraphDesc& desc)
	{
		std::map<ObjectId, std::shared_ptr<ChannelState>> states;
		const size_t firstChannel = snapshot.busses.size();
		std::map<ObjectId, size_t> slotNodes;
		for (size_t n = 0; n < desc.nodes.size(); ++n)
			if (desc.nodes[n].kind == graph::NodeKind::PluginSlot)
				slotNodes[desc.nodes[n].object] = n;
		size_t sendNode = 0;
		for (size_t i = 0; i < snapshot.channels.size(); ++i)
		{
			const RenderChannel& channel = snapshot.channels[i];
			const auto kept = m_channelStates.find(channel.id);
			const std::shared_ptr<ChannelState> state =
				kept != m_channelStates.end() ? kept->second : std::make_shared<ChannelState>();
			std::unique_ptr<ChannelAutomation> automation = channelAutomation(channel, snapshot.maxVolume);
			if (kept != m_channelStates.end())
			{
				// A fader or pre gain set to another value glides there.
				for (PlayedLane& lane : automation->lanes)
					for (const PlayedLane& old : state->automation.latest().lanes)
						lane.smooth = lane.smooth ||
							(lane.glides() && lane.target == old.target && lane.points.empty() && old.points.empty() &&
								lane.staticValue != old.staticValue);
			}
			if (kept == m_channelStates.end() || !automation->sameContent(state->automation.latest()))
				state->automation.setAutomation(std::move(automation));
			states[channel.id] = state;
			const Panning panning{channel.panner, channel.stereoPanner, channel.panBypass, snapshot.panLaw};
			// The limiter's look-ahead counts while it is on or its automation could turn it on.
			const bool lookAhead = channel.sections.limiter.on ||
				std::any_of(channel.automation.begin(), channel.automation.end(),
					[](const RenderAutomationLane& lane)
					{
						return lane.target ==
							AutomationTarget{AutomationTargetType::Strip, StripParameter::LimiterOn, 0, 0, 0};
					});
			std::optional<Monitoring> monitoring;
			if (channel.input >= 0)
				monitoring = Monitoring{channel.monitor, channel.recordEnable, m_autoMonitoring};
			graph::NodeDesc& node = desc.nodes[firstChannel + i];
			const auto source = static_cast<size_t>(sourceNode(channel.id));
			// The mute button mutes through the fader's mute (channelAutomation), which mute automation takes over in
			// the same change, without a gap; the strips' silence is the rest (RenderChannel::quiet).
			const bool silent = channel.quiet;
			std::shared_ptr<ChannelStrip> fader;
			std::shared_ptr<ChannelStrip> sourceStrip; // the strip that runs the source
			if (source == firstChannel + i)
			{
				fader = std::make_shared<ChannelStrip>(
					state, node.processor, panning, monitoring, silent, StripPart::Whole, lookAhead);
				sourceStrip = fader;
			}
			else
			{
				// Plugin slots between them: the source node plays the source and the automation, the slot nodes
				// give their plugins their automation, the channel's node fades.
				std::vector<const void*> plugins;
				std::vector<ObjectId> slotIds;
				for (size_t index = 0; index < channel.slots.size(); ++index)
				{
					graph::NodeDesc& slot = desc.nodes[slotNodes.at(channel.slots[index].id)];
					plugins.push_back(slot.processor.get());
					slotIds.push_back(channel.slots[index].id);
					if (slot.processor)
						slot.processor =
							std::make_shared<SlotStrip>(state, slot.processor, static_cast<uint8_t>(index));
				}
				graph::NodeDesc& sourceDesc = desc.nodes[source];
				sourceStrip = std::make_shared<ChannelStrip>(state, sourceDesc.processor, panning, monitoring, silent,
					StripPart::Source, false, std::move(plugins), std::move(slotIds));
				sourceDesc.processor = sourceStrip;
				fader = std::make_shared<ChannelStrip>(
					state, nullptr, panning, std::nullopt, silent, StripPart::Fader, lookAhead);
			}
			node.processor = fader;
			if (const auto sequencer = m_sequencers.find(channel.id);
				channel.type == TrackType::Midi && sequencer != m_sequencers.end())
			{
				if (channel.output.kind == RenderTarget::Kind::Channel)
					sourceStrip->chaseFrom(sequencer->second->stripChase(), channel.muted);
				// Every sequencer has the same chase settings (setChase).
				const std::vector<uint16_t> chaseKinds = channelChaseKinds(channel);
				std::vector<graph::ProcessEvent> kinds;
				kinds.reserve(chaseKinds.size());
				for (const uint16_t kind : chaseKinds)
					kinds.push_back(chaseKindEvent(kind));
				sourceStrip->chasedWhenHeard(std::move(kinds), sequencer->second);
				sourceStrip->playsProject(snapshot.opened);
			}
			if (const auto tap = m_renderTaps.find(channel.id); tap != m_renderTaps.end())
				(tap->second.point == RenderPoint::Source ? sourceStrip : fader)
					->tapRender(tap->second.tap, tap->second.point);

			// The send nodes come in the order of the channels and their sends (graph::describe).
			std::shared_ptr<PreFaderTap> tap;
			for (const RenderSend& send : channel.sends)
			{
				if (send.preFader && !tap)
				{
					tap = std::make_shared<PreFaderTap>(channelCount(channel.format));
					fader->tapPreFader(tap, m_mutePreSends);
				}
				while (desc.nodes[sendNode].kind != graph::NodeKind::Send)
					++sendNode;
				// Mute Pre-Send when Mute (reference manual p. 1623): the mute button mutes pre-fader sends too, and
				// mute automation through the tap.
				const float gain = send.preFader && channel.muted && m_mutePreSends ? 0.0f : send.gain;
				desc.nodes[sendNode++].processor =
					std::make_shared<SendStrip>(state, send.number, gain, send.preFader ? tap : nullptr);
			}
		}
		const std::scoped_lock lock(m_channelStatesMutex); // readMeters; the old states go after it unlocks
		m_channelStates.swap(states);
	}

	void AudioEngine::attachMetronome(const RenderSnapshot& snapshot, graph::GraphDesc& desc)
	{
		// Always there, so the metronome switch (transport.set metronome) clicks without a rebuild.
		const metronome::MetronomeSettings& settings = snapshot.metronome;
		const double deviceRate = m_device->format().sampleRate;
		std::array<metronome::SoundLibrary::Stamp, metronome::g_accents> samples;
		if (settings.sounds == metronome::Sounds::Samples)
			for (size_t accent = 0; accent < samples.size(); ++accent)
				if (!settings.samples[accent].path.empty())
					samples[accent] = metronome::SoundLibrary::stamp(settings.samples[accent].path);
		const bool same = m_clickSource && m_clickSource->settings == settings &&
			m_clickSource->tempo == snapshot.tempo && m_clickSource->signatures == snapshot.signatures &&
			m_clickSource->projectRate == snapshot.sampleRate && m_clickSource->deviceRate == deviceRate &&
			m_clickSource->samples == samples;
		if (!same)
		{
			m_metronome->setConfig(metronome::compile(
				settings, snapshot.tempo, snapshot.signatures, snapshot.sampleRate, deviceRate, m_clickSounds));
			m_clickSource =
				ClickSource{settings, snapshot.tempo, snapshot.signatures, snapshot.sampleRate, deviceRate, samples};
		}

		// After every other node, so busses add the click last. Outputs and destinations that are gone are left out.
		const int node = static_cast<int>(desc.nodes.size());
		std::vector<int> outputs;
		int destination = -1;
		for (size_t i = 0; i < desc.nodes.size(); ++i)
		{
			const graph::NodeDesc& other = desc.nodes[i];
			if (other.kind == graph::NodeKind::OutputBus &&
				std::find(settings.outputs.begin(), settings.outputs.end(), other.object) != settings.outputs.end())
			{
				outputs.push_back(static_cast<int>(i));
			}
			else if (other.kind == graph::NodeKind::Channel && settings.destination != 0 &&
				other.object == settings.destination && m_instruments.count(other.object) != 0)
			{
				destination = static_cast<int>(i);
			}
		}
		graph::NodeDesc click;
		click.kind = graph::NodeKind::Builtin;
		click.format = ChannelFormat::Mono;
		click.processor = m_metronome;
		desc.nodes.push_back(std::move(click));
		if (settings.audio)
			for (const int output : outputs)
				desc.audio.push_back({node, output});
		if (settings.midi && destination >= 0)
			desc.events.push_back({node, destination});
	}

	namespace
	{
		// The tracks that direct monitoring is for: groups aren't on the device, so what plays into one is
		// monitored through the graph, and so is a track with mute automation, which the device can't follow.
		bool monitoredDirectly(const RenderChannel& channel)
		{
			return channel.type == TrackType::Audio && channel.input >= 0 &&
				channel.output.kind == RenderTarget::Kind::Bus &&
				std::none_of(channel.automation.begin(), channel.automation.end(),
					[](const RenderAutomationLane& lane) { return lane.target.type == AutomationTargetType::Mute; });
		}
	}

	void AudioEngine::monitorDirectly(
		const RenderSnapshot& snapshot, graph::GraphDesc& desc, const std::vector<std::vector<int>>& ports)
	{
		for (const RenderChannel& channel : snapshot.channels)
		{
			if (!monitoredDirectly(channel))
				continue;
			const int node = sourceNode(channel.id);
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
			if (!monitoredDirectly(channel) || channel.silent ||
				!monitoring.active(m_transport.playing(), m_transport.recording()))
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
				instrument->second.channel != pending.channel ||
				(pending.departedSend ? instrument->second.sends.count({pending.source, pending.sendSlot}) != 0
									  : instrument->second.sources.count(pending.source) != 0);
		};
		m_releases.erase(std::remove_if(m_releases.begin(), m_releases.end(), done), m_releases.end());

		std::vector<Release> result;
		result.reserve(m_releases.size());
		for (const PendingRelease& pending : m_releases)
		{
			Release release{pending.channel, pending.departed, pending.departedSend, {}};
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
				if (release.departedSend)
					release.channel->releaseNotesOf(*release.departedSend, release.staying);
				else
					release.channel->releaseNotesOf(*release.departed, release.staying);
			for (size_t i = 0; i < program.sequencers.size(); ++i)
				program.sequencers[i].second->playOn(program.destinations[i]);
			m_started.store(program.generation, std::memory_order_release);
			if (m_slots)
				m_slots->wake(); // plugins that left the graph can go: a semaphore release, which never blocks
		}
		const audio::DeviceFormat& format = m_device->format();
		m_midiInput.clear();
		m_midi.take(block, format.sampleRate, m_midiInput, program.chordPadKeys);
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
		graph::TransportBlock transport = m_transport.advance(block.frames, program.arranger.get());
		m_played.add(block.position, block.frames, transport);
		captureInput(block);
		m_tap.deliver(block.position, block.frames, program.sequencers);
		MidiTap::thru(m_midiInput, program.sequencers, program.thru);
		for (size_t i = 0; i < program.sequencers.size() && i < program.inputTransformers.size(); ++i)
			if (const InputTransform& transform = program.inputTransformers[i]; transform.processor)
				transform.processor->process(program.sequencers[i].second->input(), transform.rules);
		transport.tempo = program.tempo.bpmAt(program.tempo.toTicks(transport.position, program.sampleRate));
		transport.tempoMap = &program.tempo;
		// The MIDI inserts above the divider play the input before it is recorded.
		for (const auto& [channel, sequencer] : program.sequencers)
			sequencer->processInput(block.frames, transport);
		m_recorder.process(block, m_played, m_recordOffset.load(std::memory_order_relaxed), program.sequencers);
		program.graph.process(block.frames, transport, m_workers.get());
		m_tap.capture(block.position, program.instruments);
		m_playback.endBlock();
		output(block, program);
		m_audition.process(block.outputs, format.outputChannels, program.mainPorts, block.frames);
		meterMainMix(block, program.mainPorts, transport.playing);

		const std::chrono::duration<double> busy = std::chrono::steady_clock::now() - start;
		std::array<double, PerformanceData::g_maxThreads> threads{};
		const size_t count = std::min(m_workers ? m_workers->workers() + 1 : size_t{1}, threads.size());
		for (size_t t = 0; t < count; ++t)
			threads[t] = static_cast<double>(program.graph.busyNanoseconds(t)) * 1e-9;
		m_meter.cycle(busy.count(), block.frames / format.sampleRate, threads.data(), count);
		publishPerformance(m_meter.data());
		m_programs->release();
	}

	void AudioEngine::output(const audio::DeviceBlock& block, const Program& program) noexcept
	{
		// Outputs start silent; busses that share a port add up there, in bus order. While channels listen, the main
		// mix glides to the listen dim over the block, and back once none does.
		const int outputChannels = m_device->format().outputChannels;
		const float dimFrom = m_dim;
		const float dimTo = program.listenNode >= 0 ? program.listen.dim : 1.0f;
		m_dim = dimTo;
		for (size_t l = 0; l < program.outputs.size(); ++l)
		{
			const BusLink& link = program.outputs[l];
			const bool dims = static_cast<int>(l) == program.mainOutput && (dimFrom != 1.0f || dimTo != 1.0f);
			float* const* bus = program.graph.audio(link.node);
			for (size_t c = 0; c < link.ports.size(); ++c)
			{
				const int port = link.ports[c];
				if (port < 0 || port >= outputChannels)
					continue;
				float* out = block.outputs[port];
				const float* in = bus[c];
				if (!dims)
				{
					for (uint32_t i = 0; i < block.frames; ++i)
						out[i] += in[i];
					continue;
				}
				const float step = (dimTo - dimFrom) / static_cast<float>(block.frames);
				for (uint32_t i = 0; i < block.frames; ++i)
					out[i] += in[i] * (dimFrom + (step * static_cast<float>(i + 1)));
			}
		}
		if (program.listenNode < 0)
			return;
		// ponytail: the listening channels come in and go at once; a glide needs the listen bus in every graph.
		float* const* listen = program.graph.audio(program.listenNode);
		for (size_t c = 0; c < program.mainPorts.size(); ++c)
		{
			const int port = program.mainPorts[c];
			if (port < 0 || port >= outputChannels)
				continue;
			float* out = block.outputs[port];
			const float* in = listen[c];
			for (uint32_t i = 0; i < block.frames; ++i)
				out[i] += in[i] * program.listen.level;
		}
	}

	void AudioEngine::meterMainMix(
		const audio::DeviceBlock& block, const std::vector<int>& ports, bool playing) noexcept
	{
		if (playing && !m_meterPlaying && m_meterResetOnStart.load(std::memory_order_relaxed))
		{
			m_mainMeter.reset();
			m_loudness.reset();
		}
		m_meterPlaying = playing;
		std::array<const float*, g_maxMeterChannels> channels{};
		int count = 0;
		for (const int port : ports)
			if (port >= 0 && port < m_device->format().outputChannels && count < g_maxMeterChannels)
				channels[static_cast<size_t>(count++)] = block.outputs[port];
		m_mainMeter.process(channels.data(), count, block.frames, m_device->format().sampleRate);
		m_loudness.process(channels.data(), count, block.frames, playing);
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
		m_recorder.prepare(0, 0); // ends a recording
		m_capturedInput.clear();
		m_tap.clear(); // the next device starts at position 0
		m_lost.reset();
		m_waitingForDevice = false;
	}

	void AudioEngine::replaceDevice(CommandContext& context)
	{
		Json parameters = context.parameters();
		const bool remember = parameters.value("remember", false);
		const bool fallBack = parameters.value("fallBackToDummy", false);
		const bool running = m_device ? m_device->running() : m_lost && m_lost->restart;
		const bool start = parameters.value("start", running || m_startAfterFailure);
		// What the command does with the device, not what audio.reset opens it again with.
		for (const char* key : {"remember", "fallBackToDummy", "start"})
			parameters.erase(key);
		const std::string chosen = parameters.value("backend", std::string("dummy")); // as remember saves it
		if (chosen.empty())
		{
			const std::string platform = audio::defaultBackend();
			const bool has = std::any_of(m_backends.begin(), m_backends.end(),
				[&platform](const std::unique_ptr<audio::Backend>& backend) { return backend->name() == platform; });
			parameters["backend"] = has || m_backends.empty() ? platform : m_backends.front()->name();
		}
		const std::string backend = parameters.value("backend", std::string("dummy"));
		const std::string device = parameters.value("device", std::string());
		const uint64_t closed = m_closed;
		// A failure leaves no device, or one that doesn't run: the next device runs if this one was to, so that a
		// driver that failed in Studio Setup doesn't stop playback for good. And failed only describes the dummy
		// device that plays instead while it does.
		const auto failing = [this, start, closed]
		{
			m_startAfterFailure = start && !(m_device && m_device->running());
			if (m_closed != closed)
				m_failed = nullptr;
		};
		try
		{
			setDevice(parameters);
			if (start)
				startDevice();
		}
		catch (const std::exception& error)
		{
			if (!fallBack || backend == "dummy")
			{
				failing();
				throw;
			}
			const std::exception_ptr failure = std::current_exception();
			logging::warning("Cannot use the audio device ({} {}), playing on the dummy device instead: {}", backend,
				device, error.what());
			Json failed = {{"backend", backend}, {"device", device}, {"reason", error.what()}};
			closeDevice(); // one that opened but didn't start
			// The dummy device without capture memory, at the sample rate and block size asked for.
			Json dummy = {{"backend", "dummy"}, {"captureSeconds", 0}};
			if (parameters.contains("sampleRate"))
				dummy["sampleRate"] = parameters.at("sampleRate");
			if (parameters.value("blockSize", int64_t{0}) > 0)
				dummy["blockSize"] = parameters.at("blockSize");
			try
			{
				setDevice(dummy);
				if (start)
					startDevice();
			}
			catch (const std::exception& fallbackError)
			{
				logging::warning("Cannot play on the dummy device either: {}", fallbackError.what());
				failing();
				std::rethrow_exception(failure);
			}
			m_failed = std::move(failed);
			m_startAfterFailure = false;
			return;
		}
		m_failed = nullptr;
		m_startAfterFailure = false;
		if (remember)
			m_engine->setSettings(context, {{"audio.backend", chosen}, {"audio.device", device}});
	}

	void AudioEngine::setDevice(const Json& parameters, bool keepMeasurement)
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
			setDevice(atProjectRate, keepMeasurement);
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
		m_recorder.prepare(m_device->format().inputChannels, m_device->format().blockSize);
		m_loudness.prepare(m_device->format().sampleRate, keepMeasurement);
		m_recordSetup = {}; // its pre-record follows the device's sample rate
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
		std::string name = lost ? lost->name : m_device->info().name;
		if (name.empty()) // a driver that reports none: the status line shows something
			name = id;
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
			setDevice(parameters, true); // the loudness measurement goes on too
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
			m_lost = Lost{backend, parameters, id, name, error.what(), running};
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
		const Json& saved = m_settings.value("audio.portSettings");
		const auto list = [this, &saved](const std::vector<std::string>& names, bool input)
		{
			Json ports = Json::array();
			for (const std::string& name : names)
			{
				const Json setting = saved.value(portKey(input, name), Json::object());
				const std::string showAs = setting.value("showAs", std::string());
				ports.push_back({{"name", name}, {"showAs", showAs.empty() ? name : showAs},
					{"visible", setting.value("visible", true)}, {"busses", Json::array()}});
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

	Json AudioEngine::setPort(Engine& engine, CommandContext& context)
	{
		require(m_device != nullptr, "there is no audio device; set one with audio.setDevice");
		Json changes = context.parameters();
		const bool input = changes.at("input").get<bool>();
		const auto name = changes.at("name").get<std::string>();
		const std::vector<std::string>& names = input ? m_inputPorts : m_outputPorts;
		const auto found = std::find(names.begin(), names.end(), name);
		require(found != names.end(), std::string(input ? "no input port " : "no output port ") + name);
		changes.erase("input");
		changes.erase("name");
		// The engine applies the changed setting and sends audio.portsChanged (portSettingsChanged).
		engine.modifySetting(context, "audio.portSettings",
			[this, input, &name, &changes](const Json& value)
			{
				return settings::changeEntry(value, portKey(input, name), changes, {{"showAs", ""}, {"visible", true}});
			});
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
		const double projectRate = m_snapshots.latest().sampleRate;
		const std::string recordBitDepth = m_snapshots.latest().recordBitDepth;
		Json lost = nullptr;
		if (m_lost)
			lost = {{"backend", m_lost->backend}, {"device", m_lost->device}, {"name", m_lost->name},
				{"reason", m_lost->reason}, {"restart", m_lost->restart}};
		if (!m_device)
		{
			return {{"device", nullptr}, {"running", false}, {"position", 0}, {"projectSampleRate", projectRate},
				{"recordBitDepth", recordBitDepth}, {"capturedFrames", 0}, {"droppedFrames", 0},
				{"playbackDropouts", m_playback.dropouts()}, {"processing", processing}, {"lost", std::move(lost)},
				{"failed", m_failed}};
		}
		const audio::DeviceFormat& format = m_device->format();
		const audio::DeviceLatency latency = m_device->latency();
		Json device = toJson(m_device->info());
		device.update({{"backend", m_backend}, {"sampleRate", format.sampleRate}, {"blockSize", format.blockSize},
			{"inputChannels", format.inputChannels}, {"outputChannels", format.outputChannels},
			{"inputLatency", latency.input}, {"outputLatency", latency.output}, {"recordShift", m_recordShift},
			{"recordOffset", m_recordOffset.load(std::memory_order_relaxed)}, {"directMonitoring", m_directMonitoring},
			{"projectSampleRate", projectRate}, {"sampleRateMismatch", format.sampleRate != projectRate}});
		return {{"device", std::move(device)}, {"running", m_device->running()}, {"position", m_device->position()},
			{"projectSampleRate", projectRate}, {"recordBitDepth", recordBitDepth},
			{"capturedFrames", m_dummy != nullptr ? m_dummy->capturedFrames() : 0},
			{"droppedFrames", m_dummy != nullptr ? m_dummy->droppedFrames() : 0},
			{"playbackDropouts", m_playback.dropouts()}, {"processing", processing}, {"lost", std::move(lost)},
			{"failed", m_failed}};
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
		m_slots = std::make_unique<plugins::SlotInstances>(
			engine.pluginHosts(),
			[this]
			{
				if (!m_updatingPlugins.exchange(true))
					m_engine->post(
						"audio.updatePlugins", Json::object(), [this](const Result&) { m_updatingPlugins = false; });
			},
			[this](uint64_t generation)
			{ return m_started.load(std::memory_order_acquire) >= generation || !m_deviceRunning; });
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
				announceSlots(nullptr); // e.g. noDevice, if it closed the device
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
						"properties": {"processor": {"type": "string"}, "parameters": {"type": "object"}}},
					"start": {"type": "boolean"}, "fallBackToDummy": {"type": "boolean"},
					"remember": {"type": "boolean"}}})");
		engine.registerCommand(
			{"audio.setDevice", 1,
				"Replaces the audio device. backend: one from audio.devices, by default \"dummy\"; empty for the "
				"platform's default backend, whose default device the OS plays on (wasapi, coreaudio or pipewire), or "
				"the preferred one if this build lacks it. device: its id, by default the backend's default device. "
				"sampleRate: by default the project's if the device supports it, otherwise the driver's current rate; "
				"when the project's sample rate changes, the device follows it where it can. blockSize: by default "
				"the driver's preferred size. clockSource: a name from the device's clockSources. The device starts "
				"at position 0, running if start says so, by default if the device it replaces ran (or, lost, was to "
				"start again), or if the last audio.setDevice failed to run one it was to start. Another backend's "
				"device is closed before the new one opens, since drivers often allow only one; if opening fails, "
				"there is no device, and if starting fails, the device stays open and stopped. With fallBackToDummy, "
				"a device that can't be opened or started is replaced by the dummy device without capture memory, at "
				"the sampleRate and blockSize asked for, started if start says so, and failed in audio.get says what "
				"failed and why, until another device replaces that dummy device; the windowed app opens its device "
				"like that. remember: "
				"once the device runs as asked (not the fallback), saves backend and device as given as the user "
				"settings audio.backend and audio.device, which the windowed app opens when it starts, and sends "
				"settings.changed.\n"
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
				"Sends audio.changed, and plugin.changed for the slots whose plugin's status changed, e.g. to "
				"noDevice, also "
				"when it fails after closing the old device.",
				deviceSchema, statusSchema(), false},
			[this, announcingFailure](CommandContext& context)
			{
				announcingFailure([this, &context] { replaceDevice(context); });
				Json result = status();
				context.emit("audio.changed", result);
				announceSlots(&context);
				return result;
			});
		Json settingsSchema = deviceSchema;
		for (const char* key : {"backend", "device", "start", "fallBackToDummy", "remember"})
			settingsSchema.at("properties").erase(key);
		engine.registerCommand(
			{"audio.set", 1,
				"Changes the audio device's settings and keeps the others: the parameters of audio.setDevice except "
				"backend, device, start, fallBackToDummy and remember, e.g. sampleRate, blockSize, clockSource, "
				"recordShift or directMonitoring. The device opens again with them, as for audio.reset, and runs on if "
				"it ran. If it can't open with them, "
				"it opens again with the settings it had and the command fails; if those fail too, there is no device. "
				"Fails while a fallback plays for a lost device (see audio.reset). "
				"Sends audio.changed, and plugin.changed for the slots whose plugin's status changed, also when it "
				"fails "
				"after the device closed: with the device opened again with "
				"its old settings, at position 0, or with none.",
				settingsSchema, statusSchema(), false},
			[this, announcingFailure](CommandContext& context)
			{
				announcingFailure([this, &context] { set(context.parameters()); });
				Json result = status();
				context.emit("audio.changed", result);
				announceSlots(&context);
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
				"in the user setting audio.portSettings, so they come back with the device, also after a restart. "
				"Returns the port as audio.ports lists it. If that changes the setting, sends audio.ports as the event "
				"audio.portsChanged, as settings.set and settings.reset on audio.portSettings do.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["input", "name"],
					"properties": {"input": {"type": "boolean"}, "name": {"type": "string"},
					"showAs": {"type": "string"}, "visible": {"type": "boolean"}}})"),
				Json::parse(R"({"type": "object", "properties": {"name": {"type": "string"},
					"showAs": {"type": "string"}, "visible": {"type": "boolean"}, "busses": {"type": "array"}},
					"required": ["name", "showAs", "visible", "busses"]})"),
				false},
			[this, &engine](CommandContext& context) { return setPort(engine, context); });
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
				"sample rate, also in device, where sampleRateMismatch says that the device runs at another one, as "
				"it doesn't support the project's or was asked for another, and plays faster or slower; a new project "
				"sample rate sends audio.changed. recordBitDepth is the project's (see project.set), for the record "
				"format; a new one also sends audio.changed. lost: the device that went away (its backend, device id, "
				"its name "
				"while it was open, the reason it can't be opened and whether it ran and restarts), while the engine "
				"waits for it to return (see audio.reset); otherwise null. While a device is lost, device is the "
				"fallback that plays meanwhile (the user setting audio.fallbackDevice), or null.",
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
				"the measurement. Sends audio.changed when the device closes, opens or is lost, and plugin.changed for "
				"the "
				"slots whose plugin's status changed, also when it fails "
				"after the device opened again.",
				noParameters(), statusSchema(), false},
			[this, announcingFailure](CommandContext& context)
			{
				bool changed = false;
				announcingFailure([this, &changed] { changed = reset(); });
				Json result = status();
				if (changed)
					context.emit("audio.changed", result);
				announceSlots(&context);
				return result;
			});
		engine.registerCommand(
			{"audio.reconnect", 1,
				"Tries once to open the lost audio device again (lost in audio.get), as audio.reset does; does nothing "
				"unless a device is lost. While one is, the engine runs it on its own about every half second, one "
				"try at a time. While a fallback device plays, it tries only once the backend lists the lost device "
				"as available again, so that the fallback isn't interrupted, or every few seconds if the backend "
				"learns only by opening it whether it is there (ASIO). Tries that find the device still gone "
				"change nothing and are left out of engine.commandLog. Sends audio.changed when the device opens, and "
				"plugin.changed for the slots whose plugin's status changed.",
				noParameters(), statusSchema(), false},
			[this, announcingFailure](CommandContext& context)
			{
				bool changed = false;
				announcingFailure([this, &changed] { changed = reconnect(); });
				Json result = status();
				if (changed)
				{
					context.emit("audio.changed", result);
					announceSlots(&context);
				}
				else
				{
					context.leaveOutOfLog();
				}
				return result;
			});
		engine.registerCommand(
			{"audio.updatePlugins", 1,
				"Builds the processing graph again with the plugins of the slots as they are now. The engine runs it "
				"on "
				"its own when a plugin is ready after it was loaded or activated, failed, or asked to be restarted; a "
				"slot's audio passes its plugin unchanged until then. Emits plugin.changed for the slots whose "
				"plugin's status changed. Left out of engine.commandLog.",
				noParameters(), {{"type", "object"}}, false},
			[this](CommandContext& context)
			{
				// Reset before the rebuild: a plugin that gets ready during it posts again.
				m_updatingPlugins = false;
				if (m_device)
				{
					rebuild();
					announceSlots(&context);
				}
				context.leaveOutOfLog();
				return Json::object();
			});
		engine.registerCommand(
			{"audio.updateSamples", 1,
				"Hands the samples of sampler tracks that were read since to the samplers that wait for them. The "
				"engine reads a sampler's sample on a thread of its own when it changes or a project loads, and runs "
				"this on its own once the file is read; until then the sampler plays the sample before, or nothing. "
				"Starting playback and offline renders wait for the samples. Left out of engine.commandLog.",
				noParameters(), {{"type", "object"}}, false},
			[this](CommandContext& context)
			{
				// Reset first: a file read meanwhile posts again.
				m_updatingSamples = false;
				updateSamples();
				context.leaveOutOfLog();
				return Json::object();
			});
		engine.registerCommand(
			{"audio.simulateDeviceLoss", 1,
				"Only the dummy device: simulates its hardware being unplugged (connected false) and its driver "
				"asking for a reset, after which the device is lost (see audio.reset), or plugged in again (connected "
				"true), after which the engine opens it again on its own. While it is unplugged, audio.setDevice "
				"can't open it either. The dummy device as a fallback (audio.fallbackDevice) can't be unplugged: while "
				"it plays for another backend's lost device, connected false fails.",
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
				// Unplugged now, it would stay unplugged once the lost device returned, and audio.setDevice couldn't
				// open it.
				if (m_lost && m_lost->backend != "dummy")
					throw CommandError("the dummy device plays as the fallback for the lost device " + m_lost->name +
						" and can't be unplugged");
				m_dummyUnplugged = true;
				if (m_dummy != nullptr && !m_lost)
					m_dummy->requestReset(); // as a driver does whose device went away
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
				const audio::DummyDevice& device = requireDummy();
				require(!device.running(), "the audio device is running");
				render(static_cast<uint64_t>(frames));
				if (syncTransport()) // punched in or out, or stopped after a punch out
					context.emit("transport.changed", transportStatus());
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
				m_startAfterFailure = false;
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
				m_startAfterFailure = false;
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
		registerMixdownCommands(engine);
		registerMeterCommands(engine);
		registerTransportCommands(engine);
		registerArrangerCommands(engine);
		m_midi.registerCommands(engine);
		registerMidiTapCommands(engine);
		registerAuditionCommands(engine);
	}

	void AudioEngine::sent(bool accepted)
	{
		require(accepted, "the transport is busy; try again");
		if (!m_device || !m_device->running())
			m_transport.update(); // no audio thread applies the request
		m_transport.sync();
		const std::scoped_lock lock(m_cueMutex);
		m_cueState.playing = m_transport.playing();
		m_cueState.settings = m_transport.settings();
	}

	bool AudioEngine::syncTransport()
	{
		const ArrangerState arranger = m_transport.arranger(); // a new step or repeat is announced too
		const bool moved = !samePlace(arranger, m_announcedArranger);
		m_announcedArranger = arranger;
		if (!m_transport.sync())
			return moved;
		{
			const std::scoped_lock lock(m_cueMutex);
			m_cueState.playing = m_transport.playing();
		}
		if (m_device && m_directMonitoring) // as after a transport command
			routeDirectly(m_snapshots.latest(), devicePorts(m_snapshots.latest().busses, m_inputPorts, m_outputPorts));
		return true;
	}

	void AudioEngine::play()
	{
		// In arranger mode playback outside the chain's events starts at the chain's start.
		const ArrangerPlan& plan = *m_snapshots.latest().arranger;
		if (plan.plays() && !m_transport.playing())
		{
			const time::Samples at = m_transport.position();
			const bool inside = std::any_of(plan.steps.begin(), plan.steps.end(),
				[&](const ArrangerPlanStep& step)
				{
					const ArrangerSection& section = plan.sections.at(static_cast<size_t>(step.section));
					return section.start <= at && at < section.end;
				});
			if (!inside)
			{
				ArrangerState first;
				first.step = 0;
				arrange(first);
			}
		}
		if (m_device && !m_transport.playing())
		{
			cue(m_transport.startPosition(), false, m_device->format().blockSize, true, m_transport.playStart());
			// Only starting playback waits for the samplers' samples, not locating or a count-in (#1180).
			waitForSamples(g_startWait);
		}
		sent(m_transport.play());
	}

	graph::CountIn AudioEngine::countInFrom(time::Samples start, time::Samples recordStart) const
	{
		const RenderSnapshot& snapshot = m_snapshots.latest();
		const metronome::MetronomeSettings& settings = snapshot.metronome;
		graph::CountIn countIn;
		if (!settings.countIn)
			return countIn;
		const double rate = snapshot.sampleRate;
		const time::Ticks tick = snapshot.tempo.toTicks(start, rate);
		const time::Ticks at =
			snapshot.tempo.toTicks(recordStart, rate); // the signature and tempo where recording starts
		if (settings.countInSignature == metronome::CountInSignature::Project)
		{
			// From the start of the bar countInBars before the one playback starts in. A start that rounds to just
			// before a bar line is in the bar that starts there.
			int64_t bar = snapshot.signatures.toBarsBeats(tick).bar;
			if (snapshot.tempo.toSamples(snapshot.signatures.barStart(bar + 1), rate) <= start)
				++bar;
			countIn.projectTime = true;
			countIn.origin = snapshot.tempo.toSamples(snapshot.signatures.barStart(bar - settings.countInBars), rate);
			countIn.length = start - countIn.origin;
			return countIn;
		}
		const time::TimeSignature signature = settings.countInSignature == metronome::CountInSignature::Custom
			? settings.customSignature
			: snapshot.signatures.signatureAt(at);
		countIn.bpm = snapshot.tempo.bpmAt(at);
		countIn.numerator = signature.numerator;
		countIn.denominator = signature.denominator;
		const metronome::ConstantTimeline timeline{countIn.bpm, signature, rate};
		countIn.length = timeline.toSamples(timeline.barStart(settings.countInBars));
		return countIn;
	}

	graph::CountIn AudioEngine::cueCountIn()
	{
		const time::Samples from = m_transport.startPosition();
		if (m_device)
			cue(from, false, m_device->format().blockSize, true);
		const TransportSettings& settings = m_transport.settings();
		const time::Samples start =
			settings.startMode == StartMode::CycleStart ? settings.left : m_transport.position();
		return countInFrom(from, Transport::recordStart(settings, start, from));
	}

	void AudioEngine::record(bool on)
	{
		if (on && m_transport.countingIn())
			return; // already on its way
		sent(m_transport.record(on, on && !m_transport.playing() ? cueCountIn() : graph::CountIn{}));
	}

	void AudioEngine::setTransportSettings(const TransportSettings& settings)
	{
		const time::Samples before = m_transport.startPosition();
		sent(m_transport.set(settings));
		if (m_transport.countingIn() && m_transport.startPosition() != before)
			sent(m_transport.record(true, cueCountIn()));
	}

	Json AudioEngine::transportStatus() const
	{
		const TransportSettings& settings = m_transport.settings();
		return {{"playing", m_transport.playing()}, {"recording", m_transport.recording()},
			{"countingIn", m_transport.countingIn()}, {"position", m_transport.position()},
			{"start", m_transport.playbackStart()}, {"left", settings.left}, {"right", settings.right},
			{"cycle", settings.cycle},
			{"startMode", settings.startMode == StartMode::CycleStart ? "cycleStart" : "cursor"},
			{"returnToStart", settings.returnToStart}, {"punchIn", settings.punchIn}, {"punchOut", settings.punchOut},
			{"preRoll", settings.preRoll}, {"preRollAmount", settings.preRollAmount}, {"postRoll", settings.postRoll},
			{"postRollAmount", settings.postRollAmount}, {"metronome", settings.metronome},
			{"audioRecordMode", g_audioRecordModes.at(static_cast<size_t>(settings.audioRecordMode))},
			{"midiRecordMode", g_midiRecordModes.at(static_cast<size_t>(settings.midiRecordMode))},
			{"midiCycleRecordMode", g_midiCycleRecordModes.at(static_cast<size_t>(settings.midiCycleRecordMode))},
			{"lockRecord", settings.lockRecord}, {"arranger", arrangerStatus()}};
	}

	Json AudioEngine::arrangerStatus() const
	{
		const ArrangerPlan& plan = *m_snapshots.latest().arranger;
		const ArrangerState state = m_transport.arranger();
		const auto step = static_cast<size_t>(state.step);
		const int32_t section = state.step >= 0 && step < plan.steps.size() ? plan.steps[step].section : state.section;
		const auto event = section >= 0 && static_cast<size_t>(section) < plan.sections.size()
			? plan.sections[static_cast<size_t>(section)].event
			: uint64_t{0};
		return {{"step", state.step}, {"repeat", state.repeat}, {"event", event}};
	}

	void AudioEngine::arrange(const ArrangerState& state)
	{
		const ArrangerPlan& plan = *m_snapshots.latest().arranger;
		const int32_t section =
			state.step >= 0 ? plan.steps.at(static_cast<size_t>(state.step)).section : state.section;
		if (m_transport.playing() && plan.jump != ArrangerJump::None)
		{
			sent(m_transport.arrange(state, true));
			return;
		}
		locate({{"position", plan.sections.at(static_cast<size_t>(section)).start}});
		sent(m_transport.arrange(state, false));
	}

	void AudioEngine::registerArrangerCommands(Engine& engine)
	{
		const auto command = [this, &engine](const char* name, const char* description, const char* parameters,
								 std::function<void(const Json&)> handle)
		{
			engine.registerCommand({name, 1, description, Json::parse(parameters), transportSchema(), false},
				[this, handle = std::move(handle)](CommandContext& context)
				{
					handle(context.parameters());
					m_announcedArranger = m_transport.arranger();
					Json result = transportStatus();
					context.emit("transport.changed", result);
					return result;
				});
		};
		command("arranger.goToStep",
			"Arranger mode: plays the active chain from a step (0 for the first) and repeat (default 0): at once while "
			"stopped (the cursor goes to the step's event) or with the jump mode none, otherwise at the next jump "
			"point of the event playing (arranger.setMode jumpMode). Previous and Next Chain Step, First and Last "
			"Repeat, and a click in the chain list do this. Returns the transport's state.",
			R"({"type": "object", "additionalProperties": false, "required": ["step"],
				"properties": {"step": {"type": "integer"}, "repeat": {"type": "integer"}}})",
			[this](const Json& parameters)
			{
				const ArrangerPlan& plan = *m_snapshots.latest().arranger;
				require(plan.plays(), "arranger mode is off or the active chain is empty");
				ArrangerState state;
				state.step = parameters.at("step").get<int32_t>();
				state.repeat = parameters.value("repeat", 0);
				require(state.step >= 0 && static_cast<size_t>(state.step) < plan.steps.size(),
					"the active chain has no step " + std::to_string(state.step));
				const ArrangerPlanStep& step = plan.steps.at(static_cast<size_t>(state.step));
				require(state.repeat >= 0 && (step.mode == ArrangerRepeat::Forever || state.repeat < step.repeats),
					"step " + std::to_string(state.step) + " has " + std::to_string(step.repeats) + " repeats");
				arrange(state);
			});
		command("arranger.trigger",
			"Arranger mode, Jump Mode: plays an arranger event (event, its id) in a loop until another one is "
			"triggered or a step is chosen with arranger.goToStep: at once while stopped or with the jump mode none, "
			"otherwise at the next jump point of the event playing. Returns the transport's state.",
			R"({"type": "object", "additionalProperties": false, "required": ["event"],
				"properties": {"event": {"type": "integer"}}})",
			[this](const Json& parameters)
			{
				const ArrangerPlan& plan = *m_snapshots.latest().arranger;
				require(plan.plays(), "arranger mode is off or the active chain is empty");
				const auto id = parameters.at("event").get<uint64_t>();
				const auto found = std::find_if(plan.sections.begin(), plan.sections.end(),
					[id](const ArrangerSection& section) { return section.event == id; });
				require(found != plan.sections.end(), "no arranger event " + std::to_string(id));
				ArrangerState state;
				state.section = static_cast<int32_t>(found - plan.sections.begin());
				arrange(state);
			});
	}

	void AudioEngine::portSettingsChanged(CommandContext& context, bool audioPorts, bool midiPorts)
	{
		if (audioPorts)
			context.emit("audio.portsChanged", ports());
		if (midiPorts)
			m_midi.applySettings(context);
	}

	void AudioEngine::setStopAfterPunchOut(bool stop)
	{
		if (m_transport.settings().stopAfterPunchOut == stop)
			return;
		TransportSettings settings = m_transport.settings();
		settings.stopAfterPunchOut = stop;
		sent(m_transport.set(settings));
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
			require(position >= 0 && position <= g_maxPosition,
				"position must be 0 to " + std::to_string(g_maxPosition) + " samples");
		}
		else if (const auto by = parameters.find("by"); by != parameters.end())
		{
			const double seconds = by->get<double>();
			require(std::isfinite(seconds), "by must be a number of seconds");
			const double rate = m_snapshots.latest().sampleRate;
			// In doubles first: seconds times the rate may be past what an integer holds.
			const double target = static_cast<double>(m_transport.position()) + (seconds * rate);
			position =
				static_cast<time::Samples>(std::llround(std::clamp(target, 0.0, static_cast<double>(g_maxPosition))));
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
			throw CommandError("give a position, by or target");
		}
		// Exactly there, like a start; while playing, playback plays on from the cursor until the audio thread
		// takes the locate.
		if (m_device)
			cue(position, false, m_device->format().blockSize, true, -1,
				m_transport.playing() ? m_transport.position() : -1);
		sent(m_transport.locate(position));
		if (m_transport.countingIn()) // again, so that it ends on the bar line where playback now starts
			sent(m_transport.record(true, cueCountIn()));
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
			require(value >= 0 && value <= g_maxPosition,
				name + " must be 0 to " + std::to_string(g_maxPosition) + " samples");
		};
		locator("left", settings.left);
		locator("right", settings.right);
		settings.cycle = parameters.value("cycle", settings.cycle);
		settings.returnToStart = parameters.value("returnToStart", settings.returnToStart);
		settings.punchIn = parameters.value("punchIn", settings.punchIn);
		settings.punchOut = parameters.value("punchOut", settings.punchOut);
		settings.preRoll = parameters.value("preRoll", settings.preRoll);
		settings.postRoll = parameters.value("postRoll", settings.postRoll);
		for (const auto& [name, value] : {std::pair{"preRollAmount", &settings.preRollAmount},
				 std::pair{"postRollAmount", &settings.postRollAmount}})
			if (const auto found = parameters.find(name); found != parameters.end())
			{
				*value = found->get<time::Samples>();
				require(*value >= 0 && *value <= g_maxRollAmount,
					std::string(name) + " must be 0 to " + std::to_string(g_maxRollAmount) + " samples");
			}
		settings.metronome = parameters.value("metronome", settings.metronome);
		settings.lockRecord = parameters.value("lockRecord", settings.lockRecord);
		if (const auto mode = parameters.find("startMode"); mode != parameters.end())
		{
			const auto name = mode->get<std::string>();
			require(name == "cursor" || name == "cycleStart", "startMode must be cursor or cycleStart");
			settings.startMode = name == "cycleStart" ? StartMode::CycleStart : StartMode::Cursor;
		}
		if (const auto mode = parameters.find("audioRecordMode"); mode != parameters.end())
		{
			const auto name = mode->get<std::string>();
			const auto index = static_cast<size_t>(
				std::find(g_audioRecordModes.begin(), g_audioRecordModes.end(), name) - g_audioRecordModes.begin());
			require(index < g_audioRecordModes.size(),
				"audioRecordMode must be keepHistory, cycleHistoryReplace or replace");
			settings.audioRecordMode = static_cast<AudioRecordMode>(index);
		}
		if (const auto mode = parameters.find("midiRecordMode"); mode != parameters.end())
		{
			const auto name = mode->get<std::string>();
			const auto index = static_cast<size_t>(
				std::find(g_midiRecordModes.begin(), g_midiRecordModes.end(), name) - g_midiRecordModes.begin());
			require(index < g_midiRecordModes.size(), "midiRecordMode must be newParts, merge or replace");
			settings.midiRecordMode = static_cast<MidiRecordMode>(index);
		}
		if (const auto mode = parameters.find("midiCycleRecordMode"); mode != parameters.end())
		{
			const auto name = mode->get<std::string>();
			const auto index =
				static_cast<size_t>(std::find(g_midiCycleRecordModes.begin(), g_midiCycleRecordModes.end(), name) -
					g_midiCycleRecordModes.begin());
			require(index < g_midiCycleRecordModes.size(),
				"midiCycleRecordMode must be mix, overwrite, keepLast, stacked or mixStacked");
			settings.midiCycleRecordMode = static_cast<MidiCycleRecordMode>(index);
		}
		setTransportSettings(settings);
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
			"Starts playback from the project cursor, or from the left locator if startMode is cycleStart, less the "
			"pre-roll if preRoll is on (not before the project start). Does nothing while playing.",
			nullptr, [this](const Json&) { play(); });
		// Lock Record: a recording doesn't stop or punch out by these commands, only by a confirmed stop.
		const auto unlocked = [this]
		{
			// A count-in hasn't recorded anything yet.
			require(!m_transport.settings().lockRecord || !m_transport.recording() || m_transport.countingIn(),
				"recording is locked: stop it with transport.stop confirmed, or unlock it with transport.set "
				"lockRecord false first");
		};
		command("transport.stop",
			"Stops playback and recording. With returnToStart the cursor goes back to where playback started or was "
			"last located to. While lockRecord is on (see transport.set), a recording stops only with confirmed: the "
			"user confirmed it.",
			R"({"type": "object", "additionalProperties": false, "properties": {"confirmed": {"type": "boolean"}}})",
			[this, unlocked](const Json& parameters)
			{
				if (!parameters.value("confirmed", false))
					unlocked();
				sent(m_transport.stop());
			});
		command("transport.record",
			"Starts recording (record true, the default): playback starts as with transport.play if it stood, and "
			"recording where playback would start without the pre-roll; or recording punches in where it plays. With "
			"punchIn before the left locator, recording starts at the left locator instead. From stop, with the "
			"metronome's count-in on (metronome.set countIn), the metronome counts in first and playback starts at "
			"the exact sample where the count-in ends; transport.stop ends a count-in. record false punches out "
			"and playback goes on, refused while lockRecord is on. Record-enabled tracks may monitor their input "
			"meanwhile, by the user setting audio.autoMonitoring (see channel.set).",
			R"({"type": "object", "additionalProperties": false, "properties": {"record": {"type": "boolean"}}})",
			[this, unlocked](const Json& parameters)
			{
				const bool on = parameters.value("record", true);
				if (!on)
					unlocked();
				record(on);
			});
		command("transport.toggle",
			"Stops playback if it plays, otherwise starts it. With setting, switches that on or off instead: cycle, "
			"punchIn, punchOut, preRoll, postRoll, metronome (see transport.set), or record (on as transport.record, "
			"off punches out). While lockRecord is on, it neither stops nor punches out a recording.",
			R"({"type": "object", "additionalProperties": false, "properties": {"setting": {"type": "string",
				"enum": ["cycle", "punchIn", "punchOut", "preRoll", "postRoll", "metronome", "record"]}}})",
			[this, unlocked](const Json& parameters)
			{
				const std::string setting = parameters.value("setting", "");
				if (setting == "record")
				{
					const bool on = m_transport.recording() || m_transport.countingIn();
					if (on)
						unlocked();
					record(!on);
					return;
				}
				if (!setting.empty())
				{
					TransportSettings settings = m_transport.settings();
					bool* value = &settings.metronome;
					if (setting == "cycle")
						value = &settings.cycle;
					else if (setting == "punchIn")
						value = &settings.punchIn;
					else if (setting == "punchOut")
						value = &settings.punchOut;
					else if (setting == "preRoll")
						value = &settings.preRoll;
					else if (setting == "postRoll")
						value = &settings.postRoll;
					*value = !*value;
					setTransportSettings(settings);
					return;
				}
				if (m_transport.playing())
				{
					unlocked();
					sent(m_transport.stop());
				}
				else
				{
					play();
				}
			});
		command("transport.locate",
			"Moves the project cursor to a position in samples, by seconds from where it is (back if negative, not "
			"before the project start: the transport bar's rewind and forward), or to a target: projectStart, "
			"projectEnd (the end of the last event), leftLocator or rightLocator. While playing, playback continues "
			"from there.",
			R"({"type": "object", "additionalProperties": false, "properties": {"position": {"type": "integer",
				"minimum": 0, "maximum": 281474976710656}, "by": {"type": "number"}, "target": {"type": "string"}}})",
			[this](const Json& parameters) { locate(parameters); });
		command("transport.set",
			"Changes the given settings and keeps the rest. left and right: the locators, in samples, or \"cursor\" "
			"for the cursor position. cycle: cycle playback between the locators; with the right locator before the "
			"left one, playback skips the range instead. startMode: cursor or cycleStart (the left locator). "
			"returnToStart: on stop, return to where playback started. punchIn: playback that reaches the left "
			"locator starts recording there. punchOut: recording that reaches the right locator ends there and "
			"playback goes on, or stops with the user setting audio.stopAfterPunchOut, unless it cycles. preRoll and "
			"preRollAmount (samples, up to an hour at 768 kHz): playback starts that much before the cursor. postRoll "
			"and postRollAmount (the same): after an automatic punch out with audio.stopAfterPunchOut, playback goes "
			"on that much longer before it stops. Recording that "
			"punches in or out and playback that stops on their own announce transport.changed. metronome is the "
			"transport bar's switch for the metronome click (metronome.set). audioRecordMode: what a recording does to "
			"the "
			"events it overlaps on its track (see "
			"recording.add): keepHistory (the default), cycleHistoryReplace or replace. midiRecordMode: what a MIDI "
			"recording does to the MIDI parts it overlaps (see recording.add): newParts (the default), merge or "
			"replace. midiCycleRecordMode: what a MIDI cycle recording makes of its laps (see recording.add): mix "
			"(the default), overwrite, keepLast, stacked or mixStacked. lockRecord: Lock Record, which "
			"keeps a recording from stopping or punching out by accident: transport.stop stops it only with "
			"confirmed, transport.record and transport.toggle don't end it, and recording ignores the punch out at "
			"the right locator; it stays on until it is set off.",
			R"({"type": "object", "additionalProperties": false, "properties": {
				"left": {"type": ["integer", "string"], "maximum": 281474976710656},
				"right": {"type": ["integer", "string"], "maximum": 281474976710656},
				"cycle": {"type": "boolean"}, "startMode": {"type": "string"}, "returnToStart": {"type": "boolean"},
				"punchIn": {"type": "boolean"}, "punchOut": {"type": "boolean"}, "preRoll": {"type": "boolean"},
				"preRollAmount": {"type": "integer"}, "postRoll": {"type": "boolean"},
				"postRollAmount": {"type": "integer"}, "metronome": {"type": "boolean"},
				"audioRecordMode": {"type": "string"}, "midiRecordMode": {"type": "string"},
				"midiCycleRecordMode": {"type": "string"},
				"lockRecord": {"type": "boolean"}}})",
			[this](const Json& parameters) { setTransport(parameters); });
		command("transport.exchangeLocators", "Swaps the left and right locators.", nullptr,
			[this](const Json&)
			{
				TransportSettings settings = m_transport.settings();
				std::swap(settings.left, settings.right);
				sent(m_transport.set(settings));
			});
		engine.registerCommand(
			{"transport.update", 1,
				"Takes over what the audio thread changed on its own: recording that punched in or out at the "
				"locators, and playback that stopped after a punch out, and announces it with transport.changed. The "
				"engine runs it itself when the audio thread played or recorded differently or applied a request; "
				"runs that change nothing stay out of engine.commandLog.",
				noParameters(), transportSchema(), false},
			[this](CommandContext& context)
			{
				if (!syncTransport())
				{
					context.leaveOutOfLog(); // the engine's own run that changed nothing
					return transportStatus();
				}
				Json result = transportStatus();
				context.emit("transport.changed", result);
				return result;
			});
		engine.registerCommand(
			{"transport.get", 1,
				"Whether playback and recording run, the project cursor in samples, where the last playback started "
				"(start, in samples, without the pre-roll), the locators and the modes. "
				"After a locate the cursor is where it goes, even before the audio thread has applied it. countingIn: "
				"the metronome counts in before a recording from stop (see transport.record); meanwhile playing is "
				"true, and recording is whether recording starts where playback starts.",
				noParameters(), transportSchema(), true},
			[this](CommandContext&) { return transportStatus(); });
	}
}
