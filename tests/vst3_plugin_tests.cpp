// SPDX-License-Identifier: GPL-3.0-or-later
// Loader smoke tests for the VST3 test plugins (tests/plugins): each one is loaded through its module entry and
// GetPluginFactory, the way a plugin host loads it, and processes blocks. GNUbase's own host (#64) will run plugins in
// a separate process; these tests load them into the test process, so they never trigger the crash and hang plugins.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "pluginhost/vst3/vst3ns.h"
#include "testutils/library.h"

#include <pluginterfaces/base/ibstream.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivstevents.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/vst/ivstparameterchanges.h>
#include <pluginterfaces/vst/vstspeaker.h>
#include <pluginterfaces/vst/vsttypes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using gnubase::testutils::Library;
using vst::int32;
using vst::tresult;
using vst::TUID;
using vst::uint32;
using vst::Vst::ParamID;
using vst::Vst::ParamValue;
using vst::Vst::TChar;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr int32 g_maxFrames = 512;
	const std::array<const char*, 7> g_pluginNames = {
		"gain", "synth", "latency", "paramstress", "crash", "hang", "stateblob"};

	// The plugin as a host finds it: a <name>.vst3 bundle folder on every platform.
	std::filesystem::path pluginPath(const std::string& name)
	{
		return std::filesystem::path(GNUBASE_TEST_PLUGINS_DIR) / "vst3" / ("gnubase-test-" + name + ".vst3");
	}

	// The binary inside the bundle, in the folder VST3 names for this platform and architecture.
	std::filesystem::path binaryPath(const std::string& name)
	{
		const std::filesystem::path contents = pluginPath(name) / "Contents";
		const std::string file = "gnubase-test-" + name;
#ifdef __APPLE__
		return contents / "MacOS" / file;
#elif defined(_WIN32) && defined(_M_ARM64)
		return contents / "arm64-win" / (file + ".vst3");
#elif defined(_WIN32)
		return contents / "x86_64-win" / (file + ".vst3");
#elif defined(__aarch64__)
		return contents / "aarch64-linux" / (file + ".so");
#else
		return contents / "x86_64-linux" / (file + ".so");
#endif
	}

	bool sameId(const void* a, const void* b)
	{
		return std::memcmp(a, b, sizeof(TUID)) == 0; // bytewise: ids may be unaligned for iidEqual's 64-bit reads
	}

	std::string narrow(const TChar* text)
	{
		std::string result;
		for (; *text != 0; ++text)
			result += static_cast<char>(*text);
		return result;
	}

	std::u16string wide(const std::string& text)
	{
		return {text.begin(), text.end()};
	}

	// FUnknown for objects the test owns: no reference counting, and one interface besides FUnknown.
	template <typename Interface> class TestObject : public Interface
	{
	public:
		tresult PLUGIN_API queryInterface(const TUID interfaceId, void** obj) override
		{
			if (sameId(interfaceId, vst::FUnknown_iid) || sameId(interfaceId, m_iid))
			{
				*obj = this;
				return vst::kResultOk;
			}
			*obj = nullptr;
			return vst::kNoInterface;
		}
		uint32 PLUGIN_API addRef() override { return 1; }
		uint32 PLUGIN_API release() override { return 1; }

	protected:
		explicit TestObject(const char* iid) : m_iid(iid) {}

	private:
		const char* m_iid;
	};

	class HostApplication final : public TestObject<vst::Vst::IHostApplication>
	{
	public:
		HostApplication() : TestObject(vst::Vst::IHostApplication_iid) {}

		tresult PLUGIN_API getName(vst::Vst::String128 name) override
		{
			const std::u16string text = wide("GNUbase tests");
			std::copy(text.begin(), text.end(), name);
			name[text.size()] = 0;
			return vst::kResultOk;
		}

		tresult PLUGIN_API createInstance(TUID /*cid*/, TUID /*iid*/, void** obj) override
		{
			*obj = nullptr;
			return vst::kResultFalse;
		}
	};

	// Counts the controller's requests.
	class ComponentHandler final : public TestObject<vst::Vst::IComponentHandler>
	{
	public:
		ComponentHandler() : TestObject(vst::Vst::IComponentHandler_iid) {}

		tresult PLUGIN_API beginEdit(ParamID /*id*/) override { return vst::kResultOk; }
		tresult PLUGIN_API performEdit(ParamID /*id*/, ParamValue /*valueNormalized*/) override
		{
			return vst::kResultOk;
		}
		tresult PLUGIN_API endEdit(ParamID /*id*/) override { return vst::kResultOk; }
		tresult PLUGIN_API restartComponent(int32 flags) override
		{
			restarts.push_back(flags);
			return vst::kResultOk;
		}

		std::vector<int32> restarts;
	};

	class ParamQueue final : public TestObject<vst::Vst::IParamValueQueue>
	{
	public:
		explicit ParamQueue(ParamID paramId) : TestObject(vst::Vst::IParamValueQueue_iid), id(paramId) {}

		ParamID PLUGIN_API getParameterId() override { return id; }
		int32 PLUGIN_API getPointCount() override { return static_cast<int32>(points.size()); }

		tresult PLUGIN_API getPoint(int32 index, int32& sampleOffset, ParamValue& value) override
		{
			if (index < 0 || static_cast<size_t>(index) >= points.size())
				return vst::kInvalidArgument;
			sampleOffset = points[static_cast<size_t>(index)].first;
			value = points[static_cast<size_t>(index)].second;
			return vst::kResultOk;
		}

		tresult PLUGIN_API addPoint(int32 sampleOffset, ParamValue value, int32& index) override
		{
			index = static_cast<int32>(points.size());
			points.emplace_back(sampleOffset, value);
			return vst::kResultOk;
		}

		ParamID id;
		std::vector<std::pair<int32, ParamValue>> points;
	};

	class ParamChanges final : public TestObject<vst::Vst::IParameterChanges>
	{
	public:
		ParamChanges() : TestObject(vst::Vst::IParameterChanges_iid) {}

		int32 PLUGIN_API getParameterCount() override { return static_cast<int32>(queues.size()); }

		vst::Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override
		{
			return index >= 0 && static_cast<size_t>(index) < queues.size() ? &queues[static_cast<size_t>(index)]
																			: nullptr;
		}

		vst::Vst::IParamValueQueue* PLUGIN_API addParameterData(const ParamID& id, int32& index) override
		{
			const auto it =
				std::find_if(queues.begin(), queues.end(), [&id](const ParamQueue& q) { return q.id == id; });
			index = static_cast<int32>(it - queues.begin());
			return it != queues.end() ? &*it : &queues.emplace_back(id);
		}

		// One normalized value at a sample offset.
		void add(ParamID id, int32 offset, ParamValue normalized)
		{
			int32 index = 0;
			int32 point = 0;
			addParameterData(id, index)->addPoint(offset, normalized, point);
		}

		std::deque<ParamQueue> queues; // a deque keeps the queues' addresses
	};

	class EventList final : public TestObject<vst::Vst::IEventList>
	{
	public:
		EventList() : TestObject(vst::Vst::IEventList_iid) {}

		int32 PLUGIN_API getEventCount() override { return static_cast<int32>(events.size()); }

		tresult PLUGIN_API getEvent(int32 index, vst::Vst::Event& e) override
		{
			if (index < 0 || static_cast<size_t>(index) >= events.size())
				return vst::kInvalidArgument;
			e = events[static_cast<size_t>(index)];
			return vst::kResultOk;
		}

		tresult PLUGIN_API addEvent(vst::Vst::Event& e) override
		{
			events.push_back(e);
			return vst::kResultOk;
		}

		void note(bool on, int32 offset, int16_t pitch, float velocity)
		{
			vst::Vst::Event event{};
			event.sampleOffset = offset;
			if (on)
			{
				event.type = vst::Vst::Event::kNoteOnEvent;
				event.noteOn.pitch = pitch;
				event.noteOn.velocity = velocity;
				event.noteOn.noteId = pitch; // one note per key in these tests
			}
			else
			{
				event.type = vst::Vst::Event::kNoteOffEvent;
				event.noteOff.pitch = pitch;
				event.noteOff.velocity = velocity;
				event.noteOff.noteId = pitch;
			}
			events.push_back(event);
		}

		std::vector<vst::Vst::Event> events;
	};

	// A memory stream that moves at most maxChunk bytes per call, as hosts may.
	class Stream final : public TestObject<vst::IBStream>
	{
	public:
		explicit Stream(size_t chunk, std::vector<uint8_t> data = {})
			: TestObject(vst::IBStream_iid), bytes(std::move(data)), maxChunk(chunk)
		{
		}

		tresult PLUGIN_API read(void* buffer, int32 numBytes, int32* numBytesRead) override
		{
			const size_t count = std::min({static_cast<size_t>(std::max(numBytes, 0)), maxChunk, bytes.size() - at});
			std::memcpy(buffer, bytes.data() + at, count);
			at += count;
			if (numBytesRead != nullptr)
				*numBytesRead = static_cast<int32>(count);
			return vst::kResultOk;
		}

		tresult PLUGIN_API write(void* buffer, int32 numBytes, int32* numBytesWritten) override
		{
			const size_t count = std::min(static_cast<size_t>(std::max(numBytes, 0)), maxChunk);
			const auto* data = static_cast<const uint8_t*>(buffer);
			bytes.insert(bytes.end(), data, data + count);
			at = bytes.size();
			if (numBytesWritten != nullptr)
				*numBytesWritten = static_cast<int32>(count);
			return vst::kResultOk;
		}

		tresult PLUGIN_API seek(vst::int64 pos, int32 mode, vst::int64* result) override
		{
			vst::int64 target = pos;
			if (mode == vst::IBStream::kIBSeekCur)
				target += static_cast<vst::int64>(at);
			else if (mode == vst::IBStream::kIBSeekEnd)
				target += static_cast<vst::int64>(bytes.size());
			if (target < 0 || target > static_cast<vst::int64>(bytes.size()))
				return vst::kInvalidArgument;
			at = static_cast<size_t>(target);
			if (result != nullptr)
				*result = target;
			return vst::kResultOk;
		}

		tresult PLUGIN_API tell(vst::int64* pos) override
		{
			*pos = static_cast<vst::int64>(at);
			return vst::kResultOk;
		}

		std::vector<uint8_t> bytes;
		size_t maxChunk;
		size_t at = 0;
	};

	// A loaded plugin module: entered through the platform's entry point, its factory taken from GetPluginFactory.
	class Module
	{
	public:
		explicit Module(const std::string& name) : m_library(binaryPath(name))
		{
			REQUIRE(m_library.loaded());
#ifdef _WIN32
			const auto init = m_library.function<bool (*)()>("InitDll");
			REQUIRE(init != nullptr);
			REQUIRE(init());
			m_exit = m_library.function<bool (*)()>("ExitDll");
#elif defined(__APPLE__)
			// A host passes the bundle's CFBundleRef; the test plugins don't use it.
			const auto init = m_library.function<bool (*)(void*)>("bundleEntry");
			REQUIRE(init != nullptr);
			REQUIRE(init(nullptr));
			m_exit = m_library.function<bool (*)()>("bundleExit");
#else
			const auto init = m_library.function<bool (*)(void*)>("ModuleEntry");
			REQUIRE(init != nullptr);
			REQUIRE(init(m_library.handle()));
			m_exit = m_library.function<bool (*)()>("ModuleExit");
#endif
			REQUIRE(m_exit != nullptr);
			const auto getFactory = m_library.function<vst::IPluginFactory*(PLUGIN_API*)()>("GetPluginFactory");
			REQUIRE(getFactory != nullptr);
			m_factory = getFactory();
			REQUIRE(m_factory != nullptr);
		}
		~Module()
		{
			if (m_factory != nullptr)
				m_factory->release();
			if (m_exit != nullptr)
				m_exit();
		}
		Module(const Module&) = delete;
		Module& operator=(const Module&) = delete;
		Module(Module&&) = delete;
		Module& operator=(Module&&) = delete;

		vst::IPluginFactory& factory() const { return *m_factory; }

		vst::PClassInfo classInfo() const
		{
			vst::PClassInfo info;
			REQUIRE(m_factory->getClassInfo(0, &info) == vst::kResultOk);
			return info;
		}

	private:
		Library m_library;
		bool (*m_exit)() = nullptr;
		vst::IPluginFactory* m_factory = nullptr;
	};

	// One plugin instance: the component created by the factory and initialized, its processor and its controller,
	// which is the same object. Set up for stereo, 32-bit samples and g_maxFrames; terminated with the object.
	class Instance
	{
	public:
		explicit Instance(const Module& module)
		{
			const vst::PClassInfo info = module.classInfo();
			void* component = nullptr;
			REQUIRE(module.factory().createInstance(info.cid, vst::Vst::IComponent_iid, &component) == vst::kResultOk);
			m_component = static_cast<vst::Vst::IComponent*>(component);
			REQUIRE(m_component != nullptr);
			REQUIRE(m_component->initialize(&m_host) == vst::kResultOk);
			void* processor = nullptr;
			REQUIRE(m_component->queryInterface(vst::Vst::IAudioProcessor_iid, &processor) == vst::kResultOk);
			m_processor = static_cast<vst::Vst::IAudioProcessor*>(processor);
			void* controller = nullptr;
			REQUIRE(m_component->queryInterface(vst::Vst::IEditController_iid, &controller) == vst::kResultOk);
			m_controller = static_cast<vst::Vst::IEditController*>(controller);
			REQUIRE(m_controller->setComponentHandler(&handler) == vst::kResultOk);

			std::array<vst::Vst::SpeakerArrangement, 1> inputs = {vst::Vst::SpeakerArr::kStereo};
			std::array<vst::Vst::SpeakerArrangement, 1> outputs = {vst::Vst::SpeakerArr::kStereo};
			REQUIRE(
				m_processor->setBusArrangements(inputs.data(), audioInputs(), outputs.data(), 1) == vst::kResultTrue);
			REQUIRE(m_processor->canProcessSampleSize(vst::Vst::kSample32) == vst::kResultTrue);
			vst::Vst::ProcessSetup setup{vst::Vst::kRealtime, vst::Vst::kSample32, g_maxFrames, g_sampleRate};
			REQUIRE(m_processor->setupProcessing(setup) == vst::kResultOk);
		}
		~Instance()
		{
			if (m_active)
				deactivate();
			m_controller->setComponentHandler(nullptr);
			m_component->terminate();
			m_controller->release();
			m_processor->release();
			m_component->release();
		}
		Instance(const Instance&) = delete;
		Instance& operator=(const Instance&) = delete;
		Instance(Instance&&) = delete;
		Instance& operator=(Instance&&) = delete;

		vst::Vst::IComponent& component() const { return *m_component; }
		vst::Vst::IAudioProcessor& processor() const { return *m_processor; }
		vst::Vst::IEditController& controller() const { return *m_controller; }

		int32 audioInputs() const { return m_component->getBusCount(vst::Vst::kAudio, vst::Vst::kInput); }

		void activate()
		{
			REQUIRE(m_component->setActive(1) == vst::kResultOk);
			REQUIRE(m_processor->setProcessing(1) == vst::kResultOk);
			m_active = true;
		}

		void deactivate()
		{
			m_processor->setProcessing(0);
			m_component->setActive(0);
			m_active = false;
		}

		vst::Vst::ParameterInfo paramInfo(int32 index) const
		{
			vst::Vst::ParameterInfo info{};
			REQUIRE(m_controller->getParameterInfo(index, info) == vst::kResultOk);
			return info;
		}

		// The parameter id with this title.
		ParamID paramId(const std::string& title) const
		{
			ParamID id = vst::Vst::kNoParamId;
			for (int32 i = 0; i < m_controller->getParameterCount() && id == vst::Vst::kNoParamId; ++i)
			{
				const vst::Vst::ParameterInfo info = paramInfo(i);
				if (narrow(info.title) == title)
					id = info.id;
			}
			INFO("parameter " << title);
			REQUIRE(id != vst::Vst::kNoParamId);
			return id;
		}

		std::string paramText(ParamID id, ParamValue normalized) const
		{
			vst::Vst::String128 text = {};
			REQUIRE(m_controller->getParamStringByValue(id, normalized, text) == vst::kResultOk);
			return narrow(text);
		}

		double plainValue(ParamID id) const
		{
			return m_controller->normalizedParamToPlain(id, m_controller->getParamNormalized(id));
		}

		ComponentHandler handler;

	private:
		HostApplication m_host;
		vst::Vst::IComponent* m_component = nullptr;
		vst::Vst::IAudioProcessor* m_processor = nullptr;
		vst::Vst::IEditController* m_controller = nullptr;
		bool m_active = false;
	};

	// Stereo input and output buffers of one block.
	struct Block
	{
		explicit Block(int32 frameCount) : frames(frameCount)
		{
			for (auto& channel : in)
				channel.assign(static_cast<size_t>(frames), 0.0f);
			for (auto& channel : out)
				channel.assign(static_cast<size_t>(frames), 0.0f);
		}

		int32 frames;
		std::array<std::vector<float>, 2> in;
		std::array<std::vector<float>, 2> out;
	};

	// What a block sends to the plugin and what the plugin sends back.
	struct BlockEvents
	{
		ParamChanges params;
		EventList notes;
		ParamChanges outParams;
		EventList outNotes;

		void clear()
		{
			params.queues.clear();
			notes.events.clear();
			outParams.queues.clear();
			outNotes.events.clear();
		}
	};

	void process(const Instance& plugin, Block& block, BlockEvents& events)
	{
		std::array<float*, 2> inputs = {block.in[0].data(), block.in[1].data()};
		std::array<float*, 2> outputs = {block.out[0].data(), block.out[1].data()};
		vst::Vst::AudioBusBuffers input;
		input.numChannels = 2;
		input.channelBuffers32 = inputs.data();
		vst::Vst::AudioBusBuffers output;
		output.numChannels = 2;
		output.channelBuffers32 = outputs.data();
		vst::Vst::ProcessData data;
		data.processMode = vst::Vst::kRealtime;
		data.symbolicSampleSize = vst::Vst::kSample32;
		data.numSamples = block.frames;
		data.numInputs = plugin.audioInputs();
		data.numOutputs = 1;
		data.inputs = &input;
		data.outputs = &output;
		data.inputParameterChanges = &events.params;
		data.outputParameterChanges = &events.outParams;
		data.inputEvents = &events.notes;
		data.outputEvents = &events.outNotes;
		REQUIRE(plugin.processor().process(data) == vst::kResultOk);
	}

	std::vector<uint8_t> saveState(const Instance& plugin, size_t chunk)
	{
		Stream stream(chunk);
		REQUIRE(plugin.component().getState(&stream) == vst::kResultOk);
		return stream.bytes;
	}

	bool loadState(const Instance& plugin, const std::vector<uint8_t>& bytes, size_t chunk)
	{
		Stream stream(chunk, bytes);
		return plugin.component().setState(&stream) == vst::kResultOk;
	}

	// Sets a parameter the way a host does while the plugin is inactive: through the controller on the main thread.
	void setPlain(const Instance& plugin, ParamID id, double plain)
	{
		vst::Vst::IEditController& controller = plugin.controller();
		REQUIRE(controller.setParamNormalized(id, controller.plainParamToNormalized(id, plain)) == vst::kResultOk);
	}

	bool silent(const std::vector<float>& channel)
	{
		return std::all_of(channel.begin(), channel.end(), [](float s) { return s == 0.0f; });
	}
}

TEST_CASE("every VST3 test plugin loads through GetPluginFactory and processes a block", "[integration]")
{
	std::set<std::string> classIds;
	for (const char* name : g_pluginNames)
	{
		INFO("plugin " << name);
#ifdef __APPLE__
		CHECK(std::filesystem::is_regular_file(pluginPath(name) / "Contents" / "Info.plist"));
#endif
		CHECK(std::filesystem::is_directory(pluginPath(name)));
		CHECK(std::filesystem::is_regular_file(binaryPath(name)));

		const Module module(name);
		vst::PFactoryInfo factoryInfo;
		REQUIRE(module.factory().getFactoryInfo(&factoryInfo) == vst::kResultOk);
		CHECK(std::string(factoryInfo.vendor) == "GNUbase");
		CHECK((factoryInfo.flags & vst::PFactoryInfo::kUnicode) != 0);
		REQUIRE(module.factory().countClasses() == 1);
		const vst::PClassInfo info = module.classInfo();
		CHECK(std::string(info.category) == kVstAudioEffectClass);
		CHECK(std::string(info.name).rfind("GNUbase Test ", 0) == 0);
		classIds.insert(std::string(std::begin(info.cid), std::end(info.cid)));

		void* factory3 = nullptr;
		REQUIRE(module.factory().queryInterface(vst::IPluginFactory3_iid, &factory3) == vst::kResultOk);
		auto* unicode = static_cast<vst::IPluginFactory3*>(factory3);
		vst::PClassInfoW infoW;
		REQUIRE(unicode->getClassInfoUnicode(0, &infoW) == vst::kResultOk);
		CHECK(narrow(infoW.name) == info.name);
		const std::string subCategories = infoW.subCategories;
		CHECK(subCategories.rfind(name == std::string("synth") ? "Instrument" : "Fx", 0) == 0);
		CHECK(sameId(infoW.cid, info.cid));
		unicode->release();

		TUID unknown = {};
		void* nothing = nullptr;
		CHECK(module.factory().createInstance(unknown, vst::Vst::IComponent_iid, &nothing) != vst::kResultOk);
		CHECK(nothing == nullptr);

		Instance plugin(module);
		TUID controllerClass = {};
		CHECK(plugin.component().getControllerClassId(controllerClass) != vst::kResultOk); // a single component
		void* requirements = nullptr;
		REQUIRE(plugin.processor().queryInterface(vst::Vst::IProcessContextRequirements_iid, &requirements) ==
			vst::kResultOk);
		CHECK(static_cast<vst::Vst::IProcessContextRequirements*>(requirements)->getProcessContextRequirements() == 0);
		static_cast<vst::Vst::IProcessContextRequirements*>(requirements)->release();
		REQUIRE(plugin.component().getBusCount(vst::Vst::kAudio, vst::Vst::kOutput) == 1);
		vst::Vst::BusInfo bus{};
		REQUIRE(plugin.component().getBusInfo(vst::Vst::kAudio, vst::Vst::kOutput, 0, bus) == vst::kResultOk);
		CHECK(bus.channelCount == 2);
		CHECK(plugin.controller().getParameterCount() > 0);
		CHECK(!saveState(plugin, 1 << 20).empty());

		plugin.activate();
		Block block(256);
		BlockEvents events;
		process(plugin, block, events);
		for (const auto& channel : block.out)
			CHECK(silent(channel));
		plugin.deactivate();
	}
	CHECK(classIds.size() == g_pluginNames.size());
}

TEST_CASE("the VST3 gain test plugin applies its gain from the change's sample offset on", "[integration]")
{
	const Module module("gain");
	Instance plugin(module);
	const ParamID gain = plugin.paramId("Gain");
	CHECK_THAT(plugin.plainValue(gain), WithinAbs(0.0, 1e-9));
	plugin.activate();

	Block block(256);
	for (auto& channel : block.in)
		std::fill(channel.begin(), channel.end(), 0.5f);
	BlockEvents events;
	events.params.add(gain, 128, plugin.controller().plainParamToNormalized(gain, -6.0));
	process(plugin, block, events);

	const auto factor = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
	for (const auto& channel : block.out)
	{
		CHECK(channel[0] == 0.5f);
		CHECK(channel[127] == 0.5f);
		CHECK_THAT(channel[128], WithinRel(0.5f * factor, 1e-6f));
		CHECK_THAT(channel[255], WithinRel(0.5f * factor, 1e-6f));
	}
	CHECK(plugin.plainValue(gain) == -6.0);
	CHECK(plugin.paramText(gain, plugin.controller().plainParamToNormalized(gain, -6.0)) == "-6.000 dB");

	ParamValue normalized = 0.0;
	std::u16string text = wide("-12");
	REQUIRE(plugin.controller().getParamValueByString(gain, text.data(), normalized) == vst::kResultOk);
	CHECK(plugin.controller().normalizedParamToPlain(gain, normalized) == -12.0);
}

TEST_CASE("the VST3 synth test plugin plays notes and fades out after the note off", "[integration]")
{
	const Module module("synth");
	Instance plugin(module);
	CHECK(plugin.audioInputs() == 0);
	REQUIRE(plugin.component().getBusCount(vst::Vst::kEvent, vst::Vst::kInput) == 1);
	plugin.activate();

	Block block(512);
	BlockEvents events;
	events.notes.note(true, 100, 69, 1.0f);
	process(plugin, block, events);
	CHECK(std::all_of(block.out[0].begin(), block.out[0].begin() + 100, [](float s) { return s == 0.0f; }));
	const float peak = *std::max_element(block.out[0].begin() + 100, block.out[0].end());
	CHECK(peak > 0.1f);
	CHECK(block.out[0] == block.out[1]);

	// The default release is 20 ms, so the voice is silent within three blocks after its note off. VST3 has no note
	// end event, so silence is the only sign.
	events.clear();
	events.notes.note(false, 0, 69, 0.0f);
	bool ended = false;
	for (int i = 0; i < 3 && !ended; ++i)
	{
		process(plugin, block, events);
		events.clear();
		ended = silent(block.out[0]);
	}
	CHECK(ended);
	CHECK(events.outNotes.events.empty());
}

TEST_CASE("the VST3 latency test plugin reports its latency and delays by it", "[integration]")
{
	const Module module("latency");
	Instance plugin(module);
	plugin.activate();
	REQUIRE(plugin.processor().getLatencySamples() == 64);

	Block block(256);
	block.in[0][10] = 1.0f;
	block.in[1][20] = -1.0f;
	BlockEvents events;
	process(plugin, block, events);
	CHECK(block.out[0][74] == 1.0f);
	CHECK(block.out[1][84] == -1.0f);
	CHECK(std::count(block.out[0].begin(), block.out[0].end(), 0.0f) == 255);

	// A new latency while active applies on the next activation. The processor sees it first, on the audio thread;
	// the restart request reaches the host when the host tells the controller about the change, on the main thread.
	const ParamID latency = plugin.paramId("Latency");
	const ParamValue normalized = plugin.controller().plainParamToNormalized(latency, 128.0);
	events.params.add(latency, 0, normalized);
	process(plugin, block, events);
	CHECK(plugin.handler.restarts.empty());
	REQUIRE(plugin.controller().setParamNormalized(latency, normalized) == vst::kResultOk);
	REQUIRE(plugin.handler.restarts.size() == 1);
	CHECK((plugin.handler.restarts[0] & vst::Vst::kLatencyChanged) != 0);
	CHECK(plugin.processor().getLatencySamples() == 64);
	plugin.deactivate();
	plugin.activate();
	CHECK(plugin.processor().getLatencySamples() == 128);
}

TEST_CASE("the VST3 parameter stress test plugin has 1024 parameters and sends automation", "[integration]")
{
	const Module module("paramstress");
	Instance plugin(module);
	vst::Vst::IEditController& controller = plugin.controller();
	REQUIRE(controller.getParameterCount() == 1024);
	std::vector<ParamID> ids;
	for (int32 i = 0; i < 1024; ++i)
	{
		const vst::Vst::ParameterInfo info = plugin.paramInfo(i);
		CHECK(info.defaultNormalizedValue >= 0.0);
		CHECK(info.defaultNormalizedValue <= 1.0);
		CHECK(info.stepCount >= 0);
		CHECK(info.id < 0x80000000u);
		ids.push_back(info.id);
		if ((info.flags & vst::Vst::ParameterInfo::kIsList) != 0)
		{
			std::u16string text = wide(plugin.paramText(info.id, 1.0));
			ParamValue value = -1.0;
			REQUIRE(controller.getParamValueByString(info.id, text.data(), value) == vst::kResultOk);
			CHECK(value == 1.0);
		}
	}
	std::sort(ids.begin(), ids.end());
	CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

	plugin.activate();
	Block block(256);
	BlockEvents events;
	process(plugin, block, events);
	CHECK(events.outParams.queues.empty());

	events.params.add(plugin.paramId("Emit automation"), 0, 1.0);
	process(plugin, block, events);
	// Eight parameters, one value each. Their gestures have no VST3 equivalent in process.
	const std::deque<ParamQueue>& queues = events.outParams.queues;
	REQUIRE(queues.size() == 8);
	int32 previous = 0;
	for (const ParamQueue& queue : queues)
	{
		REQUIRE(queue.points.size() == 1);
		const int32 offset = queue.points[0].first;
		CHECK(offset >= previous);
		CHECK(offset < block.frames);
		previous = offset;
		CHECK(controller.getParamNormalized(queue.id) == queue.points[0].second);
	}
	CHECK(block.out[0] == block.in[0]);
}

TEST_CASE("the VST3 state blob test plugin round-trips its state byte for byte", "[integration]")
{
	const Module module("stateblob");
	const Instance source(module);
	setPlain(source, source.paramId("Seed"), 1234.0);
	setPlain(source, source.paramId("Size"), 300000.0);
	const std::vector<uint8_t> state = saveState(source, 1000);
	CHECK(state.size() == 300000 + 20);

	const Instance copy(module);
	CHECK(saveState(copy, 1 << 20) != state);
	REQUIRE(loadState(copy, state, 777));
	CHECK(copy.plainValue(copy.paramId("Seed")) == 1234.0);
	CHECK(saveState(copy, 1 << 20) == state);
	// The controller of a single component already has the state.
	Stream stream(1 << 20, state);
	CHECK(copy.controller().setComponentState(&stream) == vst::kResultOk);

	std::vector<uint8_t> corrupt = state;
	corrupt[corrupt.size() / 2] ^= 1;
	CHECK(!loadState(copy, corrupt, 1 << 20));
	corrupt = state;
	corrupt.pop_back();
	CHECK(!loadState(copy, corrupt, 1 << 20));
}

TEST_CASE("the VST3 crash and hang test plugins have their trigger parameters, off by default", "[integration]")
{
	// Only checked, never set: setting them crashes or hangs the process. Host tests do that in a plugin host process.
	for (const std::string name : {"crash", "hang"})
	{
		INFO("plugin " << name);
		const Module module(name);
		const Instance plugin(module);
		const ParamID id = plugin.paramId(name == "crash" ? "Crash" : "Hang");
		CHECK(plugin.controller().getParamNormalized(id) == 0.0);
		CHECK(plugin.paramText(id, 1.0) == "On");
		CHECK(plugin.paramInfo(0).stepCount == 1);
		CHECK(!saveState(plugin, 1 << 20).empty());
	}
}
