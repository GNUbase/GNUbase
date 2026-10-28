// SPDX-License-Identifier: GPL-3.0-or-later
// Loader smoke tests for the CLAP test plugins (tests/plugins): each one is loaded through clap_entry and its plugin
// factory, the way a plugin host loads it, and processes blocks. GNUbase's own host (#64) will run plugins in a
// separate process; these tests load them into the test process, so they never trigger the crash and hang plugins.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using Catch::Matchers::WithinRel;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr uint32_t g_maxFrames = 512;
	const std::array<const char*, 7> g_pluginNames = {
		"gain", "synth", "latency", "paramstress", "crash", "hang", "stateblob"};

	// The plugin as a host finds it: a bundle folder on macOS, a library file elsewhere.
	std::filesystem::path pluginPath(const std::string& name)
	{
		return std::filesystem::path(GNUBASE_TEST_PLUGINS_DIR) / "clap" / ("gnubase-test-" + name + ".clap");
	}

	std::filesystem::path binaryPath(const std::string& name)
	{
#ifdef __APPLE__
		return pluginPath(name) / "Contents" / "MacOS" / ("gnubase-test-" + name);
#else
		return pluginPath(name);
#endif
	}

	class Library
	{
	public:
		explicit Library(const std::filesystem::path& path)
		{
#ifdef _WIN32
			m_handle = LoadLibraryW(path.c_str());
#else
			m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
		}
		~Library()
		{
			if (m_handle == nullptr)
				return;
#ifdef _WIN32
			FreeLibrary(static_cast<HMODULE>(m_handle));
#else
			dlclose(m_handle);
#endif
		}
		Library(const Library&) = delete;
		Library& operator=(const Library&) = delete;
		Library(Library&&) = delete;
		Library& operator=(Library&&) = delete;

		bool loaded() const { return m_handle != nullptr; }

		const void* symbol(const char* name) const
		{
#ifdef _WIN32
			return reinterpret_cast<const void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
			return dlsym(m_handle, name);
#endif
		}

	private:
		void* m_handle = nullptr;
	};

	// A minimal host that counts the plugin's requests.
	struct TestHost
	{
		TestHost()
		{
			host.clap_version = CLAP_VERSION;
			host.host_data = this;
			host.name = "GNUbase tests";
			host.vendor = "GNUbase";
			host.url = "https://github.com/GNUbase/GNUbase";
			host.version = "0.1.0";
			host.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
			host.request_restart = [](const clap_host_t* h) { ++static_cast<TestHost*>(h->host_data)->restarts; };
			host.request_process = [](const clap_host_t*) {};
			host.request_callback = [](const clap_host_t*) {};
		}
		TestHost(const TestHost&) = delete;
		TestHost& operator=(const TestHost&) = delete;
		TestHost(TestHost&&) = delete;
		TestHost& operator=(TestHost&&) = delete;
		~TestHost() = default;

		clap_host_t host{};
		int restarts = 0;
	};

	// A loaded plugin file: clap_entry initialized, the plugin factory found.
	class ClapLibrary
	{
	public:
		explicit ClapLibrary(const std::string& name) : m_library(binaryPath(name))
		{
			REQUIRE(m_library.loaded());
			m_entry = static_cast<const clap_plugin_entry_t*>(m_library.symbol("clap_entry"));
			REQUIRE(m_entry != nullptr);
			REQUIRE(clap_version_is_compatible(m_entry->clap_version));
			REQUIRE(m_entry->init(pluginPath(name).string().c_str()));
			m_factory = static_cast<const clap_plugin_factory_t*>(m_entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
			REQUIRE(m_factory != nullptr);
		}
		~ClapLibrary() { m_entry->deinit(); }
		ClapLibrary(const ClapLibrary&) = delete;
		ClapLibrary& operator=(const ClapLibrary&) = delete;
		ClapLibrary(ClapLibrary&&) = delete;
		ClapLibrary& operator=(ClapLibrary&&) = delete;

		const clap_plugin_factory_t& factory() const { return *m_factory; }

	private:
		Library m_library;
		const clap_plugin_entry_t* m_entry = nullptr;
		const clap_plugin_factory_t* m_factory = nullptr;
	};

	// One plugin instance, created by the factory and initialized; destroyed with the object.
	class ClapInstance
	{
	public:
		ClapInstance(const ClapLibrary& library, TestHost& host)
		{
			const clap_plugin_descriptor_t* descriptor = library.factory().get_plugin_descriptor(&library.factory(), 0);
			REQUIRE(descriptor != nullptr);
			m_plugin = library.factory().create_plugin(&library.factory(), &host.host, descriptor->id);
			REQUIRE(m_plugin != nullptr);
			REQUIRE(m_plugin->init(m_plugin));
		}
		~ClapInstance()
		{
			if (m_active)
				deactivate();
			m_plugin->destroy(m_plugin);
		}
		ClapInstance(const ClapInstance&) = delete;
		ClapInstance& operator=(const ClapInstance&) = delete;
		ClapInstance(ClapInstance&&) = delete;
		ClapInstance& operator=(ClapInstance&&) = delete;

		const clap_plugin_t* operator->() const { return m_plugin; }
		const clap_plugin_t* get() const { return m_plugin; }

		template <typename T> const T& extension(const char* id) const
		{
			const auto* ext = static_cast<const T*>(m_plugin->get_extension(m_plugin, id));
			REQUIRE(ext != nullptr);
			return *ext;
		}

		void activate()
		{
			REQUIRE(m_plugin->activate(m_plugin, g_sampleRate, 1, g_maxFrames));
			REQUIRE(m_plugin->start_processing(m_plugin));
			m_active = true;
		}

		void deactivate()
		{
			m_plugin->stop_processing(m_plugin);
			m_plugin->deactivate(m_plugin);
			m_active = false;
		}

		// The parameter id with this name.
		clap_id paramId(const std::string& name) const
		{
			const auto& params = extension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
			clap_id id = CLAP_INVALID_ID;
			for (uint32_t i = 0; i < params.count(m_plugin) && id == CLAP_INVALID_ID; ++i)
			{
				clap_param_info_t info{};
				REQUIRE(params.get_info(m_plugin, i, &info));
				if (name == info.name)
					id = info.id;
			}
			INFO("parameter " << name);
			REQUIRE(id != CLAP_INVALID_ID);
			return id;
		}

		double paramValue(clap_id id) const
		{
			double value = 0.0;
			REQUIRE(extension<clap_plugin_params_t>(CLAP_EXT_PARAMS).get_value(m_plugin, id, &value));
			return value;
		}

	private:
		const clap_plugin_t* m_plugin = nullptr;
		bool m_active = false;
	};

	class InputEvents
	{
	public:
		InputEvents()
		{
			m_list.ctx = this;
			m_list.size = [](const clap_input_events_t* list) { return self(list)->size(); };
			m_list.get = [](const clap_input_events_t* list, uint32_t index) { return self(list)->get(index); };
		}
		InputEvents(const InputEvents&) = delete;
		InputEvents& operator=(const InputEvents&) = delete;
		InputEvents(InputEvents&&) = delete;
		InputEvents& operator=(InputEvents&&) = delete;
		~InputEvents() = default;

		void param(uint32_t time, clap_id id, double value)
		{
			clap_event_param_value_t& event = m_params.emplace_back();
			event.header = header(time, CLAP_EVENT_PARAM_VALUE, sizeof(event));
			event.param_id = id;
			event.cookie = nullptr;
			event.note_id = -1;
			event.port_index = -1;
			event.channel = -1;
			event.key = -1;
			event.value = value;
			add(&event.header);
		}

		void note(uint16_t type, uint32_t time, int16_t key, double velocity)
		{
			clap_event_note_t& event = m_notes.emplace_back();
			event.header = header(time, type, sizeof(event));
			event.note_id = key; // one note per key in these tests
			event.port_index = 0;
			event.channel = 0;
			event.key = key;
			event.velocity = velocity;
			add(&event.header);
		}

		void clear()
		{
			m_events.clear();
			m_params.clear();
			m_notes.clear();
		}

		const clap_input_events_t* list() const { return &m_list; }

	private:
		static const InputEvents* self(const clap_input_events_t* list)
		{
			return static_cast<const InputEvents*>(list->ctx);
		}

		static clap_event_header_t header(uint32_t time, uint16_t type, size_t size)
		{
			clap_event_header_t header{};
			header.size = static_cast<uint32_t>(size);
			header.time = time;
			header.space_id = CLAP_CORE_EVENT_SPACE_ID;
			header.type = type;
			header.flags = 0;
			return header;
		}

		void add(const clap_event_header_t* event)
		{
			// Sorted by time, events at the same time in the order they were added.
			const auto at = std::upper_bound(m_events.begin(), m_events.end(), event,
				[](const clap_event_header_t* a, const clap_event_header_t* b) { return a->time < b->time; });
			m_events.insert(at, event);
		}

		uint32_t size() const { return static_cast<uint32_t>(m_events.size()); }
		const clap_event_header_t* get(uint32_t index) const { return index < size() ? m_events[index] : nullptr; }

		clap_input_events_t m_list{};
		std::vector<const clap_event_header_t*> m_events;
		std::deque<clap_event_param_value_t> m_params; // deques keep the events' addresses
		std::deque<clap_event_note_t> m_notes;
	};

	struct OutputEvent
	{
		uint16_t type = 0;
		uint32_t time = 0;
		clap_id paramId = CLAP_INVALID_ID;
		double value = 0.0;
		int16_t key = -1;
	};

	class OutputEvents
	{
	public:
		OutputEvents()
		{
			m_list.ctx = this;
			m_list.try_push = [](const clap_output_events_t* list, const clap_event_header_t* event)
			{
				static_cast<OutputEvents*>(list->ctx)->push(*event);
				return true;
			};
		}
		OutputEvents(const OutputEvents&) = delete;
		OutputEvents& operator=(const OutputEvents&) = delete;
		OutputEvents(OutputEvents&&) = delete;
		OutputEvents& operator=(OutputEvents&&) = delete;
		~OutputEvents() = default;

		const clap_output_events_t* list() const { return &m_list; }
		const std::vector<OutputEvent>& events() const { return m_events; }
		void clear() { m_events.clear(); }

	private:
		void push(const clap_event_header_t& header)
		{
			REQUIRE(header.space_id == CLAP_CORE_EVENT_SPACE_ID);
			OutputEvent event;
			event.type = header.type;
			event.time = header.time;
			switch (header.type)
			{
			case CLAP_EVENT_PARAM_VALUE:
			{
				const auto& param = reinterpret_cast<const clap_event_param_value_t&>(header);
				event.paramId = param.param_id;
				event.value = param.value;
				break;
			}
			case CLAP_EVENT_PARAM_GESTURE_BEGIN:
			case CLAP_EVENT_PARAM_GESTURE_END:
				event.paramId = reinterpret_cast<const clap_event_param_gesture_t&>(header).param_id;
				break;
			case CLAP_EVENT_NOTE_END:
				event.key = reinterpret_cast<const clap_event_note_t&>(header).key;
				break;
			default:
				break;
			}
			m_events.push_back(event);
		}

		clap_output_events_t m_list{};
		mutable std::vector<OutputEvent> m_events; // pushed through m_list, also when const
	};

	// Stereo input and output buffers of one block.
	struct Block
	{
		explicit Block(uint32_t frameCount) : frames(frameCount)
		{
			for (auto& channel : in)
				channel.assign(frames, 0.0f);
			for (auto& channel : out)
				channel.assign(frames, 0.0f);
		}

		uint32_t frames;
		std::array<std::vector<float>, 2> in;
		std::array<std::vector<float>, 2> out;
	};

	void process(const ClapInstance& plugin, Block& block, const InputEvents& in, OutputEvents& out)
	{
		const auto& ports = plugin.extension<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS);
		std::array<float*, 2> inputs = {block.in[0].data(), block.in[1].data()};
		std::array<float*, 2> outputs = {block.out[0].data(), block.out[1].data()};
		clap_audio_buffer_t input{};
		input.data32 = inputs.data();
		input.channel_count = 2;
		clap_audio_buffer_t output{};
		output.data32 = outputs.data();
		output.channel_count = 2;
		clap_process_t process{};
		process.steady_time = -1;
		process.frames_count = block.frames;
		process.transport = nullptr;
		process.audio_inputs = &input;
		process.audio_inputs_count = ports.count(plugin.get(), true);
		process.audio_outputs = &output;
		process.audio_outputs_count = 1;
		process.in_events = in.list();
		process.out_events = out.list();
		REQUIRE(plugin->process(plugin.get(), &process) != CLAP_PROCESS_ERROR);
	}

	// Streams that move at most maxChunk bytes per call, as hosts may.
	struct OutStream
	{
		explicit OutStream(size_t chunk) : maxChunk(chunk)
		{
			stream.ctx = this;
			stream.write = [](const clap_ostream_t* s, const void* buffer, uint64_t size) -> int64_t
			{
				auto* self = static_cast<OutStream*>(s->ctx);
				const auto count = static_cast<size_t>(std::min<uint64_t>(size, self->maxChunk));
				const auto* bytes = static_cast<const uint8_t*>(buffer);
				self->bytes.insert(self->bytes.end(), bytes, bytes + count);
				return static_cast<int64_t>(count);
			};
		}

		clap_ostream_t stream{};
		mutable std::vector<uint8_t> bytes; // written through stream, also when const
		size_t maxChunk;
	};

	struct InStream
	{
		InStream(const std::vector<uint8_t>& data, size_t chunk) : bytes(data), maxChunk(chunk)
		{
			stream.ctx = this;
			stream.read = [](const clap_istream_t* s, void* buffer, uint64_t size) -> int64_t
			{
				auto* self = static_cast<InStream*>(s->ctx);
				const size_t count =
					std::min({static_cast<size_t>(size), self->maxChunk, self->bytes.size() - self->at});
				std::memcpy(buffer, self->bytes.data() + self->at, count);
				self->at += count;
				return static_cast<int64_t>(count);
			};
		}

		clap_istream_t stream{};
		const std::vector<uint8_t>& bytes;
		size_t maxChunk;
		mutable size_t at = 0; // read through stream, also when const
	};

	std::vector<uint8_t> saveState(const ClapInstance& plugin, size_t chunk)
	{
		const OutStream out(chunk);
		REQUIRE(plugin.extension<clap_plugin_state_t>(CLAP_EXT_STATE).save(plugin.get(), &out.stream));
		return out.bytes;
	}

	bool loadState(const ClapInstance& plugin, const std::vector<uint8_t>& bytes, size_t chunk)
	{
		const InStream in(bytes, chunk);
		return plugin.extension<clap_plugin_state_t>(CLAP_EXT_STATE).load(plugin.get(), &in.stream);
	}

	// Sets parameters the way a host does while the plugin is inactive: through params.flush on the main thread.
	void flushParam(const ClapInstance& plugin, clap_id id, double value)
	{
		InputEvents in;
		in.param(0, id, value);
		const OutputEvents out;
		plugin.extension<clap_plugin_params_t>(CLAP_EXT_PARAMS).flush(plugin.get(), in.list(), out.list());
	}
}

TEST_CASE("every CLAP test plugin loads through clap_entry and its factory and processes a block", "[integration]")
{
	for (const char* name : g_pluginNames)
	{
		INFO("plugin " << name);
#ifdef __APPLE__
		CHECK(std::filesystem::is_regular_file(pluginPath(name) / "Contents" / "Info.plist"));
#endif
		CHECK(std::filesystem::is_regular_file(binaryPath(name)));

		const ClapLibrary library(name);
		REQUIRE(library.factory().get_plugin_count(&library.factory()) == 1);
		const clap_plugin_descriptor_t* descriptor = library.factory().get_plugin_descriptor(&library.factory(), 0);
		CHECK(descriptor->id == std::string("org.gnubase.test.") + name);
		CHECK(library.factory().create_plugin(&library.factory(), nullptr, "org.gnubase.test.unknown") == nullptr);

		TestHost host;
		ClapInstance plugin(library, host);
		const auto& ports = plugin.extension<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS);
		REQUIRE(ports.count(plugin.get(), false) == 1);
		clap_audio_port_info_t port{};
		REQUIRE(ports.get(plugin.get(), 0, false, &port));
		CHECK(port.channel_count == 2);
		CHECK(plugin.extension<clap_plugin_params_t>(CLAP_EXT_PARAMS).count(plugin.get()) > 0);
		plugin.extension<clap_plugin_state_t>(CLAP_EXT_STATE);

		plugin.activate();
		Block block(256);
		const InputEvents in;
		OutputEvents out;
		process(plugin, block, in, out);
		for (const auto& channel : block.out)
			CHECK(std::all_of(channel.begin(), channel.end(), [](float s) { return s == 0.0f; }));
		plugin.deactivate();
	}
}

TEST_CASE("the CLAP gain test plugin applies its gain from the event's sample offset on", "[integration]")
{
	const ClapLibrary library("gain");
	TestHost host;
	ClapInstance plugin(library, host);
	const clap_id gain = plugin.paramId("Gain");
	CHECK(plugin.paramValue(gain) == 0.0);
	plugin.activate();

	Block block(256);
	for (auto& channel : block.in)
		std::fill(channel.begin(), channel.end(), 0.5f);
	InputEvents in;
	in.param(128, gain, -6.0);
	OutputEvents out;
	process(plugin, block, in, out);

	const auto factor = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
	for (const auto& channel : block.out)
	{
		CHECK(channel[0] == 0.5f);
		CHECK(channel[127] == 0.5f);
		CHECK_THAT(channel[128], WithinRel(0.5f * factor, 1e-6f));
		CHECK_THAT(channel[255], WithinRel(0.5f * factor, 1e-6f));
	}
	CHECK(plugin.paramValue(gain) == -6.0);

	char text[CLAP_NAME_SIZE] = {};
	const auto& params = plugin.extension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
	REQUIRE(params.value_to_text(plugin.get(), gain, -6.0, text, sizeof(text)));
	CHECK(std::string(text) == "-6.000 dB");
}

TEST_CASE("the CLAP synth test plugin plays notes and ends its voices", "[integration]")
{
	const ClapLibrary library("synth");
	TestHost host;
	ClapInstance plugin(library, host);
	CHECK(plugin.extension<clap_plugin_audio_ports_t>(CLAP_EXT_AUDIO_PORTS).count(plugin.get(), true) == 0);
	const auto& notePorts = plugin.extension<clap_plugin_note_ports_t>(CLAP_EXT_NOTE_PORTS);
	REQUIRE(notePorts.count(plugin.get(), true) == 1);
	plugin.activate();

	Block block(512);
	InputEvents in;
	OutputEvents out;
	in.note(CLAP_EVENT_NOTE_ON, 100, 69, 1.0);
	process(plugin, block, in, out);
	CHECK(std::all_of(block.out[0].begin(), block.out[0].begin() + 100, [](float s) { return s == 0.0f; }));
	const float peak = *std::max_element(block.out[0].begin() + 100, block.out[0].end());
	CHECK(peak > 0.1f);
	CHECK(block.out[0] == block.out[1]);

	// The default release is 20 ms, so the voice ends within the next two blocks after its note off.
	in.clear();
	in.note(CLAP_EVENT_NOTE_OFF, 0, 69, 0.0);
	bool ended = false;
	for (int i = 0; i < 3 && !ended; ++i)
	{
		process(plugin, block, in, out);
		in.clear();
		ended = std::any_of(out.events().begin(), out.events().end(),
			[](const OutputEvent& e) { return e.type == CLAP_EVENT_NOTE_END && e.key == 69; });
	}
	CHECK(ended);
	process(plugin, block, in, out);
	CHECK(std::all_of(block.out[0].begin(), block.out[0].end(), [](float s) { return s == 0.0f; }));
}

TEST_CASE("the CLAP latency test plugin reports its latency and delays by it", "[integration]")
{
	const ClapLibrary library("latency");
	TestHost host;
	ClapInstance plugin(library, host);
	const auto& latency = plugin.extension<clap_plugin_latency_t>(CLAP_EXT_LATENCY);
	plugin.activate();
	REQUIRE(latency.get(plugin.get()) == 64);

	Block block(256);
	block.in[0][10] = 1.0f;
	block.in[1][20] = -1.0f;
	InputEvents in;
	OutputEvents out;
	process(plugin, block, in, out);
	CHECK(block.out[0][74] == 1.0f);
	CHECK(block.out[1][84] == -1.0f);
	CHECK(std::count(block.out[0].begin(), block.out[0].end(), 0.0f) == 255);

	// A new latency while active asks for a restart and applies on the next activate.
	in.param(0, plugin.paramId("Latency"), 128.0);
	process(plugin, block, in, out);
	CHECK(host.restarts == 1);
	CHECK(latency.get(plugin.get()) == 64);
	plugin.deactivate();
	plugin.activate();
	CHECK(latency.get(plugin.get()) == 128);
}

TEST_CASE("the CLAP parameter stress test plugin has 1024 parameters and sends automation", "[integration]")
{
	const ClapLibrary library("paramstress");
	TestHost host;
	ClapInstance plugin(library, host);
	const auto& params = plugin.extension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
	REQUIRE(params.count(plugin.get()) == 1024);
	std::vector<clap_id> ids;
	for (uint32_t i = 0; i < 1024; ++i)
	{
		clap_param_info_t info{};
		REQUIRE(params.get_info(plugin.get(), i, &info));
		CHECK(info.min_value <= info.default_value);
		CHECK(info.default_value <= info.max_value);
		ids.push_back(info.id);
		if ((info.flags & CLAP_PARAM_IS_ENUM) != 0)
		{
			char text[CLAP_NAME_SIZE] = {};
			double value = -1.0;
			REQUIRE(params.value_to_text(plugin.get(), info.id, info.max_value, text, sizeof(text)));
			REQUIRE(params.text_to_value(plugin.get(), info.id, text, &value));
			CHECK(value == info.max_value);
		}
	}
	std::sort(ids.begin(), ids.end());
	CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

	plugin.activate();
	Block block(256);
	InputEvents in;
	OutputEvents out;
	process(plugin, block, in, out);
	CHECK(out.events().empty());

	in.param(0, plugin.paramId("Emit automation"), 1.0);
	process(plugin, block, in, out);
	const std::vector<OutputEvent>& events = out.events();
	REQUIRE(events.size() == 24); // eight parameters, each with gesture begin, value and gesture end
	for (size_t i = 0; i < events.size(); i += 3)
	{
		CHECK(events[i].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
		CHECK(events[i + 1].type == CLAP_EVENT_PARAM_VALUE);
		CHECK(events[i + 2].type == CLAP_EVENT_PARAM_GESTURE_END);
		CHECK(events[i + 1].paramId == events[i].paramId);
		CHECK(events[i + 2].paramId == events[i].paramId);
		if (i > 0)
			CHECK(events[i].time >= events[i - 1].time);
		CHECK(events[i].time < block.frames);
		CHECK(plugin.paramValue(events[i + 1].paramId) == events[i + 1].value);
	}
	CHECK(block.out[0] == block.in[0]);
}

TEST_CASE("the CLAP state blob test plugin round-trips its state byte for byte", "[integration]")
{
	const ClapLibrary library("stateblob");
	TestHost host;
	const ClapInstance source(library, host);
	flushParam(source, source.paramId("Seed"), 1234.0);
	flushParam(source, source.paramId("Size"), 300000.0);
	const std::vector<uint8_t> state = saveState(source, 1000);
	CHECK(state.size() == 300000 + 20);

	const ClapInstance copy(library, host);
	CHECK(saveState(copy, 1 << 20) != state);
	REQUIRE(loadState(copy, state, 777));
	CHECK(copy.paramValue(copy.paramId("Seed")) == 1234.0);
	CHECK(saveState(copy, 1 << 20) == state);

	std::vector<uint8_t> corrupt = state;
	corrupt[corrupt.size() / 2] ^= 1;
	CHECK_FALSE(loadState(copy, corrupt, 1 << 20));
	corrupt = state;
	corrupt.pop_back();
	CHECK_FALSE(loadState(copy, corrupt, 1 << 20));
}

TEST_CASE("the CLAP crash and hang test plugins have their trigger parameters, off by default", "[integration]")
{
	// Only checked, never set: setting them crashes or hangs the process. Host tests do that in a plugin host process.
	for (const std::string name : {"crash", "hang"})
	{
		INFO("plugin " << name);
		const ClapLibrary library(name);
		TestHost host;
		const ClapInstance plugin(library, host);
		const auto& params = plugin.extension<clap_plugin_params_t>(CLAP_EXT_PARAMS);
		const std::string param = name == "crash" ? "Crash" : "Hang";
		const clap_id id = plugin.paramId(param);
		CHECK(plugin.paramValue(id) == 0.0);
		char text[CLAP_NAME_SIZE] = {};
		REQUIRE(params.value_to_text(plugin.get(), id, 1.0, text, sizeof(text)));
		CHECK(std::string(text) == "On");
		CHECK_FALSE(saveState(plugin, 1 << 20).empty());
	}
}
