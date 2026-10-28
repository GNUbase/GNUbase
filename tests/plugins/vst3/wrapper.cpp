// SPDX-License-Identifier: GPL-3.0-or-later
// Turns a test plugin (common/testplugin.h) into a VST3 plugin: the module entry points, a factory with one class, and
// a single component that is processor and edit controller at once (IComponent, IAudioProcessor, IEditController).
// Linked into every .vst3 test plugin. Only the SDK's interface headers are used; the SDK's namespace is `vst`.
#include "../common/testplugin.h"

#include "pluginhost/vst3/vst3ns.h"

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/ivstunits.h>
#include <pluginterfaces/vst/vstspeaker.h>
#include <pluginterfaces/vst/vsttypes.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::testplugins
{
	namespace
	{
		using vst::FIDString;
		using vst::FUnknown;
		using vst::int32;
		using vst::tresult;
		using vst::TUID;
		using vst::uint32;
		using vst::Vst::BusDirection;
		using vst::Vst::MediaType;
		using vst::Vst::ParamID;
		using vst::Vst::ParamValue;
		using vst::Vst::SpeakerArrangement;
		using vst::Vst::TChar;

		// More input events in one block than this are dropped: the buffer is allocated on activation, not in process.
		constexpr uint32_t g_maxEvents = 8192;

		bool sameId(const void* a, const void* b)
		{
			return std::memcmp(a, b, sizeof(TUID)) == 0; // bytewise: ids may be unaligned for iidEqual's 64-bit reads
		}

		// The class id: 16 bytes hashed from the plugin's id (FNV-1a with two offset bases), stable across builds.
		void classId(TUID cid)
		{
			uint64_t hashes[2] = {0xcbf29ce484222325ull, 0x6c62272e07bb0142ull};
			for (uint64_t& hash : hashes)
				for (const char* c = pluginDescriptor().id; *c != 0; ++c)
					hash = (hash ^ static_cast<uint8_t>(*c)) * 0x100000001b3ull;
			for (size_t i = 0; i < 16; ++i)
				cid[i] = static_cast<char>((hashes[i / 8] >> ((i % 8) * 8)) & 0xff);
		}

		bool isOurClass(FIDString cid)
		{
			TUID ours = {};
			classId(ours);
			return cid != nullptr && sameId(cid, ours);
		}

		template <size_t Size> void copyName(char (&to)[Size], const char* from)
		{
			std::snprintf(to, Size, "%s", from);
		}

		// The test plugins' texts are ASCII, so widening each byte gives UTF-16.
		void copyWide(TChar* to, size_t size, const char* from)
		{
			size_t i = 0;
			for (; from[i] != 0 && i + 1 < size; ++i)
				to[i] = static_cast<TChar>(static_cast<unsigned char>(from[i]));
			to[i] = 0;
		}

		std::string narrow(const TChar* text)
		{
			std::string result;
			for (; text != nullptr && *text != 0; ++text)
				result += *text < 0x80 ? static_cast<char>(*text) : '?';
			return result;
		}

		// VST3 hosts see normalized values from 0 to 1; the test plugins use plain values.
		double toNormalized(const ParamInfo& info, double plain)
		{
			const double range = info.maxValue - info.minValue;
			return range > 0.0 ? std::clamp((plain - info.minValue) / range, 0.0, 1.0) : 0.0;
		}

		double toPlain(const ParamInfo& info, double normalized)
		{
			const double plain = info.minValue + (std::clamp(normalized, 0.0, 1.0) * (info.maxValue - info.minValue));
			return info.stepped ? std::round(plain) : plain;
		}

		class Vst3EventInput : public EventInput
		{
		public:
			Vst3EventInput(const Event* events, uint32_t count) : m_events(events), m_count(count) {}

			uint32_t size() const override { return m_count; }

			bool get(uint32_t index, Event& event) const override
			{
				if (index >= m_count)
					return false;
				event = m_events[index];
				return true;
			}

		private:
			const Event* m_events;
			uint32_t m_count;
		};

		// Parameter values go to the output parameter changes and notes to the output events. Gestures and note ends
		// have no VST3 equivalent in process, so they are dropped.
		class Vst3EventOutput : public EventOutput
		{
		public:
			Vst3EventOutput(const TestPlugin& plugin, vst::Vst::ProcessData& data) : m_plugin(plugin), m_data(data) {}

			bool push(const Event& event) override
			{
				switch (event.type)
				{
				case EventType::ParamValue:
				{
					const ParamInfo* info = m_plugin.findParam(event.paramId);
					if (info == nullptr || m_data.outputParameterChanges == nullptr)
						return false;
					const ParamID id = event.paramId;
					int32 index = 0;
					vst::Vst::IParamValueQueue* queue = m_data.outputParameterChanges->addParameterData(id, index);
					int32 point = 0;
					return queue != nullptr &&
						queue->addPoint(static_cast<int32>(event.time), toNormalized(*info, event.value), point) ==
						vst::kResultOk;
				}
				case EventType::NoteOn:
				case EventType::NoteOff:
				{
					if (m_data.outputEvents == nullptr)
						return false;
					vst::Vst::Event out{};
					out.busIndex = event.port;
					out.sampleOffset = static_cast<int32>(event.time);
					if (event.type == EventType::NoteOn)
					{
						out.type = vst::Vst::Event::kNoteOnEvent;
						out.noteOn.channel = event.channel;
						out.noteOn.pitch = event.key;
						out.noteOn.velocity = static_cast<float>(event.value);
						out.noteOn.noteId = event.noteId;
					}
					else
					{
						out.type = vst::Vst::Event::kNoteOffEvent;
						out.noteOff.channel = event.channel;
						out.noteOff.pitch = event.key;
						out.noteOff.velocity = static_cast<float>(event.value);
						out.noteOff.noteId = event.noteId;
					}
					return m_data.outputEvents->addEvent(out) == vst::kResultOk;
				}
				default:
					return true;
				}
			}

		private:
			const TestPlugin& m_plugin;
			vst::Vst::ProcessData& m_data;
		};

		// One object is component, processor and edit controller, so hosts find the controller by querying the
		// component and keep one set of parameter values.
		class Vst3Plugin final : public vst::Vst::IComponent,
								 public vst::Vst::IAudioProcessor,
								 public vst::Vst::IEditController,
								 public vst::Vst::IProcessContextRequirements,
								 public Host
		{
		public:
			Vst3Plugin() : m_core(createPlugin(*this)) {}
			~Vst3Plugin() override { setHandler(nullptr); }
			Vst3Plugin(const Vst3Plugin&) = delete;
			Vst3Plugin& operator=(const Vst3Plugin&) = delete;
			Vst3Plugin(Vst3Plugin&&) = delete;
			Vst3Plugin& operator=(Vst3Plugin&&) = delete;

			// FUnknown
			tresult PLUGIN_API queryInterface(const TUID interfaceId, void** obj) override
			{
				if (sameId(interfaceId, vst::FUnknown_iid) || sameId(interfaceId, vst::IPluginBase_iid) ||
					sameId(interfaceId, vst::Vst::IComponent_iid))
					return found(static_cast<vst::Vst::IComponent*>(this), obj);
				if (sameId(interfaceId, vst::Vst::IAudioProcessor_iid))
					return found(static_cast<vst::Vst::IAudioProcessor*>(this), obj);
				if (sameId(interfaceId, vst::Vst::IEditController_iid))
					return found(static_cast<vst::Vst::IEditController*>(this), obj);
				if (sameId(interfaceId, vst::Vst::IProcessContextRequirements_iid))
					return found(static_cast<vst::Vst::IProcessContextRequirements*>(this), obj);
				*obj = nullptr;
				return vst::kNoInterface;
			}

			uint32 PLUGIN_API addRef() override { return ++m_refs; }

			uint32 PLUGIN_API release() override
			{
				const uint32 refs = --m_refs;
				if (refs == 0)
				{
					const std::unique_ptr<Vst3Plugin> owner(this);
				}
				return refs;
			}

			// IPluginBase, for the component and the controller
			tresult PLUGIN_API initialize(FUnknown* /*context*/) override { return vst::kResultOk; }

			tresult PLUGIN_API terminate() override
			{
				setHandler(nullptr);
				return vst::kResultOk;
			}

			// IComponent
			tresult PLUGIN_API getControllerClassId(TUID /*classId*/) override { return vst::kNotImplemented; }

			tresult PLUGIN_API setIoMode(vst::Vst::IoMode /*mode*/) override { return vst::kNotImplemented; }

			int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override
			{
				const bool instrument = pluginDescriptor().instrument;
				if (type == vst::Vst::kAudio)
					return dir == vst::Vst::kInput && instrument ? 0 : 1;
				if (type == vst::Vst::kEvent)
					return dir == vst::Vst::kInput && instrument ? 1 : 0;
				return 0;
			}

			tresult PLUGIN_API getBusInfo(
				MediaType type, BusDirection dir, int32 index, vst::Vst::BusInfo& bus) override
			{
				if (index < 0 || index >= getBusCount(type, dir))
					return vst::kInvalidArgument;
				bus.mediaType = type;
				bus.direction = dir;
				bus.channelCount = type == vst::Vst::kAudio ? static_cast<int32>(g_channels) : 16;
				const char* name = dir == vst::Vst::kInput ? "Main In" : "Main Out";
				if (type == vst::Vst::kEvent)
					name = "Notes";
				copyWide(bus.name, std::size(bus.name), name);
				bus.busType = vst::Vst::kMain;
				bus.flags = vst::Vst::BusInfo::kDefaultActive;
				return vst::kResultOk;
			}

			tresult PLUGIN_API getRoutingInfo(
				vst::Vst::RoutingInfo& /*inInfo*/, vst::Vst::RoutingInfo& /*outInfo*/) override
			{
				return vst::kNotImplemented;
			}

			tresult PLUGIN_API activateBus(MediaType type, BusDirection dir, int32 index, vst::TBool /*state*/) override
			{
				return index >= 0 && index < getBusCount(type, dir) ? vst::kResultOk : vst::kInvalidArgument;
			}

			tresult PLUGIN_API setActive(vst::TBool state) override
			{
				if (state != 0 && !m_active)
				{
					m_events.assign(g_maxEvents, Event{});
					m_core->activate(m_sampleRate, m_maxFrames);
					m_active = true;
				}
				else if (state == 0 && m_active)
				{
					m_core->deactivate();
					m_active = false;
				}
				return vst::kResultOk;
			}

			// The component's state. A single component has no separate controller state, so the host's calls of the
			// controller's setState and getState, which have the same signatures, end up here too.
			tresult PLUGIN_API setState(vst::IBStream* state) override
			{
				if (state == nullptr)
					return vst::kInvalidArgument;
				std::vector<uint8_t> bytes;
				constexpr int32 chunk = 65536;
				for (;;)
				{
					const size_t at = bytes.size();
					bytes.resize(at + chunk);
					int32 count = 0;
					const tresult result = state->read(bytes.data() + at, chunk, &count);
					if (count < 0 || count > chunk || (result != vst::kResultOk && count > 0))
						return vst::kResultFalse;
					bytes.resize(at + static_cast<size_t>(count));
					if (count == 0)
						break;
				}
				return m_core->loadState(bytes.data(), bytes.size()) ? vst::kResultOk : vst::kResultFalse;
			}

			tresult PLUGIN_API getState(vst::IBStream* state) override
			{
				if (state == nullptr)
					return vst::kInvalidArgument;
				std::vector<uint8_t> bytes = m_core->saveState();
				size_t written = 0;
				while (written < bytes.size())
				{
					const auto size = static_cast<int32>(std::min<size_t>(bytes.size() - written, INT32_MAX));
					int32 count = 0;
					if (state->write(bytes.data() + written, size, &count) != vst::kResultOk || count <= 0)
						return vst::kResultFalse;
					written += static_cast<size_t>(count);
				}
				return vst::kResultOk;
			}

			// IAudioProcessor
			tresult PLUGIN_API setBusArrangements(
				SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs, int32 numOuts) override
			{
				if (numIns != getBusCount(vst::Vst::kAudio, vst::Vst::kInput) || numOuts != 1)
					return vst::kResultFalse;
				for (int32 i = 0; i < numIns; ++i)
					if (inputs[i] != vst::Vst::SpeakerArr::kStereo)
						return vst::kResultFalse;
				return outputs[0] == vst::Vst::SpeakerArr::kStereo ? vst::kResultTrue : vst::kResultFalse;
			}

			tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index, SpeakerArrangement& arr) override
			{
				if (index < 0 || index >= getBusCount(vst::Vst::kAudio, dir))
					return vst::kInvalidArgument;
				arr = vst::Vst::SpeakerArr::kStereo;
				return vst::kResultTrue;
			}

			tresult PLUGIN_API canProcessSampleSize(int32 symbolicSampleSize) override
			{
				return symbolicSampleSize == vst::Vst::kSample32 ? vst::kResultTrue : vst::kResultFalse;
			}

			uint32 PLUGIN_API getLatencySamples() override { return m_core->latency(); }

			tresult PLUGIN_API setupProcessing(vst::Vst::ProcessSetup& setup) override
			{
				if (setup.symbolicSampleSize != vst::Vst::kSample32 || setup.sampleRate <= 0.0 ||
					setup.maxSamplesPerBlock <= 0)
					return vst::kResultFalse;
				m_sampleRate = setup.sampleRate;
				m_maxFrames = static_cast<uint32_t>(setup.maxSamplesPerBlock);
				return vst::kResultOk;
			}

			tresult PLUGIN_API setProcessing(vst::TBool /*state*/) override { return vst::kResultOk; }

			tresult PLUGIN_API process(vst::Vst::ProcessData& data) override
			{
				if (!m_active || data.symbolicSampleSize != vst::Vst::kSample32 || data.numSamples < 0)
					return vst::kResultFalse;
				const Vst3EventInput in(m_events.data(), gatherEvents(data));
				Vst3EventOutput out(*m_core, data);
				ProcessBlock block;
				if (data.numInputs > 0 && data.inputs != nullptr && data.inputs[0].channelBuffers32 != nullptr)
				{
					block.inputs = data.inputs[0].channelBuffers32;
					block.inputChannels = static_cast<uint32_t>(std::max(data.inputs[0].numChannels, 0));
				}
				if (data.numOutputs > 0 && data.outputs != nullptr && data.outputs[0].channelBuffers32 != nullptr)
				{
					block.outputs = data.outputs[0].channelBuffers32;
					block.outputChannels = static_cast<uint32_t>(std::max(data.outputs[0].numChannels, 0));
					data.outputs[0].silenceFlags = 0;
				}
				block.frames = static_cast<uint32_t>(data.numSamples);
				block.events = &in;
				block.outEvents = &out;
				m_core->process(block);
				return vst::kResultOk;
			}

			uint32 PLUGIN_API getTailSamples() override { return vst::Vst::kNoTail; }

			// IProcessContextRequirements: the test plugins need no transport or tempo information.
			uint32 PLUGIN_API getProcessContextRequirements() override { return 0; }

			// IEditController
			tresult PLUGIN_API setComponentState(vst::IBStream* /*state*/) override
			{
				return vst::kResultOk; // the component's setState already loaded it into the same object
			}

			int32 PLUGIN_API getParameterCount() override { return static_cast<int32>(m_core->params().size()); }

			tresult PLUGIN_API getParameterInfo(int32 paramIndex, vst::Vst::ParameterInfo& info) override
			{
				const std::vector<ParamInfo>& params = m_core->params();
				if (paramIndex < 0 || static_cast<size_t>(paramIndex) >= params.size())
					return vst::kInvalidArgument;
				const ParamInfo& param = params[static_cast<size_t>(paramIndex)];
				info.id = param.id;
				copyWide(info.title, std::size(info.title), param.name);
				copyWide(info.shortTitle, std::size(info.shortTitle), param.name);
				copyWide(info.units, std::size(info.units), param.unit);
				info.stepCount = param.stepped ? static_cast<int32>(std::lround(param.maxValue - param.minValue)) : 0;
				info.defaultNormalizedValue = toNormalized(param, param.defaultValue);
				info.unitId = vst::Vst::kRootUnitId;
				info.flags = vst::Vst::ParameterInfo::kCanAutomate;
				if (param.valueNames != nullptr)
					info.flags |= vst::Vst::ParameterInfo::kIsList;
				return vst::kResultOk;
			}

			tresult PLUGIN_API getParamStringByValue(
				ParamID id, ParamValue valueNormalized, vst::Vst::String128 string) override
			{
				const ParamInfo* info = m_core->findParam(id);
				char text[128] = {};
				if (info == nullptr || !m_core->valueToText(id, toPlain(*info, valueNormalized), text, sizeof(text)))
					return vst::kInvalidArgument;
				copyWide(string, 128, text);
				return vst::kResultOk;
			}

			tresult PLUGIN_API getParamValueByString(ParamID id, TChar* string, ParamValue& valueNormalized) override
			{
				const ParamInfo* info = m_core->findParam(id);
				double plain = 0.0;
				if (info == nullptr || !m_core->textToValue(id, narrow(string).c_str(), plain))
					return vst::kInvalidArgument;
				valueNormalized = toNormalized(*info, plain);
				return vst::kResultOk;
			}

			ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue valueNormalized) override
			{
				const ParamInfo* info = m_core->findParam(id);
				return info != nullptr ? toPlain(*info, valueNormalized) : valueNormalized;
			}

			ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue plainValue) override
			{
				const ParamInfo* info = m_core->findParam(id);
				return info != nullptr ? toNormalized(*info, plainValue) : plainValue;
			}

			ParamValue PLUGIN_API getParamNormalized(ParamID id) override
			{
				const ParamInfo* info = m_core->findParam(id);
				double plain = 0.0;
				return info != nullptr && m_core->paramValue(id, plain) ? toNormalized(*info, plain) : 0.0;
			}

			// The host tells the controller about every change, also the ones it sends to the processor. While
			// active, the processor owns the values (TestPlugin::setParam), so only an inactive plugin takes them
			// from here. Restarts the processor asked for are passed on now, on the main thread, as VST3 requires.
			tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue value) override
			{
				const ParamInfo* info = m_core->findParam(id);
				if (info == nullptr)
					return vst::kInvalidArgument;
				if (!m_active)
					m_core->setParam(id, toPlain(*info, value));
				if (m_restartPending.exchange(false) && m_handler != nullptr)
					m_handler->restartComponent(vst::Vst::kLatencyChanged);
				return vst::kResultOk;
			}

			tresult PLUGIN_API setComponentHandler(vst::Vst::IComponentHandler* handler) override
			{
				setHandler(handler);
				return vst::kResultOk;
			}

			vst::IPlugView* PLUGIN_API createView(FIDString /*name*/) override { return nullptr; }

			// Host: the latency plugin asks for a restart on the audio thread; it is passed on in setParamNormalized.
			void requestRestart() override { m_restartPending.store(true); }

		private:
			void setHandler(vst::Vst::IComponentHandler* handler)
			{
				if (handler != nullptr)
					handler->addRef();
				if (m_handler != nullptr)
					m_handler->release();
				m_handler = handler;
			}

			template <typename Interface> tresult found(Interface* object, void** obj)
			{
				addRef();
				*obj = object;
				return vst::kResultOk;
			}

			// Collects the block's parameter changes and note events into m_events, sorted by time. No allocations.
			uint32_t gatherEvents(vst::Vst::ProcessData& data)
			{
				uint32_t count = 0;
				const auto add = [this, &count](const Event& event)
				{
					if (count < m_events.size())
						m_events[count++] = event;
				};
				vst::Vst::IParameterChanges* changes = data.inputParameterChanges;
				for (int32 q = 0; changes != nullptr && q < changes->getParameterCount(); ++q)
				{
					vst::Vst::IParamValueQueue* queue = changes->getParameterData(q);
					const ParamInfo* info = queue != nullptr ? m_core->findParam(queue->getParameterId()) : nullptr;
					for (int32 p = 0; info != nullptr && p < queue->getPointCount(); ++p)
					{
						int32 offset = 0;
						ParamValue value = 0.0;
						if (queue->getPoint(p, offset, value) != vst::kResultOk)
							continue;
						Event event;
						event.type = EventType::ParamValue;
						event.time = static_cast<uint32_t>(std::max(offset, 0));
						event.paramId = info->id;
						event.value = toPlain(*info, value);
						add(event);
					}
				}
				vst::Vst::IEventList* notes = data.inputEvents;
				for (int32 i = 0; notes != nullptr && i < notes->getEventCount(); ++i)
				{
					vst::Vst::Event note{};
					if (notes->getEvent(i, note) != vst::kResultOk ||
						(note.type != vst::Vst::Event::kNoteOnEvent && note.type != vst::Vst::Event::kNoteOffEvent))
						continue;
					Event event;
					event.time = static_cast<uint32_t>(std::max(note.sampleOffset, 0));
					event.port = static_cast<int16_t>(note.busIndex);
					if (note.type == vst::Vst::Event::kNoteOnEvent)
					{
						event.type = EventType::NoteOn;
						event.channel = note.noteOn.channel;
						event.key = note.noteOn.pitch;
						event.value = note.noteOn.velocity;
						event.noteId = note.noteOn.noteId;
					}
					else
					{
						event.type = EventType::NoteOff;
						event.channel = note.noteOff.channel;
						event.key = note.noteOff.pitch;
						event.value = note.noteOff.velocity;
						event.noteId = note.noteOff.noteId;
					}
					add(event);
				}
				// Insertion sort: stable and allocation-free, and the lists are short and mostly sorted already.
				for (uint32_t i = 1; i < count; ++i)
				{
					const Event event = m_events[i];
					uint32_t j = i;
					for (; j > 0 && m_events[j - 1].time > event.time; --j)
						m_events[j] = m_events[j - 1];
					m_events[j] = event;
				}
				return count;
			}

			std::atomic<uint32> m_refs{1};
			std::unique_ptr<TestPlugin> m_core;
			vst::Vst::IComponentHandler* m_handler = nullptr;
			std::atomic<bool> m_restartPending{false};
			std::vector<Event> m_events;
			double m_sampleRate = 48000.0;
			uint32_t m_maxFrames = 4096;
			bool m_active = false;
		};

		class Factory final : public vst::IPluginFactory3
		{
		public:
			tresult PLUGIN_API queryInterface(const TUID interfaceId, void** obj) override
			{
				if (sameId(interfaceId, vst::FUnknown_iid) || sameId(interfaceId, vst::IPluginFactory_iid) ||
					sameId(interfaceId, vst::IPluginFactory2_iid) || sameId(interfaceId, vst::IPluginFactory3_iid))
				{
					addRef();
					*obj = static_cast<vst::IPluginFactory3*>(this);
					return vst::kResultOk;
				}
				*obj = nullptr;
				return vst::kNoInterface;
			}

			// A static object: the count is kept for hosts that check it, but nothing is ever deleted.
			uint32 PLUGIN_API addRef() override { return ++m_refs; }
			uint32 PLUGIN_API release() override { return --m_refs; }

			tresult PLUGIN_API getFactoryInfo(vst::PFactoryInfo* info) override
			{
				if (info == nullptr)
					return vst::kInvalidArgument;
				*info = vst::PFactoryInfo();
				copyName(info->vendor, "GNUbase");
				copyName(info->url, "https://github.com/GNUbase/GNUbase");
				copyName(info->email, "");
				info->flags = vst::PFactoryInfo::kUnicode;
				return vst::kResultOk;
			}

			int32 PLUGIN_API countClasses() override { return 1; }

			tresult PLUGIN_API getClassInfo(int32 index, vst::PClassInfo* info) override
			{
				if (index != 0 || info == nullptr)
					return vst::kInvalidArgument;
				*info = vst::PClassInfo();
				classId(info->cid);
				info->cardinality = vst::PClassInfo::kManyInstances;
				copyName(info->category, kVstAudioEffectClass);
				copyName(info->name, pluginDescriptor().name);
				return vst::kResultOk;
			}

			tresult PLUGIN_API getClassInfo2(int32 index, vst::PClassInfo2* info) override
			{
				if (index != 0 || info == nullptr)
					return vst::kInvalidArgument;
				*info = vst::PClassInfo2();
				classId(info->cid);
				info->cardinality = vst::PClassInfo::kManyInstances;
				copyName(info->category, kVstAudioEffectClass);
				copyName(info->name, pluginDescriptor().name);
				info->classFlags = 0; // processor and controller are one object, so they can't be distributed
				copyName(info->subCategories, subCategories());
				copyName(info->vendor, "GNUbase");
				copyName(info->version, "0.1.0");
				copyName(info->sdkVersion, kVstVersionString);
				return vst::kResultOk;
			}

			tresult PLUGIN_API getClassInfoUnicode(int32 index, vst::PClassInfoW* info) override
			{
				if (index != 0 || info == nullptr)
					return vst::kInvalidArgument;
				*info = vst::PClassInfoW();
				classId(info->cid);
				info->cardinality = vst::PClassInfo::kManyInstances;
				copyName(info->category, kVstAudioEffectClass);
				copyWide(info->name, std::size(info->name), pluginDescriptor().name);
				info->classFlags = 0;
				copyName(info->subCategories, subCategories());
				copyWide(info->vendor, std::size(info->vendor), "GNUbase");
				copyWide(info->version, std::size(info->version), "0.1.0");
				copyWide(info->sdkVersion, std::size(info->sdkVersion), kVstVersionString);
				return vst::kResultOk;
			}

			tresult PLUGIN_API createInstance(FIDString cid, FIDString interfaceId, void** obj) override
			{
				if (obj == nullptr)
					return vst::kInvalidArgument;
				*obj = nullptr;
				if (!isOurClass(cid) || interfaceId == nullptr)
					return vst::kInvalidArgument;
				auto* plugin = std::make_unique<Vst3Plugin>().release(); // owned through its reference count
				const tresult result = plugin->queryInterface(interfaceId, obj);
				plugin->release();
				return result;
			}

			tresult PLUGIN_API setHostContext(FUnknown* /*context*/) override { return vst::kResultOk; }

		private:
			static const char* subCategories()
			{
				return pluginDescriptor().instrument ? "Instrument|Synth|Stereo" : "Fx|Tools|Stereo";
			}

			std::atomic<uint32> m_refs{0};
		};

		Factory g_factory;
	}
}

// The module entry points. VST3 fixes their names, and hosts call them with the platform's signature.
// NOLINTBEGIN(readability-identifier-naming)
extern "C"
{
	SMTG_EXPORT_SYMBOL vst::IPluginFactory* PLUGIN_API GetPluginFactory()
	{
		gnubase::testplugins::g_factory.addRef(); // the host releases it
		return &gnubase::testplugins::g_factory;
	}

#ifdef _WIN32
	SMTG_EXPORT_SYMBOL bool InitDll()
	{
		return true;
	}

	SMTG_EXPORT_SYMBOL bool ExitDll()
	{
		return true;
	}
#elif defined(__APPLE__)
	SMTG_EXPORT_SYMBOL bool bundleEntry(void* /*bundle*/)
	{
		return true;
	}

	SMTG_EXPORT_SYMBOL bool bundleExit()
	{
		return true;
	}
#else
	SMTG_EXPORT_SYMBOL bool ModuleEntry(void* /*sharedLibraryHandle*/)
	{
		return true;
	}

	SMTG_EXPORT_SYMBOL bool ModuleExit()
	{
		return true;
	}
#endif
}
// NOLINTEND(readability-identifier-naming)
