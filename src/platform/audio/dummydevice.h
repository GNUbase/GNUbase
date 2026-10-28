// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/audio/device.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace gnubase::audio
{
	// A MIDI message at an absolute frame position.
	struct TimedMidi
	{
		uint64_t position = 0;
		MidiMessage message; // its offset is ignored

		bool operator==(const TimedMidi& other) const
		{
			return position == other.position && message.size == other.message.size &&
				message.bytes == other.message.bytes;
		}
	};

	// An audio device without hardware, for tests and headless runs. Its inputs come from a callback, its outputs and
	// MIDI output are captured into memory, and MIDI input is injected at frame positions. It runs in one of two
	// modes, both deterministic: offline, where render processes blocks on the calling thread as fast as possible,
	// and simulated real time, where its own thread processes one block per block duration.
	//
	// Capture memory is allocated up front; what doesn't fit is dropped and counted. Captured audio and MIDI can be
	// read while the device runs.
	class DummyDevice
	{
	public:
		// Fills the inputs for a block that starts at position. Runs on the audio thread: real-time rules apply.
		using InputSource = std::function<void(float* const* inputs, int channels, uint32_t frames, uint64_t position)>;

		DummyDevice(DeviceFormat format, size_t captureFrames, size_t captureMidi = 4096);
		~DummyDevice(); // stops the device
		DummyDevice(const DummyDevice&) = delete;
		DummyDevice& operator=(const DummyDevice&) = delete;

		const DeviceFormat& format() const { return m_format; }

		// While stopped. Without a source the inputs are silent.
		void setInput(InputSource source);

		// While stopped. Messages at positions that have already been processed are dropped.
		void injectMidi(uint64_t position, const MidiMessage& message);

		// Offline mode: processes frames frames in blocks of the format's block size on the calling thread; the last
		// block may be shorter. While stopped.
		void render(AudioCallback& callback, uint64_t frames);

		// Simulated real time: processes blocks on the device's thread, one per block duration, until stop.
		void start(AudioCallback& callback);
		void stop();
		bool running() const { return m_thread.joinable(); }

		// Frames processed so far.
		uint64_t position() const { return m_position.load(std::memory_order_acquire); }

		size_t capturedFrames() const { return m_capturedFrames.load(std::memory_order_acquire); }
		size_t droppedFrames() const { return m_droppedFrames.load(std::memory_order_acquire); }

		// Frames [start, start + count) of an output channel, cut to what has been captured.
		std::vector<float> captured(int channel, size_t start, size_t count) const;

		std::vector<TimedMidi> capturedMidi() const;

	private:
		void processBlock(AudioCallback& callback, uint32_t frames);

		DeviceFormat m_format;
		InputSource m_input;
		std::vector<TimedMidi> m_pendingMidi; // sorted by position
		size_t m_nextMidi = 0;

		// Block memory, allocated in the constructor.
		std::vector<float> m_inputSamples;
		std::vector<float> m_outputSamples;
		std::vector<float*> m_inputs;
		std::vector<float*> m_outputs;
		MidiBuffer m_midiIn;
		MidiBuffer m_midiOut;

		// Written by the thread that processes, read by any thread.
		std::vector<std::vector<float>> m_capture; // per output channel
		std::atomic<size_t> m_capturedFrames{0};
		std::atomic<size_t> m_droppedFrames{0};
		std::vector<TimedMidi> m_midiCapture;
		std::atomic<size_t> m_capturedMidi{0};
		std::atomic<uint64_t> m_position{0};

		std::atomic<bool> m_stopping{false};
		std::thread m_thread;
	};
}
