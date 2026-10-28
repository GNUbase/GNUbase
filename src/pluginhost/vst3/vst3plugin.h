// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/rt/spscqueue.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"
#include "pluginhost/vst3/hostobjects.h"
#include "pluginhost/vst3/vst3ns.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/vst/hosting/module.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gnubase::pluginhost::vst3
{
	// One VST3 audio module class as a GNUbase plugin (docs/plugin-api.md, "Format mapping"):
	// - init creates the component and its controller (a separate class, or the component itself), connects them and
	//   gives the controller the component's state; activate sets up processing, activates the buses and the component.
	// - Values are normalized, as VST3's: 0 to 1, or 0 to the step count for stepped parameters. Host changes become
	//   points of the parameter queues at their offsets; the controller learns them on the main thread.
	// - The editor's edits (beginEdit, performEdit, endEdit) become output events and go to the processor in the
	//   next block, or with flushParams while it doesn't process.
	// - Notes, expressions, MIDI 1.0 and MIDI 2.0 become VST3 events; controllers, pitch bend and channel pressure
	//   become parameter changes through the plugin's MIDI mapping.
	// - Blocks are split where the transport changes, so every sub-block gets its own process context.
	// - The state is a .vstpreset with the component's and the controller's state.
	class Vst3Plugin final : public plugin::Plugin,
							 public plugin::AudioPorts,
							 public plugin::ConfigurableAudioPorts,
							 public plugin::AudioPortsActivation,
							 public plugin::NotePorts,
							 public plugin::Params,
							 public plugin::State,
							 public plugin::Presets,
							 public plugin::Latency,
							 public plugin::Tail,
							 public plugin::NoteNames,
							 private ComponentHandlerListener
	{
	public:
		Vst3Plugin(VST3::Hosting::Module::Ptr module, const VST3::UID& classId, plugin::Descriptor descriptor,
			plugin::Host& host);
		~Vst3Plugin() override;
		Vst3Plugin(const Vst3Plugin&) = delete;
		Vst3Plugin& operator=(const Vst3Plugin&) = delete;

		// Plugin
		const plugin::Descriptor& descriptor() const override { return m_descriptor; }
		bool init() override;
		bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) override;
		void deactivate() override;
		bool startProcessing() noexcept override;
		void stopProcessing() noexcept override;
		void reset() noexcept override;
		plugin::ProcessStatus process(const plugin::Process& process) noexcept override;
		void* extension(std::string_view id) override;
		void onMainThread() override;

		// AudioPorts
		uint32_t audioPortCount(bool input) const override;
		bool audioPortInfo(bool input, uint32_t index, plugin::AudioPortInfo& info) const override;
		// ConfigurableAudioPorts
		bool applyAudioPortRequests(const std::vector<plugin::AudioPortRequest>& requests) override;
		// AudioPortsActivation
		bool setAudioPortActive(bool input, uint32_t index, bool active) override;
		// NotePorts
		uint32_t notePortCount(bool input) const override;
		bool notePortInfo(bool input, uint32_t index, plugin::NotePortInfo& info) const override;

		// Params
		uint32_t paramCount() const override;
		bool paramInfo(uint32_t index, plugin::ParamInfo& info) const override;
		bool paramValue(uint32_t param, double& value) const override;
		bool paramValueToText(uint32_t param, double value, std::string& text) const override;
		bool paramTextToValue(uint32_t param, std::string_view text, double& value) const override;
		void flushParams(const plugin::EventList& in, plugin::EventList& out) noexcept override;

		// State
		bool saveState(std::vector<uint8_t>& data, plugin::StateContext context) override;
		bool loadState(const uint8_t* data, size_t size, plugin::StateContext context) override;
		// Presets
		std::vector<plugin::PresetInfo> presets() override;
		bool loadPreset(const plugin::PresetInfo& preset) override;
		// Latency, Tail
		uint32_t latency() const override;
		uint32_t tail() const noexcept override;
		// NoteNames
		std::vector<plugin::NoteName> noteNames() override;

	private:
		struct Bus
		{
			std::string name;
			uint32_t channelCount = 0;
			vst::Vst::SpeakerArrangement arrangement = 0;
			bool main = false;
			bool defaultActive = false;
			bool active = false;
		};

		struct Param
		{
			plugin::ParamInfo info;
			vst::int32 stepCount = 0;
			vst::Vst::UnitID unit = vst::Vst::kRootUnitId;
			bool programChange = false;
		};

		// A change for the processor or the host, from the main thread: an edit in the editor (with output events),
		// or a value the processor has to learn (without).
		struct Edit
		{
			enum Kind : uint8_t
			{
				Begin,
				Value,
				End,
				Silent, // the value for the processor only
			};

			Kind kind = Value;
			vst::Vst::ParamID id = 0;
			double value = 0.0; // normalized
		};

		// A value for the controller, from the audio thread.
		struct Update
		{
			vst::Vst::ParamID id = 0;
			double value = 0.0;
		};

		// A note the plugin plays, so expressions and note offs without a note id find its id.
		struct Voice
		{
			int16_t port = -1;
			int16_t channel = -1;
			int16_t key = -1;
			int32_t id = -1;
		};

		// An output of a sub-block, in order of its offset: an event, or an index into the VST3 output events.
		struct Output
		{
			plugin::Event event;
			int32_t sysex = -1; // index of a data event in m_outEvents
		};

		// The output events of a sub-block. Memory is allocated once, so adding and clearing are real-time safe.
		class OutputList
		{
		public:
			explicit OutputList(size_t capacity) : m_items(capacity) {}

			// Drops the output if the list is full.
			void push(const Output& output) noexcept
			{
				if (m_size < m_items.size())
					m_items[m_size++] = output;
			}
			void clear() noexcept { m_size = 0; }
			size_t size() const noexcept { return m_size; }
			Output& operator[](size_t index) noexcept { return m_items[index]; }
			const Output* begin() const noexcept { return m_items.data(); }
			const Output* end() const noexcept { return m_items.data() + m_size; }

		private:
			std::vector<Output> m_items;
			size_t m_size = 0;
		};

		// ComponentHandlerListener
		bool editBegin(uint32_t id) override;
		bool editValue(uint32_t id, double normalized) override;
		bool editEnd(uint32_t id) override;
		bool restart(int32_t flags) override;
		void stateDirty() override;
		void programListChanged() override;

		void terminate();
		void readBuses();
		void readParams();
		void readMidiMapping();
		const Param* findParam(uint32_t id) const noexcept;
		static double toNormalized(const Param& param, double value) noexcept;
		static double fromNormalized(const Param& param, double normalized) noexcept;
		std::string unitPath(vst::Vst::UnitID unit) const;
		const Param* programParam(vst::Vst::ProgramListID list) const;
		vst::Vst::ProgramListID rootProgramList() const;
		bool loadPresetFile(const std::string& path);

		// [audio] Building a block.
		void hostParamChange(vst::Vst::ParamID id, double normalized, vst::int32 offset) noexcept;
		void inputEvent(const plugin::Event& event, const plugin::EventList& list, vst::int32 offset) noexcept;
		void midi1(const plugin::Event& event, const uint8_t* bytes, vst::int32 offset) noexcept;
		void midi2(const plugin::Event& event, vst::int32 offset) noexcept;
		void noteOn(int16_t port, int16_t channel, int16_t key, int32_t noteId, float velocity, uint16_t flags,
			vst::int32 offset) noexcept;
		void noteOff(int16_t port, int16_t channel, int16_t key, int32_t noteId, float velocity, uint16_t flags,
			vst::int32 offset) noexcept;
		void noteExpression(const plugin::Event& event, vst::int32 offset) noexcept;
		void controller(int16_t port, int16_t channel, int32_t control, double normalized, vst::int32 offset) noexcept;
		void addEvent(vst::Vst::Event& event) noexcept;
		void drainEdits(plugin::EventList* out, bool toProcessor) noexcept;
		void setContext(
			const plugin::Process& process, const plugin::TransportSegment& segment, uint32_t start) noexcept;
		void collectOutputs(uint32_t start) noexcept;
		void emitOutputs(plugin::EventList& out) noexcept;
		vst::tresult runProcess(vst::Vst::ProcessData& data) noexcept;

		VST3::Hosting::Module::Ptr m_module; // keeps the module's code loaded
		VST3::UID m_classId;
		plugin::Descriptor m_descriptor;
		plugin::Host& m_host;
		plugin::HostParams* m_hostParams = nullptr;
		plugin::HostLatency* m_hostLatency = nullptr;
		plugin::HostAudioPorts* m_hostAudioPorts = nullptr;
		plugin::HostState* m_hostState = nullptr;

		HostApplication m_application;
		ComponentHandler m_handler{*this};
		vst::IPtr<vst::Vst::IComponent> m_component;
		vst::IPtr<vst::Vst::IAudioProcessor> m_processor;
		vst::IPtr<vst::Vst::IEditController> m_controller;
		vst::IPtr<vst::Vst::IConnectionPoint> m_componentConnection;
		vst::IPtr<vst::Vst::IConnectionPoint> m_controllerConnection;
		vst::IPtr<vst::Vst::IUnitInfo> m_units;
		vst::IPtr<vst::Vst::IMidiMapping> m_midiMapping;
		bool m_separateController = false;
		bool m_initialized = false;

		std::vector<Bus> m_audioInputs;
		std::vector<Bus> m_audioOutputs;
		std::vector<Bus> m_eventInputs;
		std::vector<Bus> m_eventOutputs;
		std::vector<Param> m_params;
		std::vector<std::pair<vst::Vst::ParamID, size_t>> m_paramIndex; // sorted by id
		// For each event input bus, channel and controller (vst::Vst::kCountCtrlNumber): the mapped parameter.
		std::vector<vst::Vst::ParamID> m_midiMap;

		// While active.
		bool m_active = false;
		std::atomic<bool> m_processing{false};
		double m_sampleRate = 0.0;
		uint32_t m_maxFrames = 0;
		uint32_t m_tail = 0;
		std::vector<float> m_scratch; // silence for inputs and room for outputs the host doesn't take
		std::vector<std::vector<float*>> m_inputChannels;
		std::vector<std::vector<float*>> m_outputChannels;
		std::vector<vst::Vst::AudioBusBuffers> m_inputBuffers;
		std::vector<vst::Vst::AudioBusBuffers> m_outputBuffers;
		ParameterChanges m_inChanges;
		ParameterChanges m_outChanges;
		EventList m_inEvents;
		EventList m_outEvents;
		vst::Vst::ProcessContext m_context{};
		OutputList m_outputs; // filled per sub-block
		uint32_t m_subBlockStart = 0;
		std::array<Voice, 256> m_voices{};
		size_t m_voiceCount = 0;
		int32_t m_nextNoteId = 0;

		rt::SpscQueue<Edit> m_edits{4096};          // main -> audio: editor edits
		rt::SpscQueue<Edit> m_toProcessor{4096};    // main -> audio: values the processor has to learn
		rt::SpscQueue<Update> m_toController{4096}; // audio -> main
	};
}
