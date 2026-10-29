// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginhost/vst3/vst3plugin.h"

#include "pluginhost/vst3/presetfile.h"

#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace gnubase::pluginhost::vst3
{
	namespace
	{
		using vst::kResultOk;
		using vst::Vst::BusDirections;
		using vst::Vst::MediaTypes;

		constexpr size_t g_extraQueues = 64;  // parameter queues beyond the parameter count, for unknown ids
		constexpr size_t g_maxInPoints = 64;  // changes of one parameter in one block
		constexpr size_t g_maxOutPoints = 32; // changes of one parameter the plugin reports per block
		constexpr size_t g_maxEvents = 1024;  // VST3 events per block, in and out
		constexpr size_t g_maxOutputs = 4096; // GNUbase output events per sub-block
		constexpr size_t g_minQueue = 4096;   // values queued between the threads, at least
		constexpr size_t g_channels = 16;     // MIDI channels per event bus
		constexpr int32_t g_bankSelect = 0;   // MIDI controllers: bank select, and its LSB
		constexpr int32_t g_bankSelectLsb = 32;
		constexpr size_t g_maxText = 128;           // characters of a String128
		constexpr double g_perNoteBendRange = 48.0; // semitones of a MIDI 2.0 per-note pitch bend, as MPE's default
		constexpr double g_maxUint32 = 4294967295.0;
		constexpr vst::TBool g_true = 1;
		constexpr vst::TBool g_false = 0;
		constexpr bool g_comFormat = COM_COMPATIBLE != 0;

		void appendUtf8(std::string& text, uint32_t code)
		{
			if (code < 0x80)
			{
				text += static_cast<char>(code);
			}
			else if (code < 0x800)
			{
				text += static_cast<char>(0xc0 | (code >> 6));
				text += static_cast<char>(0x80 | (code & 0x3f));
			}
			else if (code < 0x10000)
			{
				text += static_cast<char>(0xe0 | (code >> 12));
				text += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
				text += static_cast<char>(0x80 | (code & 0x3f));
			}
			else
			{
				text += static_cast<char>(0xf0 | (code >> 18));
				text += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
				text += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
				text += static_cast<char>(0x80 | (code & 0x3f));
			}
		}

		// A plugin's UTF-16 text as UTF-8: up to the terminator, but never more than `max` characters, as plugins fill
		// String128 to the last character. Invalid UTF-16 (an unpaired surrogate) becomes U+FFFD; it never throws.
		std::string toUtf8(const vst::Vst::TChar* text, size_t max = g_maxText)
		{
			std::string result;
			for (size_t i = 0; i < max && text[i] != 0; ++i)
			{
				uint32_t code = static_cast<uint16_t>(text[i]);
				if (code >= 0xd800 && code < 0xdc00 && i + 1 < max)
				{
					const auto low = static_cast<uint16_t>(text[i + 1]);
					if (low >= 0xdc00 && low < 0xe000)
					{
						code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
						++i;
					}
				}
				if (code >= 0xd800 && code < 0xe000)
					code = 0xfffd;
				appendUtf8(result, code);
			}
			return result;
		}

		// UTF-8 as a String128; false if it isn't valid UTF-8 or too long.
		bool toString128(const std::string& text, vst::Vst::String128 string)
		{
			try
			{
				return vst::Vst::StringConvert::convert(text, string);
			}
			catch (const std::range_error&)
			{
				return false;
			}
		}

		vst::int32 vstProcessMode(plugin::ProcessMode mode)
		{
			switch (mode)
			{
			case plugin::ProcessMode::Offline:
				return vst::Vst::kOffline;
			case plugin::ProcessMode::Prefetch:
				return vst::Vst::kPrefetch;
			case plugin::ProcessMode::Realtime:
				break;
			}
			return vst::Vst::kRealtime;
		}

		bool succeeded(vst::tresult result)
		{
			return result == kResultOk;
		}

		uint64_t speakerMask(vst::Vst::SpeakerArrangement arrangement)
		{
			return static_cast<uint64_t>(arrangement);
		}

		// The speaker bits VST3 and WAVE_FORMAT_EXTENSIBLE share: front left to top back right.
		constexpr uint64_t g_waveSpeakers = 0x3ffff;

		plugin::ChannelLayout layoutOf(vst::Vst::SpeakerArrangement arrangement)
		{
			if (arrangement == vst::Vst::SpeakerArr::kMono)
				return plugin::ChannelLayout::Mono;
			if (arrangement == vst::Vst::SpeakerArr::kStereo)
				return plugin::ChannelLayout::Stereo;
			if (arrangement == vst::Vst::SpeakerArr::kAmbi1stOrderACN ||
				arrangement == vst::Vst::SpeakerArr::kAmbi2cdOrderACN ||
				arrangement == vst::Vst::SpeakerArr::kAmbi3rdOrderACN)
				return plugin::ChannelLayout::Ambisonic;
			if (arrangement != 0 && (speakerMask(arrangement) & ~g_waveSpeakers) == 0)
				return plugin::ChannelLayout::Surround;
			return plugin::ChannelLayout::Other;
		}

		bool arrangementOf(const plugin::AudioPortRequest& request, vst::Vst::SpeakerArrangement& arrangement)
		{
			switch (request.layout)
			{
			case plugin::ChannelLayout::Mono:
				arrangement = vst::Vst::SpeakerArr::kMono;
				return true;
			case plugin::ChannelLayout::Stereo:
				arrangement = vst::Vst::SpeakerArr::kStereo;
				return true;
			case plugin::ChannelLayout::Surround:
				arrangement = static_cast<vst::Vst::SpeakerArrangement>(request.speakers & g_waveSpeakers);
				return arrangement != 0;
			case plugin::ChannelLayout::Ambisonic:
				if (request.channelCount == 4)
					arrangement = vst::Vst::SpeakerArr::kAmbi1stOrderACN;
				else if (request.channelCount == 9)
					arrangement = vst::Vst::SpeakerArr::kAmbi2cdOrderACN;
				else if (request.channelCount == 16)
					arrangement = vst::Vst::SpeakerArr::kAmbi3rdOrderACN;
				else
					return false;
				return true;
			case plugin::ChannelLayout::Other:
				break;
			}
			return false;
		}

		constexpr uint32_t g_pressure = plugin::expressionId(plugin::NoteExpression::Pressure);

		// A per-note expression id as a VST3 note expression type and its normalized value. The plugin's own
		// expressions (from g_firstPluginExpression on) are its VST3 types, with normalized values. False for
		// pressure, which is a poly pressure event in VST3, and for ids VST3 has no type for.
		bool expressionToVst(
			uint32_t expression, double value, vst::Vst::NoteExpressionTypeID& type, double& normalized) noexcept
		{
			if (expression >= plugin::g_firstPluginExpression)
			{
				type = expression - plugin::g_firstPluginExpression;
				normalized = std::clamp(value, 0.0, 1.0);
				return true;
			}
			switch (expression)
			{
			case plugin::expressionId(plugin::NoteExpression::Volume):
				type = vst::Vst::kVolumeTypeID;
				normalized = value / 4.0;
				break;
			case plugin::expressionId(plugin::NoteExpression::Pan):
				type = vst::Vst::kPanTypeID;
				normalized = value;
				break;
			case plugin::expressionId(plugin::NoteExpression::Tuning):
				type = vst::Vst::kTuningTypeID;
				normalized = (value / 240.0) + 0.5;
				break;
			case plugin::expressionId(plugin::NoteExpression::Vibrato):
				type = vst::Vst::kVibratoTypeID;
				normalized = value;
				break;
			case plugin::expressionId(plugin::NoteExpression::Expression):
				type = vst::Vst::kExpressionTypeID;
				normalized = value;
				break;
			case plugin::expressionId(plugin::NoteExpression::Brightness):
				type = vst::Vst::kBrightnessTypeID;
				normalized = value;
				break;
			default:
				return false;
			}
			normalized = std::clamp(normalized, 0.0, 1.0);
			return true;
		}

		bool expressionFromVst(
			vst::Vst::NoteExpressionTypeID type, double normalized, uint32_t& expression, double& value)
		{
			value = normalized;
			switch (type)
			{
			case vst::Vst::kVolumeTypeID:
				expression = plugin::expressionId(plugin::NoteExpression::Volume);
				value = normalized * 4.0;
				return true;
			case vst::Vst::kPanTypeID:
				expression = plugin::expressionId(plugin::NoteExpression::Pan);
				return true;
			case vst::Vst::kTuningTypeID:
				expression = plugin::expressionId(plugin::NoteExpression::Tuning);
				value = (normalized - 0.5) * 240.0;
				return true;
			case vst::Vst::kVibratoTypeID:
				expression = plugin::expressionId(plugin::NoteExpression::Vibrato);
				return true;
			case vst::Vst::kExpressionTypeID:
				expression = plugin::expressionId(plugin::NoteExpression::Expression);
				return true;
			case vst::Vst::kBrightnessTypeID:
				expression = plugin::expressionId(plugin::NoteExpression::Brightness);
				return true;
			case vst::Vst::kTextTypeID: // text events, not values
			case vst::Vst::kPhonemeTypeID:
				return false;
			default:
				// The plugin's own expressions.
				if (type > UINT32_MAX - plugin::g_firstPluginExpression)
					return false;
				expression = plugin::g_firstPluginExpression + type;
				return true;
			}
		}

		bool matches(int16_t want, int16_t have)
		{
			return want < 0 || want == have;
		}

		vst::int32 busIndex(int16_t port)
		{
			return port < 0 ? 0 : port;
		}
	}

	std::string classIdText(const VST3::UID& id)
	{
		return id.toString(g_comFormat);
	}

	bool sameClassId(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() &&
			std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
				{ return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
	}

	Vst3Plugin::Vst3Plugin(
		VST3::Hosting::Module::Ptr module, const VST3::UID& classId, plugin::Descriptor descriptor, plugin::Host& host)
		: m_module(std::move(module)), m_classId(classId), m_descriptor(std::move(descriptor)), m_host(host),
		  m_inEvents(g_maxEvents), m_outEvents(g_maxEvents), m_outputs(g_maxOutputs)
	{
	}

	Vst3Plugin::~Vst3Plugin()
	{
		terminate();
	}

	void Vst3Plugin::terminate()
	{
		if (m_active)
			stopComponent(); // without reading buses the plugin asked to change: it goes now
		if (m_componentConnection != nullptr && m_controllerConnection != nullptr)
		{
			m_componentConnection->disconnect(m_controllerConnection);
			m_controllerConnection->disconnect(m_componentConnection);
		}
		m_componentConnection = nullptr;
		m_controllerConnection = nullptr;
		m_units = nullptr;
		m_midiMapping = nullptr;
		if (m_controller != nullptr)
		{
			m_controller->setComponentHandler(nullptr);
			if (m_separateController)
				m_controller->terminate();
		}
		m_controller = nullptr;
		m_processor = nullptr;
		if (m_component != nullptr)
			m_component->terminate();
		m_component = nullptr;
		m_initialized = false;
	}

	// ---- Lifecycle ----

	bool Vst3Plugin::init()
	{
		if (m_initialized)
			return true;
		m_hostParams = plugin::extension<plugin::HostParams>(m_host);
		m_hostLatency = plugin::extension<plugin::HostLatency>(m_host);
		m_hostAudioPorts = plugin::extension<plugin::HostAudioPorts>(m_host);
		m_hostNotePorts = plugin::extension<plugin::HostNotePorts>(m_host);
		m_hostState = plugin::extension<plugin::HostState>(m_host);

		vst::FUnknown* context = static_cast<vst::Vst::IHostApplication*>(&m_application);
		const VST3::Hosting::PluginFactory& factory = m_module->getFactory();
		m_component = factory.createInstance<vst::Vst::IComponent>(m_classId);
		if (m_component == nullptr)
			return false;
		if (!succeeded(m_component->initialize(context)))
		{
			m_component = nullptr;
			return false;
		}

		// The controller: the component itself, or a class of its own.
		m_controller = vst::U::cast<vst::Vst::IEditController>(m_component);
		if (m_controller == nullptr)
		{
			vst::TUID controllerId{};
			if (succeeded(m_component->getControllerClassId(controllerId)))
			{
				m_controller = factory.createInstance<vst::Vst::IEditController>(VST3::UID::fromTUID(controllerId));
				if (m_controller != nullptr && !succeeded(m_controller->initialize(context)))
					m_controller = nullptr;
				m_separateController = static_cast<bool>(m_controller);
			}
		}
		m_processor = vst::U::cast<vst::Vst::IAudioProcessor>(m_component);
		if (m_processor == nullptr || m_processor->canProcessSampleSize(vst::Vst::kSample32) != kResultOk)
		{
			terminate();
			return false;
		}
		if (m_separateController)
		{
			m_componentConnection = vst::U::cast<vst::Vst::IConnectionPoint>(m_component);
			m_controllerConnection = vst::U::cast<vst::Vst::IConnectionPoint>(m_controller);
			if (m_componentConnection != nullptr && m_controllerConnection != nullptr)
			{
				m_componentConnection->connect(m_controllerConnection);
				m_controllerConnection->connect(m_componentConnection);
			}
		}
		if (m_controller != nullptr)
		{
			m_controller->setComponentHandler(&m_handler);
			// The controller starts from the component's state, as in every VST3 host.
			MemoryStream state;
			if (succeeded(m_component->getState(&state)))
			{
				state.rewind();
				m_controller->setComponentState(&state);
			}
			m_units = vst::U::cast<vst::Vst::IUnitInfo>(m_controller);
			m_midiMapping = vst::U::cast<vst::Vst::IMidiMapping>(m_controller);
		}
		readBuses();
		readParams();
		readMidiMapping();
		// Room for a change of every parameter, several times over, between the threads.
		const size_t queue = std::max(g_minQueue, 4 * m_params.size());
		m_toProcessor = std::make_unique<rt::SpscQueue<Edit>>(queue);
		m_toController = std::make_unique<rt::SpscQueue<Update>>(queue);
		m_initialized = true;
		return true;
	}

	bool Vst3Plugin::setProcessMode(plugin::ProcessMode mode)
	{
		if (m_active)
			return false; // VST3 sets the mode in setupProcessing, before activation
		m_mode = mode;
		return true;
	}

	void Vst3Plugin::readBuses()
	{
		auto read = [this](vst::Vst::MediaType type, vst::Vst::BusDirection direction, std::vector<Bus>& buses)
		{
			const std::vector<Bus> previous = std::move(buses);
			buses.clear();
			const vst::int32 count = m_component->getBusCount(type, direction);
			for (vst::int32 i = 0; i < count; ++i)
			{
				vst::Vst::BusInfo info{};
				Bus bus;
				if (succeeded(m_component->getBusInfo(type, direction, i, info)))
				{
					bus.name = toUtf8(info.name);
					bus.channelCount = static_cast<uint32_t>(std::max(info.channelCount, 0));
					bus.main = info.busType == vst::Vst::kMain;
					bus.defaultActive = (info.flags & vst::Vst::BusInfo::kDefaultActive) != 0;
				}
				vst::Vst::SpeakerArrangement arrangement = 0;
				if (type == MediaTypes::kAudio && succeeded(m_processor->getBusArrangement(direction, i, arrangement)))
				{
					bus.arrangement = arrangement;
					bus.channelCount = static_cast<uint32_t>(vst::Vst::SpeakerArr::getChannelCount(arrangement));
				}
				const auto index = static_cast<size_t>(i);
				bus.active = index < previous.size() ? previous[index].active : bus.main || bus.defaultActive;
				buses.push_back(std::move(bus));
			}
		};
		read(MediaTypes::kAudio, BusDirections::kInput, m_audioInputs);
		read(MediaTypes::kAudio, BusDirections::kOutput, m_audioOutputs);
		read(MediaTypes::kEvent, BusDirections::kInput, m_eventInputs);
		read(MediaTypes::kEvent, BusDirections::kOutput, m_eventOutputs);
	}

	void Vst3Plugin::readParams()
	{
		m_params.clear();
		m_paramIndex.clear();
		if (m_controller == nullptr)
			return;
		const vst::int32 count = m_controller->getParameterCount();
		for (vst::int32 i = 0; i < count; ++i)
		{
			vst::Vst::ParameterInfo info{};
			if (!succeeded(m_controller->getParameterInfo(i, info)))
				continue;
			Param param;
			param.stepCount = std::max(info.stepCount, 0);
			param.unit = info.unitId;
			param.programChange = (info.flags & vst::Vst::ParameterInfo::kIsProgramChange) != 0;
			plugin::ParamInfo& out = param.info;
			out.id = info.id;
			out.name = toUtf8(info.title);
			out.module = unitPath(info.unitId);
			out.minimum = 0.0;
			out.maximum = 1.0;
			out.stepCount = static_cast<uint32_t>(param.stepCount);
			out.defaultValue = fromNormalized(param, info.defaultNormalizedValue);
			using Flags = vst::Vst::ParameterInfo::ParameterFlags;
			const auto has = [&info](Flags flag) { return (info.flags & flag) != 0; };
			if (param.stepCount > 0)
				out.flags |= plugin::ParamInfo::Stepped;
			if (has(Flags::kIsWrapAround))
				out.flags |= plugin::ParamInfo::Periodic;
			if (has(Flags::kIsHidden))
				out.flags |= plugin::ParamInfo::Hidden;
			if (has(Flags::kIsReadOnly))
				out.flags |= plugin::ParamInfo::ReadOnly;
			if (has(Flags::kIsBypass))
				out.flags |= plugin::ParamInfo::Bypass;
			if (has(Flags::kCanAutomate))
				out.flags |= plugin::ParamInfo::Automatable;
			if (has(Flags::kIsList) && param.stepCount > 0)
				out.flags |= plugin::ParamInfo::Enum;
			m_paramIndex.emplace_back(info.id, m_params.size());
			m_params.push_back(std::move(param));
		}
		std::sort(m_paramIndex.begin(), m_paramIndex.end());
	}

	void Vst3Plugin::readMidiMapping()
	{
		readProgramMap();
		m_midiMap.clear();
		if (m_midiMapping == nullptr || m_eventInputs.empty())
			return;
		const size_t controls = vst::Vst::kCountCtrlNumber;
		m_midiMap.assign(m_eventInputs.size() * g_channels * controls, vst::Vst::kNoParamId);
		for (size_t bus = 0; bus < m_eventInputs.size(); ++bus)
		{
			for (size_t channel = 0; channel < g_channels; ++channel)
			{
				for (size_t control = 0; control < controls; ++control)
				{
					vst::Vst::ParamID id = vst::Vst::kNoParamId;
					if (m_midiMapping->getMidiControllerAssignment(static_cast<vst::int32>(bus),
							static_cast<vst::int16>(channel), static_cast<vst::Vst::CtrlNumber>(control),
							id) == vst::kResultTrue)
						m_midiMap[(((bus * g_channels) + channel) * controls) + control] = id;
				}
			}
		}
	}

	void Vst3Plugin::readProgramMap()
	{
		// As VST3 hosts do: a program change on a bus and channel goes to the program change parameter of the unit
		// that plays them (IUnitInfo::getUnitByBus), or of the root unit.
		m_programMap.assign(m_eventInputs.size() * g_channels, vst::Vst::kNoParamId);
		for (size_t bus = 0; bus < m_eventInputs.size(); ++bus)
		{
			for (size_t channel = 0; channel < g_channels; ++channel)
			{
				vst::Vst::UnitID unit = vst::Vst::kRootUnitId;
				vst::Vst::UnitID found = vst::Vst::kRootUnitId;
				if (m_units != nullptr &&
					m_units->getUnitByBus(MediaTypes::kEvent, BusDirections::kInput, static_cast<vst::int32>(bus),
						static_cast<vst::int32>(channel), found) == vst::kResultTrue)
					unit = found;
				for (const Param& param : m_params)
				{
					if (param.programChange && param.unit == unit && param.stepCount > 0)
					{
						m_programMap[(bus * g_channels) + channel] = param.info.id;
						break;
					}
				}
			}
		}
	}

	bool Vst3Plugin::activate(double sampleRate, uint32_t /*minFrames*/, uint32_t maxFrames)
	{
		if (!m_initialized)
			return false;
		if (m_active)
			return true;
		vst::Vst::ProcessSetup setup{
			vstProcessMode(m_mode), vst::Vst::kSample32, static_cast<vst::int32>(maxFrames), sampleRate};
		if (!succeeded(m_processor->setupProcessing(setup)))
			return false;
		readBuses();
		for (size_t i = 0; i < m_audioInputs.size(); ++i)
			m_component->activateBus(MediaTypes::kAudio, BusDirections::kInput, static_cast<vst::int32>(i),
				static_cast<vst::TBool>(m_audioInputs[i].active));
		for (size_t i = 0; i < m_audioOutputs.size(); ++i)
			m_component->activateBus(MediaTypes::kAudio, BusDirections::kOutput, static_cast<vst::int32>(i),
				static_cast<vst::TBool>(m_audioOutputs[i].active));
		for (size_t i = 0; i < m_eventInputs.size(); ++i)
			m_component->activateBus(MediaTypes::kEvent, BusDirections::kInput, static_cast<vst::int32>(i), g_true);
		for (size_t i = 0; i < m_eventOutputs.size(); ++i)
			m_component->activateBus(MediaTypes::kEvent, BusDirections::kOutput, static_cast<vst::int32>(i), g_true);
		if (!succeeded(m_component->setActive(g_true)))
			return false;

		m_sampleRate = sampleRate;
		m_maxFrames = maxFrames;
		m_tail = m_processor->getTailSamples();

		// Every bus gets channel pointers; where the host doesn't give a buffer they point into the scratch memory,
		// silence for inputs and a place to write to for outputs.
		size_t channels = 0;
		for (const Bus& bus : m_audioInputs)
			channels += bus.channelCount;
		for (const Bus& bus : m_audioOutputs)
			channels += bus.channelCount;
		m_scratch.assign(channels * maxFrames, 0.0f);
		auto setUp = [](const std::vector<Bus>& buses, std::vector<std::vector<float*>>& pointers,
						 std::vector<vst::Vst::AudioBusBuffers>& buffers)
		{
			pointers.assign(buses.size(), {});
			buffers.assign(buses.size(), vst::Vst::AudioBusBuffers{});
			for (size_t i = 0; i < buses.size(); ++i)
			{
				pointers[i].assign(buses[i].channelCount, nullptr);
				buffers[i].numChannels = static_cast<vst::int32>(buses[i].channelCount);
				buffers[i].channelBuffers32 = pointers[i].data();
			}
		};
		setUp(m_audioInputs, m_inputChannels, m_inputBuffers);
		setUp(m_audioOutputs, m_outputChannels, m_outputBuffers);
		// A queue for every parameter, so a block takes a change of each.
		m_inChanges.resize(m_params.size() + g_extraQueues, g_maxInPoints);
		m_outChanges.resize(m_params.size() + g_extraQueues, g_maxOutPoints);
		m_hasPendingValue = false;
		m_hasPendingEdit = false;
		m_voiceCount = 0;
		m_active = true;
		return true;
	}

	void Vst3Plugin::deactivate()
	{
		if (!m_active)
			return;
		stopComponent();
		// The restart the plugin asked for while active: now its buses and MIDI mapping can be read again.
		if (m_busesChanged)
			busesChanged();
		else if (m_mappingChanged)
			readMidiMapping();
		m_busesChanged = false;
		m_mappingChanged = false;
	}

	void Vst3Plugin::stopComponent() noexcept
	{
		if (m_processing.load(std::memory_order_acquire))
			stopProcessing();
		m_component->setActive(g_false);
		m_active = false;
		// What a pending value or edit still had to tell the processor goes with the next activation's first block.
		if (m_hasPendingValue)
			m_toProcessor->tryPush(m_pendingValue);
		if (m_hasPendingEdit && m_pendingEdit.kind == Edit::Value)
			m_toProcessor->tryPush(Edit{Edit::Silent, m_pendingEdit.id, m_pendingEdit.value});
		m_hasPendingValue = false;
		m_hasPendingEdit = false;
	}

	void Vst3Plugin::busesChanged()
	{
		const auto sameNotePorts = [](const std::vector<Bus>& a, const std::vector<Bus>& b)
		{
			return std::equal(a.begin(), a.end(), b.begin(), b.end(),
				[](const Bus& x, const Bus& y) { return x.name == y.name && x.channelCount == y.channelCount; });
		};
		const std::vector<Bus> eventInputs = m_eventInputs;
		const std::vector<Bus> eventOutputs = m_eventOutputs;
		readBuses();
		readMidiMapping();
		if (m_hostAudioPorts != nullptr)
			m_hostAudioPorts->rescanAudioPorts();
		if (m_hostNotePorts != nullptr &&
			(!sameNotePorts(eventInputs, m_eventInputs) || !sameNotePorts(eventOutputs, m_eventOutputs)))
			m_hostNotePorts->rescanNotePorts(plugin::HostNotePorts::All);
	}

	bool Vst3Plugin::startProcessing() noexcept
	{
		if (!m_active)
			return false;
		const vst::tresult result = m_processor->setProcessing(1);
		if (result != kResultOk && result != vst::kNotImplemented)
			return false;
		m_processing.store(true, std::memory_order_release);
		return true;
	}

	void Vst3Plugin::stopProcessing() noexcept
	{
		if (!m_processing.load(std::memory_order_acquire))
			return;
		m_processor->setProcessing(0);
		m_processing.store(false, std::memory_order_release);
	}

	void Vst3Plugin::reset() noexcept
	{
		if (!m_processing.load(std::memory_order_acquire))
			return;
		m_processor->setProcessing(0);
		m_processor->setProcessing(1);
		m_voiceCount = 0;
	}

	void* Vst3Plugin::extension(std::string_view id)
	{
		if (id == plugin::NotePorts::extensionId() && m_eventInputs.empty() && m_eventOutputs.empty())
			return nullptr;
		if ((id == plugin::Presets::extensionId() || id == plugin::NoteNames::extensionId()) && m_units == nullptr)
			return nullptr;
		return plugin::provide<plugin::AudioPorts, plugin::ConfigurableAudioPorts, plugin::AudioPortsActivation,
			plugin::NotePorts, plugin::Params, plugin::State, plugin::Presets, plugin::Latency, plugin::Tail,
			plugin::NoteNames>(this, id);
	}

	void Vst3Plugin::onMainThread()
	{
		// What the plugin called from other threads, now on the main thread, in order.
		Deferred call;
		while (m_deferred.tryPop(call))
			runDeferred(call);
		// The controller learns the values the processor got or reported.
		Update update;
		while (m_toController->tryPop(update))
		{
			if (m_controller != nullptr)
				m_controller->setParamNormalized(update.id, update.value);
		}
	}

	// ---- Processing ----

	plugin::ProcessStatus Vst3Plugin::process(const plugin::Process& process) noexcept
	{
		if (!m_processing.load(std::memory_order_relaxed))
			return plugin::ProcessStatus::Error;

		// One sub-block per transport segment.
		const plugin::TransportSegment fallback{};
		const plugin::TransportSegment* segments = process.transportCount > 0 ? process.transport : &fallback;
		const uint32_t segmentCount = std::max(process.transportCount, 1U);
		const uint32_t frames = std::min(process.frames, m_maxFrames);
		// The mode of setupProcessing: Process::mode only informs, and VST3 wants both the same.
		const vst::int32 mode = vstProcessMode(m_mode);

		// Silence flags of the inputs: constant channels that are zero.
		for (size_t bus = 0; bus < m_inputBuffers.size(); ++bus)
		{
			uint64_t silence = 0;
			if (bus < process.audioInputCount)
			{
				const plugin::AudioBuffer& in = process.audioInputs[bus];
				for (uint32_t c = 0; c < std::min<uint32_t>(in.channelCount, 64); ++c)
				{
					if ((in.constantMask & (uint64_t{1} << c)) != 0 && (frames == 0 || in.channels[c][0] == 0.0f))
						silence |= uint64_t{1} << c;
				}
			}
			m_inputBuffers[bus].silenceFlags = silence;
		}
		std::array<uint64_t, 64> outputSilence{};
		outputSilence.fill(~uint64_t{0});

		size_t next = 0; // the next input event
		uint32_t start = 0;
		bool first = true;
		bool ok = true;
		for (uint32_t s = 0; s < segmentCount; ++s)
		{
			const bool last = s + 1 == segmentCount;
			const uint32_t end = last ? frames : std::clamp(segments[s + 1].offset, start, frames);
			if (end == start && !last && !first)
				continue;
			m_inChanges.clear();
			m_outChanges.clear();
			m_inEvents.clear();
			m_outEvents.clear();
			m_outputs.clear();
			m_subBlockStart = start;
			if (first)
				takeMainThreadChanges(); // at the start of the block
			while (next < process.eventsIn.size() && (last || process.eventsIn[next].offset < end))
			{
				const plugin::Event& event = process.eventsIn[next++];
				const uint32_t offset = end > start ? std::min(std::max(event.offset, start), end - 1) : start;
				inputEvent(event, process.eventsIn, static_cast<vst::int32>(offset - start));
			}

			// Scratch channels: the inputs' first (silence), then the outputs'.
			size_t scratch = 0;
			for (size_t bus = 0; bus < m_inputBuffers.size(); ++bus)
			{
				const plugin::AudioBuffer* in = bus < process.audioInputCount ? &process.audioInputs[bus] : nullptr;
				std::vector<float*>& pointers = m_inputChannels[bus];
				for (size_t c = 0; c < pointers.size(); ++c, ++scratch)
				{
					pointers[c] = in != nullptr && c < in->channelCount ? in->channels[c] + start
																		: m_scratch.data() + (scratch * m_maxFrames);
				}
			}
			for (size_t bus = 0; bus < m_outputBuffers.size(); ++bus)
			{
				const plugin::AudioBuffer* out = bus < process.audioOutputCount ? &process.audioOutputs[bus] : nullptr;
				std::vector<float*>& pointers = m_outputChannels[bus];
				for (size_t c = 0; c < pointers.size(); ++c, ++scratch)
				{
					pointers[c] = out != nullptr && c < out->channelCount ? out->channels[c] + start
																		  : m_scratch.data() + (scratch * m_maxFrames);
				}
				m_outputBuffers[bus].silenceFlags = 0;
			}

			setContext(process, segments[s], start);
			vst::Vst::ProcessData data;
			data.processMode = mode;
			data.symbolicSampleSize = vst::Vst::kSample32;
			data.numSamples = static_cast<vst::int32>(end - start);
			data.numInputs = static_cast<vst::int32>(m_inputBuffers.size());
			data.numOutputs = static_cast<vst::int32>(m_outputBuffers.size());
			data.inputs = m_inputBuffers.empty() ? nullptr : m_inputBuffers.data();
			data.outputs = m_outputBuffers.empty() ? nullptr : m_outputBuffers.data();
			data.inputParameterChanges = &m_inChanges;
			data.outputParameterChanges = &m_outChanges;
			data.inputEvents = &m_inEvents;
			data.outputEvents = &m_outEvents;
			data.processContext = &m_context;
			ok = succeeded(runProcess(data)) && ok;

			// A channel the plugin calls silent is made silent, so its constant mask holds.
			for (size_t bus = 0; bus < m_outputBuffers.size(); ++bus)
			{
				const uint64_t flags = m_outputBuffers[bus].silenceFlags;
				for (size_t c = 0; c < m_outputChannels[bus].size() && c < 64; ++c)
				{
					if ((flags & (uint64_t{1} << c)) != 0)
						std::fill(m_outputChannels[bus][c], m_outputChannels[bus][c] + (end - start), 0.0f);
				}
				if (bus < 64)
					outputSilence[bus] &= flags;
			}
			collectOutputs(start);
			emitOutputs(process.eventsOut);
			start = end;
			first = false;
		}

		for (uint32_t bus = 0; bus < process.audioOutputCount; ++bus)
		{
			const uint64_t silence = bus < m_outputBuffers.size() && bus < 64 ? outputSilence[bus] : 0;
			const uint32_t channels = std::min<uint32_t>(process.audioOutputs[bus].channelCount, 64);
			const uint64_t all = channels == 64 ? ~uint64_t{0} : (uint64_t{1} << channels) - 1;
			process.audioOutputs[bus].constantMask = silence & all;
		}
		return ok ? plugin::ProcessStatus::Continue : plugin::ProcessStatus::Error;
	}

	vst::tresult Vst3Plugin::runProcess(vst::Vst::ProcessData& data) noexcept
	{
		const vst::tresult result = m_processor->process(data);
		if (m_outChanges.getParameterCount() > 0)
			m_host.requestCallback();
		return result;
	}

	void Vst3Plugin::takeMainThreadChanges() noexcept
	{
		// Values from the main thread, in order: one that didn't fit into the last block first. What doesn't fit stays
		// queued for the next block, so nothing is lost.
		if (m_hasPendingValue)
		{
			if (!m_inChanges.add(m_pendingValue.id, 0, m_pendingValue.value))
				return;
			m_hasPendingValue = false;
		}
		Edit edit;
		while (m_toProcessor->tryPop(edit))
		{
			if (!m_inChanges.add(edit.id, 0, edit.value))
			{
				m_pendingValue = edit;
				m_hasPendingValue = true;
				return;
			}
		}
		drainEdits(nullptr, true);
	}

	void Vst3Plugin::drainEdits(plugin::EventList* out, bool toProcessor) noexcept
	{
		// While processing, the edits go to the processor at offset 0 and become output events of the first
		// sub-block. While not processing (flushParams on the main thread) they become output events at once, and the
		// processor learns the values at its next block.
		if (!toProcessor && out == nullptr)
			return; // process() takes the edits when it builds the first sub-block
		if (toProcessor && m_hasPendingEdit)
		{
			if (!m_inChanges.add(m_pendingEdit.id, 0, m_pendingEdit.value))
				return;
			m_hasPendingEdit = false;
		}
		Edit edit;
		while (m_edits.tryPop(edit))
		{
			const Param* param = findParam(edit.id);
			plugin::Event event;
			event.param = edit.id;
			if (edit.kind == Edit::Begin)
			{
				event.type = plugin::EventType::ParamGestureBegin;
			}
			else if (edit.kind == Edit::End)
			{
				event.type = plugin::EventType::ParamGestureEnd;
			}
			else
			{
				event.type = plugin::EventType::ParamValue;
				event.value = param != nullptr ? fromNormalized(*param, edit.value) : edit.value;
				if (!toProcessor)
				{
					m_toProcessor->tryPush(Edit{Edit::Silent, edit.id, edit.value});
				}
				else if (!m_inChanges.add(edit.id, 0, edit.value))
				{
					// The value goes to the processor with the next block, and the later edits after it.
					m_pendingEdit = edit;
					m_hasPendingEdit = true;
				}
			}
			if (out != nullptr)
				out->push(event);
			else
				m_outputs.push(Output{event, -1});
			if (m_hasPendingEdit)
				return;
		}
	}

	void Vst3Plugin::setContext(
		const plugin::Process& process, const plugin::TransportSegment& segment, uint32_t start) noexcept
	{
		const plugin::Transport& transport = segment.transport;
		m_context = vst::Vst::ProcessContext{};
		m_context.sampleRate = m_sampleRate;
		const auto since = static_cast<int64_t>(start) - static_cast<int64_t>(std::min(segment.offset, start));
		m_context.projectTimeSamples = transport.position + since;
		uint32_t state = 0;
		if ((transport.flags & plugin::Transport::Playing) != 0)
			state |= vst::Vst::ProcessContext::kPlaying;
		if ((transport.flags & plugin::Transport::Recording) != 0)
			state |= vst::Vst::ProcessContext::kRecording;
		if (process.steadyTime >= 0)
		{
			m_context.continousTimeSamples = process.steadyTime + start;
			state |= vst::Vst::ProcessContext::kContTimeValid;
		}
		const bool tempoValid = (transport.flags & plugin::Transport::TempoValid) != 0;
		if (tempoValid)
		{
			m_context.tempo = transport.tempo;
			state |= vst::Vst::ProcessContext::kTempoValid;
		}
		if ((transport.flags & plugin::Transport::MusicalValid) != 0)
		{
			const double quarters = tempoValid && m_sampleRate > 0.0
				? static_cast<double>(since) * transport.tempo / 60.0 / m_sampleRate
				: 0.0;
			m_context.projectTimeMusic = transport.positionQuarters + quarters;
			m_context.barPositionMusic = transport.barStart;
			m_context.timeSigNumerator = transport.timeSignatureNumerator;
			m_context.timeSigDenominator = transport.timeSignatureDenominator;
			state |= vst::Vst::ProcessContext::kProjectTimeMusicValid | vst::Vst::ProcessContext::kBarPositionValid |
				vst::Vst::ProcessContext::kTimeSigValid;
		}
		if ((transport.flags & plugin::Transport::Cycle) != 0)
		{
			m_context.cycleStartMusic = transport.cycleStart;
			m_context.cycleEndMusic = transport.cycleEnd;
			state |= vst::Vst::ProcessContext::kCycleActive | vst::Vst::ProcessContext::kCycleValid;
		}
		m_context.state = state;
	}

	void Vst3Plugin::hostParamChange(vst::Vst::ParamID id, double normalized, vst::int32 offset) noexcept
	{
		m_inChanges.add(id, offset, normalized);
		if (m_toController->tryPush(Update{id, normalized}))
			m_host.requestCallback();
	}

	void Vst3Plugin::inputEvent(const plugin::Event& event, const plugin::EventList& list, vst::int32 offset) noexcept
	{
		if (event.space != plugin::g_coreEventSpace)
			return; // no other namespace has a meaning in VST3 yet
		switch (event.type)
		{
		case plugin::EventType::NoteOn:
			noteOn(event.port, event.channel, event.key, event.noteId, static_cast<float>(event.value), event.flags,
				offset);
			break;
		case plugin::EventType::NoteOff:
			noteOff(event.port, event.channel, event.key, event.noteId, static_cast<float>(event.value), event.flags,
				offset);
			break;
		case plugin::EventType::NoteChoke:
			noteOff(event.port, event.channel, event.key, event.noteId, 0.0f, event.flags, offset);
			break;
		case plugin::EventType::NoteExpression:
			noteExpression(event, offset);
			break;
		case plugin::EventType::ParamValue:
			// VST3 has no per-note parameters, so only global values go to the plugin.
			if (event.noteId == -1 && event.key == -1)
			{
				if (const Param* param = findParam(event.param))
					hostParamChange(event.param, toNormalized(*param, event.value), offset);
			}
			break;
		case plugin::EventType::Midi:
		{
			const std::array<uint8_t, 3> bytes{static_cast<uint8_t>(event.data[0]),
				static_cast<uint8_t>(event.data[0] >> 8), static_cast<uint8_t>(event.data[0] >> 16)};
			midi1(event, bytes.data(), offset);
			break;
		}
		case plugin::EventType::Midi2:
			midi2(event, offset);
			break;
		case plugin::EventType::Sysex:
		{
			vst::Vst::Event out{};
			out.busIndex = busIndex(event.port);
			out.sampleOffset = offset;
			out.type = vst::Vst::Event::kDataEvent;
			out.data.type = vst::Vst::DataEvent::kMidiSysEx;
			out.data.size = event.data[1];
			out.data.bytes = list.bytes(event);
			addEvent(out);
			break;
		}
		case plugin::EventType::ParamMod: // nothing to modulate in VST3: the engine sends values
		case plugin::EventType::NoteEnd:
		case plugin::EventType::ParamGestureBegin:
		case plugin::EventType::ParamGestureEnd:
			break;
		}
	}

	void Vst3Plugin::addEvent(vst::Vst::Event& event) noexcept
	{
		if (event.busIndex < 0 || static_cast<size_t>(event.busIndex) >= m_eventInputs.size())
			return;
		m_inEvents.addEvent(event);
	}

	void Vst3Plugin::noteOn(int16_t port, int16_t channel, int16_t key, int32_t noteId, float velocity, uint16_t flags,
		vst::int32 offset) noexcept
	{
		if (key < 0 || key > 127)
			return;
		// Every note gets an id, so expressions without one can find it.
		const int32_t id = noteId >= 0 ? noteId : m_nextNoteId;
		if (noteId < 0)
			m_nextNoteId = (m_nextNoteId + 1) & 0x3fffffff;
		const int16_t ch = channel < 0 ? 0 : channel;
		if (m_voiceCount < m_voices.size())
			m_voices[m_voiceCount++] = Voice{port < 0 ? int16_t{0} : port, ch, key, id};
		vst::Vst::Event event{};
		event.busIndex = busIndex(port);
		event.sampleOffset = offset;
		event.flags = (flags & plugin::Event::Live) != 0 ? vst::Vst::Event::kIsLive : 0;
		event.type = vst::Vst::Event::kNoteOnEvent;
		event.noteOn.channel = ch;
		event.noteOn.pitch = key;
		event.noteOn.velocity = std::clamp(velocity, 0.0f, 1.0f);
		event.noteOn.noteId = id;
		addEvent(event);
	}

	void Vst3Plugin::noteOff(int16_t port, int16_t channel, int16_t key, int32_t noteId, float velocity, uint16_t flags,
		vst::int32 offset) noexcept
	{
		auto send = [&](const Voice& voice)
		{
			vst::Vst::Event event{};
			event.busIndex = voice.port;
			event.sampleOffset = offset;
			event.flags = (flags & plugin::Event::Live) != 0 ? vst::Vst::Event::kIsLive : 0;
			event.type = vst::Vst::Event::kNoteOffEvent;
			event.noteOff.channel = voice.channel;
			event.noteOff.pitch = voice.key;
			event.noteOff.velocity = std::clamp(velocity, 0.0f, 1.0f);
			event.noteOff.noteId = voice.id;
			addEvent(event);
		};
		bool found = false;
		for (size_t i = 0; i < m_voiceCount;)
		{
			const Voice voice = m_voices[i];
			if (matches(port, voice.port) && matches(channel, voice.channel) && matches(key, voice.key) &&
				(noteId < 0 || noteId == voice.id))
			{
				send(voice);
				m_voices[i] = m_voices[--m_voiceCount];
				found = true;
			}
			else
			{
				++i;
			}
		}
		if (!found && key >= 0 && key <= 127)
			send(Voice{port < 0 ? int16_t{0} : port, channel < 0 ? int16_t{0} : channel, key, noteId});
	}

	void Vst3Plugin::noteExpression(const plugin::Event& event, vst::int32 offset) noexcept
	{
		vst::Vst::NoteExpressionTypeID type = 0;
		double normalized = 0.0;
		const bool pressure = event.expression == g_pressure;
		if (!pressure && !expressionToVst(event.expression, event.value, type, normalized))
			return;
		auto send = [&](const Voice& voice)
		{
			vst::Vst::Event out{};
			out.busIndex = voice.port;
			out.sampleOffset = offset;
			if (pressure)
			{
				out.type = vst::Vst::Event::kPolyPressureEvent;
				out.polyPressure.channel = voice.channel;
				out.polyPressure.pitch = voice.key;
				out.polyPressure.pressure = static_cast<float>(std::clamp(event.value, 0.0, 1.0));
				out.polyPressure.noteId = voice.id;
			}
			else
			{
				out.type = vst::Vst::Event::kNoteExpressionValueEvent;
				out.noteExpressionValue.typeId = type;
				out.noteExpressionValue.noteId = voice.id;
				out.noteExpressionValue.value = normalized;
			}
			addEvent(out);
		};
		bool found = false;
		for (size_t i = 0; i < m_voiceCount; ++i)
		{
			const Voice& voice = m_voices[i];
			if (matches(event.port, voice.port) && matches(event.channel, voice.channel) &&
				matches(event.key, voice.key) && (event.noteId < 0 || event.noteId == voice.id))
			{
				send(voice);
				found = true;
			}
		}
		// Without a voice, an expression needs a note id; poly pressure only a key, as in MIDI.
		if (!found && (event.noteId >= 0 || (pressure && event.key >= 0 && event.key <= 127)))
			send(Voice{event.port < 0 ? int16_t{0} : event.port, event.channel < 0 ? int16_t{0} : event.channel,
				event.key, event.noteId});
	}

	void Vst3Plugin::controller(
		int16_t port, int16_t channel, int32_t control, double normalized, vst::int32 offset) noexcept
	{
		const auto bus = static_cast<size_t>(busIndex(port));
		const size_t controls = vst::Vst::kCountCtrlNumber;
		if (m_midiMap.empty() || bus >= m_eventInputs.size() || control < 0 || static_cast<size_t>(control) >= controls)
			return;
		const auto ch = static_cast<size_t>(std::clamp<int16_t>(channel, 0, 15));
		const vst::Vst::ParamID id = m_midiMap[(((bus * g_channels) + ch) * controls) + static_cast<size_t>(control)];
		if (id != vst::Vst::kNoParamId)
			mappedChange(id, std::clamp(normalized, 0.0, 1.0), offset);
	}

	void Vst3Plugin::programChange(int16_t port, int16_t channel, uint32_t program, vst::int32 offset) noexcept
	{
		const auto bus = static_cast<size_t>(busIndex(port));
		if (bus >= m_eventInputs.size() || m_programMap.empty())
			return;
		const auto ch = static_cast<size_t>(std::clamp<int16_t>(channel, 0, 15));
		const vst::Vst::ParamID id = m_programMap[(bus * g_channels) + ch];
		const Param* param = id != vst::Vst::kNoParamId ? findParam(id) : nullptr;
		// A program the list doesn't have is ignored.
		if (param == nullptr || program > static_cast<uint32_t>(param->stepCount))
			return;
		mappedChange(id, static_cast<double>(program) / param->stepCount, offset);
	}

	void Vst3Plugin::mappedChange(vst::Vst::ParamID id, double normalized, vst::int32 offset) noexcept
	{
		hostParamChange(id, normalized, offset);
		// The engine's mirror learns the value as a change the plugin made.
		plugin::Event event;
		event.type = plugin::EventType::ParamValue;
		event.flags = plugin::Event::DontRecord;
		event.param = id;
		const Param* param = findParam(id);
		event.value = param != nullptr ? fromNormalized(*param, normalized) : normalized;
		event.offset = m_subBlockStart + static_cast<uint32_t>(offset);
		m_outputs.push(Output{event, -1});
	}

	void Vst3Plugin::midi1(const plugin::Event& event, const uint8_t* bytes, vst::int32 offset) noexcept
	{
		const uint8_t status = bytes[0];
		const auto channel = static_cast<int16_t>(status & 0x0f);
		const uint8_t data1 = bytes[1] & 0x7f;
		const uint8_t data2 = bytes[2] & 0x7f;
		switch (status & 0xf0)
		{
		case 0x90:
			if (data2 > 0)
			{
				noteOn(event.port, channel, data1, -1, static_cast<float>(data2) / 127.0f, event.flags, offset);
				break;
			}
			[[fallthrough]];
		case 0x80:
			noteOff(event.port, channel, data1, -1, static_cast<float>(data2) / 127.0f, event.flags, offset);
			break;
		case 0xa0:
		{
			plugin::Event pressure = event;
			pressure.type = plugin::EventType::NoteExpression;
			pressure.expression = g_pressure;
			pressure.channel = channel;
			pressure.key = data1;
			pressure.noteId = -1;
			pressure.value = data2 / 127.0;
			noteExpression(pressure, offset);
			break;
		}
		case 0xb0:
			controller(event.port, channel, data1, data2 / 127.0, offset);
			break;
		case 0xc0:
			programChange(event.port, channel, data1, offset);
			break;
		case 0xd0:
			controller(event.port, channel, vst::Vst::kAfterTouch, data1 / 127.0, offset);
			break;
		case 0xe0:
			controller(event.port, channel, vst::Vst::kPitchBend, ((data2 << 7) | data1) / 16383.0, offset);
			break;
		default:
			break;
		}
	}

	void Vst3Plugin::midi2(const plugin::Event& event, vst::int32 offset) noexcept
	{
		const uint32_t word = event.data[0];
		const uint32_t type = word >> 28;
		const auto status = static_cast<uint8_t>(word >> 16);
		if (type == 0x2) // MIDI 1.0 channel voice message in a packet
		{
			const std::array<uint8_t, 3> bytes{status, static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
			midi1(event, bytes.data(), offset);
			return;
		}
		if (type != 0x4) // only MIDI 2.0 channel voice messages
			return;
		const auto channel = static_cast<int16_t>(status & 0x0f);
		const auto index = static_cast<int16_t>((word >> 8) & 0x7f);
		const uint32_t data = event.data[1];
		const double value = data / g_maxUint32;
		switch (status & 0xf0)
		{
		case 0x90: // a velocity of 0 is a note on in MIDI 2.0
			noteOn(event.port, channel, index, -1, static_cast<float>((data >> 16) / 65535.0), event.flags, offset);
			break;
		case 0x80:
			noteOff(event.port, channel, index, -1, static_cast<float>((data >> 16) / 65535.0), event.flags, offset);
			break;
		case 0xa0:
		case 0x60: // per-note pitch bend
		{
			plugin::Event expression = event;
			expression.type = plugin::EventType::NoteExpression;
			expression.channel = channel;
			expression.key = index;
			expression.noteId = -1;
			if ((status & 0xf0) == 0xa0)
			{
				expression.expression = g_pressure;
				expression.value = value;
			}
			else
			{
				expression.expression = plugin::expressionId(plugin::NoteExpression::Tuning);
				expression.value = (value - 0.5) * 2.0 * g_perNoteBendRange;
			}
			noteExpression(expression, offset);
			break;
		}
		case 0xb0:
			controller(event.port, channel, index, value, offset);
			break;
		case 0xc0: // with a valid bank, the bank selects come first, as in MIDI 1.0
			if ((word & 1U) != 0)
			{
				controller(event.port, channel, g_bankSelect, ((data >> 8) & 0x7f) / 127.0, offset);
				controller(event.port, channel, g_bankSelectLsb, (data & 0x7f) / 127.0, offset);
			}
			programChange(event.port, channel, (data >> 24) & 0x7f, offset);
			break;
		case 0xd0:
			controller(event.port, channel, vst::Vst::kAfterTouch, value, offset);
			break;
		case 0xe0:
			controller(event.port, channel, vst::Vst::kPitchBend, value, offset);
			break;
		default:
			break;
		}
	}

	void Vst3Plugin::collectOutputs(uint32_t start) noexcept
	{
		auto add = [this](const plugin::Event& event, int32_t sysex) { m_outputs.push(Output{event, sysex}); };
		bool changed = false;
		for (vst::int32 i = 0; i < m_outChanges.getParameterCount(); ++i)
		{
			vst::Vst::IParamValueQueue* queue = m_outChanges.getParameterData(i);
			const vst::Vst::ParamID id = queue->getParameterId();
			const Param* param = findParam(id);
			for (vst::int32 p = 0; p < queue->getPointCount(); ++p)
			{
				vst::int32 offset = 0;
				vst::Vst::ParamValue value = 0.0;
				if (!succeeded(queue->getPoint(p, offset, value)))
					continue;
				plugin::Event event;
				event.offset = start + static_cast<uint32_t>(std::max(offset, 0));
				event.type = plugin::EventType::ParamValue;
				event.flags = plugin::Event::DontRecord;
				event.param = id;
				event.value = param != nullptr ? fromNormalized(*param, value) : value;
				add(event, -1);
				changed = m_toController->tryPush(Update{id, value}) || changed;
			}
		}
		if (changed)
			m_host.requestCallback();

		for (size_t i = 0; i < m_outEvents.size(); ++i)
		{
			const vst::Vst::Event& in = m_outEvents[i];
			plugin::Event event;
			event.offset = start + static_cast<uint32_t>(std::max(in.sampleOffset, 0));
			event.port = static_cast<int16_t>(in.busIndex);
			switch (in.type)
			{
			case vst::Vst::Event::kNoteOnEvent:
				event.type = plugin::EventType::NoteOn;
				event.channel = in.noteOn.channel;
				event.key = in.noteOn.pitch;
				event.noteId = in.noteOn.noteId;
				event.value = in.noteOn.velocity;
				add(event, -1);
				break;
			case vst::Vst::Event::kNoteOffEvent:
				event.type = plugin::EventType::NoteOff;
				event.channel = in.noteOff.channel;
				event.key = in.noteOff.pitch;
				event.noteId = in.noteOff.noteId;
				event.value = in.noteOff.velocity;
				add(event, -1);
				break;
			case vst::Vst::Event::kNoteExpressionValueEvent:
				event.type = plugin::EventType::NoteExpression;
				event.noteId = in.noteExpressionValue.noteId;
				if (expressionFromVst(
						in.noteExpressionValue.typeId, in.noteExpressionValue.value, event.expression, event.value))
					add(event, -1);
				break;
			case vst::Vst::Event::kPolyPressureEvent:
				event.type = plugin::EventType::NoteExpression;
				event.expression = g_pressure;
				event.channel = in.polyPressure.channel;
				event.key = in.polyPressure.pitch;
				event.noteId = in.polyPressure.noteId;
				event.value = in.polyPressure.pressure;
				add(event, -1);
				break;
			case vst::Vst::Event::kDataEvent:
				if (in.data.type == vst::Vst::DataEvent::kMidiSysEx && in.data.bytes != nullptr)
				{
					event.type = plugin::EventType::Sysex;
					add(event, static_cast<int32_t>(i));
				}
				break;
			case vst::Vst::Event::kLegacyMIDICCOutEvent:
			{
				const vst::Vst::LegacyMIDICCOutEvent& cc = in.midiCCOut;
				const auto channel = static_cast<uint32_t>(cc.channel & 0x0f);
				const auto value = static_cast<uint32_t>(cc.value & 0x7f);
				const auto value2 = static_cast<uint32_t>(cc.value2 & 0x7f);
				uint32_t bytes = 0;
				if (cc.controlNumber < 128)
					bytes = (0xb0 | channel) | (uint32_t{cc.controlNumber} << 8) | (value << 16);
				else if (cc.controlNumber == vst::Vst::kAfterTouch)
					bytes = (0xd0 | channel) | (value << 8);
				else if (cc.controlNumber == vst::Vst::kPitchBend)
					bytes = (0xe0 | channel) | (value << 8) | (value2 << 16);
				else if (cc.controlNumber == vst::Vst::kCtrlPolyPressure)
					bytes = (0xa0 | channel) | (value << 8) | (value2 << 16);
				else if (cc.controlNumber == vst::Vst::kCtrlProgramChange)
					bytes = (0xc0 | channel) | (value << 8);
				if (bytes != 0)
				{
					event.type = plugin::EventType::Midi;
					event.data[0] = bytes;
					add(event, -1);
				}
				break;
			}
			default:
				break;
			}
		}
	}

	void Vst3Plugin::emitOutputs(plugin::EventList& out) noexcept
	{
		// In order of their offsets, keeping the order of equal ones: insertion sort, as the outputs are mostly sorted.
		for (size_t i = 1; i < m_outputs.size(); ++i)
		{
			const Output output = m_outputs[i];
			size_t j = i;
			for (; j > 0 && m_outputs[j - 1].event.offset > output.event.offset; --j)
				m_outputs[j] = m_outputs[j - 1];
			m_outputs[j] = output;
		}
		for (const Output& output : m_outputs)
		{
			if (output.sysex >= 0)
			{
				const vst::Vst::DataEvent& data = m_outEvents[static_cast<size_t>(output.sysex)].data;
				out.pushSysex(output.event.offset, output.event.port, data.bytes, data.size);
			}
			else
			{
				out.push(output.event);
			}
		}
		m_outputs.clear();
	}

	// ---- Ports ----

	uint32_t Vst3Plugin::audioPortCount(bool input) const
	{
		return static_cast<uint32_t>((input ? m_audioInputs : m_audioOutputs).size());
	}

	bool Vst3Plugin::audioPortInfo(bool input, uint32_t index, plugin::AudioPortInfo& info) const
	{
		const std::vector<Bus>& buses = input ? m_audioInputs : m_audioOutputs;
		if (index >= buses.size())
			return false;
		const Bus& bus = buses[index];
		info = plugin::AudioPortInfo{};
		info.id = index;
		info.name = bus.name;
		info.channelCount = bus.channelCount;
		info.layout = layoutOf(bus.arrangement);
		if (info.layout == plugin::ChannelLayout::Surround)
			info.speakers = speakerMask(bus.arrangement);
		info.flags = bus.main ? plugin::AudioPortInfo::Main : plugin::AudioPortInfo::Optional;
		return true;
	}

	bool Vst3Plugin::applyAudioPortRequests(const std::vector<plugin::AudioPortRequest>& requests)
	{
		if (!m_initialized || m_active)
			return false;
		std::vector<vst::Vst::SpeakerArrangement> inputs;
		std::vector<vst::Vst::SpeakerArrangement> outputs;
		inputs.reserve(m_audioInputs.size());
		outputs.reserve(m_audioOutputs.size());
		for (const Bus& bus : m_audioInputs)
			inputs.push_back(bus.arrangement);
		for (const Bus& bus : m_audioOutputs)
			outputs.push_back(bus.arrangement);
		const std::vector<vst::Vst::SpeakerArrangement> oldInputs = inputs;
		const std::vector<vst::Vst::SpeakerArrangement> oldOutputs = outputs;
		for (const plugin::AudioPortRequest& request : requests)
		{
			std::vector<vst::Vst::SpeakerArrangement>& list = request.input ? inputs : outputs;
			if (request.index >= list.size() || !arrangementOf(request, list[request.index]))
				return false;
		}
		auto apply = [this](
						 std::vector<vst::Vst::SpeakerArrangement>& in, std::vector<vst::Vst::SpeakerArrangement>& out)
		{
			return m_processor->setBusArrangements(in.empty() ? nullptr : in.data(), static_cast<vst::int32>(in.size()),
				out.empty() ? nullptr : out.data(), static_cast<vst::int32>(out.size()));
		};
		const bool accepted = succeeded(apply(inputs, outputs));
		readBuses();
		bool applied = accepted;
		for (size_t i = 0; applied && i < inputs.size(); ++i)
			applied = i < m_audioInputs.size() && m_audioInputs[i].arrangement == inputs[i];
		for (size_t i = 0; applied && i < outputs.size(); ++i)
			applied = i < m_audioOutputs.size() && m_audioOutputs[i].arrangement == outputs[i];
		if (!applied)
		{
			// All or nothing: back to the arrangements before.
			std::vector<vst::Vst::SpeakerArrangement> in = oldInputs;
			std::vector<vst::Vst::SpeakerArrangement> out = oldOutputs;
			apply(in, out);
			readBuses();
		}
		return applied;
	}

	bool Vst3Plugin::setAudioPortActive(bool input, uint32_t index, bool active)
	{
		std::vector<Bus>& buses = input ? m_audioInputs : m_audioOutputs;
		if (m_active || index >= buses.size())
			return false; // VST3 activates buses only while the component is inactive
		buses[index].active = active;
		return true;
	}

	uint32_t Vst3Plugin::notePortCount(bool input) const
	{
		return static_cast<uint32_t>((input ? m_eventInputs : m_eventOutputs).size());
	}

	bool Vst3Plugin::notePortInfo(bool input, uint32_t index, plugin::NotePortInfo& info) const
	{
		const std::vector<Bus>& buses = input ? m_eventInputs : m_eventOutputs;
		if (index >= buses.size())
			return false;
		info = plugin::NotePortInfo{};
		info.id = index;
		info.name = buses[index].name;
		// The adapter turns MIDI 1.0 and MIDI 2.0 into VST3 events; outputs come back as notes and MIDI 1.0.
		info.dialects = plugin::NotePortInfo::Notes | plugin::NotePortInfo::Midi |
			(input ? uint32_t{plugin::NotePortInfo::Midi2} : 0U);
		info.preferred = plugin::NotePortInfo::Notes;
		return true;
	}

	// ---- Parameters ----

	const Vst3Plugin::Param* Vst3Plugin::findParam(uint32_t id) const noexcept
	{
		const auto it = std::lower_bound(m_paramIndex.begin(), m_paramIndex.end(), id,
			[](const std::pair<vst::Vst::ParamID, size_t>& entry, uint32_t key) { return entry.first < key; });
		return it != m_paramIndex.end() && it->first == id ? &m_params[it->second] : nullptr;
	}

	int32_t Vst3Plugin::stepOf(const Param& param, double normalized) noexcept
	{
		// VST3's rule for discrete values: of n steps, step k covers the normalized values from k / (n + 1) to
		// (k + 1) / (n + 1).
		const double steps = param.stepCount;
		return static_cast<int32_t>(std::min(steps, std::floor(std::clamp(normalized, 0.0, 1.0) * (steps + 1.0))));
	}

	double Vst3Plugin::toNormalized(const Param& param, double value) noexcept
	{
		// The range is 0 to 1 as VST3's, so a value is its normalized value: a stepped one the step it falls on.
		return fromNormalized(param, value);
	}

	double Vst3Plugin::fromNormalized(const Param& param, double normalized) noexcept
	{
		if (param.stepCount <= 0)
			return std::clamp(normalized, 0.0, 1.0);
		return static_cast<double>(stepOf(param, normalized)) / param.stepCount;
	}

	std::string Vst3Plugin::unitPath(vst::Vst::UnitID unit) const
	{
		if (m_units == nullptr)
			return {};
		std::vector<vst::Vst::UnitInfo> units(static_cast<size_t>(std::max(m_units->getUnitCount(), 0)));
		for (size_t i = 0; i < units.size(); ++i)
		{
			if (!succeeded(m_units->getUnitInfo(static_cast<vst::int32>(i), units[i])))
				units[i].id = vst::Vst::kNoParentUnitId;
		}
		std::string path;
		for (int depth = 0; depth < 32 && unit != vst::Vst::kRootUnitId && unit != vst::Vst::kNoParentUnitId; ++depth)
		{
			const auto it = std::find_if(
				units.begin(), units.end(), [unit](const vst::Vst::UnitInfo& info) { return info.id == unit; });
			if (it == units.end())
				break;
			const std::string name = toUtf8(it->name);
			if (!path.empty())
				path.insert(0, "/");
			path.insert(0, name);
			unit = it->parentUnitId;
		}
		return path;
	}

	uint32_t Vst3Plugin::paramCount() const
	{
		return static_cast<uint32_t>(m_params.size());
	}

	bool Vst3Plugin::paramInfo(uint32_t index, plugin::ParamInfo& info) const
	{
		if (index >= m_params.size())
			return false;
		info = m_params[index].info;
		return true;
	}

	bool Vst3Plugin::paramValue(uint32_t param, double& value) const
	{
		const Param* found = findParam(param);
		if (found == nullptr || m_controller == nullptr)
			return false;
		value = fromNormalized(*found, m_controller->getParamNormalized(param));
		return true;
	}

	bool Vst3Plugin::paramValueToText(uint32_t param, double value, std::string& text) const
	{
		const Param* found = findParam(param);
		if (found == nullptr || m_controller == nullptr)
			return false;
		vst::Vst::String128 string{};
		if (!succeeded(m_controller->getParamStringByValue(param, toNormalized(*found, value), string)))
			return false;
		text = toUtf8(string);
		return true;
	}

	bool Vst3Plugin::paramTextToValue(uint32_t param, std::string_view text, double& value) const
	{
		const Param* found = findParam(param);
		if (found == nullptr || m_controller == nullptr)
			return false;
		vst::Vst::String128 string{};
		if (!toString128(std::string(text), string))
			return false;
		vst::Vst::ParamValue normalized = 0.0;
		if (!succeeded(m_controller->getParamValueByString(param, string, normalized)))
			return false;
		value = fromNormalized(*found, normalized);
		return true;
	}

	void Vst3Plugin::flushParams(const plugin::EventList& in, plugin::EventList& out) noexcept
	{
		if (!m_initialized)
			return;
		if (!m_processing.load(std::memory_order_acquire))
		{
			// Main thread: the controller learns the values now, the processor with its next block.
			for (const plugin::Event& event : in)
			{
				if (event.space != plugin::g_coreEventSpace || event.type != plugin::EventType::ParamValue ||
					event.noteId != -1 || event.key != -1)
					continue;
				if (const Param* param = findParam(event.param))
				{
					const double normalized = toNormalized(*param, event.value);
					if (m_controller != nullptr)
						m_controller->setParamNormalized(event.param, normalized);
					m_toProcessor->tryPush(Edit{Edit::Silent, event.param, normalized});
				}
			}
			drainEdits(&out, false);
			return;
		}

		// Audio thread: a block without audio, as VST3 flushes parameters.
		m_inChanges.clear();
		m_outChanges.clear();
		m_inEvents.clear();
		m_outEvents.clear();
		m_outputs.clear();
		m_subBlockStart = 0;
		takeMainThreadChanges();
		for (const plugin::Event& event : in)
		{
			if (event.space == plugin::g_coreEventSpace && event.type == plugin::EventType::ParamValue)
				inputEvent(event, in, 0);
		}
		vst::Vst::ProcessData data;
		data.processMode = vstProcessMode(m_mode);
		data.symbolicSampleSize = vst::Vst::kSample32;
		data.inputParameterChanges = &m_inChanges;
		data.outputParameterChanges = &m_outChanges;
		data.inputEvents = &m_inEvents;
		data.outputEvents = &m_outEvents;
		runProcess(data);
		collectOutputs(0);
		emitOutputs(out);
	}

	// ---- Edits, restarts and other notifications from the controller ----

	bool Vst3Plugin::defer(const Deferred& call) noexcept
	{
		// Real-time safe: the plugin may call from its audio thread.
		const bool queued = m_deferred.tryPush(call);
		m_host.requestCallback();
		return queued;
	}

	void Vst3Plugin::runDeferred(const Deferred& call)
	{
		switch (call.kind)
		{
		case Deferred::Begin:
			editBegin(call.id);
			break;
		case Deferred::Value:
			editValue(call.id, call.value);
			break;
		case Deferred::End:
			editEnd(call.id);
			break;
		case Deferred::Restart:
			restart(call.flags);
			break;
		case Deferred::Dirty:
			stateDirty();
			break;
		case Deferred::ProgramList:
			programListChanged();
			break;
		}
	}

	bool Vst3Plugin::editBegin(uint32_t id)
	{
		if (!isMainThread())
			return defer(Deferred{Deferred::Begin, id, 0.0, 0});
		const bool queued = m_edits.tryPush(Edit{Edit::Begin, id, 0.0});
		if (m_hostParams != nullptr)
			m_hostParams->requestParamFlush();
		return queued;
	}

	bool Vst3Plugin::editValue(uint32_t id, double normalized)
	{
		if (!isMainThread())
			return defer(Deferred{Deferred::Value, id, normalized, 0});
		const bool queued = m_edits.tryPush(Edit{Edit::Value, id, normalized});
		if (m_hostParams != nullptr)
			m_hostParams->requestParamFlush();
		return queued;
	}

	bool Vst3Plugin::editEnd(uint32_t id)
	{
		if (!isMainThread())
			return defer(Deferred{Deferred::End, id, 0.0, 0});
		const bool queued = m_edits.tryPush(Edit{Edit::End, id, 0.0});
		if (m_hostParams != nullptr)
			m_hostParams->requestParamFlush();
		return queued;
	}

	bool Vst3Plugin::restart(int32_t flags)
	{
		if (!isMainThread())
			return defer(Deferred{Deferred::Restart, 0, 0.0, flags});
		if ((flags & (vst::Vst::kReloadComponent | vst::Vst::kIoChanged)) != 0)
		{
			if (m_active)
			{
				// The buses change with the restart: read again, and the host told, once it deactivated the plugin.
				m_busesChanged = true;
				m_host.requestRestart();
			}
			else
			{
				busesChanged();
			}
		}
		if ((flags & vst::Vst::kLatencyChanged) != 0)
		{
			if (m_hostLatency != nullptr)
				m_hostLatency->latencyChanged();
			if (m_active)
				m_host.requestRestart();
		}
		if ((flags & vst::Vst::kParamTitlesChanged) != 0 && m_controller != nullptr)
		{
			// Names and flags change in place; the audio thread only reads ids and step counts, which stay.
			for (Param& param : m_params)
			{
				for (vst::int32 i = 0; i < m_controller->getParameterCount(); ++i)
				{
					vst::Vst::ParameterInfo info{};
					if (succeeded(m_controller->getParameterInfo(i, info)) && info.id == param.info.id)
					{
						param.info.name = toUtf8(info.title);
						param.info.module = unitPath(info.unitId);
						break;
					}
				}
			}
			if (m_hostParams != nullptr)
				m_hostParams->rescanParams(plugin::HostParams::Info | plugin::HostParams::Text);
		}
		if ((flags & vst::Vst::kParamValuesChanged) != 0 && m_hostParams != nullptr)
			m_hostParams->rescanParams(plugin::HostParams::Values);
		if ((flags & vst::Vst::kMidiCCAssignmentChanged) != 0)
		{
			if (!m_active)
			{
				readMidiMapping();
			}
			else
			{
				m_mappingChanged = true; // read again once it is deactivated
				m_host.requestRestart();
			}
		}
		return true;
	}

	void Vst3Plugin::stateDirty()
	{
		if (!isMainThread())
		{
			defer(Deferred{Deferred::Dirty, 0, 0.0, 0});
			return;
		}
		if (m_hostState != nullptr)
			m_hostState->markStateDirty();
	}

	void Vst3Plugin::programListChanged()
	{
		if (!isMainThread())
		{
			defer(Deferred{Deferred::ProgramList, 0, 0.0, 0});
			return;
		}
		if (m_hostParams != nullptr)
			m_hostParams->rescanParams(plugin::HostParams::Values | plugin::HostParams::Text);
	}

	// ---- State and presets ----

	bool Vst3Plugin::saveState(std::vector<uint8_t>& data, plugin::StateContext /*context*/)
	{
		if (!m_initialized)
			return false;
		MemoryStream component;
		if (!succeeded(m_component->getState(&component)))
			return false;
		PresetFile preset;
		preset.classId = classIdText(m_classId);
		preset.component = component.data();
		if (m_controller != nullptr)
		{
			MemoryStream controller;
			if (succeeded(m_controller->getState(&controller)))
			{
				preset.controller = controller.data();
				preset.hasController = true;
			}
		}
		data = writePresetFile(preset);
		return true;
	}

	bool Vst3Plugin::loadState(const uint8_t* data, size_t size, plugin::StateContext /*context*/)
	{
		PresetFile preset;
		if (!m_initialized || !readPresetFile(data, size, preset) ||
			!sameClassId(preset.classId, classIdText(m_classId)))
			return false;
		MemoryStream component(preset.component.data(), preset.component.size());
		if (!succeeded(m_component->setState(&component)))
			return false;
		if (m_controller != nullptr)
		{
			component.rewind();
			m_controller->setComponentState(&component);
			if (preset.hasController)
			{
				MemoryStream controller(preset.controller.data(), preset.controller.size());
				m_controller->setState(&controller);
			}
		}
		if (m_hostParams != nullptr)
			m_hostParams->rescanParams(plugin::HostParams::Values);
		return true;
	}

	vst::Vst::ProgramListID Vst3Plugin::rootProgramList() const
	{
		if (m_units == nullptr)
			return vst::Vst::kNoProgramListId;
		for (vst::int32 i = 0; i < m_units->getUnitCount(); ++i)
		{
			vst::Vst::UnitInfo info{};
			if (succeeded(m_units->getUnitInfo(i, info)) && info.id == vst::Vst::kRootUnitId)
				return info.programListId;
		}
		return vst::Vst::kNoProgramListId;
	}

	const Vst3Plugin::Param* Vst3Plugin::programParam(vst::Vst::ProgramListID list) const
	{
		if (m_units == nullptr)
			return nullptr;
		for (const Param& param : m_params)
		{
			if (!param.programChange)
				continue;
			for (vst::int32 i = 0; i < m_units->getUnitCount(); ++i)
			{
				vst::Vst::UnitInfo info{};
				if (succeeded(m_units->getUnitInfo(i, info)) && info.id == param.unit && info.programListId == list)
					return &param;
			}
		}
		return nullptr;
	}

	std::vector<plugin::PresetInfo> Vst3Plugin::presets()
	{
		std::vector<plugin::PresetInfo> result;
		if (m_units == nullptr)
			return result;
		for (vst::int32 l = 0; l < m_units->getProgramListCount(); ++l)
		{
			vst::Vst::ProgramListInfo list{};
			if (!succeeded(m_units->getProgramListInfo(l, list)))
				continue;
			for (vst::int32 p = 0; p < list.programCount; ++p)
			{
				vst::Vst::String128 name{};
				if (!succeeded(m_units->getProgramName(list.id, p, name)))
					continue;
				plugin::PresetInfo preset;
				preset.name = toUtf8(name);
				preset.key = std::to_string(list.id) + ":" + std::to_string(p);
				preset.category = toUtf8(list.name);
				preset.flags = plugin::PresetInfo::Factory;
				result.push_back(std::move(preset));
			}
		}
		return result;
	}

	bool Vst3Plugin::loadPreset(const plugin::PresetInfo& preset)
	{
		if (!m_initialized)
			return false;
		if (!preset.location.empty())
			return loadPresetFile(preset.location);
		const size_t colon = preset.key.find(':');
		if (colon == std::string::npos)
			return false;
		vst::Vst::ProgramListID list = 0;
		int32_t index = 0;
		try
		{
			list = std::stoi(preset.key.substr(0, colon));
			index = std::stoi(preset.key.substr(colon + 1));
		}
		catch (const std::exception&)
		{
			return false;
		}
		const Param* param = programParam(list);
		if (param == nullptr || param->stepCount <= 0 || index < 0 || index > param->stepCount)
			return false;
		// A program change is a parameter change: the controller and the processor both take it.
		const double normalized = static_cast<double>(index) / param->stepCount;
		m_controller->setParamNormalized(param->info.id, normalized);
		m_toProcessor->tryPush(Edit{Edit::Silent, param->info.id, normalized});
		if (m_hostParams != nullptr)
			m_hostParams->rescanParams(plugin::HostParams::Values);
		return true;
	}

	bool Vst3Plugin::loadPresetFile(const std::string& path)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return false;
		const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
		return loadState(bytes.data(), bytes.size(), plugin::StateContext::Preset);
	}

	std::vector<plugin::NoteName> Vst3Plugin::noteNames()
	{
		std::vector<plugin::NoteName> names;
		const vst::Vst::ProgramListID list = rootProgramList();
		if (list == vst::Vst::kNoProgramListId || m_controller == nullptr)
			return names;
		int32_t program = 0;
		if (const Param* param = programParam(list))
			program = stepOf(*param, m_controller->getParamNormalized(param->info.id));
		if (m_units->hasProgramPitchNames(list, program) != vst::kResultTrue)
			return names;
		for (int16_t key = 0; key < 128; ++key)
		{
			vst::Vst::String128 name{};
			if (m_units->getProgramPitchName(list, program, key, name) == vst::kResultTrue)
				names.push_back(plugin::NoteName{toUtf8(name), -1, -1, key});
		}
		return names;
	}

	// ---- Latency and tail ----

	uint32_t Vst3Plugin::latency() const
	{
		return m_processor != nullptr ? m_processor->getLatencySamples() : 0;
	}

	uint32_t Vst3Plugin::tail() const noexcept
	{
		return m_tail; // VST3's kInfiniteTail is g_infiniteTail
	}
}
