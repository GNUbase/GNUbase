// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/automation.h"
#include "engine/audio/midiports.h"
#include "engine/audio/miditap.h"
#include "engine/audio/performance.h"
#include "engine/audio/playback.h"
#include "engine/audio/sequencer.h"
#include "engine/audio/transport.h"
#include "engine/graph/graph.h"
#include "engine/schema.h"
#include "platform/audio/device.h"
#include "platform/audio/dummydevice.h"
#include "platform/rt/snapshotpublisher.h"
#include "platform/rt/triplebuffer.h"
#include "platform/rt/workerpool.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace gnubase::engine
{
	class Engine;
	struct RenderSnapshot;

	namespace settings
	{
		class Store;
	}

	// Plays the project through an audio device. On the command thread it compiles the processing graph for the
	// device's format whenever the project's render snapshot changes, and hands it to the audio thread. On the audio
	// thread it copies the device inputs into the input busses, processes the graph where the transport plays and
	// copies the output busses to the device outputs. Audio tracks play their events from disk streams (Playback):
	// offline rendering waits for the streams before every block; in real time a cue thread keeps them ahead. MIDI
	// and instrument tracks play their parts through sequencers, which last across rebuilds, as do instruments.
	//
	// Busses use device ports by name (docs/features/busses.md): the engine finds each bus channel's device channel
	// when it compiles the graph. Input busses copy their ports, output busses add to theirs in bus order; ports the
	// device doesn't have are silent, and so are device outputs no bus reaches.
	//
	// The graph runs on the audio thread and a pool of real-time workers (audio.setProcessing): by default half the
	// hardware threads, the audio thread included, and only workers that got real-time priority.
	//
	// The device comes from one of the audio backends (audio.devices, audio.setDevice). When its driver asks for a
	// reset, the engine opens it again with the same settings and restarts it if it ran (audio.reset); when it reports
	// a new latency, the engine reads it into the record offset (audio.updateLatency). The dummy
	// device also renders offline and captures its output; its input can be one of the built-in generators.
	//
	// Audio tracks hear their input bus while they monitor it: with the monitor button, or record-enabled by the user
	// setting audio.autoMonitoring and the transport's state (docs/features/latency-and-monitoring.md).
	class AudioEngine : public audio::AudioCallback
	{
	public:
		// The user settings are the engine's; only the command thread reads them.
		AudioEngine(rt::SnapshotPublisher<RenderSnapshot>& snapshots, const settings::Store& settings,
			std::vector<std::unique_ptr<audio::Backend>> backends,
			std::vector<std::unique_ptr<gnubase::midi::Backend>> midiBackends);
		~AudioEngine() override; // stops the device

		// Registers audio.devices, audio.setDevice, audio.ports, audio.setProcessing, audio.get, audio.performance,
		// audio.resetPerformance, audio.simulateOverload, audio.reset, audio.updateLatency, audio.controlPanel,
		// audio.render, audio.start, audio.stop, audio.capture and audio.captureInput, the transport commands
		// transport.play, transport.stop, transport.toggle, transport.record, transport.locate, transport.set,
		// transport.exchangeLocators and transport.get, and the MIDI port commands (MidiPorts). audio.analyze,
		// audio.compare and audio.record are in analysiscommands.cpp, midi.inject and midi.capture in miditap.cpp.
		void registerCommands(Engine& engine);

		// Command thread: what the MIDI and instrument tracks chase when playback jumps (MidiSequencer).
		void setChase(const MidiChase& chase);

		// Any thread but the audio thread, without waiting: the project cursor (Transport::position).
		time::Samples position() const { return m_transport.position(); }

		// Command thread: compiles a new graph if the project or the automatic monitoring mode changed since the last
		// one.
		void sync();

		// Audio thread.
		void process(const audio::DeviceBlock& block) noexcept override;

	private:
		// Where a bus node meets the device.
		struct BusLink
		{
			int node = 0;
			std::vector<int> ports; // the device channel each channel of the bus uses, or -1
		};

		// The notes of a MIDI track that left an instrument track, for the audio thread to end
		// (InstrumentChannel::releaseNotesOf).
		struct Release
		{
			std::shared_ptr<InstrumentChannel> channel;
			std::shared_ptr<MidiSequencer> departed;
			std::vector<std::shared_ptr<MidiSequencer>> staying; // the MIDI tracks still routed to the instrument
		};

		// What the audio thread runs: the graph and its links to the device, and the releases to do before its first
		// block. A program carries the releases of the programs before it until the audio thread started one of them,
		// as it may skip programs that follow each other quickly.
		struct Program
		{
			Program(const graph::GraphDesc& desc, double sampleRate, uint32_t maxFrames)
				: graph(desc, sampleRate, maxFrames)
			{
			}

			graph::CompiledGraph graph;
			std::vector<BusLink> inputs;
			std::vector<BusLink> outputs;
			time::TempoMap tempo{120.0}; // the project's, for the tempo in each transport block
			double sampleRate = 48000.0; // the project's: transport positions are in its samples
			uint64_t generation = 0;     // counts up from 1
			std::vector<Release> releases;
			MidiTap::Sequencers sequencers;   // of the MIDI and instrument channels, for midi.inject
			MidiTap::Instruments instruments; // for midi.capture
		};

		// A release waiting for the audio thread to start a program that has it.
		struct PendingRelease
		{
			ObjectId instrument = 0; // the instrument channel
			ObjectId source = 0;     // the MIDI channel that left it
			std::shared_ptr<InstrumentChannel> channel;
			std::shared_ptr<MidiSequencer> departed;
			uint64_t generation = 0; // the first program that has it
		};

		void rebuild();
		void setDevice(const Json& parameters);
		void openDummy(const Json& parameters);
		void closeDevice();
		void reset();
		// Opens the device again with the parameters, and starts it again if it was running.
		void reopen(const Json& parameters, bool running);
		void set(const Json& changes);              // audio.set
		int64_t recordOffset() const;               // from the device's latency and the record shift
		bool updateLatency(const Json& parameters); // audio.updateLatency; true if anything changed
		Json devices() const;
		Json ports() const;                   // audio.ports
		Json setPort(const Json& parameters); // audio.setPort
		// Where m_portSettings keeps a port of the device: its backend, device id, direction and name.
		std::string portKey(bool input, const std::string& name) const;
		void setProcessing(const Json& parameters);
		// Workers for the device's format and the processing settings, as many as get real-time priority. Throws
		// std::system_error if threads can't be started; the fallback processes on the audio thread only.
		void startWorkers();
		void startWorkersOrFallBack();
		size_t plannedThreads() const; // the setting, at most one per hardware thread
		static size_t defaultThreads();
		Json processingStatus() const;
		Json status() const;
		Json performance();
		Json resetPerformance();
		// Command thread, while no block runs: the next measurement starts from nothing.
		void clearPerformance();
		void publishPerformance(const PerformanceData& data);
		audio::DummyDevice& requireDummy();
		void registerTransportCommands(Engine& engine);
		void registerAnalysisCommands(Engine& engine); // analysiscommands.cpp
		void registerMidiTapCommands(Engine& engine);  // miditap.cpp
		void setTransport(const Json& parameters);
		void locate(const Json& parameters);
		void sent(bool accepted); // after a transport request
		void play();
		Json transportStatus() const;
		time::Samples projectEnd() const;
		void render(audio::DummyDevice& device, uint64_t frames);
		void cue(time::Samples position, bool playing, time::Samples window, bool wait);
		void startCueing();
		void stopCueing();
		void stopDevice();
		// Gives MIDI and instrument channels their sequencers and instruments, and notes the MIDI tracks that left an
		// instrument track for the program of the generation.
		void attachMidi(const RenderSnapshot& snapshot, graph::GraphDesc& desc, uint64_t generation);
		// The releases for a new program: those pending that the audio thread hasn't done yet.
		std::vector<Release> releases();
		// Puts every channel's processor into a channel strip with the channel's fader and automation.
		void attachStrips(const RenderSnapshot& snapshot, graph::GraphDesc& desc);
		AutoMonitoring autoMonitoring() const; // the user setting
		// Direct monitoring: audio tracks that play into an output bus hear their input bus through the device
		// instead of the graph. ports: the device channels of each bus (devicePorts).
		void monitorDirectly(
			const RenderSnapshot& snapshot, graph::GraphDesc& desc, const std::vector<std::vector<int>>& ports);
		// Routes the inputs of the tracks that monitor while the transport is as requested; again after every
		// transport command, since monitoring may follow the transport.
		void routeDirectly(const RenderSnapshot& snapshot, const std::vector<std::vector<int>>& ports);
		void stopMonitoringDirectly();
		// Audio thread: the dummy device's input where recording places it (audio.captureInput).
		void captureInput(const audio::DeviceBlock& block) noexcept;

		rt::SnapshotPublisher<RenderSnapshot>& m_snapshots;
		uint64_t m_revision = 0;
		const settings::Store& m_settings;
		AutoMonitoring m_autoMonitoring = AutoMonitoring::TapeMachine; // the graph's

		Engine* m_engine = nullptr;
		std::vector<std::unique_ptr<audio::Backend>> m_backends;
		std::string m_backend; // the device's
		Json m_parameters;     // what audio.setDevice opened the device with
		std::unique_ptr<audio::Device> m_device;
		audio::DummyDevice* m_dummy = nullptr; // m_device, if it is the dummy device
		std::vector<std::string> m_inputPorts; // the device's port names, each one once
		std::vector<std::string> m_outputPorts;
		// The user's names for ports and which ones they see (audio.setPort), by portKey, for every device opened.
		struct PortSettings
		{
			std::string showAs; // empty: the system name
			bool visible = true;
		};
		std::map<std::string, PortSettings> m_portSettings;

		// Latency compensation and monitoring (docs/features/latency-and-monitoring.md), set with the device.
		int64_t m_recordShift = 0;
		int64_t m_recordOffset = 0; // input + output latency - record shift, at least 0
		PlayedPositions m_played;   // audio thread
		bool m_directMonitoring = false;
		std::map<int, audio::InputMonitor> m_monitors; // by input port, those the device monitors directly
		// The dummy device's: per input channel, what it recorded at each project position from 0. Written by the
		// audio thread, read while the device stands.
		std::vector<std::vector<float>> m_capturedInput;
		Playback m_playback; // before m_programs: their track players use it

		struct Instrument
		{
			std::string id;             // the built-in instrument, or empty
			std::set<ObjectId> sources; // the MIDI channels routed to it
			std::shared_ptr<InstrumentChannel> channel;
		};
		std::map<ObjectId, std::shared_ptr<MidiSequencer>> m_sequencers;   // per MIDI and instrument channel
		MidiChase m_chase;                                                 // what they chase
		std::map<ObjectId, Instrument> m_instruments;                      // per instrument channel
		std::map<ObjectId, std::shared_ptr<ChannelState>> m_channelStates; // per channel
		std::vector<PendingRelease> m_releases;

		std::unique_ptr<rt::SnapshotPublisher<Program>> m_programs;
		uint64_t m_generation = 0;          // the newest program's
		std::atomic<uint64_t> m_started{0}; // the newest program the audio thread started
		uint64_t m_running = 0;             // audio thread: the program it runs
		Transport m_transport;              // advanced by the audio thread
		MidiPorts m_midi;
		audio::MidiBuffer m_midiInput; // the block's messages from all MIDI inputs, for MIDI recording (#97)
		MidiTap m_tap;                 // midi.inject and midi.capture

		// Changed only while the device is stopped.
		enum class Limit : uint8_t
		{
			None,
			Realtime, // workers didn't get real-time priority
			System,   // threads couldn't be started
		};
		bool m_multiProcessing = true;
		size_t m_threads = defaultThreads(); // processing threads, the audio thread included
		std::unique_ptr<rt::WorkerPool> m_workers;
		Limit m_limitedBy = Limit::None; // why m_workers has fewer than planned

		// Performance data: measured by the audio thread, read by the command thread.
		PerformanceMeter m_meter; // audio thread, or the command thread while the device is stopped
		rt::TripleBuffer<PerformanceData> m_performance;
		std::atomic<bool> m_resetPerformance{false};
		uint64_t m_deviceOverloadsBefore = 0; // the device's overloads when the measurement started

		// The cue thread, while the device runs in real time. It reads the transport's settings and play state from
		// m_cueState, which the command thread updates.
		struct CueState
		{
			bool playing = false;
			TransportSettings settings;
		};
		std::mutex m_cueMutex; // guards m_cueState
		CueState m_cueState;
		std::atomic<bool> m_cueing{false};
		std::thread m_cueThread;

		// The dummy device's input generator, if any; runs on the audio thread.
		std::shared_ptr<graph::Processor> m_generator;
		graph::EventBuffer m_generatorEvents;
		graph::EventBuffer m_generatorOut;
	};
}
