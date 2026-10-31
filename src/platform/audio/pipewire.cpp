// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/pipewire.h"

#include "platform/hosttime.h"
#include "platform/log.h"
#include "platform/rt/denormals.h"
#include "platform/rt/realtime.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/latency-utils.h>
#include <spa/utils/json.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gnubase::audio
{
	namespace
	{
		constexpr std::array<uint32_t, 7> g_blockSizes{32, 64, 128, 256, 512, 1024, 2048};
		constexpr auto g_timeout = std::chrono::seconds(5); // for the daemon's answers
		constexpr double g_offRateSeconds = 0.5;            // at another rate for this long, a device asks for a reset

		struct Node
		{
			std::string name; // node.name: what device ids use
			std::string description;
			// By media class: Audio/Sink..., Audio/Source... or Audio/Duplex, not a stream or filter.
			bool sink = false;
			bool source = false;
		};

		struct Port
		{
			uint32_t node = 0;
			bool output = false; // port.direction "out"
			bool monitor = false;
			uint32_t index = 0; // port.id: its place among the node's ports of its direction
			std::string channel;
		};

		std::string property(const spa_dict* props, const char* key)
		{
			const char* value = props != nullptr ? spa_dict_lookup(props, key) : nullptr;
			return value != nullptr ? value : "";
		}

		uint32_t number(const std::string& text, uint32_t fallback)
		{
			char* end = nullptr;
			const unsigned long value = std::strtoul(text.c_str(), &end, 10);
			return end != text.c_str() ? static_cast<uint32_t>(value) : fallback;
		}

		// The name in a default node's metadata value: {"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"}.
		std::string nameIn(const char* json)
		{
			if (json == nullptr)
				return {};
			spa_json outer{};
			spa_json object{};
			spa_json_init(&outer, json, std::strlen(json));
			if (spa_json_enter_object(&outer, &object) <= 0)
				return {};
			std::array<char, 256> key{};
			while (spa_json_get_string(&object, key.data(), static_cast<int>(key.size())) > 0)
			{
				std::array<char, 1024> value{};
				if (std::strcmp(key.data(), "name") == 0)
					return spa_json_get_string(&object, value.data(), static_cast<int>(value.size())) > 0 ? value.data()
																										  : "";
				const char* skipped = nullptr;
				if (spa_json_next(&object, &skipped) <= 0)
					break;
			}
			return {};
		}

		// The numbers in a settings array: "[ 44100 48000 ]".
		std::vector<double> numbersIn(const std::string& json)
		{
			std::vector<double> numbers;
			spa_json outer{};
			spa_json array{};
			spa_json_init(&outer, json.c_str(), json.size());
			if (spa_json_enter_array(&outer, &array) <= 0)
				return numbers;
			float value = 0.0f;
			while (spa_json_get_float(&array, &value) > 0)
				numbers.push_back(std::round(value));
			return numbers;
		}

		void initPipeWire()
		{
			static const bool initialized = []
			{
				pw_init(nullptr, nullptr);
				return true;
			}();
			(void)initialized;
		}

		// A connection to the PipeWire daemon, with its own thread for the daemon's events, and what the registry and
		// the "default" and "settings" metadata tell about the graph. Everything here is touched with the thread loop
		// locked (Lock), or on its thread, which holds the lock while it handles events.
		class Connection
		{
		public:
			class Lock
			{
			public:
				explicit Lock(const Connection& connection) : m_loop(connection.m_loop) { pw_thread_loop_lock(m_loop); }
				~Lock() { pw_thread_loop_unlock(m_loop); }
				Lock(const Lock&) = delete;
				Lock& operator=(const Lock&) = delete;

			private:
				pw_thread_loop* m_loop;
			};

			Connection()
			{
				initPipeWire();
				m_loop = pw_thread_loop_new("gnubase-pipewire", nullptr);
				if (m_loop == nullptr)
					throw DeviceError("PipeWire: cannot create a thread loop");
				m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
				if (m_context == nullptr)
				{
					pw_thread_loop_destroy(m_loop);
					throw DeviceError("PipeWire: cannot create a context");
				}
				if (pw_thread_loop_start(m_loop) < 0)
				{
					pw_context_destroy(m_context);
					pw_thread_loop_destroy(m_loop);
					throw DeviceError("PipeWire: cannot start a thread loop");
				}
				pw_thread_loop_lock(m_loop);
				m_core = pw_context_connect(m_context, nullptr, 0);
				const int error = errno;
				pw_thread_loop_unlock(m_loop);
				if (m_core == nullptr)
				{
					pw_thread_loop_stop(m_loop);
					close();
					throw DeviceError(std::string("PipeWire: no daemon to connect to (") + std::strerror(error) + ")");
				}
				try
				{
					const Lock lock(*this);
					pw_core_add_listener(m_core, &m_coreHook, &coreEvents(), this);
					m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
					pw_registry_add_listener(m_registry, &m_registryHook, &registryEvents(), this);
					sync(); // the globals
					sync(); // the metadata bound for them
				}
				catch (...)
				{
					destroy();
					throw;
				}
			}

			~Connection() { destroy(); }

			Connection(const Connection&) = delete;
			Connection& operator=(const Connection&) = delete;

			pw_core* core() const { return m_core; }
			pw_thread_loop* loop() const { return m_loop; }

			// Waits for the daemon to handle everything sent so far. Throws if it reports an error meanwhile.
			void sync()
			{
				const int seq = pw_core_sync(m_core, PW_ID_CORE, 0);
				waitFor([this, seq] { return m_synced == seq; });
				if (!m_error.empty())
					throw DeviceError("PipeWire: " + m_error);
			}

			// Waits until done() holds, with the lock held. Throws after the timeout.
			void waitFor(const std::function<bool()>& done)
			{
				const auto deadline = std::chrono::steady_clock::now() + g_timeout;
				while (!done() && m_error.empty())
				{
					if (std::chrono::steady_clock::now() > deadline)
						throw DeviceError("PipeWire: the daemon doesn't answer");
					pw_thread_loop_timed_wait(m_loop, 1);
				}
			}

			const std::map<uint32_t, Node>& nodes() const { return m_nodes; }

			// The sink (or source) of this name, or 0.
			uint32_t find(const std::string& name, bool sink) const
			{
				for (const auto& [id, node] : m_nodes)
					if ((sink ? node.sink : node.source) && node.name == name)
						return id;
				return 0;
			}

			// A node's playback (input) or capture (output) ports in their order. A sink's monitor ports are no capture
			// ports, but a virtual source's capture ports are monitor ports.
			std::vector<std::pair<uint32_t, Port>> ports(uint32_t node, bool output) const
			{
				std::vector<std::pair<uint32_t, Port>> result;
				const auto found = m_nodes.find(node);
				const bool source = found != m_nodes.end() && found->second.source;
				for (const auto& [id, port] : m_ports)
					if (port.node == node && port.output == output && (!port.monitor || source))
						result.emplace_back(id, port);
				std::sort(result.begin(), result.end(),
					[](const auto& a, const auto& b) { return a.second.index < b.second.index; });
				return result;
			}

			// The session manager's default sink or source, by node name; empty if it names none.
			const std::string& defaultSink() const { return m_defaultSink; }
			const std::string& defaultSource() const { return m_defaultSource; }

			// The rate another client forces the graph to run at (clock.force-rate), or 0.
			uint32_t forcedRate() const { return number(setting("clock.force-rate"), 0); }

			// The rate the graph runs at when nobody asks for another, and the rates it may switch to: only the forced
			// one while a rate is forced.
			double rate() const
			{
				const uint32_t forced = forcedRate();
				return forced != 0 ? forced : number(setting("clock.rate"), 48000);
			}

			std::vector<double> rates() const
			{
				if (forcedRate() != 0)
					return {rate()};
				std::vector<double> result = numbersIn(setting("clock.allowed-rates"));
				result.push_back(rate());
				std::sort(result.begin(), result.end());
				result.erase(std::unique(result.begin(), result.end()), result.end());
				return result;
			}

			uint32_t quantum() const
			{
				const uint32_t forced = number(setting("clock.force-quantum"), 0);
				return forced != 0 ? forced : number(setting("clock.quantum"), 1024);
			}

			// Called on the loop thread when one of these globals (nodes, ports) goes away, when the daemon does, when
			// another client forces a rate or lifts the force (clock.force-rate) and, if followsDefault, when the
			// default sink or source changes.
			void watch(std::vector<uint32_t> globals, bool followsDefault, std::function<void()> changed)
			{
				m_watched = std::move(globals);
				m_followsDefault = followsDefault;
				m_changed = std::move(changed);
			}

		private:
			std::string setting(const char* key) const
			{
				const auto found = m_settingsValues.find(key);
				return found != m_settingsValues.end() ? found->second : "";
			}

			void changed()
			{
				if (m_changed)
					m_changed();
			}

			void destroy()
			{
				pw_thread_loop_stop(m_loop);
				for (pw_proxy* metadata : {m_defaults, m_settings})
					if (metadata != nullptr)
						pw_proxy_destroy(metadata);
				if (m_registry != nullptr)
					pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
				pw_core_disconnect(m_core);
				close();
			}

			void close()
			{
				pw_context_destroy(m_context);
				pw_thread_loop_destroy(m_loop);
			}

			static void onDone(void* data, uint32_t id, int seq)
			{
				auto& self = *static_cast<Connection*>(data);
				if (id == PW_ID_CORE)
				{
					self.m_synced = seq;
					pw_thread_loop_signal(self.m_loop, false);
				}
			}

			static void onError(void* data, uint32_t id, int /*seq*/, int res, const char* message)
			{
				auto& self = *static_cast<Connection*>(data);
				self.m_error = std::string(message != nullptr ? message : "error") + " (" + std::strerror(-res) + ")";
				if (id == PW_ID_CORE && res == -EPIPE)
					self.changed(); // the daemon went away
				pw_thread_loop_signal(self.m_loop, false);
			}

			static void onGlobal(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
				uint32_t /*version*/, const spa_dict* props)
			{
				auto& self = *static_cast<Connection*>(data);
				if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
				{
					Node node;
					node.name = property(props, PW_KEY_NODE_NAME);
					node.description = property(props, PW_KEY_NODE_DESCRIPTION);
					if (node.description.empty())
						node.description = node.name;
					const std::string mediaClass = property(props, PW_KEY_MEDIA_CLASS);
					const bool duplex = mediaClass == "Audio/Duplex";
					node.sink = duplex || mediaClass.rfind("Audio/Sink", 0) == 0;
					node.source = duplex || mediaClass.rfind("Audio/Source", 0) == 0;
					self.m_nodes[id] = std::move(node);
				}
				else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0)
				{
					Port port;
					port.node = number(property(props, PW_KEY_NODE_ID), 0);
					port.output = property(props, PW_KEY_PORT_DIRECTION) == "out";
					port.monitor = property(props, PW_KEY_PORT_MONITOR) == "true";
					port.index = number(property(props, PW_KEY_PORT_ID), 0);
					port.channel = property(props, PW_KEY_AUDIO_CHANNEL);
					if (port.channel.empty())
						port.channel = property(props, PW_KEY_PORT_NAME);
					if (port.channel.empty())
						port.channel = "Channel " + std::to_string(port.index + 1);
					self.m_ports[id] = std::move(port);
				}
				else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0)
				{
					const std::string name = property(props, PW_KEY_METADATA_NAME);
					pw_proxy** proxy = nullptr;
					if (name == "default")
						proxy = &self.m_defaults;
					else if (name == "settings")
						proxy = &self.m_settings;
					if (proxy == nullptr || *proxy != nullptr)
						return;
					*proxy = static_cast<pw_proxy*>(
						pw_registry_bind(self.m_registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
					if (*proxy == nullptr)
						return;
					pw_metadata_add_listener(reinterpret_cast<pw_metadata*>(*proxy),
						name == "default" ? &self.m_defaultsHook : &self.m_settingsHook,
						name == "default" ? &defaultsEvents() : &settingsEvents(), &self);
				}
			}

			static void onGlobalRemove(void* data, uint32_t id)
			{
				auto& self = *static_cast<Connection*>(data);
				self.m_nodes.erase(id);
				self.m_ports.erase(id);
				if (id != PW_ID_CORE &&
					std::find(self.m_watched.begin(), self.m_watched.end(), id) != self.m_watched.end())
					self.changed();
			}

			static int onDefault(void* data, uint32_t subject, const char* key, const char* /*type*/, const char* value)
			{
				auto& self = *static_cast<Connection*>(data);
				if (subject != PW_ID_CORE)
					return 0;
				const bool all = key == nullptr; // everything cleared
				for (const auto& [name, current] : {std::pair{"default.audio.sink", &self.m_defaultSink},
						 std::pair{"default.audio.source", &self.m_defaultSource}})
					if (all || std::strcmp(key, name) == 0)
					{
						std::string changed = nameIn(value);
						if (changed == *current)
							continue;
						*current = std::move(changed);
						if (self.m_followsDefault)
							self.changed();
					}
				return 0;
			}

			static int onSetting(void* data, uint32_t subject, const char* key, const char* /*type*/, const char* value)
			{
				auto& self = *static_cast<Connection*>(data);
				if (subject != PW_ID_CORE)
					return 0;
				const uint32_t forced = self.forcedRate();
				if (key == nullptr)
					self.m_settingsValues.clear();
				else if (value == nullptr)
					self.m_settingsValues.erase(key);
				else
					self.m_settingsValues[key] = value;
				// Another client forces a rate or lifts the force: the device opens at the graph's rate again.
				if (self.forcedRate() != forced)
					self.changed();
				return 0;
			}

			static const pw_core_events& coreEvents()
			{
				static const pw_core_events events = []
				{
					pw_core_events result{};
					result.version = PW_VERSION_CORE_EVENTS;
					result.done = &Connection::onDone;
					result.error = &Connection::onError;
					return result;
				}();
				return events;
			}

			static const pw_registry_events& registryEvents()
			{
				static const pw_registry_events events = []
				{
					pw_registry_events result{};
					result.version = PW_VERSION_REGISTRY_EVENTS;
					result.global = &Connection::onGlobal;
					result.global_remove = &Connection::onGlobalRemove;
					return result;
				}();
				return events;
			}

			static pw_metadata_events metadataEvents(
				int (*property)(void*, uint32_t, const char*, const char*, const char*))
			{
				pw_metadata_events events{};
				events.version = PW_VERSION_METADATA_EVENTS;
				events.property = property;
				return events;
			}

			static const pw_metadata_events& defaultsEvents()
			{
				static const pw_metadata_events events = metadataEvents(&Connection::onDefault);
				return events;
			}

			static const pw_metadata_events& settingsEvents()
			{
				static const pw_metadata_events events = metadataEvents(&Connection::onSetting);
				return events;
			}

			pw_thread_loop* m_loop = nullptr;
			pw_context* m_context = nullptr;
			pw_core* m_core = nullptr;
			pw_registry* m_registry = nullptr;
			pw_proxy* m_defaults = nullptr;
			pw_proxy* m_settings = nullptr;
			spa_hook m_coreHook{};
			spa_hook m_registryHook{};
			spa_hook m_defaultsHook{};
			spa_hook m_settingsHook{};

			int m_synced = -1;
			std::string m_error;
			std::map<uint32_t, Node> m_nodes;
			std::map<uint32_t, Port> m_ports;
			std::string m_defaultSink;
			std::string m_defaultSource;
			std::map<std::string, std::string> m_settingsValues;

			std::vector<uint32_t> m_watched;
			bool m_followsDefault = false;
			std::function<void()> m_changed;
		};

		std::vector<std::string> channels(const std::vector<std::pair<uint32_t, Port>>& ports)
		{
			std::vector<std::string> names;
			names.reserve(ports.size());
			for (const auto& entry : ports)
				names.push_back(entry.second.channel);
			return names;
		}

		// What a device is made of: a sink and maybe a source.
		struct Endpoints
		{
			uint32_t sink = 0;
			uint32_t source = 0; // 0: none
			std::string id;
			std::string name;
		};

		// The default sink and source: the session manager's, or the first sink if it names none.
		Endpoints defaults(const Connection& connection)
		{
			Endpoints endpoints;
			endpoints.id = "default";
			endpoints.name = "Default output and input";
			endpoints.sink = connection.find(connection.defaultSink(), true);
			if (endpoints.sink == 0 || connection.ports(endpoints.sink, false).empty())
			{
				endpoints.sink = 0;
				for (const auto& [id, node] : connection.nodes())
					if (node.sink && !connection.ports(id, false).empty())
					{
						endpoints.sink = id;
						break;
					}
			}
			endpoints.source = connection.find(connection.defaultSource(), false);
			if (endpoints.source != 0 && connection.ports(endpoints.source, true).empty())
				endpoints.source = 0;
			return endpoints;
		}

		DeviceInfo makeInfo(const Connection& connection, const Endpoints& endpoints)
		{
			DeviceInfo info;
			info.id = endpoints.id;
			info.name = endpoints.name;
			info.outputs = channels(connection.ports(endpoints.sink, false));
			if (endpoints.source != 0)
				info.inputs = channels(connection.ports(endpoints.source, true));
			info.sampleRates = connection.rates();
			info.blockSizes = {g_blockSizes.begin(), g_blockSizes.end()};
			const uint32_t quantum = connection.quantum();
			if (std::find(info.blockSizes.begin(), info.blockSizes.end(), quantum) == info.blockSizes.end())
			{
				info.blockSizes.push_back(quantum);
				std::sort(info.blockSizes.begin(), info.blockSizes.end());
			}
			return info;
		}

		// In frames, from a port's latency parameter.
		uint32_t frames(const spa_latency_info& latency, uint32_t quantum, double rate)
		{
			return static_cast<uint32_t>(std::lround(latency.min_quantum * static_cast<float>(quantum)) +
				latency.min_rate + std::llround(static_cast<double>(latency.min_ns) * rate / 1e9));
		}

		class PipeWireDevice : public Device
		{
		public:
			// Takes over the connection the endpoints were found on.
			PipeWireDevice(std::unique_ptr<Connection> connection, DeviceInfo info, const Endpoints& endpoints,
				double rate, uint32_t blockSize)
				: m_connectionOwner(std::move(connection)), m_connection(*m_connectionOwner), m_info(std::move(info))
			{
				const Connection::Lock lock(m_connection);
				m_format.sampleRate = rate;
				m_format.blockSize = blockSize != 0 ? blockSize : m_connection.quantum();
				if (std::find(m_info.blockSizes.begin(), m_info.blockSizes.end(), m_format.blockSize) ==
					m_info.blockSizes.end())
				{
					m_info.blockSizes.push_back(m_format.blockSize);
					std::sort(m_info.blockSizes.begin(), m_info.blockSizes.end());
				}
				m_quantum.store(m_format.blockSize, std::memory_order_relaxed);

				const auto playback = m_connection.ports(endpoints.sink, false);
				const auto capture = endpoints.source != 0 ? m_connection.ports(endpoints.source, true)
														   : std::vector<std::pair<uint32_t, Port>>{};
				if (playback.empty() || (endpoints.source != 0 && capture.empty()))
					throw DeviceError("the device " + m_info.id + " went away while it was opened");
				m_format.outputChannels = static_cast<int>(playback.size());
				m_format.inputChannels = static_cast<int>(capture.size());
				m_info.outputs = channels(playback);
				m_info.inputs = channels(capture);
				const size_t block = m_format.blockSize;
				m_silence.resize(block);
				m_scratch.resize(block * static_cast<size_t>(m_format.outputChannels));
				m_inputs.resize(static_cast<size_t>(m_format.inputChannels));
				m_outputs.resize(static_cast<size_t>(m_format.outputChannels));
				m_inputBuffers.resize(m_inputs.size());
				m_outputBuffers.resize(m_outputs.size());

				createFilter();
				try
				{
					link(playback, capture);
				}
				catch (...)
				{
					destroyFilter();
					throw;
				}
				// The node is inactive until start, so process() sees the events once it runs.
				pw_loop* loop = pw_thread_loop_get_loop(m_connection.loop());
				m_latencyEvent = pw_loop_add_event(loop, &PipeWireDevice::onLatencyEvent, this);
				m_resetEvent = pw_loop_add_event(loop, &PipeWireDevice::onResetEvent, this);
				// The linked ports too: the session manager may give a sink other ports, e.g. for another channel
				// count, which unlinks the node from it.
				std::vector<uint32_t> watched{endpoints.sink, endpoints.source};
				for (const auto* ports : {&playback, &capture})
					for (const auto& entry : *ports)
						watched.push_back(entry.first);
				m_connection.watch(std::move(watched), m_info.id == "default", [this] { requestReset(); });
			}

			~PipeWireDevice() override
			{
				PipeWireDevice::stop(); // not virtually: a derived device would be gone already
				const Connection::Lock lock(m_connection);
				m_connection.watch({}, false, nullptr);
				destroyFilter();
				for (spa_source* event : {m_latencyEvent, m_resetEvent})
					if (event != nullptr)
						pw_loop_destroy_source(pw_thread_loop_get_loop(m_connection.loop()), event);
			}

			const DeviceFormat& format() const override { return m_format; }
			DeviceInfo info() const override { return m_info; }

			DeviceLatency latency() const override
			{
				const Connection::Lock lock(m_connection);
				const uint32_t quantum = m_quantum.load(std::memory_order_relaxed);
				DeviceLatency latency;
				latency.output = m_playbackLatency ? frames(*m_playbackLatency, quantum, m_format.sampleRate) : quantum;
				if (m_format.inputChannels > 0)
					latency.input =
						m_captureLatency ? frames(*m_captureLatency, quantum, m_format.sampleRate) : quantum;
				return latency;
			}

			bool running() const override { return m_running; }
			uint64_t position() const override { return m_position.load(std::memory_order_acquire); }

			void start(AudioCallback& callback) override
			{
				if (m_running)
					throw std::logic_error("the PipeWire device is already running");
				m_callback.store(&callback);
				m_xruns.restart(); // xruns while stopped aren't the device's
				const Connection::Lock lock(m_connection);
				if (pw_filter_set_active(m_filter, true) < 0)
				{
					m_callback.store(nullptr);
					throw DeviceError("PipeWire: cannot activate the node");
				}
				m_running = true;
			}

			void stop() override
			{
				if (!m_running)
					return;
				// Once the audio thread is out of process(), it sees no callback any more (both are sequentially
				// consistent: either it saw the callback and counts as busy, or it sees none).
				m_callback.store(nullptr);
				while (m_busy.load() != 0)
					std::this_thread::yield();
				const Connection::Lock lock(m_connection);
				pw_filter_set_active(m_filter, false);
				m_running = false;
			}

		private:
			void createFilter()
			{
				pw_properties* props = pw_properties_new(nullptr, nullptr);
				pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
				pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Duplex");
				pw_properties_set(props, PW_KEY_MEDIA_ROLE, "Production");
				pw_properties_set(props, PW_KEY_NODE_NAME, "gnubase");
				pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, "GNUbase");
				pw_properties_set(props, PW_KEY_NODE_AUTOCONNECT, "false");
				pw_properties_set(props, PW_KEY_NODE_ALWAYS_PROCESS, "true");
				const long rate = std::lround(m_format.sampleRate);
				pw_properties_set(props, PW_KEY_NODE_RATE, ("1/" + std::to_string(rate)).c_str());
				pw_properties_set(props, PW_KEY_NODE_FORCE_RATE, std::to_string(rate).c_str());
				pw_properties_set(props, PW_KEY_NODE_LATENCY,
					(std::to_string(m_format.blockSize) + "/" + std::to_string(rate)).c_str());
				m_filter = pw_filter_new(m_connection.core(), "gnubase", props);
				if (m_filter == nullptr)
					throw DeviceError("PipeWire: cannot create a filter node");
				pw_filter_add_listener(m_filter, &m_filterHook, &filterEvents(), this);
				const auto addPorts = [this](pw_direction direction, const std::vector<std::string>& names)
				{
					std::vector<void*> ports;
					for (const std::string& channel : names)
					{
						pw_properties* portProps = pw_properties_new(nullptr, nullptr);
						pw_properties_set(portProps, PW_KEY_FORMAT_DSP, "32 bit float mono audio");
						pw_properties_set(portProps, PW_KEY_AUDIO_CHANNEL, channel.c_str());
						pw_properties_set(portProps, PW_KEY_PORT_NAME,
							((direction == PW_DIRECTION_INPUT ? "input_" : "output_") + channel).c_str());
						ports.push_back(pw_filter_add_port(
							m_filter, direction, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 0, portProps, nullptr, 0));
						if (ports.back() == nullptr)
							throw DeviceError("PipeWire: cannot add a port");
					}
					return ports;
				};
				try
				{
					m_inputPorts = addPorts(PW_DIRECTION_INPUT, m_info.inputs);
					m_outputPorts = addPorts(PW_DIRECTION_OUTPUT, m_info.outputs);
					// pw_filter_flags are bit flags, which the enum doesn't list combined.
					// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
					const auto flags =
						static_cast<pw_filter_flags>(PW_FILTER_FLAG_RT_PROCESS | PW_FILTER_FLAG_INACTIVE);
					// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
					if (pw_filter_connect(m_filter, flags, nullptr, 0) < 0)
						throw DeviceError("PipeWire: cannot connect the filter node");
					m_connection.waitFor([this] { return pw_filter_get_node_id(m_filter) != SPA_ID_INVALID; });
					m_connection.sync(); // so that the registry has the node's ports
				}
				catch (...)
				{
					destroyFilter();
					throw;
				}
			}

			// Links the node's outputs to the sink's playback ports and the source's capture ports to its inputs.
			void link(const std::vector<std::pair<uint32_t, Port>>& playback,
				const std::vector<std::pair<uint32_t, Port>>& capture)
			{
				const uint32_t node = pw_filter_get_node_id(m_filter);
				const auto ours = [this, node](bool output) { return m_connection.ports(node, output); };
				const auto outputs = ours(true);
				const auto inputs = ours(false);
				if (outputs.size() != playback.size() || inputs.size() != capture.size())
					throw DeviceError("PipeWire: the node's ports don't show");
				const auto connect = [this](uint32_t from, uint32_t to)
				{
					pw_properties* props = pw_properties_new(nullptr, nullptr);
					pw_properties_set(props, PW_KEY_LINK_OUTPUT_PORT, std::to_string(from).c_str());
					pw_properties_set(props, PW_KEY_LINK_INPUT_PORT, std::to_string(to).c_str());
					pw_properties_set(props, PW_KEY_OBJECT_LINGER, "false");
					auto* proxy = static_cast<pw_proxy*>(pw_core_create_object(
						m_connection.core(), "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0));
					pw_properties_free(props);
					if (proxy == nullptr)
						throw DeviceError("PipeWire: cannot link a port");
					m_links.push_back(proxy);
				};
				for (size_t i = 0; i < playback.size(); ++i)
					connect(outputs[i].first, playback[i].first);
				for (size_t i = 0; i < capture.size(); ++i)
					connect(capture[i].first, inputs[i].first);
				m_connection.sync(); // reports links that fail
			}

			void destroyFilter()
			{
				for (pw_proxy* link : m_links)
					pw_proxy_destroy(link);
				m_links.clear();
				if (m_filter != nullptr)
					pw_filter_destroy(m_filter);
				m_filter = nullptr;
			}

			static void onProcess(void* data, spa_io_position* position)
			{
				static_cast<PipeWireDevice*>(data)->process(position);
			}

			void process(const spa_io_position* position) noexcept
			{
				if (position == nullptr)
					return;
				if (m_xruns.cycle(position->clock.id, position->clock.xrun))
					reportOverload();
				const auto frames = static_cast<uint32_t>(position->clock.duration);
				// The latency counts in quanta: a new quantum is a new latency, which the loop thread reports
				// (signalling the event only writes to an eventfd).
				if (m_quantum.exchange(frames, std::memory_order_relaxed) != frames && m_latencyEvent != nullptr)
					pw_loop_signal_event(pw_thread_loop_get_loop(m_connection.loop()), m_latencyEvent);
				for (size_t c = 0; c < m_inputPorts.size(); ++c)
					m_inputBuffers[c] = static_cast<const float*>(pw_filter_get_dsp_buffer(m_inputPorts[c], frames));
				for (size_t c = 0; c < m_outputPorts.size(); ++c)
				{
					m_outputBuffers[c] = static_cast<float*>(pw_filter_get_dsp_buffer(m_outputPorts[c], frames));
					if (m_outputBuffers[c] != nullptr)
						std::fill_n(m_outputBuffers[c], frames, 0.0f);
				}

				m_busy.fetch_add(1);
				AudioCallback* callback = m_callback.load();
				// The graph runs at another rate if another client forced one: then the device plays silence rather
				// than at the wrong speed, and asks for a reset if that lasts, so that the engine opens it again at the
				// graph's rate or learns why it can't (open refuses a rate it asks for while another is forced).
				const bool rate = static_cast<double>(position->clock.rate.denom) == m_format.sampleRate &&
					position->clock.rate.num == 1;
				if (callback != nullptr && rate)
				{
					m_offRateFrames = 0;
					run(*callback, frames);
				}
				else if (callback != nullptr)
				{
					const auto limit = static_cast<uint64_t>(m_format.sampleRate * g_offRateSeconds);
					if (m_offRateFrames < limit && m_offRateFrames + frames >= limit && m_resetEvent != nullptr)
						pw_loop_signal_event(pw_thread_loop_get_loop(m_connection.loop()), m_resetEvent);
					m_offRateFrames += frames;
					if (!m_rateWarned.exchange(true))
					{
						const rt::ScopedRealtimeContext realtime;
						logging::warning("PipeWire runs at {} Hz, not at the device's {} Hz: playing silence",
							position->clock.rate.denom, m_format.sampleRate);
					}
				}
				m_busy.fetch_sub(1);
			}

			void run(AudioCallback& callback, uint32_t frames) noexcept
			{
				const rt::ScopedRealtimeContext realtime;
				const rt::ScopedDenormalsDisabled denormals;
				const HostTime started = hostTime();
				for (uint32_t done = 0; done < frames;)
				{
					const uint32_t block = std::min(frames - done, m_format.blockSize);
					for (size_t c = 0; c < m_inputs.size(); ++c)
						m_inputs[c] = m_inputBuffers[c] != nullptr ? m_inputBuffers[c] + done : m_silence.data();
					for (size_t c = 0; c < m_outputs.size(); ++c)
					{
						m_outputs[c] = m_outputBuffers[c] != nullptr ? m_outputBuffers[c] + done
																	 : m_scratch.data() + (c * m_format.blockSize);
						std::fill_n(m_outputs[c], block, 0.0f); // output buffers are zero already; the scratch isn't
					}
					m_midiIn.clear();
					m_midiOut.clear();
					const uint64_t position = m_position.load(std::memory_order_relaxed);
					const HostTime time = std::max(
						started + static_cast<HostTime>(std::llround(done * 1e9 / m_format.sampleRate)), m_lastTime);
					m_lastTime = time;
					callback.process({m_inputs.data(), m_outputs.data(), block, position, time, m_midiIn, m_midiOut});
					m_position.store(position + block, std::memory_order_release);
					done += block;
				}
			}

			static void onParamChanged(void* data, void* port, uint32_t id, const spa_pod* param)
			{
				auto& self = *static_cast<PipeWireDevice*>(data);
				spa_latency_info latency{};
				if (port == nullptr || param == nullptr || id != SPA_PARAM_Latency ||
					spa_latency_parse(param, &latency) < 0)
					return;
				// An output port learns the latency downstream to the sink, an input port upstream from the source.
				const bool output =
					std::find(self.m_outputPorts.begin(), self.m_outputPorts.end(), port) != self.m_outputPorts.end();
				std::optional<spa_latency_info>* known = nullptr;
				if (output && latency.direction == SPA_DIRECTION_INPUT)
					known = &self.m_playbackLatency;
				else if (!output && latency.direction == SPA_DIRECTION_OUTPUT)
					known = &self.m_captureLatency;
				if (known == nullptr)
					return;
				const bool changed = !*known || (*known)->min_quantum != latency.min_quantum ||
					(*known)->min_rate != latency.min_rate || (*known)->min_ns != latency.min_ns;
				*known = latency;
				if (changed)
					self.reportLatencyChange();
			}

			static void onLatencyEvent(void* data, uint64_t /*count*/)
			{
				static_cast<PipeWireDevice*>(data)->reportLatencyChange();
			}

			static void onResetEvent(void* data, uint64_t /*count*/)
			{
				static_cast<PipeWireDevice*>(data)->requestReset();
			}

			static void onStateChanged(void* data, pw_filter_state /*old*/, pw_filter_state state, const char* error)
			{
				auto& self = *static_cast<PipeWireDevice*>(data);
				if (state == PW_FILTER_STATE_ERROR)
				{
					logging::warning("The PipeWire node failed: {}", error != nullptr ? error : "unknown error");
					self.requestReset();
				}
				pw_thread_loop_signal(self.m_connection.loop(), false);
			}

			static const pw_filter_events& filterEvents()
			{
				static const pw_filter_events events = []
				{
					pw_filter_events result{};
					result.version = PW_VERSION_FILTER_EVENTS;
					result.state_changed = &PipeWireDevice::onStateChanged;
					result.param_changed = &PipeWireDevice::onParamChanged;
					result.process = &PipeWireDevice::onProcess;
					return result;
				}();
				return events;
			}

			std::unique_ptr<Connection> m_connectionOwner;
			Connection& m_connection;
			DeviceInfo m_info;
			DeviceFormat m_format;
			pw_filter* m_filter = nullptr;
			spa_hook m_filterHook{};
			std::vector<void*> m_inputPorts;
			std::vector<void*> m_outputPorts;
			std::vector<pw_proxy*> m_links;
			std::optional<spa_latency_info> m_playbackLatency; // with the loop locked
			std::optional<spa_latency_info> m_captureLatency;
			spa_source* m_latencyEvent = nullptr; // process() signals it when the quantum changes
			spa_source* m_resetEvent = nullptr;   // and this one when the graph stays at another rate
			bool m_running = false;

			// Audio thread memory, allocated when opened.
			std::vector<const float*> m_inputBuffers;
			std::vector<float*> m_outputBuffers;
			std::vector<float> m_silence;
			std::vector<float> m_scratch; // for output ports without a buffer
			std::vector<const float*> m_inputs;
			std::vector<float*> m_outputs;
			MidiBuffer m_midiIn;
			MidiBuffer m_midiOut;
			HostTime m_lastTime = 0;
			uint64_t m_offRateFrames = 0; // processed in a row while the graph runs at another rate

			std::atomic<AudioCallback*> m_callback{nullptr};
			std::atomic<int> m_busy{0};
			std::atomic<uint32_t> m_quantum{0};
			std::atomic<uint64_t> m_position{0};
			std::atomic<bool> m_rateWarned{false};
			XrunCounter m_xruns;
		};

		class PipeWireBackend : public Backend
		{
		public:
			std::string name() const override { return "pipewire"; }

			std::vector<DeviceInfo> devices() override
			{
				const Connection connection;
				const Connection::Lock lock(connection);
				std::vector<uint32_t> sinks;
				std::vector<uint32_t> sources;
				for (const auto& [id, node] : connection.nodes())
				{
					if (node.sink && !connection.ports(id, false).empty())
						sinks.push_back(id);
					if (node.source && !connection.ports(id, true).empty())
						sources.push_back(id);
				}
				std::vector<DeviceInfo> devices;
				if (sinks.empty())
					return devices;
				devices.push_back(makeInfo(connection, defaults(connection)));
				const auto& nodes = connection.nodes();
				for (const uint32_t sink : sinks)
					devices.push_back(makeInfo(connection, {sink, 0, nodes.at(sink).name, nodes.at(sink).description}));
				for (const uint32_t sink : sinks)
					for (const uint32_t source : sources)
						devices.push_back(makeInfo(connection,
							{sink, source, nodes.at(sink).name + "|" + nodes.at(source).name,
								nodes.at(sink).description + " + " + nodes.at(source).description}));
				return devices;
			}

			std::unique_ptr<Device> open(const DeviceConfig& config) override
			{
				if (!config.clockSource.empty())
					throw DeviceError("PipeWire devices have no clock sources");
				if (config.blockSize > 8192)
					throw DeviceError("PipeWire blocks have at most 8192 frames");
				auto connection = std::make_unique<Connection>();
				std::optional<Connection::Lock> lock(std::in_place, *connection);
				Endpoints endpoints;
				if (config.device.empty() || config.device == "default")
				{
					endpoints = defaults(*connection);
					if (endpoints.sink == 0)
						throw DeviceError("there is no audio output");
				}
				else
				{
					const size_t bar = config.device.find('|');
					endpoints.id = config.device;
					endpoints.sink = connection->find(config.device.substr(0, bar), true);
					if (bar != std::string::npos)
						endpoints.source = connection->find(config.device.substr(bar + 1), false);
					if (endpoints.sink == 0 || connection->ports(endpoints.sink, false).empty() ||
						(bar != std::string::npos &&
							(endpoints.source == 0 || connection->ports(endpoints.source, true).empty())))
						throw DeviceError("no device " + config.device);
					const auto& nodes = connection->nodes();
					endpoints.name = nodes.at(endpoints.sink).description;
					if (endpoints.source != 0)
						endpoints.name += " + " + nodes.at(endpoints.source).description;
				}
				DeviceInfo info = makeInfo(*connection, endpoints);
				const double rate = config.sampleRate != 0.0 ? config.sampleRate : connection->rate();
				if (const uint32_t forced = connection->forcedRate(); forced != 0 && rate != forced)
					throw DeviceError("PipeWire runs at " + std::to_string(forced) +
						" Hz, which another client forces (clock.force-rate)");
				if (std::find(info.sampleRates.begin(), info.sampleRates.end(), rate) == info.sampleRates.end())
					throw DeviceError("PipeWire doesn't run at " + std::to_string(std::lround(rate)) + " Hz");
				lock.reset(); // the thread loop's lock nests, but waiting in it would keep the outer level locked
				return std::make_unique<PipeWireDevice>(
					std::move(connection), std::move(info), endpoints, rate, config.blockSize);
			}
		};
	}

	std::unique_ptr<Backend> makePipeWireBackend()
	{
		return std::make_unique<PipeWireBackend>();
	}
}
