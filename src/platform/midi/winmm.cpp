// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/midi/winmm.h"

#include "platform/log.h"
#include "platform/winutf8.h"

#include <cfgmgr32.h>
#include <mmeapi.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gnubase::midi
{
	namespace
	{
		// A port as WinMM lists it now: WinMM's index changes when devices come and go, the id doesn't.
		struct Listed
		{
			std::string id;
			std::string name;
			UINT index = 0;
		};

		std::vector<Listed> list(bool input)
		{
			std::vector<Listed> ports;
			std::map<std::string, int> named; // ports so far per name
			const UINT count = input ? midiInGetNumDevs() : midiOutGetNumDevs();
			for (UINT i = 0; i < count; ++i)
			{
				std::string name;
				if (input)
				{
					MIDIINCAPSW caps{};
					if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
						continue;
					name = utf8(caps.szPname);
				}
				else
				{
					MIDIOUTCAPSW caps{};
					if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
						continue;
					name = utf8(caps.szPname);
				}
				const int number = ++named[name];
				ports.push_back({number == 1 ? name : name + " #" + std::to_string(number), name, i});
			}
			return ports;
		}

		UINT indexOf(const std::string& id, bool input)
		{
			for (const Listed& port : list(input))
				if (port.id == id)
					return port.index;
			throw std::runtime_error(std::string(input ? "no MIDI input " : "no MIDI output ") + id);
		}

		std::runtime_error failure(const std::string& what, MMRESULT result, bool input)
		{
			std::array<wchar_t, MAXERRORLENGTH> text{};
			if (input)
				midiInGetErrorTextW(result, text.data(), static_cast<UINT>(text.size()));
			else
				midiOutGetErrorTextW(result, text.data(), static_cast<UINT>(text.size()));
			return std::runtime_error(what + ": " + utf8(text.data()));
		}

		class WinmmInput : public Input
		{
		public:
			WinmmInput(UINT index, Receiver receiver) : m_receiver(std::move(receiver))
			{
				MMRESULT result = midiInOpen(&m_handle, index, reinterpret_cast<DWORD_PTR>(&WinmmInput::received),
					reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
				if (result != MMSYSERR_NOERROR)
					throw failure("cannot open the MIDI input", result, true);
				result = midiInStart(m_handle);
				if (result != MMSYSERR_NOERROR)
				{
					midiInClose(m_handle);
					throw failure("cannot start the MIDI input", result, true);
				}
			}

			~WinmmInput() override
			{
				midiInStop(m_handle);
				midiInReset(m_handle);
				midiInClose(m_handle);
			}

			WinmmInput(const WinmmInput&) = delete;
			WinmmInput& operator=(const WinmmInput&) = delete;

		private:
			// WinMM's thread. SysEx (MIM_LONGDATA) isn't carried.
			static void CALLBACK received(
				HMIDIIN /*handle*/, UINT message, DWORD_PTR instance, DWORD_PTR packed, DWORD_PTR /*milliseconds*/)
			{
				if (message != MIM_DATA)
					return;
				const HostTime time = hostTime(); // finer than WinMM's milliseconds
				audio::MidiMessage midi;
				midi.bytes = {static_cast<uint8_t>(packed), static_cast<uint8_t>(packed >> 8U),
					static_cast<uint8_t>(packed >> 16U)};
				const int size = messageSize(midi.bytes[0]);
				if (size == 0)
					return;
				midi.size = static_cast<uint8_t>(size);
				for (size_t i = midi.size; i < midi.bytes.size(); ++i)
					midi.bytes[i] = 0;
				// NOLINTNEXTLINE(performance-no-int-to-ptr): WinMM hands the instance back as an integer
				reinterpret_cast<WinmmInput*>(instance)->m_receiver(midi, time);
			}

			HMIDIIN m_handle = nullptr;
			Receiver m_receiver;
		};

		// WinMM sends at once; a thread sends what is scheduled for later.
		class WinmmOutput : public Output
		{
		public:
			explicit WinmmOutput(UINT index)
			{
				const MMRESULT result = midiOutOpen(&m_handle, index, 0, 0, CALLBACK_NULL);
				if (result != MMSYSERR_NOERROR)
					throw failure("cannot open the MIDI output", result, false);
				m_thread = std::thread([this] { run(); });
			}

			~WinmmOutput() override
			{
				{
					const std::scoped_lock lock(m_mutex);
					m_stopping = true;
				}
				m_wake.notify_one();
				m_thread.join();
				midiOutReset(m_handle);
				midiOutClose(m_handle);
			}

			WinmmOutput(const WinmmOutput&) = delete;
			WinmmOutput& operator=(const WinmmOutput&) = delete;

			void send(const audio::MidiMessage& message, HostTime time) override
			{
				{
					const std::scoped_lock lock(m_mutex);
					if (time <= hostTime())
					{
						sendNow(message);
						return;
					}
					m_scheduled.emplace(time, message);
				}
				m_wake.notify_one();
			}

		private:
			// With m_mutex held.
			void sendNow(const audio::MidiMessage& message)
			{
				const DWORD packed =
					message.bytes[0] | (DWORD{message.bytes[1]} << 8U) | (DWORD{message.bytes[2]} << 16U);
				midiOutShortMsg(m_handle, packed);
			}

			void run()
			{
				std::unique_lock lock(m_mutex);
				while (!m_stopping)
				{
					if (m_scheduled.empty())
					{
						m_wake.wait(lock);
						continue;
					}
					const auto next = m_scheduled.begin();
					const HostTime now = hostTime();
					if (next->first > now)
					{
						m_wake.wait_for(lock, std::chrono::nanoseconds(next->first - now));
						continue;
					}
					sendNow(next->second);
					m_scheduled.erase(next);
				}
			}

			HMIDIOUT m_handle = nullptr;
			std::mutex m_mutex; // guards everything below and the handle's use
			std::condition_variable m_wake;
			std::multimap<HostTime, audio::MidiMessage> m_scheduled;
			bool m_stopping = false;
			std::thread m_thread;
		};

		class WinmmBackend : public Backend
		{
		public:
			WinmmBackend() = default;

			~WinmmBackend() override
			{
				if (m_notification != nullptr)
					CM_Unregister_Notification(m_notification);
			}

			WinmmBackend(const WinmmBackend&) = delete;
			WinmmBackend& operator=(const WinmmBackend&) = delete;

			std::string name() const override { return "winmm"; }

			std::vector<PortInfo> ports() override
			{
				watch();
				std::vector<PortInfo> ports;
				for (const bool input : {true, false})
					for (const Listed& port : list(input))
						ports.push_back({port.id, port.name, input, false});
				return ports;
			}

			std::unique_ptr<Input> openInput(const std::string& id, Receiver receiver) override
			{
				return std::make_unique<WinmmInput>(indexOf(id, true), std::move(receiver));
			}

			std::unique_ptr<Output> openOutput(const std::string& id) override
			{
				return std::make_unique<WinmmOutput>(indexOf(id, false));
			}

		private:
			// From the first listing on, any device interface that comes or goes may be a MIDI port.
			void watch()
			{
				if (m_notification != nullptr)
					return;
				CM_NOTIFY_FILTER filter{};
				filter.cbSize = sizeof(filter);
				filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
				filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
				if (CM_Register_Notification(&filter, this, &WinmmBackend::changed, &m_notification) != CR_SUCCESS)
				{
					m_notification = nullptr;
					logging::warning("WinMM won't notice MIDI devices plugged in or out");
				}
			}

			static DWORD CALLBACK changed(HCMNOTIFICATION /*notification*/, PVOID context, CM_NOTIFY_ACTION action,
				PCM_NOTIFY_EVENT_DATA /*data*/, DWORD /*size*/)
			{
				if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ||
					action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
					static_cast<WinmmBackend*>(context)->portsChanged();
				return ERROR_SUCCESS;
			}

			HCMNOTIFICATION m_notification = nullptr;
		};
	}

	std::unique_ptr<Backend> makeWinmmBackend()
	{
		return std::make_unique<WinmmBackend>();
	}
}
