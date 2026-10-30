// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/wasapi.h"

#include "platform/audio/inputfifo.h"
#include "platform/audio/sampleformat.h"
#include "platform/hosttime.h"
#include "platform/log.h"
#include "platform/rt/denormals.h"
#include "platform/rt/realtime.h"
#include "platform/winutf8.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
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
		using Microsoft::WRL::ComPtr;

		constexpr double g_hnsPerSecond = 10000000.0; // REFERENCE_TIME counts 100 ns
		constexpr std::array<double, 6> g_sampleRates{44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
		constexpr std::array<uint32_t, 7> g_blockSizes{32, 64, 128, 256, 512, 1024, 2048};
		constexpr DWORD g_hangMilliseconds = 2000; // an endpoint silent this long is gone

		// Defined here rather than through initguid.h, which would define every GUID of the headers in this file.
		constexpr PROPERTYKEY g_friendlyName{
			{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
		constexpr GUID g_subtypePcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
		constexpr GUID g_subtypeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

		// What exclusive mode tries, best first.
		struct ExclusiveFormat
		{
			SampleFormat sample;
			WORD validBits;
		};
		constexpr std::array<ExclusiveFormat, 5> g_exclusiveFormats{
			{{SampleFormat::Float32, WORD{32}}, {SampleFormat::Int32, WORD{32}}, {SampleFormat::Int32, WORD{24}},
				{SampleFormat::Int24, WORD{24}}, {SampleFormat::Int16, WORD{16}}}};

		// Speaker positions in the order of the channel mask's bits.
		constexpr std::array<const char*, 18> g_positions{"Left", "Right", "Center", "LFE", "Rear Left", "Rear Right",
			"Front Left of Center", "Front Right of Center", "Rear Center", "Side Left", "Side Right", "Top Center",
			"Top Front Left", "Top Front Center", "Top Front Right", "Top Rear Left", "Top Rear Center",
			"Top Rear Right"};

		void check(HRESULT result, const char* what)
		{
			if (FAILED(result))
			{
				std::array<char, 16> code{};
				std::snprintf(code.data(), code.size(), "0x%08lx", static_cast<unsigned long>(result));
				throw DeviceError(std::string("WASAPI: ") + what + " failed (" + code.data() + ")");
			}
		}

		// Keeps COM's multithreaded apartment alive, so threads that haven't joined an apartment, like the engine's,
		// can use the endpoints.
		class MtaUsage
		{
		public:
			MtaUsage() : m_result(CoIncrementMTAUsage(&m_cookie)) {}
			~MtaUsage()
			{
				if (SUCCEEDED(m_result))
					CoDecrementMTAUsage(m_cookie);
			}
			MtaUsage(const MtaUsage&) = delete;
			MtaUsage& operator=(const MtaUsage&) = delete;

		private:
			CO_MTA_USAGE_COOKIE m_cookie{};
			HRESULT m_result;
		};

		class Event
		{
		public:
			explicit Event(bool manualReset)
				: m_handle(CreateEventW(nullptr, manualReset ? TRUE : FALSE, FALSE, nullptr))
			{
				if (m_handle == nullptr)
					throw DeviceError("WASAPI: cannot create an event");
			}
			~Event() { CloseHandle(m_handle); }
			Event(const Event&) = delete;
			Event& operator=(const Event&) = delete;

			HANDLE get() const { return m_handle; }

		private:
			HANDLE m_handle;
		};

		std::string idOf(IMMDevice& device)
		{
			LPWSTR id = nullptr;
			check(device.GetId(&id), "IMMDevice::GetId");
			std::string result = utf8(id);
			CoTaskMemFree(id);
			return result;
		}

		ComPtr<IAudioClient> activate(IMMDevice& device)
		{
			ComPtr<IAudioClient> client;
			check(device.Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client), "IMMDevice::Activate");
			return client;
		}

		// An input or output endpoint.
		struct Endpoint
		{
			ComPtr<IMMDevice> device;
			std::string id;
			std::string name;
			int channels = 0;
			DWORD channelMask = 0;
			double mixRate = 0.0; // what Windows mixes at in shared mode
		};

		Endpoint describe(ComPtr<IMMDevice> device)
		{
			Endpoint endpoint;
			endpoint.id = idOf(*device.Get());
			ComPtr<IPropertyStore> properties;
			if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)))
			{
				PROPVARIANT name;
				PropVariantInit(&name);
				if (SUCCEEDED(properties->GetValue(g_friendlyName, &name)) && name.vt == VT_LPWSTR)
					endpoint.name = utf8(name.pwszVal);
				PropVariantClear(&name);
			}
			if (endpoint.name.empty())
				endpoint.name = endpoint.id;
			const ComPtr<IAudioClient> client = activate(*device.Get());
			WAVEFORMATEX* mix = nullptr;
			check(client->GetMixFormat(&mix), "IAudioClient::GetMixFormat");
			endpoint.channels = mix->nChannels;
			endpoint.mixRate = mix->nSamplesPerSec;
			if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
				endpoint.channelMask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
			CoTaskMemFree(mix);
			endpoint.device = std::move(device);
			return endpoint;
		}

		ComPtr<IMMDeviceEnumerator> makeEnumerator()
		{
			ComPtr<IMMDeviceEnumerator> enumerator;
			check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
				"creating the device enumerator");
			return enumerator;
		}

		std::vector<Endpoint> endpoints(IMMDeviceEnumerator& enumerator, EDataFlow flow)
		{
			ComPtr<IMMDeviceCollection> collection;
			check(enumerator.EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection), "listing the endpoints");
			UINT count = 0;
			check(collection->GetCount(&count), "IMMDeviceCollection::GetCount");
			std::vector<Endpoint> result;
			for (UINT i = 0; i < count; ++i)
			{
				ComPtr<IMMDevice> device;
				if (FAILED(collection->Item(i, &device)))
					continue;
				try
				{
					result.push_back(describe(device));
				}
				catch (const DeviceError& error)
				{
					logging::warning("Skipping an audio endpoint: {}", error.what());
				}
			}
			return result;
		}

		// The default endpoint's id, or empty if there is none.
		std::string defaultId(IMMDeviceEnumerator& enumerator, EDataFlow flow)
		{
			ComPtr<IMMDevice> device;
			if (FAILED(enumerator.GetDefaultAudioEndpoint(flow, eConsole, &device)))
				return {};
			return idOf(*device.Get());
		}

		std::vector<std::string> channelNames(int channels, DWORD mask)
		{
			std::vector<std::string> names;
			for (size_t bit = 0; bit < g_positions.size() && static_cast<int>(names.size()) < channels; ++bit)
				if ((mask & (1UL << bit)) != 0)
					names.emplace_back(g_positions[bit]);
			while (static_cast<int>(names.size()) < channels)
				names.push_back("Channel " + std::to_string(names.size() + 1));
			return names;
		}

		struct StreamFormat
		{
			WAVEFORMATEXTENSIBLE wave{};
			SampleFormat sample = SampleFormat::Float32;
		};

		StreamFormat makeFormat(double rate, const Endpoint& endpoint, SampleFormat sample, WORD validBits)
		{
			StreamFormat format;
			format.sample = sample;
			WAVEFORMATEX& wave = format.wave.Format;
			wave.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
			wave.nChannels = static_cast<WORD>(endpoint.channels);
			wave.nSamplesPerSec = static_cast<DWORD>(rate);
			wave.wBitsPerSample = static_cast<WORD>(bytesPerSample(sample) * 8);
			wave.nBlockAlign = static_cast<WORD>(bytesPerSample(sample) * static_cast<size_t>(endpoint.channels));
			wave.nAvgBytesPerSec = wave.nSamplesPerSec * wave.nBlockAlign;
			wave.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
			format.wave.Samples.wValidBitsPerSample = validBits;
			format.wave.dwChannelMask = endpoint.channelMask;
			format.wave.SubFormat = sample == SampleFormat::Float32 ? g_subtypeFloat : g_subtypePcm;
			return format;
		}

		// The best format the endpoint takes exclusively at a sample rate, if any.
		std::optional<StreamFormat> exclusiveFormat(IAudioClient& client, const Endpoint& endpoint, double rate)
		{
			for (const ExclusiveFormat& candidate : g_exclusiveFormats)
			{
				StreamFormat format = makeFormat(rate, endpoint, candidate.sample, candidate.validBits);
				if (client.IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.wave.Format, nullptr) == S_OK)
					return format;
			}
			return std::nullopt;
		}

		// Shared mode takes every rate (Windows converts); exclusive mode those the endpoint has a format for.
		std::vector<double> sampleRates(const Endpoint& endpoint, bool exclusive)
		{
			if (!exclusive)
				return {g_sampleRates.begin(), g_sampleRates.end()};
			const ComPtr<IAudioClient> client = activate(*endpoint.device.Get());
			std::vector<double> rates;
			for (const double rate : g_sampleRates)
				if (exclusiveFormat(*client.Get(), endpoint, rate))
					rates.push_back(rate);
			return rates;
		}

		std::vector<double> intersect(const std::vector<double>& a, const std::vector<double>& b)
		{
			std::vector<double> result;
			std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(result));
			return result;
		}

		DeviceInfo makeInfo(
			std::string id, std::string name, const Endpoint& output, const Endpoint* input, std::vector<double> rates)
		{
			DeviceInfo info;
			info.id = std::move(id);
			info.name = std::move(name);
			info.outputs = channelNames(output.channels, output.channelMask);
			if (input != nullptr)
				info.inputs = channelNames(input->channels, input->channelMask);
			info.sampleRates = std::move(rates);
			info.blockSizes = {g_blockSizes.begin(), g_blockSizes.end()};
			info.controlPanel = true;
			return info;
		}

		// Tells devices when one of their endpoints goes away or, for those that follow them, the default endpoints
		// change. There is one for the whole process, registered on first use and never released: a call may still be
		// on its way when UnregisterEndpointNotificationCallback returns, and would then reach a freed object (#316).
		class Notifications
			: public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
				  IMMNotificationClient>
		{
		public:
			// Needs COM's multithreaded apartment. Throws DeviceError if it can't register.
			static Notifications& instance()
			{
				static Notifications& client = registered(); // tried again on the next call if it throws
				return client;
			}

			// Calls changed on the notification thread until unsubscribe(key).
			void subscribe(const void* key, std::vector<std::wstring> endpoints, bool followsDefault,
				std::function<void()> changed)
			{
				const std::scoped_lock lock(m_mutex);
				m_subscribers[key] = Subscriber{std::move(endpoints), followsDefault, std::move(changed)};
			}

			// No more calls for key after this returns.
			void unsubscribe(const void* key)
			{
				const std::scoped_lock lock(m_mutex);
				m_subscribers.erase(key);
			}

			HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override
			{
				if (state != DEVICE_STATE_ACTIVE)
					gone(id);
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override
			{
				gone(id);
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow /*flow*/, ERole role, LPCWSTR /*id*/) override
			{
				if (role == eConsole)
					notify([](const Subscriber& subscriber) { return subscriber.followsDefault; });
				return S_OK;
			}

			HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR /*id*/) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR /*id*/, const PROPERTYKEY /*key*/) override
			{
				return S_OK;
			}

		private:
			struct Subscriber
			{
				std::vector<std::wstring> endpoints;
				bool followsDefault;
				std::function<void()> changed;
			};

			static Notifications& registered()
			{
				ComPtr<Notifications> client = Microsoft::WRL::Make<Notifications>();
				ComPtr<IMMDeviceEnumerator> enumerator = makeEnumerator();
				check(enumerator->RegisterEndpointNotificationCallback(client.Get()),
					"IMMDeviceEnumerator::RegisterEndpointNotificationCallback");
				// Both stay for the rest of the process, and so does the apartment they live in.
				CO_MTA_USAGE_COOKIE cookie{};
				CoIncrementMTAUsage(&cookie);
				enumerator.Detach();
				return *client.Detach();
			}

			void gone(LPCWSTR id)
			{
				if (id == nullptr)
					return;
				notify(
					[id](const Subscriber& subscriber)
					{
						return std::find(subscriber.endpoints.begin(), subscriber.endpoints.end(), id) !=
							subscriber.endpoints.end();
					});
			}

			template <typename Concerns> void notify(Concerns concerns)
			{
				const std::scoped_lock lock(m_mutex);
				for (const auto& [key, subscriber] : m_subscribers)
					if (concerns(subscriber))
						subscriber.changed();
			}

			std::mutex m_mutex; // guards m_subscribers
			std::map<const void*, Subscriber> m_subscribers;
		};

		// One endpoint's stream.
		struct Stream
		{
			explicit Stream(Endpoint endpoint) : endpoint(std::move(endpoint)) {}

			Endpoint endpoint;
			Event event{false}; // before the client, which signals it until it is released
			ComPtr<IAudioClient> client;
			SampleFormat format = SampleFormat::Float32;
			UINT32 bufferFrames = 0;
			uint32_t periodFrames = 0;
			uint32_t latencyFrames = 0;
		};

		void initialize(Stream& stream, double rate, bool exclusive)
		{
			IMMDevice& device = *stream.endpoint.device.Get();
			stream.client = activate(device);
			REFERENCE_TIME defaultPeriod = 0;
			REFERENCE_TIME minimumPeriod = 0;
			check(stream.client->GetDevicePeriod(&defaultPeriod, &minimumPeriod), "IAudioClient::GetDevicePeriod");
			if (exclusive)
			{
				const std::optional<StreamFormat> format = exclusiveFormat(*stream.client.Get(), stream.endpoint, rate);
				if (!format)
					throw DeviceError(stream.endpoint.name + " doesn't run exclusively at this sample rate");
				stream.format = format->sample;
				HRESULT result = stream.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE,
					AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minimumPeriod, minimumPeriod, &format->wave.Format, nullptr);
				if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
				{
					// The device wants a period it can align: the buffer size it reports.
					UINT32 frames = 0;
					check(stream.client->GetBufferSize(&frames), "IAudioClient::GetBufferSize");
					const auto aligned = static_cast<REFERENCE_TIME>(std::llround(frames * g_hnsPerSecond / rate));
					stream.client = activate(device);
					result = stream.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
						aligned, aligned, &format->wave.Format, nullptr);
				}
				check(result, "IAudioClient::Initialize");
			}
			else
			{
				const StreamFormat format = makeFormat(rate, stream.endpoint, SampleFormat::Float32, 32);
				check(stream.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
						  AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
							  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
						  0, 0, &format.wave.Format, nullptr),
					"IAudioClient::Initialize");
			}
			check(stream.client->GetBufferSize(&stream.bufferFrames), "IAudioClient::GetBufferSize");
			stream.periodFrames = exclusive
				? stream.bufferFrames
				: static_cast<uint32_t>(std::llround(static_cast<double>(defaultPeriod) * rate / g_hnsPerSecond));
			REFERENCE_TIME latency = 0;
			if (SUCCEEDED(stream.client->GetStreamLatency(&latency)))
				stream.latencyFrames =
					static_cast<uint32_t>(std::llround(static_cast<double>(latency) * rate / g_hnsPerSecond));
			check(stream.client->SetEventHandle(stream.event.get()), "IAudioClient::SetEventHandle");
		}

		std::vector<float*> channelPointers(std::vector<float>& samples, int channels, uint32_t frames)
		{
			std::vector<float*> pointers;
			pointers.reserve(static_cast<size_t>(std::max(channels, 0)));
			for (int c = 0; c < channels; ++c)
				pointers.push_back(samples.data() + (static_cast<size_t>(c) * frames));
			return pointers;
		}

		class WasapiDevice : public Device
		{
		public:
			// A block size of 0 makes a block a period of the output.
			WasapiDevice(DeviceInfo info, const Endpoint& output, const Endpoint* input, double rate,
				uint32_t blockSize, bool exclusive, bool followsDefault)
				: m_info(std::move(info)), m_exclusive(exclusive), m_output(output)
			{
				initialize(m_output, rate, exclusive);
				check(m_output.client->GetService(IID_PPV_ARGS(&m_render)), "IAudioClient::GetService");
				std::vector<std::wstring> ids{wide(output.id)};
				if (input != nullptr)
				{
					m_input.emplace(*input);
					initialize(*m_input, rate, exclusive);
					check(m_input->client->GetService(IID_PPV_ARGS(&m_capture)), "IAudioClient::GetService");
					ids.push_back(wide(input->id));
				}

				m_format.sampleRate = rate;
				m_format.blockSize = blockSize != 0 ? blockSize : m_output.periodFrames;
				blockSize = m_format.blockSize;
				m_format.outputChannels = output.channels;
				m_format.inputChannels = input != nullptr ? input->channels : 0;
				m_latency.output = m_output.bufferFrames + m_output.latencyFrames;
				m_outputSamples.resize(static_cast<size_t>(m_format.outputChannels) * blockSize);
				m_outputs = channelPointers(m_outputSamples, m_format.outputChannels, blockSize);
				if (m_input)
				{
					// The input buffers a period of each endpoint: enough to follow the output's reads.
					const uint32_t target = m_input->periodFrames + m_output.periodFrames;
					m_fifo.emplace(m_format.inputChannels, 4 * (target + m_input->bufferFrames), target);
					m_latency.input = m_input->latencyFrames + target;
					m_inputSamples.resize(static_cast<size_t>(m_format.inputChannels) * blockSize);
					m_inputs = channelPointers(m_inputSamples, m_format.inputChannels, blockSize);
					m_captureSamples.resize(static_cast<size_t>(m_format.inputChannels) * m_input->bufferFrames);
					m_captured = channelPointers(m_captureSamples, m_format.inputChannels, m_input->bufferFrames);
				}
				if (std::find(m_info.blockSizes.begin(), m_info.blockSizes.end(), m_output.periodFrames) ==
					m_info.blockSizes.end())
				{
					m_info.blockSizes.push_back(m_output.periodFrames);
					std::sort(m_info.blockSizes.begin(), m_info.blockSizes.end());
				}

				// Last: if the constructor throws, the destructor that unsubscribes doesn't run.
				Notifications::instance().subscribe(this, std::move(ids), followsDefault, [this] { requestReset(); });
			}

			~WasapiDevice() override
			{
				WasapiDevice::stop(); // not virtually: a derived device would be gone already
				Notifications::instance().unsubscribe(this);
			}

			const DeviceFormat& format() const override { return m_format; }
			DeviceInfo info() const override { return m_info; }
			DeviceLatency latency() const override { return m_latency; }
			bool running() const override { return m_thread.joinable(); }
			uint64_t position() const override { return m_position.load(std::memory_order_acquire); }

			void start(AudioCallback& callback) override
			{
				if (m_thread.joinable())
					throw std::logic_error("the WASAPI device is already running");
				// Silence first, so that the output doesn't start with an underrun.
				BYTE* data = nullptr;
				check(m_render->GetBuffer(m_output.bufferFrames, &data), "IAudioRenderClient::GetBuffer");
				check(m_render->ReleaseBuffer(m_output.bufferFrames, AUDCLNT_BUFFERFLAGS_SILENT),
					"IAudioRenderClient::ReleaseBuffer");
				if (m_input)
					check(m_input->client->Start(), "starting the input");
				const HRESULT started = m_output.client->Start();
				if (FAILED(started) && m_input)
				{
					// Else the input would go on capturing and signalling its event.
					m_input->client->Stop();
					m_input->client->Reset();
				}
				check(started, "starting the output");
				ResetEvent(m_stop.get());
				m_thread = std::thread([this, &callback] { run(callback); });
			}

			void stop() override
			{
				if (!m_thread.joinable())
					return;
				SetEvent(m_stop.get());
				m_thread.join();
				m_output.client->Stop();
				m_output.client->Reset();
				if (m_input)
				{
					m_input->client->Stop();
					m_input->client->Reset();
				}
			}

			void openControlPanel() override
			{
				ShellExecuteW(nullptr, L"open", L"control.exe", L"mmsys.cpl", nullptr, SW_SHOWNORMAL);
			}

		private:
			void run(AudioCallback& callback)
			{
				DWORD task = 0;
				HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
				const rt::ScopedDenormalsDisabled denormals;
				const std::array<HANDLE, 3> events{
					m_stop.get(), m_output.event.get(), m_input ? m_input->event.get() : nullptr};
				const DWORD count = m_input ? 3 : 2;
				bool lost = false;
				while (!lost)
				{
					const DWORD woken = WaitForMultipleObjects(count, events.data(), FALSE, g_hangMilliseconds);
					if (woken == WAIT_OBJECT_0)
						break;
					if (woken != WAIT_OBJECT_0 + 1 && woken != WAIT_OBJECT_0 + 2)
					{
						lost = true; // a timeout: the endpoint hangs
						break;
					}
					const rt::ScopedRealtimeContext realtime;
					lost = !readInput() || (woken == WAIT_OBJECT_0 + 1 && !writeOutput(callback));
				}
				if (mmcss != nullptr)
					AvRevertMmThreadCharacteristics(mmcss);
				if (lost)
				{
					logging::warning("The audio device {} stopped working", m_info.name);
					requestReset();
				}
			}

			// Moves what the input captured into the FIFO. False if the endpoint is gone.
			bool readInput() noexcept
			{
				if (m_capture.Get() == nullptr)
					return true;
				for (;;)
				{
					UINT32 packet = 0;
					if (FAILED(m_capture->GetNextPacketSize(&packet)))
						return false;
					if (packet == 0)
						return true;
					BYTE* data = nullptr;
					UINT32 frames = 0;
					DWORD flags = 0;
					if (FAILED(m_capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
						return false;
					const uint32_t kept = std::min<uint32_t>(frames, m_input->bufferFrames);
					if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0)
						std::fill(m_captureSamples.begin(), m_captureSamples.end(), 0.0f);
					else
						toFloat(data, m_input->format, m_format.inputChannels, kept, m_captured.data());
					m_fifo->write(m_captured.data(), kept);
					if (FAILED(m_capture->ReleaseBuffer(frames)))
						return false;
				}
			}

			// Fills the output's free buffer, in blocks of at most the block size. False if the endpoint is gone.
			bool writeOutput(AudioCallback& callback) noexcept
			{
				UINT32 frames = m_output.bufferFrames;
				if (!m_exclusive)
				{
					UINT32 padding = 0;
					if (FAILED(m_output.client->GetCurrentPadding(&padding)))
						return false;
					frames -= padding;
				}
				if (frames == 0)
					return true;
				BYTE* data = nullptr;
				if (FAILED(m_render->GetBuffer(frames, &data)))
					return false;
				const HostTime started = hostTime();
				const size_t frameBytes =
					bytesPerSample(m_output.format) * static_cast<size_t>(m_format.outputChannels);
				for (UINT32 done = 0; done < frames;)
				{
					const uint32_t block = std::min<uint32_t>(frames - done, m_format.blockSize);
					if (m_fifo)
						m_fifo->read(m_inputs.data(), block);
					std::fill(m_outputSamples.begin(), m_outputSamples.end(), 0.0f);
					m_midiIn.clear();
					m_midiOut.clear();
					const uint64_t position = m_position.load(std::memory_order_relaxed);
					// Never before the previous block, which reaches past this wake-up when the last one had more
					// than a period to fill: some endpoints wake irregularly.
					const HostTime time = std::max(
						started + static_cast<HostTime>(std::llround(done * 1e9 / m_format.sampleRate)), m_lastTime);
					m_lastTime = time;
					callback.process({m_inputs.data(), m_outputs.data(), block, position, time, m_midiIn, m_midiOut});
					fromFloat(m_outputs.data(), m_format.outputChannels, block, m_output.format,
						data + (static_cast<size_t>(done) * frameBytes));
					m_position.store(position + block, std::memory_order_release);
					done += block;
				}
				return SUCCEEDED(m_render->ReleaseBuffer(frames, 0));
			}

			MtaUsage m_mta;
			DeviceInfo m_info;
			bool m_exclusive;
			Stream m_output;
			std::optional<Stream> m_input;
			ComPtr<IAudioRenderClient> m_render;
			ComPtr<IAudioCaptureClient> m_capture;
			DeviceFormat m_format;
			DeviceLatency m_latency;

			// Audio thread memory, allocated when opened.
			std::vector<float> m_outputSamples;
			std::vector<float*> m_outputs;
			std::vector<float> m_inputSamples;
			std::vector<float*> m_inputs;
			std::vector<float> m_captureSamples;
			std::vector<float*> m_captured;
			std::optional<InputFifo> m_fifo;
			MidiBuffer m_midiIn;
			MidiBuffer m_midiOut;
			HostTime m_lastTime = 0; // of the last block

			std::atomic<uint64_t> m_position{0};
			Event m_stop{true};
			std::thread m_thread;
		};

		class WasapiBackend : public Backend
		{
		public:
			explicit WasapiBackend(bool exclusive) : m_exclusive(exclusive) {}

			std::string name() const override { return m_exclusive ? "wasapi-exclusive" : "wasapi"; }

			std::vector<DeviceInfo> devices() override
			{
				const MtaUsage mta;
				const ComPtr<IMMDeviceEnumerator> enumerator = makeEnumerator();
				const std::vector<Endpoint> outputs = endpoints(*enumerator.Get(), eRender);
				const std::vector<Endpoint> inputs = endpoints(*enumerator.Get(), eCapture);
				if (outputs.empty())
					return {};
				std::map<std::string, std::vector<double>> rates;
				for (const Endpoint& endpoint : outputs)
					rates[endpoint.id] = sampleRates(endpoint, m_exclusive);
				for (const Endpoint& endpoint : inputs)
					rates[endpoint.id] = sampleRates(endpoint, m_exclusive);
				const auto find = [](const std::vector<Endpoint>& list, const std::string& id)
				{
					const auto found = std::find_if(
						list.begin(), list.end(), [&id](const Endpoint& endpoint) { return endpoint.id == id; });
					return found != list.end() ? &*found : nullptr;
				};

				std::vector<DeviceInfo> devices;
				const Endpoint* output = find(outputs, defaultId(*enumerator.Get(), eRender));
				const Endpoint* input = find(inputs, defaultId(*enumerator.Get(), eCapture));
				if (output != nullptr)
				{
					const std::vector<double> both =
						input != nullptr ? intersect(rates[output->id], rates[input->id]) : rates[output->id];
					if (!both.empty())
						devices.push_back(makeInfo("default", "Default output and input", *output, input, both));
				}
				for (const Endpoint& out : outputs)
					if (!rates[out.id].empty())
						devices.push_back(makeInfo(out.id, out.name, out, nullptr, rates[out.id]));
				for (const Endpoint& out : outputs)
					for (const Endpoint& in : inputs)
						if (std::vector<double> both = intersect(rates[out.id], rates[in.id]); !both.empty())
							devices.push_back(
								makeInfo(out.id + "|" + in.id, out.name + " + " + in.name, out, &in, std::move(both)));
				return devices;
			}

			std::unique_ptr<Device> open(const DeviceConfig& config) override
			{
				const MtaUsage mta;
				const ComPtr<IMMDeviceEnumerator> enumerator = makeEnumerator();
				std::string outputId;
				std::string inputId;
				const bool followsDefault = config.device.empty() || config.device == "default";
				if (followsDefault)
				{
					outputId = defaultId(*enumerator.Get(), eRender);
					inputId = defaultId(*enumerator.Get(), eCapture);
					if (outputId.empty())
						throw DeviceError("there is no audio output");
				}
				else
				{
					const size_t bar = config.device.find('|');
					outputId = config.device.substr(0, bar);
					if (bar != std::string::npos)
						inputId = config.device.substr(bar + 1);
				}
				const auto endpoint = [&enumerator, &config](const std::string& id)
				{
					ComPtr<IMMDevice> device;
					if (FAILED(enumerator->GetDevice(wide(id).c_str(), &device)))
						throw DeviceError("no device " + config.device);
					return describe(device);
				};
				const Endpoint output = endpoint(outputId);
				const std::optional<Endpoint> input =
					inputId.empty() ? std::nullopt : std::optional<Endpoint>(endpoint(inputId));

				std::vector<double> rates = sampleRates(output, m_exclusive);
				if (input)
					rates = intersect(rates, sampleRates(*input, m_exclusive));
				const auto has = [&rates](double rate)
				{ return std::find(rates.begin(), rates.end(), rate) != rates.end(); };
				double rate = config.sampleRate;
				if (rate == 0.0)
					rate = has(output.mixRate) || rates.empty() ? output.mixRate : rates.front();
				if (!has(rate))
					throw DeviceError(output.name + " doesn't run at " + std::to_string(std::lround(rate)) + " Hz");
				if (config.blockSize > 8192)
					throw DeviceError("WASAPI blocks have at most 8192 frames");
				if (!config.clockSource.empty())
					throw DeviceError("WASAPI devices have no clock sources");

				std::string id = "default";
				std::string name = "Default output and input";
				if (!followsDefault)
				{
					id = config.device;
					name = input ? output.name + " + " + input->name : output.name;
				}
				DeviceInfo info = makeInfo(id, name, output, input ? &*input : nullptr, rates);
				return std::make_unique<WasapiDevice>(std::move(info), output, input ? &*input : nullptr, rate,
					config.blockSize, m_exclusive, followsDefault);
			}

		private:
			bool m_exclusive;
		};
	}

	std::unique_ptr<Backend> makeWasapiBackend(bool exclusive)
	{
		return std::make_unique<WasapiBackend>(exclusive);
	}
}
