// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/dummydevice.h"

#include "platform/rt/realtime.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace gnubase::audio
{
	namespace
	{
		// More than one block can hold of injected MIDI is dropped.
		constexpr size_t g_maxMidiPerBlock = 1024;

		std::vector<float*> channelPointers(std::vector<float>& samples, int channels, uint32_t frames)
		{
			std::vector<float*> pointers;
			pointers.reserve(static_cast<size_t>(std::max(channels, 0)));
			for (int c = 0; c < channels; ++c)
				pointers.push_back(samples.data() + (static_cast<size_t>(c) * frames));
			return pointers;
		}

		// Adds a value to a sorted list unless it is there.
		template <typename T> void include(std::vector<T>& values, T value)
		{
			const auto at = std::lower_bound(values.begin(), values.end(), value);
			if (at == values.end() || *at != value)
				values.insert(at, value);
		}
	}

	DummyDevice::DummyDevice(DeviceFormat format, size_t captureFrames, size_t captureMidi)
		: m_format(format), m_inputSamples(static_cast<size_t>(std::max(format.inputChannels, 0)) * format.blockSize),
		  m_outputSamples(static_cast<size_t>(std::max(format.outputChannels, 0)) * format.blockSize),
		  m_midiIn(g_maxMidiPerBlock), m_midiOut(g_maxMidiPerBlock),
		  m_capture(static_cast<size_t>(std::max(format.outputChannels, 0)), std::vector<float>(captureFrames)),
		  m_midiCapture(captureMidi)
	{
		if (format.blockSize == 0 || format.sampleRate <= 0.0)
			throw DeviceError("the dummy device needs a block size and a sample rate");
		m_inputs = channelPointers(m_inputSamples, format.inputChannels, format.blockSize);
		m_outputs = channelPointers(m_outputSamples, format.outputChannels, format.blockSize);
	}

	DummyDevice::~DummyDevice()
	{
		stop();
	}

	DeviceInfo DummyDevice::info() const
	{
		DeviceInfo info;
		info.id = "dummy";
		info.name = "Dummy device";
		for (int c = 0; c < m_format.inputChannels; ++c)
			info.inputs.push_back("Input " + std::to_string(c + 1));
		for (int c = 0; c < m_format.outputChannels; ++c)
			info.outputs.push_back("Output " + std::to_string(c + 1));
		info.sampleRates = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
		info.blockSizes = {32, 64, 128, 256, 512, 1024, 2048};
		include(info.sampleRates, m_format.sampleRate);
		include(info.blockSizes, m_format.blockSize);
		return info;
	}

	void DummyDevice::setInput(InputSource source)
	{
		m_input = std::move(source);
	}

	void DummyDevice::injectMidi(uint64_t position, const MidiMessage& message)
	{
		const TimedMidi timed{position, message};
		const auto at = std::upper_bound(m_pendingMidi.begin() + static_cast<std::ptrdiff_t>(m_nextMidi),
			m_pendingMidi.end(), position, [](uint64_t p, const TimedMidi& m) { return p < m.position; });
		m_pendingMidi.insert(at, timed);
	}

	void DummyDevice::render(AudioCallback& callback, uint64_t frames)
	{
		rt::assertNotRealtime("audio::DummyDevice::render");
		if (running())
			throw std::logic_error("the dummy device renders offline only while stopped");
		while (frames > 0)
		{
			const auto block = static_cast<uint32_t>(std::min<uint64_t>(frames, m_format.blockSize));
			processBlock(callback, block);
			frames -= block;
		}
	}

	void DummyDevice::start(AudioCallback& callback)
	{
		if (running())
			throw std::logic_error("the dummy device is already running");
		m_stopping = false;
		m_thread = std::thread(
			[this, &callback]
			{
				const auto blockDuration = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
					std::chrono::duration<double>(m_format.blockSize / m_format.sampleRate));
				auto next = std::chrono::steady_clock::now();
				while (!m_stopping.load(std::memory_order_acquire))
				{
					processBlock(callback, m_format.blockSize);
					next += blockDuration;
					std::this_thread::sleep_until(next);
				}
			});
	}

	void DummyDevice::stop()
	{
		if (!running())
			return;
		m_stopping = true;
		m_thread.join();
	}

	void DummyDevice::processBlock(AudioCallback& callback, uint32_t frames)
	{
		const uint64_t position = m_position.load(std::memory_order_relaxed);

		m_midiIn.clear();
		m_midiOut.clear();
		for (; m_nextMidi < m_pendingMidi.size() && m_pendingMidi[m_nextMidi].position < position + frames;
			++m_nextMidi)
		{
			const TimedMidi& timed = m_pendingMidi[m_nextMidi];
			if (timed.position < position)
				continue;
			MidiMessage message = timed.message;
			message.offset = static_cast<uint32_t>(timed.position - position);
			m_midiIn.add(message);
		}
		std::fill(m_inputSamples.begin(), m_inputSamples.end(), 0.0f);
		std::fill(m_outputSamples.begin(), m_outputSamples.end(), 0.0f);

		{
			const rt::ScopedRealtimeContext realtime;
			if (m_input)
				m_input(m_inputs.data(), m_format.inputChannels, frames, position);
			callback.process({m_inputs.data(), m_outputs.data(), frames, position, m_midiIn, m_midiOut});
		}

		// Capture, then publish what was captured.
		const size_t captured = m_capturedFrames.load(std::memory_order_relaxed);
		const size_t room = m_capture.empty() ? 0 : m_capture[0].size() - captured;
		const size_t keep = std::min<size_t>(room, frames);
		for (size_t c = 0; c < m_capture.size(); ++c)
			std::memcpy(m_capture[c].data() + captured, m_outputs[c], keep * sizeof(float));
		m_droppedFrames.fetch_add(frames - keep, std::memory_order_release);
		m_capturedFrames.store(captured + keep, std::memory_order_release);

		size_t midi = m_capturedMidi.load(std::memory_order_relaxed);
		for (const MidiMessage& message : m_midiOut)
			if (midi < m_midiCapture.size())
				m_midiCapture[midi++] = {position + message.offset, message};
		m_capturedMidi.store(midi, std::memory_order_release);

		m_position.store(position + frames, std::memory_order_release);
	}

	std::vector<float> DummyDevice::captured(int channel, size_t start, size_t count) const
	{
		const size_t available = m_capturedFrames.load(std::memory_order_acquire);
		if (channel < 0 || static_cast<size_t>(channel) >= m_capture.size() || start >= available)
			return {};
		const std::vector<float>& samples = m_capture[static_cast<size_t>(channel)];
		const size_t end = std::min(available, start + count);
		return {
			samples.begin() + static_cast<std::ptrdiff_t>(start), samples.begin() + static_cast<std::ptrdiff_t>(end)};
	}

	std::vector<TimedMidi> DummyDevice::capturedMidi() const
	{
		const size_t count = m_capturedMidi.load(std::memory_order_acquire);
		return {m_midiCapture.begin(), m_midiCapture.begin() + static_cast<std::ptrdiff_t>(count)};
	}

	std::vector<DeviceInfo> DummyBackend::devices()
	{
		return {DummyDevice(DeviceFormat{}, 0).info()};
	}

	std::unique_ptr<Device> DummyBackend::open(const DeviceConfig& config)
	{
		if (!config.device.empty() && config.device != "dummy")
			throw DeviceError("the dummy backend has no device " + config.device);
		if (!config.clockSource.empty())
			throw DeviceError("the dummy device has no clock source " + config.clockSource);
		DeviceFormat format;
		if (config.sampleRate != 0.0)
			format.sampleRate = config.sampleRate;
		if (config.blockSize != 0)
			format.blockSize = config.blockSize;
		if (format.sampleRate < 8000.0 || format.sampleRate > 384000.0)
			throw DeviceError("the dummy device runs at 8000 to 384000 Hz");
		if (format.blockSize > 8192)
			throw DeviceError("the dummy device's blocks have at most 8192 frames");
		return std::make_unique<DummyDevice>(format, static_cast<size_t>(format.sampleRate));
	}
}
