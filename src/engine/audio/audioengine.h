// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/playback.h"
#include "engine/audio/sequencer.h"
#include "engine/audio/transport.h"
#include "engine/graph/graph.h"
#include "engine/schema.h"
#include "platform/audio/device.h"
#include "platform/audio/dummydevice.h"
#include "platform/rt/snapshotpublisher.h"
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

	// Plays the project through an audio device. On the command thread it compiles the processing graph for the
	// device's format whenever the project's render snapshot changes, and hands it to the audio thread. On the audio
	// thread it copies the device inputs into the input busses, processes the graph where the transport plays and
	// copies the output busses to the device outputs. Audio tracks play their events from disk streams (Playback):
	// offline rendering waits for the streams before every block; in real time a cue thread keeps them ahead. MIDI
	// and instrument tracks play their parts through sequencers, which last across rebuilds, as do instruments.
	//
	// Busses take consecutive device channels in bus order: the first input bus the first input channels, and so on.
	// Bus channels beyond the device's are silent, device outputs no bus reaches stay silent. Choosing device ports
	// per bus comes with the hardware backends (M03).
	//
	// The graph runs on the audio thread and a pool of real-time workers (audio.setProcessing): by default one thread
	// per hardware thread, the audio thread included.
	//
	// For now the only device is the dummy device (audio.setDevice). Its input can be one of the built-in generators.
	class AudioEngine : public audio::AudioCallback
	{
	public:
		explicit AudioEngine(rt::SnapshotPublisher<RenderSnapshot>& snapshots);
		~AudioEngine() override; // stops the device

		// Registers audio.setDevice, audio.setProcessing, audio.get, audio.render, audio.start, audio.stop and
		// audio.capture, and the transport commands transport.play, transport.stop, transport.toggle,
		// transport.locate, transport.set, transport.exchangeLocators and transport.get.
		void registerCommands(Engine& engine);

		// Command thread: compiles a new graph if the project changed since the last one.
		void sync();

		// Audio thread.
		void process(const audio::DeviceBlock& block) noexcept override;

	private:
		// Where a bus node meets the device.
		struct BusLink
		{
			int node = 0;
			int firstChannel = 0; // the device channel the bus's first channel uses
			int channels = 0;
		};

		// What the audio thread runs: the graph and its links to the device.
		struct Program
		{
			Program(const graph::GraphDesc& desc, double sampleRate, uint32_t maxFrames)
				: graph(desc, sampleRate, maxFrames)
			{
			}

			graph::CompiledGraph graph;
			std::vector<BusLink> inputs;
			std::vector<BusLink> outputs;
		};

		void rebuild();
		void setDevice(const Json& parameters);
		void setProcessing(const Json& parameters);
		void startWorkers(); // for the device's format and the processing settings
		Json status() const;
		audio::DummyDevice& requireStopped();
		void registerTransportCommands(Engine& engine);
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
		// Gives MIDI and instrument channels their sequencers and instruments.
		void attachMidi(const RenderSnapshot& snapshot, graph::GraphDesc& desc);

		rt::SnapshotPublisher<RenderSnapshot>& m_snapshots;
		uint64_t m_revision = 0;

		std::unique_ptr<audio::DummyDevice> m_device;
		Playback m_playback; // before m_programs: their track players use it

		struct Instrument
		{
			std::string id;             // the built-in instrument, or empty
			std::set<ObjectId> sources; // the MIDI channels routed to it
			std::shared_ptr<InstrumentChannel> channel;
		};
		std::map<ObjectId, std::shared_ptr<MidiSequencer>> m_sequencers; // per MIDI and instrument channel
		std::map<ObjectId, Instrument> m_instruments;                    // per instrument channel

		std::unique_ptr<rt::SnapshotPublisher<Program>> m_programs;
		Transport m_transport; // advanced by the audio thread

		// Changed only while the device is stopped.
		bool m_multiProcessing = true;
		size_t m_threads = rt::hardwareThreads(); // processing threads, the audio thread included
		std::unique_ptr<rt::WorkerPool> m_workers;

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
