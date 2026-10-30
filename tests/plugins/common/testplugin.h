// SPDX-License-Identifier: GPL-3.0-or-later
// GNUbase's test plugins, independent of any plugin format. Each plugin implements TestPlugin once; a format wrapper
// (clap/wrapper.cpp) turns it into a plugin of that format. See tests/plugins/README.md.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace gnubase::testplugins
{
	struct ParamInfo
	{
		uint32_t id = 0;
		const char* name = "";
		double minValue = 0.0;
		double maxValue = 1.0;
		double defaultValue = 0.0;
		bool stepped = false;
		// For stepped parameters: one name per step from minValue up, or nullptr for plain numbers.
		const char* const* valueNames = nullptr;
		const char* unit = "";
	};

	enum class EventType : uint8_t
	{
		NoteOn,
		NoteOff,
		NoteEnd,
		ParamValue,
		GestureBegin,
		GestureEnd
	};

	// One event inside a block. Fields that don't apply to the type keep their defaults; -1 is a wildcard.
	struct Event
	{
		EventType type = EventType::ParamValue;
		uint32_t time = 0; // sample offset in the block
		uint32_t paramId = 0;
		double value = 0.0; // parameter value, or note velocity from 0 to 1
		int32_t noteId = -1;
		int16_t port = 0;
		int16_t channel = 0;
		int16_t key = 0;
	};

	// The block's input events, sorted by time. Converted on access, so formats need no event buffer.
	class EventInput
	{
	public:
		virtual ~EventInput() = default;
		virtual uint32_t size() const = 0;
		// False for events the test plugins don't handle (MIDI CCs, transport, other event spaces).
		virtual bool get(uint32_t index, Event& event) const = 0;
	};

	class EventOutput
	{
	public:
		virtual ~EventOutput() = default;
		// Events must be pushed in time order. False if the host's list is full.
		virtual bool push(const Event& event) = 0;
	};

	// What a plugin may ask of its host, from any thread.
	class Host
	{
	public:
		virtual ~Host() = default;
		// Deactivate and activate the plugin again, e.g. to apply a new latency.
		virtual void requestRestart() = 0;
		// Read the parameter values again, e.g. after the plugin changed them itself.
		virtual void requestValuesRescan() = 0;
	};

	struct ProcessBlock
	{
		const float* const* inputs = nullptr; // nullptr for plugins without an audio input
		float* const* outputs = nullptr;
		uint32_t inputChannels = 0;
		uint32_t outputChannels = 0;
		uint32_t frames = 0;
		const EventInput* events = nullptr;
		EventOutput* outEvents = nullptr;
	};

	struct Descriptor
	{
		const char* id = "";
		const char* name = "";
		const char* description = "";
		bool instrument = false; // an instrument has a note input and no audio input
	};

	constexpr uint32_t g_channels = 2; // every test plugin has stereo ports

	class TestPlugin
	{
	public:
		explicit TestPlugin(std::vector<ParamInfo> params);
		virtual ~TestPlugin() = default;
		TestPlugin(const TestPlugin&) = delete;
		TestPlugin& operator=(const TestPlugin&) = delete;
		TestPlugin(TestPlugin&&) = delete;
		TestPlugin& operator=(TestPlugin&&) = delete;

		const std::vector<ParamInfo>& params() const { return m_params; }
		const ParamInfo* findParam(uint32_t id) const;
		bool paramValue(uint32_t id, double& value) const;
		// Sets a parameter from any thread: the main thread while inactive, the audio thread while active. The value
		// is clamped to the parameter's range. Must not allocate.
		void setParam(uint32_t id, double value);

		// Text for a value, written to text (at most size bytes including the terminating zero).
		bool valueToText(uint32_t id, double value, char* text, size_t size) const;
		bool textToValue(uint32_t id, const char* text, double& value) const;

		// Main thread. activate may allocate; process must not.
		virtual void activate(double sampleRate, uint32_t maxFrames);
		virtual void deactivate() {}
		virtual void resetState() {}
		// Handles the events at their sample offsets and renders the audio between them.
		virtual void process(const ProcessBlock& block);
		virtual uint32_t latency() const { return 0; }
		// How long the output can go on after the input ends, at most: 0 for none.
		virtual double tailSeconds() const { return 0.0; }

		// State as bytes. The default saves every parameter value; loading sets them through setParam.
		virtual std::vector<uint8_t> saveState() const;
		virtual bool loadState(const uint8_t* data, size_t size);

	protected:
		// Renders frames [from, to) of the block.
		virtual void render(const ProcessBlock& block, uint32_t from, uint32_t to);
		virtual void handleEvent(const Event& event);
		// Called after a parameter value changed, on the thread that changed it.
		virtual void paramChanged(uint32_t /*id*/, double /*value*/) {}
		double sampleRate() const { return m_sampleRate; }

	private:
		std::vector<ParamInfo> m_params;
		std::vector<std::atomic<double>> m_values;
		double m_sampleRate = 48000.0;
	};

	// Each plugin's source file defines these two; the format wrapper calls them.
	const Descriptor& pluginDescriptor();
	std::unique_ptr<TestPlugin> createPlugin(Host& host);

	// Crashes the process with an access violation (not an abort, which shows a dialog in Windows debug builds).
	[[noreturn]] void crashNow();
	// Never returns, without using the CPU.
	[[noreturn]] void hangNow();
}
