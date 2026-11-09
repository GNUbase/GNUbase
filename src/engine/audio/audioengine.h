// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/audition.h"
#include "engine/audio/automation.h"
#include "engine/audio/meters.h"
#include "engine/audio/midiports.h"
#include "engine/audio/miditap.h"
#include "engine/audio/performance.h"
#include "engine/audio/playback.h"
#include "engine/audio/recorder.h"
#include "engine/audio/sequencer.h"
#include "engine/audio/transport.h"
#include "engine/graph/graph.h"
#include "engine/metronome/metronome.h"
#include "engine/processors/midieffects.h"
#include "engine/schema.h"
#include "platform/audio/device.h"
#include "platform/audio/dummydevice.h"
#include "platform/rt/snapshotpublisher.h"
#include "platform/rt/triplebuffer.h"
#include "platform/rt/workerpool.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace gnubase::engine
{
	class CommandContext;
	class Engine;
	struct RenderSnapshot;

	namespace plugins
	{
		class SlotInstances;
	}

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
	// a new latency, the engine reads it into the record offset (audio.updateLatency). A device that can't be opened
	// again, e.g. because it was unplugged, is lost: the engine goes on without it, or on the fallback device the user
	// setting audio.fallbackDevice names, and tries again until it returns.
	// The device runs at the project's sample rate where it can (docs/features/audio-devices.md). The dummy
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
		// audio.resetPerformance, audio.simulateOverload, audio.simulateDeviceLoss, audio.reset, audio.reconnect,
		// audio.updateLatency, audio.updatePlugins,
		// audio.controlPanel, audio.render, audio.start, audio.stop, audio.capture and audio.captureInput, the
		// transport commands transport.play, transport.stop, transport.toggle, transport.record, transport.locate,
		// transport.set, transport.exchangeLocators and transport.get, and the MIDI port commands (MidiPorts).
		// audition.start, audition.scrub and audition.stop are in audition.cpp.
		// audio.analyze, audio.compare and audio.record are in analysiscommands.cpp, midi.inject and midi.capture in
		// miditap.cpp.
		void registerCommands(Engine& engine);

		// Command thread: the user setting audio.stopAfterPunchOut, for the transport.
		void setStopAfterPunchOut(bool stop);
		// The user setting meter.resetOnStart.
		void setMeterResetOnStart(bool reset) { m_meterResetOnStart.store(reset, std::memory_order_relaxed); }
		// The user settings meter.rmsResolution and meter.rmsAes17, for the master meter.
		void setMeterRms(int milliseconds, bool aes17) { m_mainMeter.setRms(milliseconds, aes17); }
		// The one reader that reads all meters once a frame (Engine::readMeters); not the audio thread.
		void readMeters(MeterFrame& frame); // metercommands.cpp
		// Command thread: what the MIDI and instrument tracks chase when playback jumps (MidiSequencer).
		void setChase(const MidiChase& chase);

		// Command thread, after a command changed the user settings audio.portSettings (audioPorts) or
		// midi.portSettings (midiPorts): emits the audio ports as audio.portsChanged, and the MIDI ports follow their
		// settings (MidiPorts::applySettings).
		void portSettingsChanged(CommandContext& context, bool audioPorts, bool midiPorts);

		// Any thread but the audio thread, without waiting: the project cursor (Transport::position).
		time::Samples position() const { return m_transport.position(); }

		// Command thread: opens the device again at a new project sample rate if it supports it, and compiles a new
		// graph if the project or the automatic monitoring mode changed since the last one.
		void sync();

		// Command thread: the status of a slot's plugin, as the plugin.* commands report it.
		std::string slotStatus(ObjectId slot) const;
		// Command thread: describes a slot for the plugin.changed that audio.updatePlugins and the device commands emit
		// when the status of its plugin changed (ProjectModel::describeSlot).
		void setSlotDescriber(std::function<Json(ObjectId slot)> describe) { m_describeSlot = std::move(describe); }
		// Command thread: where a recording that starts writes, in which format, and which tracks it records, from
		// the project; the audio engine adds the device inputs of each track's input bus and the transport's audio
		// record mode (docs/features/recording.md).
		void setRecordSetup(std::function<RecordSetup()> setup) { m_describeRecording = std::move(setup); }

		// Audio thread.
		void process(const audio::DeviceBlock& block) noexcept override;

	private:
		// Where a bus node meets the device.
		// The user settings audio.listenDim and audio.listenLevel, as linear gains.
		struct ListenGains
		{
			float dim = 0.0f;
			float level = 1.0f;

			bool operator==(const ListenGains& other) const { return dim == other.dim && level == other.level; }
			bool operator!=(const ListenGains& other) const { return !(*this == other); }
		};

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
		// block, then each sequencer's instrument channel (MidiSequencer::playOn). A program carries the releases of
		// the programs before it until the audio thread started one of them, as it may skip programs that follow each
		// other quickly.
		// A track's input transformer and the rules it applies; no processor for none.
		struct InputTransform
		{
			std::shared_ptr<processors::InputTransformerProcessor> processor;
			std::vector<midi::LogicalRules> rules;
		};

		struct Program
		{
			Program(const graph::GraphDesc& desc, double sampleRate, uint32_t maxFrames)
				: graph(desc, sampleRate, maxFrames)
			{
			}

			graph::CompiledGraph graph;
			std::vector<BusLink> inputs;
			std::vector<BusLink> outputs;
			time::TempoMap tempo{120.0};                  // the project's, for the tempo in each transport block
			std::shared_ptr<const ArrangerPlan> arranger; // what arranger mode plays
			double sampleRate = 48000.0;                  // the project's: transport positions are in its samples
			uint64_t generation = 0;                      // counts up from 1
			std::vector<Release> releases;
			MidiTap::Sequencers sequencers;     // of the MIDI and instrument channels, for midi.inject
			std::vector<uint64_t> destinations; // per sequencer, where it plays (MidiSequencer::playOn)
			std::vector<uint8_t> thru;          // per sequencer, 1 if it plays all MIDI inputs (MidiTap::thru)
			ChordPadKeys chordPadKeys;          // what MIDI inputs' notes on the chord pads' remote keys play
			// Per sequencer, its input transformer, which changes its input after the MIDI thru.
			std::vector<InputTransform> inputTransformers;
			MidiTap::Instruments instruments; // for midi.capture
			std::vector<int> mainPorts;       // the main mix's device channels, for auditioning and listening
			int mainOutput = -1;              // the main mix's index in outputs, or -1
			int listenNode = -1;              // the listen bus (graph::NodeKind::ListenBus), or -1
			ListenGains listen;
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
		// audio.setDevice: opens the device, starts it if asked or if the one it replaces ran, falls back to the dummy
		// device and saves the choice as the user's if asked (start, fallBackToDummy, remember).
		void replaceDevice(CommandContext& context);
		// keepMeasurement: a reset, which keeps the loudness measurement if the sample rate stays.
		void setDevice(const Json& parameters, bool keepMeasurement = false);
		void openDummy(const Json& parameters);
		void closeDevice();
		// audio.reset; a device that can't be opened is lost. While one is, opens the lost device instead of the
		// fallback that plays. True if the status changed.
		bool reset();
		// audio.reconnect: while a fallback plays, only once the lost device is listed as available again, so that
		// tries don't interrupt the fallback, or every g_uncertainReconnectInterval while it is listed as unavailable
		// by a backend that doesn't know whether it is there. True if the status changed.
		bool reconnect();
		// Whether the lost device is available again, as far as its backend's device list says: Maybe if it is listed
		// as unavailable by a backend whose list doesn't say whether it is there (Backend::listsPresence).
		enum class Listed : uint8_t
		{
			No,
			Yes,
			Maybe,
		};
		Listed lostDeviceListed() const;
		// Opens the fallback device for the lost device (audio.fallbackDevice) and starts it if the lost one ran.
		void failOver();
		// Starts trying to open the lost device again, every g_reconnectInterval, with audio.reconnect: one try at a
		// time, so that tries don't queue up behind a long command and reset the device again once it is back.
		void waitForDevice();
		// Opens the device again with the parameters, and starts it again if it was running.
		void reopen(const Json& parameters, bool running);
		void set(const Json& changes);              // audio.set
		int64_t recordOffset() const;               // from the device's latency and the record shift
		bool updateLatency(const Json& parameters); // audio.updateLatency; true if anything changed
		Json devices() const;
		Json ports() const;                                    // audio.ports
		Json setPort(Engine& engine, CommandContext& context); // audio.setPort
		// Where the user setting audio.portSettings keeps a port of the device: its backend, device id, direction and
		// name.
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
		Json performance(uint64_t deviceOverloads); // with what the device's overloads() returned
		Json resetPerformance();
		// Command thread, while no block runs: the next measurement starts from nothing.
		void clearPerformance();
		void publishPerformance(const PerformanceData& data);
		audio::DummyDevice& requireDummy();
		void registerTransportCommands(Engine& engine);
		void registerArrangerCommands(Engine& engine); // arranger.goToStep and arranger.trigger (#167)
		// Arranger playback: makes state the one playing, from the start of its step or event, or while playing with a
		// jump mode lets it take over at the next jump point.
		void arrange(const ArrangerState& state);
		void registerAnalysisCommands(Engine& engine); // analysiscommands.cpp
		void registerMeterCommands(Engine& engine);    // metercommands.cpp
		void registerMixdownCommands(Engine& engine);  // mixdown.cpp
		void registerMidiTapCommands(Engine& engine);  // miditap.cpp
		void registerAuditionCommands(Engine& engine); // audition.cpp
		void setTransport(const Json& parameters);
		void locate(const Json& parameters);
		void sent(bool accepted); // after a transport request
		bool syncTransport();     // Transport::sync, and the cue thread's copy; whether anything changed
		void play();
		void record(bool on);
		// Cues where a recording from stop starts playback and returns its count-in (length 0 for none).
		graph::CountIn cueCountIn();
		// New transport settings; a count-in that runs starts again if they move where playback starts.
		void setTransportSettings(const TransportSettings& settings);
		// The count-in before a recording from stop, as the metronome's settings have it, or length 0 for none: it ends
		// where playback starts (start, the pre-roll's start), in the signature and tempo where recording starts.
		graph::CountIn countInFrom(time::Samples start, time::Samples recordStart) const;
		Json transportStatus() const;
		Json arrangerStatus() const; // transport.get's arranger: step, repeat and the event playing
		time::Samples projectEnd() const;
		// Command thread, while the device stands: processes frames frames offline, as fast as possible, calling
		// rendered after each block with its frames. The dummy device renders them; any other device's blocks are
		// processed here, with silent inputs.
		void render(uint64_t frames, const std::function<void(uint32_t)>& rendered = {});
		void cue(time::Samples position, bool playing, time::Samples window, bool wait, time::Samples enter = -1);
		void startCueing();
		void stopCueing();
		void startDevice();
		void stopDevice();
		// Gives MIDI and instrument channels their sequencers and instruments, and notes the MIDI tracks that left an
		// instrument track for the program of the generation.
		void attachMidi(const RenderSnapshot& snapshot, graph::GraphDesc& desc, uint64_t generation);
		// The releases for a new program: those pending that the audio thread hasn't done yet.
		std::vector<Release> releases();
		// Puts every channel's processor into a channel strip with the channel's fader and automation, and the
		// plugins of its slots into slot strips that get their automation; after attachSlots.
		void attachStrips(const RenderSnapshot& snapshot, graph::GraphDesc& desc);
		// Adds the metronome's node, always, with its audio and MIDI routing, and compiles what it plays
		// when that changed.
		void attachMetronome(const RenderSnapshot& snapshot, graph::GraphDesc& desc);
		// Brings the slots' plugins in line with the snapshot and gives the slot nodes those that are ready.
		void attachSlots(const RenderSnapshot& snapshot, graph::GraphDesc& desc);
		// Emits plugin.changed for the slots whose plugin's status changed since the last call, e.g. to or from
		// noDevice when the device closed or opened. Without a context, as for a failed command, every client gets it
		// at once.
		void announceSlots(CommandContext* context);
		// The node that plays what a channel plays in the graph rebuild describes: its source node if it has plugin
		// slots (graph::describe), else its own; -1 for none.
		int sourceNode(ObjectId channel) const;
		AutoMonitoring autoMonitoring() const; // the user setting
		ListenGains listenGains() const;       // the user settings
		// Audio thread: adds the outputs to the device, the main mix turned down while channels listen, and the
		// listening channels to the main mix's ports.
		void output(const audio::DeviceBlock& block, const Program& program) noexcept;
		// Direct monitoring: audio tracks that play into an output bus hear their input bus through the device
		// instead of the graph. ports: the device channels of each bus (devicePorts).
		void monitorDirectly(
			const RenderSnapshot& snapshot, graph::GraphDesc& desc, const std::vector<std::vector<int>>& ports);
		// Routes the inputs of the tracks that monitor while the transport is as requested; again after every
		// transport command, since monitoring may follow the transport.
		void routeDirectly(const RenderSnapshot& snapshot, const std::vector<std::vector<int>>& ports);
		void stopMonitoringDirectly();
		// Audio thread: the master meter and the loudness meter on the main mix's device channels.
		void meterMainMix(const audio::DeviceBlock& block, const std::vector<int>& ports, bool playing) noexcept;
		// Audio thread: the device's input where recording places it (audio.captureInput).
		void captureInput(const audio::DeviceBlock& block) noexcept;
		// Command thread: tells the recorder what the next recording records, if that changed.
		void updateRecorder();

		rt::SnapshotPublisher<RenderSnapshot>& m_snapshots;
		uint64_t m_revision = 0;
		const settings::Store& m_settings;
		AutoMonitoring m_autoMonitoring = AutoMonitoring::TapeMachine; // the graph's
		ListenGains m_listenGains;                                     // the graph's
		bool m_mutePreSends = false;                                   // the graph's audio.mutePreSends
		float m_dim = 1.0f; // audio thread: the main mix's gain at the end of the last block

		Engine* m_engine = nullptr;
		std::vector<std::unique_ptr<audio::Backend>> m_backends;
		std::string m_backend; // the device's
		Json m_parameters;     // what audio.setDevice opened the device with
		// The device the last audio.setDevice couldn't open or start, so the dummy device plays instead
		// (fallBackToDummy): its backend, id and the reason. Null if it opened.
		Json m_failed = nullptr;
		// The last audio.setDevice failed and left no running device although it was to run one: the next one
		// starts unless told otherwise. audio.start and audio.stop decide anew.
		bool m_startAfterFailure = false;
		std::unique_ptr<audio::Device> m_device;
		audio::DummyDevice* m_dummy = nullptr; // m_device, if it is the dummy device
		std::vector<std::string> m_inputPorts; // the device's port names, each one once
		std::vector<std::string> m_outputPorts;
		double m_projectRate = 0.0;   // the project's sample rate the device last followed
		std::string m_recordBitDepth; // the project's record bit depth last announced; empty before the first sync

		// The device that couldn't be opened again (audio.reset), while the engine waits for it to return. A device
		// that plays meanwhile is the fallback (failOver).
		struct Lost
		{
			std::string backend;
			Json parameters;    // what audio.setDevice opened it with
			std::string device; // its id
			std::string name;   // its name while it was open
			std::string reason;
			bool restart = false; // it ran, or the fallback runs
		};
		std::optional<Lost> m_lost;
		bool m_dummyUnplugged = false; // audio.simulateDeviceLoss
		bool m_failingOver = false;    // the fallback dummy device can't be unplugged
		// When reconnect next tries a Listed::Maybe device while a fallback plays.
		std::chrono::steady_clock::time_point m_nextUncertainTry;
		// The thread that tries to open a lost device again, started with the first loss.
		std::mutex m_reconnectMutex; // guards m_stopReconnecting
		bool m_stopReconnecting = false;
		std::atomic<bool> m_waitingForDevice{false}; // m_lost, for that thread
		std::atomic<bool> m_reconnecting{false};     // a try it posted hasn't run yet
		std::condition_variable m_reconnectWake;
		uint64_t m_closed = 0; // counts the devices closed and the losses ended, to announce what a failed command did
		std::thread m_reconnectThread;

		// Latency compensation and monitoring (docs/features/latency-and-monitoring.md), set with the device.
		int64_t m_recordShift = 0;
		// input + output latency - record shift, from 0 to g_maxRecordOffset. Written by the command thread, taken by
		// the audio thread at the start of each block.
		std::atomic<int64_t> m_recordOffset{0};
		PlayedPositions m_played; // audio thread; for any record offset
		bool m_directMonitoring = false;
		std::map<int, audio::InputMonitor> m_monitors; // by input port, those the device monitors directly
		// Per input channel of the device, what it recorded at each project position from 0, for captureSeconds.
		// Written by the audio thread, read while the device stands.
		std::vector<std::vector<float>> m_capturedInput;
		Recorder m_recorder;                              // records the record-enabled audio tracks
		RecordSetup m_recordSetup;                        // what it was told last
		std::function<RecordSetup()> m_describeRecording; // setRecordSetup
		Playback m_playback;                              // before m_programs: their track players use it
		Audition m_audition;
		std::map<ObjectId, Playback::Track> m_auditionTracks; // rebuild: each audio track's channel and playlist
		std::map<ObjectId, std::string> m_mediaPaths;         // rebuild: by media

		struct Instrument
		{
			std::string id;             // the built-in instrument, or empty
			std::set<ObjectId> sources; // the MIDI channels routed to it
			std::shared_ptr<InstrumentChannel> channel;
		};
		std::map<ObjectId, std::shared_ptr<MidiSequencer>> m_sequencers; // per MIDI and instrument channel
		// Per MIDI and instrument channel with an input transformer.
		std::map<ObjectId, std::shared_ptr<processors::InputTransformerProcessor>> m_inputTransformers;
		MidiChase m_chase;                                                 // what they chase
		std::map<ObjectId, Instrument> m_instruments;                      // per instrument channel
		std::map<ObjectId, std::shared_ptr<ChannelState>> m_channelStates; // per channel
		std::mutex m_channelStatesMutex; // the command thread changes m_channelStates with it, readMeters reads with it
		// The channels that mixdown.export renders from a point inside their strip while it renders, and where their
		// audio goes; attachStrips taps their strips.
		struct RenderTap
		{
			RenderPoint point = RenderPoint::Strip;
			std::shared_ptr<PreFaderTap> tap;
		};
		std::map<ObjectId, RenderTap> m_renderTaps;
		std::vector<PendingRelease> m_releases;
		uint64_t m_instrumentSerial = 0; // the last InstrumentChannel::serial given, counting up from 1

		// The metronome lasts across rebuilds; its sounds are compiled again only when they change.
		struct ClickSource
		{
			metronome::MetronomeSettings settings;
			time::TempoMap tempo{120.0};
			time::SignatureMap signatures;
			double projectRate = 0.0;
			double deviceRate = 0.0;
			std::array<metronome::SoundLibrary::Stamp, metronome::g_accents> samples; // the files as they were
		};
		std::shared_ptr<metronome::Metronome> m_metronome = std::make_shared<metronome::Metronome>();
		metronome::SoundLibrary m_clickSounds;
		std::optional<ClickSource> m_clickSource; // what m_metronome's config was compiled from

		std::unique_ptr<rt::SnapshotPublisher<Program>> m_programs;
		uint64_t m_generation = 0;          // the newest program's
		std::atomic<uint64_t> m_started{0}; // the newest program the audio thread started
		uint64_t m_running = 0;             // audio thread: the program it runs
		std::atomic<bool> m_deviceRunning{false};
		// The plugins of the project's slots. After m_started and m_programs: it waits for jobs that read the former,
		// and destroys the plugins before the programs that hold their slots go.
		std::unique_ptr<plugins::SlotInstances> m_slots;
		std::atomic<bool> m_updatingPlugins{false}; // an audio.updatePlugins it posted hasn't run yet
		std::function<Json(ObjectId)> m_describeSlot;
		std::map<ObjectId, std::string> m_slotStatuses; // as announced last
		bool m_slotsHadDevice = false;                  // whether a device was open when they were announced last
		std::map<ObjectId, int> m_sourceNodes;          // rebuild: per channel
		Transport m_transport;                          // advanced by the audio thread
		MidiPorts m_midi;
		audio::MidiBuffer m_midiInput; // the block's messages from all MIDI inputs, for MIDI thru (MidiTap::thru)
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
		// The main mix as the device plays it (docs/features/metering.md).
		LevelMeter m_mainMeter;
		LoudnessMeter m_loudness;
		bool m_meterPlaying = false; // audio thread: whether the last block played, for meter.resetOnStart
		std::atomic<bool> m_meterResetOnStart{false};
		rt::TripleBuffer<PerformanceData> m_performance;
		std::atomic<bool> m_resetPerformance{false};
		uint64_t m_deviceOverloadsBefore = 0; // the device's overloads when the measurement started
		uint64_t m_deviceOverloadsKept = 0;   // the dropouts of the devices before an audio.reset

		// The cue thread, while the device runs in real time. It reads the transport's settings and play state from
		// m_cueState, which the command thread updates.
		struct CueState
		{
			bool playing = false;
			TransportSettings settings;
			std::shared_ptr<const ArrangerPlan> arranger = std::make_shared<const ArrangerPlan>();
		};
		std::mutex m_cueMutex;             // guards m_cueState
		ArrangerState m_announcedArranger; // as transport.changed last announced it
		CueState m_cueState;
		std::atomic<bool> m_cueing{false};
		std::thread m_cueThread;

		// The dummy device's input generator, if any; runs on the audio thread.
		std::shared_ptr<graph::Processor> m_generator;
		graph::EventBuffer m_generatorEvents;
		graph::EventBuffer m_generatorOut;
	};
}
