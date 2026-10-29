// SPDX-License-Identifier: GPL-3.0-or-later
// The VST3 plugin of the adapter tests (adaptertest.h, README.md). Written against the VST3 interfaces only, without
// the SDK's plugin classes, so every call between host and plugin is visible here.
//
// VST3 objects are reference counted and delete themselves on their last release, so they are created with new and
// handed over to their first owner; that is the format's ownership model, not a GNUbase one.
#include "adaptertest.h"

#include "pluginhost/vst3/vst3ns.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

namespace
{
	using namespace gnubase::tests::vst3adapter;
	using vst::kResultFalse;
	using vst::kResultOk;
	using vst::kResultTrue;
	using vst::tresult;
	namespace sv = vst::Vst;

	const vst::TUID g_componentId = INLINE_UID(0x6E7A2B11, 0x4C3D4E5F, 0x8A9B0C1D, 0x2E3F4051);
	const vst::TUID g_controllerId = INLINE_UID(0x1F2E3D4C, 0x5B6A7988, 0x97A6B5C4, 0xD3E2F100);
	constexpr std::array<double, 3> g_programGains{0.5, 1.0, 0.25}; // normalized Gain of each program
	constexpr std::array<const char*, 3> g_programNames{"Init", "Loud", "Quiet"};
	constexpr std::array<const char*, 3> g_modeNames{"Clean", "Invert", "Mute"};
	constexpr int32_t g_componentMagic = 0x33424e47;  // "GNB3"
	constexpr int32_t g_controllerMagic = 0x43424e47; // "GNBC"

	void copy(sv::TChar* to, const char* from, size_t size = 128)
	{
		size_t i = 0;
		for (; i + 1 < size && from[i] != 0; ++i)
			to[i] = static_cast<sv::TChar>(from[i]);
		to[i] = 0;
	}

	std::string ascii(const sv::TChar* text)
	{
		std::string result;
		for (size_t i = 0; i < 128 && text[i] != 0; ++i)
			result += static_cast<char>(text[i]);
		return result;
	}

	void copy(char* to, const char* from, size_t size)
	{
		std::snprintf(to, size, "%s", from);
	}

	bool sameId(const char* a, const vst::TUID b)
	{
		return std::memcmp(a, b, sizeof(vst::TUID)) == 0;
	}

	// VST3's rule for the steps of a parameter with the step count 2: each step covers a third of the range.
	int programIndex(double normalized)
	{
		return std::min(2, static_cast<int>(std::floor(std::clamp(normalized, 0.0, 1.0) * 3.0)));
	}

	uint32_t latencyOf(double normalized)
	{
		return static_cast<uint32_t>(std::lround(std::clamp(normalized, 0.0, 1.0) * g_maxLatency));
	}

	// The controller's message to the component with its component handler: the bytes of this struct, which only
	// mean something in the same process.
	struct HandlerMessage
	{
		sv::IComponentHandler* handler = nullptr;
	};

	// The component state: the magic number and the normalized values of Gain to Program.
	struct ComponentState
	{
		std::array<double, Program + 1> values{0.5, 0.0, 0.0, 0.064, 0.0};

		bool write(vst::IBStream* stream)
		{
			int32_t magic = g_componentMagic;
			return stream->write(&magic, sizeof(magic)) == kResultOk &&
				stream->write(values.data(), static_cast<vst::int32>(sizeof(values))) == kResultOk;
		}

		bool read(vst::IBStream* stream)
		{
			int32_t magic = 0;
			vst::int32 count = 0;
			if (stream == nullptr || stream->read(&magic, sizeof(magic), &count) != kResultOk ||
				count != sizeof(magic) || magic != g_componentMagic)
				return false;
			std::array<double, Program + 1> read{};
			if (stream->read(read.data(), static_cast<vst::int32>(sizeof(read)), &count) != kResultOk ||
				count != static_cast<vst::int32>(sizeof(read)))
				return false;
			values = read;
			return true;
		}
	};

	// ---- The component: buses, processing, state ----

	class Component final
		: public vst::U::Implements<vst::U::Directly<sv::IComponent, sv::IAudioProcessor, sv::IConnectionPoint>,
			  vst::U::Indirectly<vst::IPluginBase>>
	{
	public:
		// IPluginBase
		tresult PLUGIN_API initialize(vst::FUnknown* context) override
		{
			m_host = vst::U::cast<sv::IHostApplication>(context);
			return kResultOk;
		}
		tresult PLUGIN_API terminate() override
		{
			m_host = nullptr;
			return kResultOk;
		}

		// IComponent
		tresult PLUGIN_API getControllerClassId(vst::TUID classId) override
		{
			std::memcpy(classId, g_controllerId, sizeof(vst::TUID));
			return kResultOk;
		}
		tresult PLUGIN_API setIoMode(sv::IoMode /*mode*/) override { return kResultOk; }

		vst::int32 PLUGIN_API getBusCount(sv::MediaType type, sv::BusDirection direction) override
		{
			if (type == sv::kAudio)
				return direction == sv::kInput ? 2 : 1;
			return 1;
		}

		tresult PLUGIN_API getBusInfo(
			sv::MediaType type, sv::BusDirection direction, vst::int32 index, sv::BusInfo& info) override
		{
			if (index < 0 || index >= getBusCount(type, direction))
				return vst::kInvalidArgument;
			info.mediaType = type;
			info.direction = direction;
			info.busType = index == 0 ? sv::kMain : sv::kAux;
			info.flags = index == 0 ? sv::BusInfo::kDefaultActive : 0;
			if (type == sv::kAudio)
			{
				const sv::SpeakerArrangement arrangement =
					direction == sv::kInput ? m_inputs[static_cast<size_t>(index)] : m_output;
				info.channelCount = sv::SpeakerArr::getChannelCount(arrangement);
				const char* name = "Output";
				if (direction == sv::kInput)
					name = index == 0 ? "Input" : "Sidechain";
				copy(info.name, name);
			}
			else
			{
				info.channelCount = 16;
				copy(info.name, direction == sv::kInput ? "MIDI In" : "MIDI Out");
			}
			return kResultOk;
		}

		tresult PLUGIN_API getRoutingInfo(sv::RoutingInfo& /*in*/, sv::RoutingInfo& /*out*/) override
		{
			return vst::kNotImplemented;
		}

		tresult PLUGIN_API activateBus(
			sv::MediaType type, sv::BusDirection direction, vst::int32 index, vst::TBool state) override
		{
			if (type == sv::kAudio && direction == sv::kInput && index == 1)
				m_sidechain = state != 0;
			return kResultOk;
		}

		tresult PLUGIN_API setActive(vst::TBool state) override
		{
			if (state != 0)
			{
				m_latency = latencyOf(m_state.values[Latency]);
				for (std::vector<float>& line : m_delay)
					line.assign(g_maxLatency + 1, 0.0f);
				m_position = 0;
			}
			else
			{
				// The layout the Layout parameter asked for, now that the component is in the setup state again.
				const sv::SpeakerArrangement main = m_wantMono ? sv::SpeakerArr::kMono : sv::SpeakerArr::kStereo;
				m_inputs[0] = main;
				m_output = main;
			}
			return kResultOk;
		}

		tresult PLUGIN_API setState(vst::IBStream* state) override
		{
			return m_state.read(state) ? kResultOk : kResultFalse;
		}
		tresult PLUGIN_API getState(vst::IBStream* state) override
		{
			return m_state.write(state) ? kResultOk : kResultFalse;
		}

		// IAudioProcessor
		tresult PLUGIN_API setBusArrangements(sv::SpeakerArrangement* inputs, vst::int32 numIns,
			sv::SpeakerArrangement* outputs, vst::int32 numOuts) override
		{
			// Mono or stereo, the same on the main input and output; the side-chain takes either.
			if (numIns != 2 || numOuts != 1 || inputs[0] != outputs[0])
				return kResultFalse;
			for (vst::int32 i = 0; i < numIns; ++i)
			{
				if (inputs[i] != sv::SpeakerArr::kMono && inputs[i] != sv::SpeakerArr::kStereo)
					return kResultFalse;
			}
			m_inputs = {inputs[0], inputs[1]};
			m_output = outputs[0];
			return kResultTrue;
		}

		tresult PLUGIN_API getBusArrangement(
			sv::BusDirection direction, vst::int32 index, sv::SpeakerArrangement& arrangement) override
		{
			if (direction == sv::kInput && index >= 0 && index < 2)
				arrangement = m_inputs[static_cast<size_t>(index)];
			else if (direction == sv::kOutput && index == 0)
				arrangement = m_output;
			else
				return vst::kInvalidArgument;
			return kResultOk;
		}

		tresult PLUGIN_API canProcessSampleSize(vst::int32 size) override
		{
			return size == sv::kSample32 ? kResultTrue : kResultFalse;
		}

		vst::uint32 PLUGIN_API getLatencySamples() override { return latencyOf(m_state.values[Latency]); }
		tresult PLUGIN_API setupProcessing(sv::ProcessSetup& setup) override
		{
			m_setupMode = setup.processMode;
			return kResultOk;
		}
		tresult PLUGIN_API setProcessing(vst::TBool /*state*/) override { return kResultOk; }
		vst::uint32 PLUGIN_API getTailSamples() override { return g_tail; }

		tresult PLUGIN_API process(sv::ProcessData& data) override
		{
			collectChanges(data);
			handleEvents(data);
			if (data.processContext != nullptr && (data.processContext->state & sv::ProcessContext::kTempoValid) != 0)
				output(data, Tempo, 0, data.processContext->tempo / 1000.0);
			output(data, ProcessMode, 0, data.processMode == m_setupMode ? (m_setupMode + 1) / 4.0 : 0.0);
			if (data.numSamples > 0 && data.numOutputs > 0)
				render(data);
			else
				applyChanges(std::numeric_limits<vst::int32>::max());
			return kResultOk;
		}

		// IConnectionPoint: the component greets the controller, through a message the host allocates.
		tresult PLUGIN_API connect(sv::IConnectionPoint* other) override
		{
			if (other == nullptr || m_host == nullptr)
				return kResultFalse;
			// createInstance takes the class and the interface id; for a message both are IMessage's.
			sv::IMessage* message = nullptr;
			vst::TUID classId{};
			vst::TUID interfaceId{};
			sv::IMessage::iid.toTUID(classId);
			sv::IMessage::iid.toTUID(interfaceId);
			if (m_host->createInstance(classId, interfaceId, reinterpret_cast<void**>(&message)) != kResultOk ||
				message == nullptr)
				return kResultFalse;
			message->setMessageID("hello");
			other->notify(message);
			message->release();
			return kResultOk;
		}
		tresult PLUGIN_API disconnect(sv::IConnectionPoint* /*other*/) override { return kResultOk; }
		// The controller hands over its component handler, so the component can call it from the audio thread.
		tresult PLUGIN_API notify(sv::IMessage* message) override
		{
			const void* data = nullptr;
			vst::uint32 size = 0;
			if (message != nullptr && std::strcmp(message->getMessageID(), "handler") == 0 &&
				message->getAttributes() != nullptr &&
				message->getAttributes()->getBinary("pointer", data, size) == kResultOk && data != nullptr &&
				size == sizeof(HandlerMessage))
			{
				HandlerMessage received;
				std::memcpy(static_cast<void*>(&received), data, sizeof(HandlerMessage));
				m_handler = received.handler;
			}
			return kResultOk;
		}

	private:
		struct Change
		{
			vst::int32 offset = 0;
			sv::ParamID id = 0;
			double value = 0.0;
		};

		void collectChanges(sv::ProcessData& data)
		{
			m_changeCount = 0;
			m_nextChange = 0;
			if (data.inputParameterChanges == nullptr)
				return;
			for (vst::int32 i = 0; i < data.inputParameterChanges->getParameterCount(); ++i)
			{
				sv::IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
				if (queue == nullptr)
					continue;
				for (vst::int32 p = 0; p < queue->getPointCount() && m_changeCount < m_changes.size(); ++p)
				{
					Change change;
					change.id = queue->getParameterId();
					if (queue->getPoint(p, change.offset, change.value) == kResultOk)
						m_changes[m_changeCount++] = change;
				}
			}
			// In order of their offsets, keeping the order of equal ones; insertion sort doesn't allocate.
			for (size_t i = 1; i < m_changeCount; ++i)
			{
				const Change change = m_changes[i];
				size_t j = i;
				for (; j > 0 && m_changes[j - 1].offset > change.offset; --j)
					m_changes[j] = m_changes[j - 1];
				m_changes[j] = change;
			}
		}

		// Applies the changes up to and including `offset`.
		void applyChanges(vst::int32 offset)
		{
			for (; m_nextChange < m_changeCount && m_changes[m_nextChange].offset <= offset; ++m_nextChange)
			{
				const Change& change = m_changes[m_nextChange];
				if (change.id <= Program)
					m_state.values[change.id] = change.value;
				if (change.id == Program)
					m_state.values[Gain] = g_programGains[static_cast<size_t>(programIndex(change.value))];
				if (change.id == Layout)
					m_wantMono = change.value >= 0.5;
				if (change.id == AudioEdit && change.value >= 0.5 && m_handler != nullptr)
				{
					// An edit and a restart request from the audio thread, which the host must not handle here.
					m_handler->beginEdit(Gain);
					m_handler->performEdit(Gain, 0.75);
					m_handler->endEdit(Gain);
					m_handler->restartComponent(sv::kParamValuesChanged);
				}
			}
		}

		static void output(sv::ProcessData& data, sv::ParamID id, vst::int32 offset, double value)
		{
			if (data.outputParameterChanges == nullptr)
				return;
			vst::int32 index = 0;
			if (sv::IParamValueQueue* queue = data.outputParameterChanges->addParameterData(id, index))
				queue->addPoint(offset, value, index);
		}

		void handleEvents(sv::ProcessData& data)
		{
			if (data.inputEvents == nullptr)
				return;
			for (vst::int32 i = 0; i < data.inputEvents->getEventCount(); ++i)
			{
				sv::Event event{};
				if (data.inputEvents->getEvent(i, event) != kResultOk)
					continue;
				if (event.type == sv::Event::kNoteOnEvent)
					output(data, NoteCount, event.sampleOffset, static_cast<double>(++m_notes) / 128.0);
				if (event.type == sv::Event::kNoteExpressionValueEvent &&
					event.noteExpressionValue.typeId == sv::kTuningTypeID)
					output(data, LastTuning, event.sampleOffset, event.noteExpressionValue.value);
				// Every event comes back on the output bus, as it came in.
				if (data.outputEvents != nullptr)
				{
					event.busIndex = 0;
					data.outputEvents->addEvent(event);
				}
			}
		}

		void render(sv::ProcessData& data)
		{
			const sv::AudioBusBuffers& out = data.outputs[0];
			const sv::AudioBusBuffers* in = data.numInputs > 0 ? &data.inputs[0] : nullptr;
			const sv::AudioBusBuffers* side = m_sidechain && data.numInputs > 1 ? &data.inputs[1] : nullptr;
			const size_t channels = static_cast<size_t>(std::min(out.numChannels, 2));
			for (vst::int32 i = 0; i < data.numSamples; ++i)
			{
				applyChanges(i);
				const double gain = 2.0 * m_state.values[Gain];
				const int mode = programIndex(m_state.values[Mode]);
				auto factor = static_cast<float>(mode == 1 ? -gain : gain); // Invert
				if (mode == 2)
					factor = 0.0f; // Mute
				const bool bypass = m_state.values[Bypass] >= 0.5;
				const size_t size = m_delay[0].size();
				for (size_t c = 0; c < channels; ++c)
				{
					const float x =
						in != nullptr && static_cast<size_t>(in->numChannels) > c ? in->channelBuffers32[c][i] : 0.0f;
					const float s = side != nullptr && static_cast<size_t>(side->numChannels) > c
						? side->channelBuffers32[c][i]
						: 0.0f;
					std::vector<float>& line = m_delay[c];
					line[m_position] = bypass ? x : (x * factor) + s;
					out.channelBuffers32[c][i] = line[(m_position + size - m_latency) % size];
				}
				m_position = (m_position + 1) % size;
			}
			applyChanges(std::numeric_limits<vst::int32>::max());
		}

		vst::IPtr<sv::IHostApplication> m_host;
		sv::IComponentHandler* m_handler = nullptr; // the controller's, which outlives processing
		vst::int32 m_setupMode = -1;
		bool m_wantMono = false;
		ComponentState m_state;
		std::array<sv::SpeakerArrangement, 2> m_inputs{sv::SpeakerArr::kStereo, sv::SpeakerArr::kStereo};
		sv::SpeakerArrangement m_output = sv::SpeakerArr::kStereo;
		bool m_sidechain = false;
		uint32_t m_latency = 0;
		std::array<std::vector<float>, 2> m_delay{
			std::vector<float>(g_maxLatency + 1), std::vector<float>(g_maxLatency + 1)};
		size_t m_position = 0;
		std::array<Change, 256> m_changes{};
		size_t m_changeCount = 0;
		size_t m_nextChange = 0;
		uint32_t m_notes = 0;
	};

	// ---- The controller: parameters, units, programs, MIDI mapping, edits ----

	class Controller final
		: public vst::U::Implements<
			  vst::U::Directly<sv::IEditController, sv::IUnitInfo, sv::IMidiMapping, sv::IConnectionPoint>,
			  vst::U::Indirectly<vst::IPluginBase>>
	{
	public:
		Controller()
		{
			for (vst::int32 i = 0; i < ParamCount; ++i)
			{
				sv::ParameterInfo info{};
				info.id = static_cast<sv::ParamID>(i);
				info.flags = sv::ParameterInfo::kCanAutomate;
				static constexpr std::array<const char*, ParamCount> titles{"Gain", "Bypass", "Mode", "Latency",
					"Program", "Note count", "Last tuning", "Tempo", "Edit trigger", "Layout", "Audio edit",
					"Process mode", ""};
				copy(info.title, titles[static_cast<size_t>(i)]);
				switch (i)
				{
				case Gain:
					info.defaultNormalizedValue = 0.5;
					info.unitId = 1;
					break;
				case Bypass:
					info.stepCount = 1;
					info.flags |= sv::ParameterInfo::kIsBypass;
					break;
				case Mode:
					info.stepCount = 2;
					info.flags |= sv::ParameterInfo::kIsList;
					info.unitId = 1;
					break;
				case Latency:
					info.defaultNormalizedValue = 0.064;
					break;
				case Program:
					info.stepCount = 2;
					info.flags = sv::ParameterInfo::kIsProgramChange | sv::ParameterInfo::kIsList;
					info.defaultNormalizedValue = 0.7;
					break;
				case Odd:
					// An unpaired surrogate, then characters up to the last one of String128, without a terminator.
					info.title[0] = static_cast<sv::TChar>(0xd800);
					info.title[1] = 'O';
					info.title[2] = 'd';
					info.title[3] = 'd';
					for (size_t c = 4; c < std::size(info.title); ++c)
						info.title[c] = 'x';
					info.flags = sv::ParameterInfo::kIsReadOnly;
					break;
				case NoteCount:
				case LastTuning:
				case Tempo:
				case ProcessMode:
					info.flags = sv::ParameterInfo::kIsReadOnly;
					break;
				default: // EditTrigger, Layout, AudioEdit
					info.stepCount = 1;
					info.flags = sv::ParameterInfo::kIsHidden;
					break;
				}
				m_values[static_cast<size_t>(i)] = info.defaultNormalizedValue;
				m_infos[static_cast<size_t>(i)] = info;
			}
		}

		// IPluginBase
		tresult PLUGIN_API initialize(vst::FUnknown* context) override
		{
			m_host = vst::U::cast<sv::IHostApplication>(context);
			return kResultOk;
		}
		tresult PLUGIN_API terminate() override
		{
			m_handler = nullptr;
			m_host = nullptr;
			return kResultOk;
		}

		// IEditController
		tresult PLUGIN_API setComponentState(vst::IBStream* state) override
		{
			ComponentState component;
			if (!component.read(state))
				return kResultFalse;
			std::copy(component.values.begin(), component.values.end(), m_values.begin());
			return kResultOk;
		}

		tresult PLUGIN_API setState(vst::IBStream* state) override
		{
			std::array<int32_t, 2> data{};
			vst::int32 count = 0;
			if (state->read(data.data(), sizeof(data), &count) != kResultOk || count != sizeof(data) ||
				data[0] != g_controllerMagic)
				return kResultFalse;
			m_edits = data[1];
			return kResultOk;
		}

		tresult PLUGIN_API getState(vst::IBStream* state) override
		{
			std::array<int32_t, 2> data{g_controllerMagic, m_edits};
			return state->write(data.data(), sizeof(data)) == kResultOk ? kResultOk : kResultFalse;
		}

		vst::int32 PLUGIN_API getParameterCount() override { return ParamCount; }

		tresult PLUGIN_API getParameterInfo(vst::int32 index, sv::ParameterInfo& info) override
		{
			if (index < 0 || index >= ParamCount)
				return vst::kInvalidArgument;
			info = m_infos[static_cast<size_t>(index)];
			return kResultOk;
		}

		tresult PLUGIN_API getParamStringByValue(sv::ParamID id, sv::ParamValue value, sv::String128 string) override
		{
			std::array<char, 64> text{};
			if (id == Gain)
				std::snprintf(text.data(), text.size(), "%.2f", 2.0 * value);
			else if (id == Mode)
				copy(text.data(), g_modeNames[static_cast<size_t>(programIndex(value))], text.size());
			else if (id == Program)
				copy(text.data(), g_programNames[static_cast<size_t>(programIndex(value))], text.size());
			else if (id == EditTrigger)
				copy(text.data(), m_connected ? "Connected" : "Unconnected", text.size());
			else if (id < ParamCount)
				std::snprintf(text.data(), text.size(), "%.3f", value);
			else
				return vst::kInvalidArgument;
			copy(string, text.data());
			return kResultOk;
		}

		tresult PLUGIN_API getParamValueByString(sv::ParamID id, sv::TChar* string, sv::ParamValue& value) override
		{
			const std::string text = ascii(string);
			if (id == Mode || id == Program)
			{
				const std::array<const char*, 3>& names = id == Mode ? g_modeNames : g_programNames;
				for (size_t i = 0; i < names.size(); ++i)
				{
					if (text == names[i])
					{
						value = static_cast<double>(i) / 2.0;
						return kResultOk;
					}
				}
				return kResultFalse;
			}
			char* end = nullptr;
			const double number = std::strtod(text.c_str(), &end);
			if (end == text.c_str() || id >= ParamCount)
				return kResultFalse;
			value = id == Gain ? number / 2.0 : number;
			return kResultOk;
		}

		sv::ParamValue PLUGIN_API normalizedParamToPlain(sv::ParamID /*id*/, sv::ParamValue value) override
		{
			return value;
		}
		sv::ParamValue PLUGIN_API plainParamToNormalized(sv::ParamID /*id*/, sv::ParamValue value) override
		{
			return value;
		}

		sv::ParamValue PLUGIN_API getParamNormalized(sv::ParamID id) override
		{
			return id < ParamCount ? m_values[id] : 0.0;
		}

		tresult PLUGIN_API setParamNormalized(sv::ParamID id, sv::ParamValue value) override
		{
			if (id >= ParamCount)
				return vst::kInvalidArgument;
			const double old = m_values[id];
			m_values[id] = value;
			if (m_handler == nullptr)
				return kResultOk;
			if (id == Latency && value != old)
				m_handler->restartComponent(sv::kLatencyChanged);
			if (id == Program)
			{
				m_values[Gain] = g_programGains[static_cast<size_t>(programIndex(value))];
				m_handler->restartComponent(sv::kParamValuesChanged);
			}
			if (id == EditTrigger && value >= 0.5)
			{
				// As if the user dragged Gain in the editor.
				++m_edits;
				m_values[EditTrigger] = 0.0;
				m_handler->beginEdit(Gain);
				m_values[Gain] = 0.25;
				m_handler->performEdit(Gain, 0.25);
				m_handler->endEdit(Gain);
				if (const auto handler2 = vst::U::cast<sv::IComponentHandler2>(m_handler))
					handler2->setDirty(1);
			}
			if (id == Layout && value != old)
				m_handler->restartComponent(sv::kIoChanged);
			return kResultOk;
		}

		tresult PLUGIN_API setComponentHandler(sv::IComponentHandler* handler) override
		{
			m_handler = handler;
			// The component gets it too, through a message, for its edits on the audio thread.
			if (m_peer != nullptr && m_host != nullptr)
			{
				sv::IMessage* message = nullptr;
				vst::TUID classId{};
				vst::TUID interfaceId{};
				sv::IMessage::iid.toTUID(classId);
				sv::IMessage::iid.toTUID(interfaceId);
				if (m_host->createInstance(classId, interfaceId, reinterpret_cast<void**>(&message)) == kResultOk &&
					message != nullptr)
				{
					message->setMessageID("handler");
					if (message->getAttributes() != nullptr)
					{
						const HandlerMessage sent{handler};
						message->getAttributes()->setBinary("pointer", static_cast<const void*>(&sent),
							static_cast<vst::uint32>(sizeof(HandlerMessage)));
					}
					m_peer->notify(message);
					message->release();
				}
			}
			return kResultOk;
		}

		vst::IPlugView* PLUGIN_API createView(vst::FIDString /*name*/) override { return nullptr; }

		// IUnitInfo: the root unit with the program list, and the unit "Amp".
		vst::int32 PLUGIN_API getUnitCount() override { return 2; }

		tresult PLUGIN_API getUnitInfo(vst::int32 index, sv::UnitInfo& info) override
		{
			if (index == 0)
			{
				info.id = sv::kRootUnitId;
				info.parentUnitId = sv::kNoParentUnitId;
				info.programListId = 1;
				copy(info.name, "Root");
			}
			else if (index == 1)
			{
				info.id = 1;
				info.parentUnitId = sv::kRootUnitId;
				info.programListId = sv::kNoProgramListId;
				copy(info.name, "Amp");
			}
			else
			{
				return vst::kInvalidArgument;
			}
			return kResultOk;
		}

		vst::int32 PLUGIN_API getProgramListCount() override { return 1; }

		tresult PLUGIN_API getProgramListInfo(vst::int32 index, sv::ProgramListInfo& info) override
		{
			if (index != 0)
				return vst::kInvalidArgument;
			info.id = 1;
			info.programCount = static_cast<vst::int32>(g_programNames.size());
			copy(info.name, "Factory");
			return kResultOk;
		}

		tresult PLUGIN_API getProgramName(sv::ProgramListID list, vst::int32 index, sv::String128 name) override
		{
			if (list != 1 || index < 0 || index >= static_cast<vst::int32>(g_programNames.size()))
				return vst::kInvalidArgument;
			copy(name, g_programNames[static_cast<size_t>(index)]);
			return kResultOk;
		}

		tresult PLUGIN_API getProgramInfo(sv::ProgramListID /*list*/, vst::int32 /*index*/, sv::CString /*attribute*/,
			sv::String128 /*value*/) override
		{
			return vst::kNotImplemented;
		}

		tresult PLUGIN_API hasProgramPitchNames(sv::ProgramListID list, vst::int32 /*index*/) override
		{
			return list == 1 ? kResultTrue : kResultFalse;
		}

		tresult PLUGIN_API getProgramPitchName(
			sv::ProgramListID list, vst::int32 /*index*/, vst::int16 pitch, sv::String128 name) override
		{
			if (list != 1 || (pitch != 36 && pitch != 38))
				return kResultFalse;
			copy(name, pitch == 36 ? "Kick" : "Snare");
			return kResultTrue;
		}

		sv::UnitID PLUGIN_API getSelectedUnit() override { return sv::kRootUnitId; }
		tresult PLUGIN_API selectUnit(sv::UnitID /*id*/) override { return kResultOk; }
		tresult PLUGIN_API getUnitByBus(sv::MediaType /*type*/, sv::BusDirection /*direction*/, vst::int32 /*bus*/,
			vst::int32 /*channel*/, sv::UnitID& /*unit*/) override
		{
			return kResultFalse;
		}
		tresult PLUGIN_API setUnitProgramData(
			vst::int32 /*list*/, vst::int32 /*index*/, vst::IBStream* /*data*/) override
		{
			return vst::kNotImplemented;
		}

		// IMidiMapping: CC 7 on every channel of the event input controls Gain.
		tresult PLUGIN_API getMidiControllerAssignment(
			vst::int32 bus, vst::int16 /*channel*/, sv::CtrlNumber control, sv::ParamID& id) override
		{
			if (bus != 0 || control != g_gainController)
				return kResultFalse;
			id = Gain;
			return kResultTrue;
		}

		// IConnectionPoint
		tresult PLUGIN_API connect(sv::IConnectionPoint* other) override
		{
			m_peer = other;
			return kResultOk;
		}
		tresult PLUGIN_API disconnect(sv::IConnectionPoint* /*other*/) override
		{
			m_peer = nullptr;
			return kResultOk;
		}
		tresult PLUGIN_API notify(sv::IMessage* message) override
		{
			if (message != nullptr && std::strcmp(message->getMessageID(), "hello") == 0)
				m_connected = true;
			return kResultOk;
		}

	private:
		std::array<sv::ParameterInfo, ParamCount> m_infos{};
		std::array<double, ParamCount> m_values{};
		vst::IPtr<sv::IComponentHandler> m_handler;
		vst::IPtr<sv::IHostApplication> m_host;
		sv::IConnectionPoint* m_peer = nullptr; // the component, while connected
		bool m_connected = false;
		int32_t m_edits = 0;
	};

	// ---- The factory ----

	class Factory final : public vst::U::ImplementsNonDestroyable<vst::U::Directly<vst::IPluginFactory2>,
							  vst::U::Indirectly<vst::IPluginFactory>>
	{
	public:
		tresult PLUGIN_API getFactoryInfo(vst::PFactoryInfo* info) override
		{
			copy(info->vendor, "GNUbase", sizeof(info->vendor));
			copy(info->url, "https://github.com/GNUbase/GNUbase", sizeof(info->url));
			copy(info->email, "", sizeof(info->email));
			info->flags = vst::PFactoryInfo::kNoFlags;
			return kResultOk;
		}

		vst::int32 PLUGIN_API countClasses() override { return 2; }

		tresult PLUGIN_API getClassInfo(vst::int32 index, vst::PClassInfo* info) override
		{
			vst::PClassInfo2 full{};
			if (getClassInfo2(index, &full) != kResultOk)
				return vst::kInvalidArgument;
			std::memcpy(info->cid, full.cid, sizeof(vst::TUID));
			info->cardinality = full.cardinality;
			copy(info->category, full.category, sizeof(info->category));
			copy(info->name, full.name, sizeof(info->name));
			return kResultOk;
		}

		tresult PLUGIN_API getClassInfo2(vst::int32 index, vst::PClassInfo2* info) override
		{
			if (index != 0 && index != 1)
				return vst::kInvalidArgument;
			std::memcpy(info->cid, index == 0 ? g_componentId : g_controllerId, sizeof(vst::TUID));
			info->cardinality = vst::PClassInfo::kManyInstances;
			copy(info->category, index == 0 ? kVstAudioEffectClass : kVstComponentControllerClass,
				sizeof(info->category));
			copy(info->name, index == 0 ? "Adapter Test" : "Adapter Test Controller", sizeof(info->name));
			info->classFlags = 0;
			copy(info->subCategories, "Fx|Tools", sizeof(info->subCategories));
			copy(info->vendor, "GNUbase", sizeof(info->vendor));
			copy(info->version, "1.0.0", sizeof(info->version));
			copy(info->sdkVersion, kVstVersionString, sizeof(info->sdkVersion));
			return kResultOk;
		}

		tresult PLUGIN_API createInstance(vst::FIDString classId, vst::FIDString interfaceId, void** object) override
		{
			*object = nullptr;
			vst::FUnknown* instance = nullptr;
			if (sameId(classId, g_componentId))
				instance = static_cast<sv::IComponent*>(new Component);
			else if (sameId(classId, g_controllerId))
				instance = static_cast<sv::IEditController*>(new Controller);
			else
				return vst::kNoInterface;
			vst::TUID wanted{};
			std::memcpy(wanted, interfaceId, sizeof(vst::TUID));
			const tresult result = instance->queryInterface(wanted, object);
			instance->release();
			return result;
		}
	};
}

// The module's entry points. Windows needs only GetPluginFactory; Linux and macOS also call an entry and an exit
// function when they load and unload the module.
extern "C"
{
	// NOLINTNEXTLINE(readability-identifier-naming): VST3's name for the entry point
	SMTG_EXPORT_SYMBOL vst::IPluginFactory* PLUGIN_API GetPluginFactory()
	{
		static Factory factory;
		return &factory;
	}

#ifdef __APPLE__
	SMTG_EXPORT_SYMBOL bool bundleEntry(void* /*bundle*/)
	{
		return true;
	}
	SMTG_EXPORT_SYMBOL bool bundleExit()
	{
		return true;
	}
#elif !defined(_WIN32)
	// NOLINTNEXTLINE(readability-identifier-naming): VST3's name for the entry point
	SMTG_EXPORT_SYMBOL bool ModuleEntry(void* /*library*/)
	{
		return true;
	}
	// NOLINTNEXTLINE(readability-identifier-naming): VST3's name for the exit point
	SMTG_EXPORT_SYMBOL bool ModuleExit()
	{
		return true;
	}
#endif
}
