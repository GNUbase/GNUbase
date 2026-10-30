// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/hosttime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Audio devices and their backends, and what devices and the engine exchange once per block. A backend is a kind of
// driver (dummy, ASIO, WASAPI, CoreAudio, PipeWire, JACK); it lists its devices and opens them.
namespace gnubase::audio
{
	struct DeviceFormat
	{
		double sampleRate = 48000.0;
		uint32_t blockSize = 256; // the most frames a block has
		int inputChannels = 2;
		int outputChannels = 2;
	};

	// Thrown when a device can't be listed, opened or started.
	class DeviceError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// What a backend knows about a device. Some drivers tell more once the device is open: Backend::devices may leave
	// lists empty that Device::info fills.
	struct DeviceInfo
	{
		std::string id; // what Backend::open takes; stays the same across runs where the driver allows
		std::string name;
		std::vector<std::string> inputs; // channel names
		std::vector<std::string> outputs;
		std::vector<double> sampleRates; // the device opens with each of these
		std::vector<uint32_t> blockSizes;
		std::vector<std::string> clockSources;
		int clockSource = -1;      // the current one, an index into clockSources; -1 if there are none
		bool controlPanel = false; // the driver has a control panel (Device::openControlPanel)
		// Why the device can't be used now, e.g. because its hardware isn't connected; empty if it can, as far as the
		// backend knows. Backend::open still tries such a device.
		std::string unavailableReason;
		bool canMonitorDirectly = false; // the driver routes inputs to outputs itself (Device::monitorInput)
	};

	// How to open a device. Zero and empty values leave the choice to the driver.
	struct DeviceConfig
	{
		std::string device;      // an id from Backend::devices; empty: the default device
		double sampleRate = 0.0; // 0: the device's current sample rate
		uint32_t blockSize = 0;  // 0: the driver's preferred block size
		std::string clockSource; // a name from DeviceInfo::clockSources; empty: as it is
	};

	// In frames, as the driver reports them.
	struct DeviceLatency
	{
		uint32_t input = 0;
		uint32_t output = 0;
	};

	// Direct monitoring: the driver plays an input on outputs itself, without the latency of the round trip through
	// the engine. The input goes to output, and with a pan above -1 partly to the output after it: pan 1 plays it on
	// that one only, as the second of a stereo pair.
	struct InputMonitor
	{
		int input = 0;
		int output = 0;
		bool on = false;
		double gain = 1.0; // linear, at most 4 (+12 dB)
		double pan = -1.0;
	};

	// A MIDI 1.0 channel message at a frame within a block.
	struct MidiMessage
	{
		uint32_t offset = 0;
		uint8_t size = 0; // bytes used, 1 to 3
		std::array<uint8_t, 3> bytes{};

		bool operator==(const MidiMessage& other) const
		{
			return offset == other.offset && size == other.size && bytes == other.bytes;
		}
	};

	// The MIDI messages of one block in offset order. Memory is allocated in the constructor only.
	class MidiBuffer
	{
	public:
		explicit MidiBuffer(size_t capacity = 0) : m_messages(capacity) {}

		// Appends a message; offsets must not decrease. Returns false and drops it if the buffer is full.
		bool add(const MidiMessage& message) noexcept
		{
			if (m_size == m_messages.size())
				return false;
			m_messages[m_size++] = message;
			return true;
		}

		void clear() noexcept { m_size = 0; }
		size_t size() const { return m_size; }
		bool empty() const { return m_size == 0; }
		const MidiMessage& operator[](size_t index) const { return m_messages[index]; }
		const MidiMessage* begin() const { return m_messages.data(); }
		const MidiMessage* end() const { return m_messages.data() + m_size; }

	private:
		std::vector<MidiMessage> m_messages;
		size_t m_size = 0;
	};

	struct DeviceBlock
	{
		const float* const* inputs = nullptr; // inputChannels channels
		float* const* outputs = nullptr;      // outputChannels channels, silent on entry
		uint32_t frames = 0;                  // at most the format's blockSize
		uint64_t position = 0;                // frames processed before this block
		HostTime time = 0;                    // when the block started (hostTime): where MIDI input is placed
		const MidiBuffer& midiIn;
		MidiBuffer& midiOut; // empty on entry
	};

	// Called by a device on its audio thread, once per block. Real-time rules apply.
	class AudioCallback
	{
	public:
		AudioCallback() = default;
		virtual ~AudioCallback() = default;
		AudioCallback(const AudioCallback&) = delete;
		AudioCallback& operator=(const AudioCallback&) = delete;

		virtual void process(const DeviceBlock& block) noexcept = 0;
	};

	// An open audio device. While it runs, it calls its callback once per block on its audio thread, in the realtime
	// context (rt::ScopedRealtimeContext), with positions counting on from block to block and across restarts.
	class Device
	{
	public:
		Device() = default;
		virtual ~Device() = default; // a running device stops first
		Device(const Device&) = delete;
		Device& operator=(const Device&) = delete;

		virtual const DeviceFormat& format() const = 0;
		virtual DeviceInfo info() const = 0; // its sample rates and block sizes include the current ones
		virtual DeviceLatency latency() const = 0;

		// Throws DeviceError if the device can't start.
		virtual void start(AudioCallback& callback) = 0;
		// Returns after the last callback. Does nothing while stopped.
		virtual void stop() = 0;
		virtual bool running() const = 0;

		// Frames processed since the device was opened.
		virtual uint64_t position() const = 0;

		// Opens the driver's control panel, if it has one (DeviceInfo::controlPanel).
		virtual void openControlPanel() {}

		// Starts or stops monitoring an input directly, if the driver can (DeviceInfo::canMonitorDirectly). Each input
		// plays on one output (pair) at a time. Not on the audio thread; also while the device runs.
		virtual void monitorInput(const InputMonitor& /*monitor*/) {}

		// The audio workgroup of the device's audio thread (macOS: an os_workgroup_t), which threads that help it join
		// (rt::WorkerPool), or null if it has none. Valid while the device is open.
		virtual void* workgroup() const { return nullptr; }

		// Called when the driver asks to be opened again, e.g. because its settings changed in its control panel. Never
		// called on the audio thread. Set it while the device is stopped.
		void onReset(std::function<void()> handler) { m_reset = std::move(handler); }

	protected:
		// For backends, when their driver asks for a reset.
		void requestReset() const
		{
			if (m_reset)
				m_reset();
		}

	private:
		std::function<void()> m_reset;
	};

	// A kind of driver. It lists its devices and opens them.
	class Backend
	{
	public:
		Backend() = default;
		virtual ~Backend() = default;
		Backend(const Backend&) = delete;
		Backend& operator=(const Backend&) = delete;

		virtual std::string name() const = 0; // e.g. "dummy", "wasapi"

		// The devices there are now. Throws DeviceError if the driver can't list them.
		virtual std::vector<DeviceInfo> devices() = 0;

		// Opens a device stopped. Throws DeviceError if it can't, e.g. for an unknown device or an unsupported sample
		// rate, block size or clock source.
		virtual std::unique_ptr<Device> open(const DeviceConfig& config) = 0;
	};

	// The backends this build has, the preferred one first and the dummy backend last.
	std::vector<std::unique_ptr<Backend>> makeBackends();
}
