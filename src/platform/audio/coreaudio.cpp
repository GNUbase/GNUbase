// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/coreaudio.h"

#include "platform/hosttime.h"
#include "platform/log.h"
#include "platform/rt/denormals.h"
#include "platform/rt/realtime.h"

#include <Block.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <os/workgroup.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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
		constexpr AudioObjectPropertyElement g_main = 0; // kAudioObjectPropertyElementMain, so named from macOS 12 on
		constexpr std::array<double, 6> g_sampleRates{44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
		constexpr std::array<uint32_t, 7> g_blockSizes{32, 64, 128, 256, 512, 1024, 2048};
		constexpr auto g_rateTimeout = std::chrono::seconds(3); // for a device to take on a new sample rate

		void check(OSStatus status, const char* what)
		{
			if (status != noErr)
				throw DeviceError(std::string("CoreAudio: ") + what + " failed (" + std::to_string(status) + ")");
		}

		AudioObjectPropertyAddress address(
			AudioObjectPropertySelector selector, AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
		{
			return {selector, scope, g_main};
		}

		// Reads a property of a fixed size; false if the object doesn't have it.
		template <typename T> bool read(AudioObjectID object, const AudioObjectPropertyAddress& where, T& value)
		{
			UInt32 size = sizeof(T);
			return AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, &value) == noErr && size == sizeof(T);
		}

		template <typename T> std::vector<T> readArray(AudioObjectID object, const AudioObjectPropertyAddress& where)
		{
			UInt32 size = 0;
			if (AudioObjectGetPropertyDataSize(object, &where, 0, nullptr, &size) != noErr)
				return {};
			std::vector<T> values(size / sizeof(T));
			size = static_cast<UInt32>(values.size() * sizeof(T));
			if (AudioObjectGetPropertyData(object, &where, 0, nullptr, &size, values.data()) != noErr)
				return {};
			values.resize(size / sizeof(T));
			return values;
		}

		std::string readString(AudioObjectID object, const AudioObjectPropertyAddress& where)
		{
			CFStringRef string = nullptr;
			if (!read(object, where, string) || string == nullptr)
				return {};
			const CFIndex capacity =
				CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
			std::string result(static_cast<size_t>(capacity), '\0');
			if (!CFStringGetCString(string, result.data(), capacity, kCFStringEncodingUTF8))
				result.clear();
			CFRelease(string);
			result.resize(std::char_traits<char>::length(result.c_str()));
			return result;
		}

		int channelCount(AudioObjectID device, AudioObjectPropertyScope scope)
		{
			const AudioObjectPropertyAddress where = address(kAudioDevicePropertyStreamConfiguration, scope);
			UInt32 size = 0;
			if (AudioObjectGetPropertyDataSize(device, &where, 0, nullptr, &size) != noErr)
				return 0;
			std::vector<uint64_t> storage((std::max<size_t>(size, sizeof(AudioBufferList)) + 7) / 8); // aligned
			auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
			if (AudioObjectGetPropertyData(device, &where, 0, nullptr, &size, list) != noErr)
				return 0;
			int count = 0;
			for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
				count += static_cast<int>(list->mBuffers[i].mNumberChannels);
			return count;
		}

		std::vector<std::string> channelNames(
			AudioObjectID device, AudioObjectPropertyScope scope, int count, const std::string& prefix)
		{
			std::vector<std::string> names;
			for (int c = 0; c < count; ++c)
			{
				std::string name =
					readString(device, {kAudioObjectPropertyElementName, scope, static_cast<UInt32>(c + 1)});
				names.push_back(name.empty() ? prefix + std::to_string(c + 1) : std::move(name));
			}
			return names;
		}

		template <typename T> void sortUnique(std::vector<T>& values)
		{
			values.erase(std::remove(values.begin(), values.end(), T{}), values.end());
			std::sort(values.begin(), values.end());
			values.erase(std::unique(values.begin(), values.end()), values.end());
		}

		// The usual rates within the device's ranges, the rates it lists one by one and the current one.
		std::vector<double> sampleRates(AudioObjectID device, double current)
		{
			std::vector<double> rates{current};
			for (const AudioValueRange& range :
				readArray<AudioValueRange>(device, address(kAudioDevicePropertyAvailableNominalSampleRates)))
			{
				if (range.mMinimum == range.mMaximum)
					rates.push_back(range.mMinimum);
				else
					for (const double rate : g_sampleRates)
						if (rate >= range.mMinimum && rate <= range.mMaximum)
							rates.push_back(rate);
			}
			sortUnique(rates);
			return rates;
		}

		AudioValueRange bufferSizeRange(AudioObjectID device)
		{
			AudioValueRange range{};
			read(device, address(kAudioDevicePropertyBufferFrameSizeRange), range);
			return range;
		}

		std::vector<uint32_t> blockSizes(AudioObjectID device, uint32_t current)
		{
			const AudioValueRange range = bufferSizeRange(device);
			std::vector<uint32_t> sizes{current};
			for (const uint32_t size : g_blockSizes)
				if (size >= range.mMinimum && size <= range.mMaximum)
					sizes.push_back(size);
			sortUnique(sizes);
			return sizes;
		}

		double nominalRate(AudioObjectID device)
		{
			Float64 rate = 0.0;
			read(device, address(kAudioDevicePropertyNominalSampleRate), rate);
			return rate;
		}

		uint32_t bufferFrames(AudioObjectID device)
		{
			UInt32 frames = 0;
			read(device, address(kAudioDevicePropertyBufferFrameSize), frames);
			return frames;
		}

		std::string uid(AudioObjectID device)
		{
			return readString(device, address(kAudioDevicePropertyDeviceUID));
		}

		// A device that isn't hidden and has channels; nothing otherwise.
		std::optional<DeviceInfo> describe(AudioObjectID device)
		{
			DeviceInfo info;
			info.id = uid(device);
			UInt32 hidden = 0;
			read(device, address(kAudioDevicePropertyIsHidden), hidden);
			const int inputs = channelCount(device, kAudioObjectPropertyScopeInput);
			const int outputs = channelCount(device, kAudioObjectPropertyScopeOutput);
			if (info.id.empty() || hidden != 0 || inputs + outputs == 0)
				return std::nullopt;
			info.name = readString(device, address(kAudioObjectPropertyName));
			if (info.name.empty())
				info.name = info.id;
			info.inputs = channelNames(device, kAudioObjectPropertyScopeInput, inputs, "Input ");
			info.outputs = channelNames(device, kAudioObjectPropertyScopeOutput, outputs, "Output ");
			info.sampleRates = sampleRates(device, nominalRate(device));
			info.blockSizes = blockSizes(device, bufferFrames(device));
			return info;
		}

		std::vector<AudioObjectID> deviceObjects()
		{
			return readArray<AudioObjectID>(kAudioObjectSystemObject, address(kAudioHardwarePropertyDevices));
		}

		// Changes a device's sample rate and waits until it runs at it.
		void changeRate(AudioObjectID device, double rate)
		{
			const Float64 wanted = rate;
			const AudioObjectPropertyAddress where = address(kAudioDevicePropertyNominalSampleRate);
			check(AudioObjectSetPropertyData(device, &where, 0, nullptr, sizeof(wanted), &wanted),
				"setting the sample rate");
			const auto deadline = std::chrono::steady_clock::now() + g_rateTimeout;
			while (nominalRate(device) != rate)
			{
				if (std::chrono::steady_clock::now() > deadline)
					throw DeviceError(
						"CoreAudio: the device didn't change to " + std::to_string(std::lround(rate)) + " Hz");
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}

		// Sets a device's sample rate for its lifetime, and then the one it had before.
		class RateSetting
		{
		public:
			RateSetting(AudioObjectID device, double rate) : m_device(device), m_previous(nominalRate(device))
			{
				if (rate == m_previous)
					return;
				m_changed = true;
				try
				{
					changeRate(device, rate);
				}
				catch (const DeviceError&)
				{
					restore();
					throw;
				}
			}
			~RateSetting()
			{
				if (m_changed)
					restore();
			}
			RateSetting(const RateSetting&) = delete;
			RateSetting& operator=(const RateSetting&) = delete;

		private:
			void restore() noexcept
			{
				try
				{
					changeRate(m_device, m_previous);
				}
				catch (const DeviceError& error)
				{
					logging::warning("Could not set the audio device's sample rate back: {}", error.what());
				}
			}

			AudioObjectID m_device;
			double m_previous;
			bool m_changed = false;
		};

		// What the device, its safety offset and its first stream add, in frames.
		uint32_t driverLatency(AudioObjectID device, AudioObjectPropertyScope scope)
		{
			UInt32 deviceLatency = 0;
			UInt32 safetyOffset = 0;
			UInt32 streamLatency = 0;
			read(device, address(kAudioDevicePropertyLatency, scope), deviceLatency);
			read(device, address(kAudioDevicePropertySafetyOffset, scope), safetyOffset);
			const std::vector<AudioStreamID> streams =
				readArray<AudioStreamID>(device, address(kAudioDevicePropertyStreams, scope));
			if (!streams.empty())
				read(streams.front(), address(kAudioStreamPropertyLatency), streamLatency);
			return deviceLatency + safetyOffset + streamLatency;
		}

		std::vector<float*> channelPointers(std::vector<float>& samples, int channels, uint32_t frames)
		{
			std::vector<float*> pointers;
			pointers.reserve(static_cast<size_t>(std::max(channels, 0)));
			for (int c = 0; c < channels; ++c)
				pointers.push_back(samples.data() + (static_cast<size_t>(c) * frames));
			return pointers;
		}

		// The frames of an I/O cycle's buffers, which hold interleaved 32-bit floats, one buffer per stream.
		uint32_t framesOf(const AudioBufferList* list) noexcept
		{
			for (UInt32 b = 0; list != nullptr && b < list->mNumberBuffers; ++b)
				if (const AudioBuffer& buffer = list->mBuffers[b];
					buffer.mNumberChannels != 0 && buffer.mData != nullptr)
					return buffer.mDataByteSize / (buffer.mNumberChannels * sizeof(float));
			return 0;
		}

		// Copies frames from offset on out of the streams' buffers, channel by channel; silence where there are none.
		void deinterleave(
			const AudioBufferList* list, float* const* channels, int count, uint32_t offset, uint32_t frames) noexcept
		{
			int c = 0;
			for (UInt32 b = 0; list != nullptr && b < list->mNumberBuffers && c < count; ++b)
			{
				const AudioBuffer& buffer = list->mBuffers[b];
				const UInt32 stride = buffer.mNumberChannels;
				const auto* data = static_cast<const float*>(buffer.mData);
				const uint32_t available = data != nullptr && stride != 0
					? static_cast<uint32_t>(buffer.mDataByteSize / (stride * sizeof(float)))
					: 0;
				for (UInt32 k = 0; k < stride && c < count; ++k, ++c)
					for (uint32_t i = 0; i < frames; ++i)
						channels[c][i] = offset + i < available ? data[((offset + i) * stride) + k] : 0.0f;
			}
			for (; c < count; ++c)
				std::fill(channels[c], channels[c] + frames, 0.0f);
		}

		// Copies frames into the streams' buffers from offset on; channels beyond count stay silent.
		void interleave(
			const float* const* channels, int count, AudioBufferList* list, uint32_t offset, uint32_t frames) noexcept
		{
			int c = 0;
			for (UInt32 b = 0; list != nullptr && b < list->mNumberBuffers; ++b)
			{
				const AudioBuffer& buffer = list->mBuffers[b];
				const UInt32 stride = buffer.mNumberChannels;
				auto* data = static_cast<float*>(buffer.mData);
				if (data == nullptr || stride == 0)
				{
					c += static_cast<int>(stride);
					continue;
				}
				const auto available = static_cast<uint32_t>(buffer.mDataByteSize / (stride * sizeof(float)));
				const uint32_t end = std::min(offset + frames, available);
				for (UInt32 k = 0; k < stride; ++k, ++c)
					for (uint32_t i = offset; i < end; ++i)
						data[(i * stride) + k] = c < count ? channels[c][i - offset] : 0.0f;
			}
		}

		void silence(AudioBufferList* list) noexcept
		{
			for (UInt32 b = 0; list != nullptr && b < list->mNumberBuffers; ++b)
				if (list->mBuffers[b].mData != nullptr)
					std::fill_n(static_cast<char*>(list->mBuffers[b].mData), list->mBuffers[b].mDataByteSize, 0);
		}

		class CoreAudioDevice : public Device
		{
		public:
			// A block size of 0 makes a block an I/O cycle.
			CoreAudioDevice(AudioObjectID object, DeviceInfo info, double rate, uint32_t blockSize)
				: m_object(object), m_info(std::move(info)), m_rate(object, rate)
			{
				if (blockSize != 0)
				{
					// Blocks split larger I/O cycles: ask for the size nearest to the block size the device allows.
					const AudioValueRange range = bufferSizeRange(object);
					auto frames = static_cast<UInt32>(blockSize);
					if (range.mMaximum > 0.0)
						frames = std::clamp(
							frames, static_cast<UInt32>(range.mMinimum), static_cast<UInt32>(range.mMaximum));
					const AudioObjectPropertyAddress where = address(kAudioDevicePropertyBufferFrameSize);
					check(AudioObjectSetPropertyData(object, &where, 0, nullptr, sizeof(frames), &frames),
						"setting the buffer size");
				}
				const uint32_t frames = bufferFrames(object);
				m_format.sampleRate = rate;
				m_format.blockSize = blockSize != 0 ? blockSize : std::max<uint32_t>(frames, 1);
				m_format.inputChannels = static_cast<int>(m_info.inputs.size());
				m_format.outputChannels = static_cast<int>(m_info.outputs.size());
				if (m_format.inputChannels > 0)
					m_latency.input = driverLatency(object, kAudioObjectPropertyScopeInput) + frames;
				if (m_format.outputChannels > 0)
					m_latency.output = driverLatency(object, kAudioObjectPropertyScopeOutput) + frames;
				if (std::find(m_info.blockSizes.begin(), m_info.blockSizes.end(), m_format.blockSize) ==
					m_info.blockSizes.end())
				{
					m_info.blockSizes.push_back(m_format.blockSize);
					std::sort(m_info.blockSizes.begin(), m_info.blockSizes.end());
				}
				m_inputSamples.resize(static_cast<size_t>(m_format.inputChannels) * m_format.blockSize);
				m_inputs = channelPointers(m_inputSamples, m_format.inputChannels, m_format.blockSize);
				m_outputSamples.resize(static_cast<size_t>(m_format.outputChannels) * m_format.blockSize);
				m_outputs = channelPointers(m_outputSamples, m_format.outputChannels, m_format.blockSize);

				check(AudioDeviceCreateIOProcID(object, &CoreAudioDevice::ioProc, this, &m_proc),
					"creating the I/O callback");
				os_workgroup_t workgroup = nullptr;
				if (read(object, address(kAudioDevicePropertyIOThreadOSWorkgroup), workgroup))
					m_workgroup = workgroup;
				listen();
			}

			~CoreAudioDevice() override
			{
				CoreAudioDevice::stop(); // not virtually: a derived device would be gone already
				for (const AudioObjectPropertyAddress& where : m_listened)
					AudioObjectRemovePropertyListenerBlock(m_object, &where, m_queue, m_listener);
				dispatch_sync(m_queue,
					^{
					}); // the last notification has been handled
				dispatch_release(m_queue);
				Block_release(m_listener);
				AudioDeviceDestroyIOProcID(m_object, m_proc);
				if (m_workgroup != nullptr)
					os_release(m_workgroup);
				// m_rate sets the sample rate back.
			}

			const DeviceFormat& format() const override { return m_format; }
			DeviceInfo info() const override { return m_info; }
			DeviceLatency latency() const override { return m_latency; }
			bool running() const override { return m_running; }
			uint64_t position() const override { return m_position.load(std::memory_order_acquire); }
			void* workgroup() const override { return m_workgroup; }

			void start(AudioCallback& callback) override
			{
				if (m_running)
					throw std::logic_error("the CoreAudio device is already running");
				m_callback.store(&callback, std::memory_order_seq_cst);
				const OSStatus status = AudioDeviceStart(m_object, m_proc);
				if (status != noErr)
					m_callback.store(nullptr, std::memory_order_seq_cst);
				check(status, "starting the device");
				m_running = true;
			}

			void stop() override
			{
				if (!m_running)
					return;
				// The callback stores m_inside before it loads m_callback, and this thread clears m_callback before it
				// loads m_inside: in sequentially consistent order, a cycle either sees no callback or is waited for.
				m_callback.store(nullptr, std::memory_order_seq_cst);
				AudioDeviceStop(m_object, m_proc);
				while (m_inside.load(std::memory_order_seq_cst))
					std::this_thread::yield();
				m_running = false;
			}

		private:
			static OSStatus ioProc(AudioObjectID /*device*/, const AudioTimeStamp* /*now*/,
				const AudioBufferList* input, const AudioTimeStamp* /*inputTime*/, AudioBufferList* output,
				const AudioTimeStamp* /*outputTime*/, void* client)
			{
				static_cast<CoreAudioDevice*>(client)->process(input, output);
				return noErr;
			}

			// One I/O cycle, in blocks of at most the block size.
			void process(const AudioBufferList* input, AudioBufferList* output) noexcept
			{
				m_inside.store(true, std::memory_order_seq_cst);
				AudioCallback* callback = m_callback.load(std::memory_order_seq_cst);
				if (callback == nullptr)
				{
					silence(output);
					m_inside.store(false, std::memory_order_seq_cst);
					return;
				}
				const rt::ScopedRealtimeContext realtime;
				const rt::ScopedDenormalsDisabled denormals;
				const uint32_t frames = std::max(framesOf(output), framesOf(input));
				const HostTime started = hostTime();
				for (uint32_t done = 0; done < frames;)
				{
					const uint32_t block = std::min(frames - done, m_format.blockSize);
					deinterleave(input, m_inputs.data(), m_format.inputChannels, done, block);
					std::fill(m_outputSamples.begin(), m_outputSamples.end(), 0.0f);
					m_midiIn.clear();
					m_midiOut.clear();
					const uint64_t position = m_position.load(std::memory_order_relaxed);
					const HostTime time = std::max(
						started + static_cast<HostTime>(std::llround(done * 1e9 / m_format.sampleRate)), m_lastTime);
					m_lastTime = time;
					callback->process({m_inputs.data(), m_outputs.data(), block, position, time, m_midiIn, m_midiOut});
					interleave(m_outputs.data(), m_format.outputChannels, output, done, block);
					m_position.store(position + block, std::memory_order_release);
					done += block;
				}
				m_inside.store(false, std::memory_order_seq_cst);
			}

			// Asks for a reset when the device goes away, its channels change or another program changes its rate.
			// Notifications come on a queue of the device's own, so that none is still running once it's gone.
			void listen()
			{
				m_queue = dispatch_queue_create("gnubase.coreaudio", DISPATCH_QUEUE_SERIAL);
				m_listener = Block_copy(^(UInt32 count, const AudioObjectPropertyAddress* addresses) {
				  bool reset = false;
				  for (UInt32 i = 0; i < count; ++i)
				  {
					  if (addresses[i].mSelector == kAudioDevicePropertyDeviceIsAlive)
					  {
						  UInt32 alive = 1;
						  reset = reset || (read(m_object, addresses[i], alive) && alive == 0);
					  }
					  else if (addresses[i].mSelector == kAudioDevicePropertyNominalSampleRate)
						  reset = reset || nominalRate(m_object) != m_format.sampleRate;
					  else
						  reset = true;
				  }
				  if (reset)
					  requestReset();
				});
				m_listened = {address(kAudioDevicePropertyDeviceIsAlive),
					address(kAudioDevicePropertyNominalSampleRate),
					address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput),
					address(kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput)};
				for (const AudioObjectPropertyAddress& where : m_listened)
					if (AudioObjectAddPropertyListenerBlock(m_object, &where, m_queue, m_listener) != noErr)
						logging::warning("CoreAudio: {} won't report changes", m_info.name);
			}

			AudioObjectID m_object;
			DeviceInfo m_info;
			RateSetting m_rate;
			DeviceFormat m_format;
			DeviceLatency m_latency;
			AudioDeviceIOProcID m_proc = nullptr;
			os_workgroup_t m_workgroup = nullptr;
			dispatch_queue_t m_queue = nullptr;
			AudioObjectPropertyListenerBlock m_listener = nullptr;
			std::array<AudioObjectPropertyAddress, 4> m_listened{};
			bool m_running = false;

			// Audio thread memory, allocated when opened.
			std::vector<float> m_inputSamples;
			std::vector<float*> m_inputs;
			std::vector<float> m_outputSamples;
			std::vector<float*> m_outputs;
			MidiBuffer m_midiIn;
			MidiBuffer m_midiOut;
			HostTime m_lastTime = 0; // of the last block

			std::atomic<AudioCallback*> m_callback{nullptr};
			std::atomic<bool> m_inside{false}; // the callback is in an I/O cycle
			std::atomic<uint64_t> m_position{0};
		};

		class CoreAudioBackend : public Backend
		{
		public:
			CoreAudioBackend()
			{
				// The HAL sends notifications on a thread of its own rather than on the main thread's run loop, which
				// a headless engine doesn't run.
				CFRunLoopRef none = nullptr;
				const AudioObjectPropertyAddress where = address(kAudioHardwarePropertyRunLoop);
				AudioObjectSetPropertyData(kAudioObjectSystemObject, &where, 0, nullptr, sizeof(none), &none);
			}

			std::string name() const override { return "coreaudio"; }

			std::vector<DeviceInfo> devices() override
			{
				std::vector<DeviceInfo> devices;
				for (const AudioObjectID object : deviceObjects())
					if (std::optional<DeviceInfo> info = describe(object))
						devices.push_back(std::move(*info));
				return devices;
			}

			std::unique_ptr<Device> open(const DeviceConfig& config) override
			{
				AudioObjectID object = kAudioObjectUnknown;
				if (config.device.empty())
				{
					read(kAudioObjectSystemObject, address(kAudioHardwarePropertyDefaultOutputDevice), object);
					if (object == kAudioObjectUnknown)
						read(kAudioObjectSystemObject, address(kAudioHardwarePropertyDefaultInputDevice), object);
					if (object == kAudioObjectUnknown)
						throw DeviceError("there is no audio device");
				}
				else
				{
					const std::vector<AudioObjectID> objects = deviceObjects();
					const auto found = std::find_if(objects.begin(), objects.end(),
						[&config](AudioObjectID candidate) { return uid(candidate) == config.device; });
					if (found == objects.end())
						throw DeviceError("no device " + config.device);
					object = *found;
				}
				std::optional<DeviceInfo> info = describe(object);
				if (!info)
					throw DeviceError("the device " + config.device + " has no channels");
				const double rate = config.sampleRate != 0.0 ? config.sampleRate : nominalRate(object);
				if (std::find(info->sampleRates.begin(), info->sampleRates.end(), rate) == info->sampleRates.end())
					throw DeviceError(info->name + " doesn't run at " + std::to_string(std::lround(rate)) + " Hz");
				if (config.blockSize > 8192)
					throw DeviceError("CoreAudio blocks have at most 8192 frames");
				if (!config.clockSource.empty())
					throw DeviceError("CoreAudio devices have no clock sources here");
				return std::make_unique<CoreAudioDevice>(object, std::move(*info), rate, config.blockSize);
			}
		};
	}

	std::unique_ptr<Backend> makeCoreAudioBackend()
	{
		return std::make_unique<CoreAudioBackend>();
	}
}
