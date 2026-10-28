// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// What audio devices and the engine exchange once per block. The dummy device implements it now; the hardware
// backends (ASIO, WASAPI, CoreAudio, PipeWire, JACK) follow in M03.
namespace gnubase::audio
{
	struct DeviceFormat
	{
		double sampleRate = 48000.0;
		uint32_t blockSize = 256; // frames per block
		int inputChannels = 2;
		int outputChannels = 2;
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
}
