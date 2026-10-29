// SPDX-License-Identifier: GPL-3.0-or-later
// Turns a test plugin (common/testplugin.h) into a CLAP plugin: the entry point, a factory with one plugin, and the
// audio-ports, note-ports, params, state and latency extensions. Linked into every .clap test plugin.
#include "../common/testplugin.h"

#include <clap/clap.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace gnubase::testplugins
{
	namespace
	{
		const char* const g_effectFeatures[] = {
			CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
		const char* const g_instrumentFeatures[] = {
			CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr};

		const clap_plugin_descriptor_t& clapDescriptor()
		{
			static const clap_plugin_descriptor_t descriptor = []
			{
				const Descriptor& plugin = pluginDescriptor();
				clap_plugin_descriptor_t d{};
				d.clap_version = CLAP_VERSION;
				d.id = plugin.id;
				d.name = plugin.name;
				d.vendor = "GNUbase";
				d.url = "https://github.com/GNUbase/GNUbase";
				d.manual_url = "";
				d.support_url = "";
				d.version = "0.1.0";
				d.description = plugin.description;
				d.features = plugin.instrument ? g_instrumentFeatures : g_effectFeatures;
				return d;
			}();
			return descriptor;
		}

		void copyName(char* to, size_t size, const char* from)
		{
			std::snprintf(to, size, "%s", from);
		}

		class ClapEventInput : public EventInput
		{
		public:
			explicit ClapEventInput(const clap_input_events_t* events) : m_events(events) {}

			uint32_t size() const override { return m_events != nullptr ? m_events->size(m_events) : 0; }

			bool get(uint32_t index, Event& event) const override
			{
				const clap_event_header_t* header = m_events->get(m_events, index);
				if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID)
					return false;
				event.time = header->time;
				switch (header->type)
				{
				case CLAP_EVENT_NOTE_ON:
				case CLAP_EVENT_NOTE_OFF:
				{
					const auto* note = reinterpret_cast<const clap_event_note_t*>(header);
					event.type = header->type == CLAP_EVENT_NOTE_ON ? EventType::NoteOn : EventType::NoteOff;
					event.noteId = note->note_id;
					event.port = note->port_index;
					event.channel = note->channel;
					event.key = note->key;
					event.value = note->velocity;
					return true;
				}
				case CLAP_EVENT_MIDI:
				{
					const auto* midi = reinterpret_cast<const clap_event_midi_t*>(header);
					const int status = midi->data[0] & 0xf0;
					if (status != 0x80 && status != 0x90)
						return false;
					event.type = status == 0x90 && midi->data[2] > 0 ? EventType::NoteOn : EventType::NoteOff;
					event.port = static_cast<int16_t>(midi->port_index);
					event.channel = static_cast<int16_t>(midi->data[0] & 0x0f);
					event.key = static_cast<int16_t>(midi->data[1]);
					event.value = midi->data[2] / 127.0;
					return true;
				}
				case CLAP_EVENT_PARAM_VALUE:
				{
					const auto* param = reinterpret_cast<const clap_event_param_value_t*>(header);
					event.type = EventType::ParamValue;
					event.paramId = param->param_id;
					event.value = param->value;
					return true;
				}
				default:
					return false;
				}
			}

		private:
			const clap_input_events_t* m_events;
		};

		uint16_t noteType(EventType type)
		{
			switch (type)
			{
			case EventType::NoteOn:
				return CLAP_EVENT_NOTE_ON;
			case EventType::NoteOff:
				return CLAP_EVENT_NOTE_OFF;
			default:
				return CLAP_EVENT_NOTE_END;
			}
		}

		class ClapEventOutput : public EventOutput
		{
		public:
			explicit ClapEventOutput(const clap_output_events_t* events) : m_events(events) {}

			bool push(const Event& event) override
			{
				if (m_events == nullptr)
					return false;
				clap_event_header_t header{};
				header.time = event.time;
				header.space_id = CLAP_CORE_EVENT_SPACE_ID;
				switch (event.type)
				{
				case EventType::NoteOn:
				case EventType::NoteOff:
				case EventType::NoteEnd:
				{
					clap_event_note_t note{};
					note.header = header;
					note.header.size = sizeof(note);
					note.header.type = noteType(event.type);
					note.note_id = event.noteId;
					note.port_index = event.port;
					note.channel = event.channel;
					note.key = event.key;
					note.velocity = event.value;
					return m_events->try_push(m_events, &note.header);
				}
				case EventType::ParamValue:
				{
					clap_event_param_value_t param{};
					param.header = header;
					param.header.size = sizeof(param);
					param.header.type = CLAP_EVENT_PARAM_VALUE;
					param.param_id = event.paramId;
					param.cookie = nullptr;
					param.note_id = -1;
					param.port_index = -1;
					param.channel = -1;
					param.key = -1;
					param.value = event.value;
					return m_events->try_push(m_events, &param.header);
				}
				case EventType::GestureBegin:
				case EventType::GestureEnd:
				{
					clap_event_param_gesture_t gesture{};
					gesture.header = header;
					gesture.header.size = sizeof(gesture);
					gesture.header.type =
						static_cast<uint16_t>(event.type == EventType::GestureBegin ? CLAP_EVENT_PARAM_GESTURE_BEGIN
																					: CLAP_EVENT_PARAM_GESTURE_END);
					gesture.param_id = event.paramId;
					return m_events->try_push(m_events, &gesture.header);
				}
				}
				return false;
			}

		private:
			const clap_output_events_t* m_events;
		};

		class ClapPlugin : public Host
		{
		public:
			explicit ClapPlugin(const clap_host_t* host) : m_host(host), m_core(createPlugin(*this))
			{
				m_plugin.desc = &clapDescriptor();
				m_plugin.plugin_data = this;
				m_plugin.init = [](const clap_plugin_t*) { return true; };
				m_plugin.destroy = [](const clap_plugin_t* plugin)
				{ const std::unique_ptr<ClapPlugin> owner(self(plugin)); };
				m_plugin.activate = [](const clap_plugin_t* plugin, double sampleRate, uint32_t, uint32_t maxFrames)
				{
					self(plugin)->m_core->activate(sampleRate, maxFrames);
					return true;
				};
				m_plugin.deactivate = [](const clap_plugin_t* plugin) { self(plugin)->m_core->deactivate(); };
				m_plugin.start_processing = [](const clap_plugin_t*) { return true; };
				m_plugin.stop_processing = [](const clap_plugin_t*) {};
				m_plugin.reset = [](const clap_plugin_t* plugin) { self(plugin)->m_core->resetState(); };
				m_plugin.process = [](const clap_plugin_t* plugin, const clap_process_t* process)
				{ return self(plugin)->process(*process); };
				m_plugin.get_extension = [](const clap_plugin_t*, const char* id) { return extension(id); };
				m_plugin.on_main_thread = [](const clap_plugin_t*) {};
			}

			const clap_plugin_t* plugin() const { return &m_plugin; }

			void requestRestart() override { m_host->request_restart(m_host); }

		private:
			static ClapPlugin* self(const clap_plugin_t* plugin)
			{
				return static_cast<ClapPlugin*>(plugin->plugin_data);
			}

			clap_process_status process(const clap_process_t& process)
			{
				const ClapEventInput in(process.in_events);
				ClapEventOutput out(process.out_events);
				ProcessBlock block;
				if (process.audio_inputs_count > 0)
				{
					block.inputs = process.audio_inputs[0].data32;
					block.inputChannels = process.audio_inputs[0].channel_count;
				}
				if (process.audio_outputs_count > 0)
				{
					block.outputs = process.audio_outputs[0].data32;
					block.outputChannels = process.audio_outputs[0].channel_count;
					process.audio_outputs[0].constant_mask = 0;
				}
				block.frames = process.frames_count;
				block.events = &in;
				block.outEvents = &out;
				m_core->process(block);
				return CLAP_PROCESS_CONTINUE;
			}

			static const void* extension(const char* id);

		public: // callbacks of the extensions below
			static uint32_t audioPortCount(const clap_plugin_t* /*plugin*/, bool isInput)
			{
				return isInput && pluginDescriptor().instrument ? 0 : 1;
			}

			static bool audioPortInfo(
				const clap_plugin_t* /*plugin*/, uint32_t index, bool isInput, clap_audio_port_info_t* info)
			{
				if (index != 0 || (isInput && pluginDescriptor().instrument))
					return false;
				info->id = 0;
				copyName(info->name, sizeof(info->name), isInput ? "Main In" : "Main Out");
				info->flags = CLAP_AUDIO_PORT_IS_MAIN;
				info->channel_count = g_channels;
				info->port_type = CLAP_PORT_STEREO;
				info->in_place_pair = CLAP_INVALID_ID;
				return true;
			}

			static uint32_t notePortCount(const clap_plugin_t* /*plugin*/, bool isInput) { return isInput ? 1 : 0; }

			static bool notePortInfo(
				const clap_plugin_t* /*plugin*/, uint32_t index, bool isInput, clap_note_port_info_t* info)
			{
				if (index != 0 || !isInput)
					return false;
				info->id = 0;
				info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
				info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
				copyName(info->name, sizeof(info->name), "Notes");
				return true;
			}

			static uint32_t paramCount(const clap_plugin_t* plugin)
			{
				return static_cast<uint32_t>(self(plugin)->m_core->params().size());
			}

			static bool paramInfo(const clap_plugin_t* plugin, uint32_t index, clap_param_info_t* info)
			{
				const std::vector<ParamInfo>& params = self(plugin)->m_core->params();
				if (index >= params.size())
					return false;
				const ParamInfo& param = params[index];
				info->id = param.id;
				info->flags = CLAP_PARAM_IS_AUTOMATABLE;
				if (param.stepped)
					info->flags |= CLAP_PARAM_IS_STEPPED;
				if (param.valueNames != nullptr)
					info->flags |= CLAP_PARAM_IS_ENUM;
				info->cookie = nullptr;
				copyName(info->name, sizeof(info->name), param.name);
				copyName(info->module, sizeof(info->module), "");
				info->min_value = param.minValue;
				info->max_value = param.maxValue;
				info->default_value = param.defaultValue;
				return true;
			}

			static bool paramValue(const clap_plugin_t* plugin, clap_id id, double* value)
			{
				return self(plugin)->m_core->paramValue(id, *value);
			}

			static bool valueToText(const clap_plugin_t* plugin, clap_id id, double value, char* text, uint32_t size)
			{
				return self(plugin)->m_core->valueToText(id, value, text, size);
			}

			static bool textToValue(const clap_plugin_t* plugin, clap_id id, const char* text, double* value)
			{
				return self(plugin)->m_core->textToValue(id, text, *value);
			}

			static void flush(
				const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t* /*out*/)
			{
				const ClapEventInput events(in);
				for (uint32_t i = 0; i < events.size(); ++i)
				{
					Event event;
					if (events.get(i, event) && event.type == EventType::ParamValue)
						self(plugin)->m_core->setParam(event.paramId, event.value);
				}
			}

			static bool saveState(const clap_plugin_t* plugin, const clap_ostream_t* stream)
			{
				const std::vector<uint8_t> bytes = self(plugin)->m_core->saveState();
				size_t written = 0;
				while (written < bytes.size())
				{
					const int64_t count = stream->write(stream, bytes.data() + written, bytes.size() - written);
					if (count <= 0)
						return false;
					written += static_cast<size_t>(count);
				}
				return true;
			}

			static bool loadState(const clap_plugin_t* plugin, const clap_istream_t* stream)
			{
				std::vector<uint8_t> bytes;
				constexpr size_t chunk = 65536;
				for (;;)
				{
					const size_t at = bytes.size();
					bytes.resize(at + chunk);
					const int64_t count = stream->read(stream, bytes.data() + at, chunk);
					if (count < 0)
						return false;
					bytes.resize(at + static_cast<size_t>(count));
					if (count == 0)
						break;
				}
				return self(plugin)->m_core->loadState(bytes.data(), bytes.size());
			}

			static uint32_t latency(const clap_plugin_t* plugin) { return self(plugin)->m_core->latency(); }

		private:
			clap_plugin_t m_plugin{};
			const clap_host_t* m_host;
			std::unique_ptr<TestPlugin> m_core;
		};

		const clap_plugin_audio_ports_t g_audioPorts = {ClapPlugin::audioPortCount, ClapPlugin::audioPortInfo};
		const clap_plugin_note_ports_t g_notePorts = {ClapPlugin::notePortCount, ClapPlugin::notePortInfo};
		const clap_plugin_params_t g_params = {ClapPlugin::paramCount, ClapPlugin::paramInfo, ClapPlugin::paramValue,
			ClapPlugin::valueToText, ClapPlugin::textToValue, ClapPlugin::flush};
		const clap_plugin_state_t g_state = {ClapPlugin::saveState, ClapPlugin::loadState};
		const clap_plugin_latency_t g_latency = {ClapPlugin::latency};

		const void* ClapPlugin::extension(const char* id)
		{
			if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
				return &g_audioPorts;
			if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0 && pluginDescriptor().instrument)
				return &g_notePorts;
			if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
				return &g_params;
			if (std::strcmp(id, CLAP_EXT_STATE) == 0)
				return &g_state;
			if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
				return &g_latency;
			return nullptr;
		}

		uint32_t factoryCount(const clap_plugin_factory_t* /*factory*/)
		{
			return 1;
		}

		const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t* /*factory*/, uint32_t index)
		{
			return index == 0 ? &clapDescriptor() : nullptr;
		}

		const clap_plugin_t* factoryCreate(
			const clap_plugin_factory_t* /*factory*/, const clap_host_t* host, const char* id)
		{
			if (host == nullptr || !clap_version_is_compatible(host->clap_version) ||
				std::strcmp(id, clapDescriptor().id) != 0)
				return nullptr;
			return std::make_unique<ClapPlugin>(host).release()->plugin(); // destroy() deletes it
		}

		const clap_plugin_factory_t g_factory = {factoryCount, factoryDescriptor, factoryCreate};

		bool entryInit(const char* /*path*/)
		{
			return true;
		}

		void entryDeinit()
		{
		}

		const void* entryFactory(const char* id)
		{
			return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &g_factory : nullptr;
		}
	}
}

// NOLINTNEXTLINE(readability-identifier-naming): CLAP fixes the entry point's name
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION, gnubase::testplugins::entryInit,
	gnubase::testplugins::entryDeinit, gnubase::testplugins::entryFactory};
