// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/hosttime.h"

#include <jack/jack.h>
#include <jack/midiport.h>

#include <atomic>
#include <string>

namespace gnubase
{
	// How many of GNUbase's JACK audio devices are open in this process. JACK MIDI has ports only while one is (#42):
	// where libjack is PipeWire's, a server always answers, and its MIDI ports are the ALSA backend's too.
	inline std::atomic<int> g_jackAudioDevicesOpen{0};

	// libjack's functions, for the JACK audio and MIDI backends. libjack is loaded when it is first used, so GNUbase
	// runs where it isn't installed, once per library and never unloaded: libjack keeps threads and globals of its
	// own. Its messages go to the debug log, not to stderr: it explains at length that no server runs.
	class JackLibrary
	{
	public:
		// The library at this path, e.g. "libjack.so.0" or PipeWire's, loaded once. Throws audio::DeviceError if it
		// can't be loaded or lacks a function.
		static const JackLibrary& get(const std::string& path);

		explicit JackLibrary(const std::string& path);
		JackLibrary(const JackLibrary&) = delete;
		JackLibrary& operator=(const JackLibrary&) = delete;

		decltype(&jack_activate) activate = nullptr;
		decltype(&jack_client_close) clientClose = nullptr;
		decltype(&jack_client_open) clientOpen = nullptr;
		decltype(&jack_connect) connect = nullptr;
		decltype(&jack_deactivate) deactivate = nullptr;
		decltype(&jack_free) free = nullptr;
		decltype(&jack_get_buffer_size) getBufferSize = nullptr;
		decltype(&jack_get_client_name) getClientName = nullptr;
		decltype(&jack_get_ports) getPorts = nullptr;
		decltype(&jack_get_sample_rate) getSampleRate = nullptr;
		decltype(&jack_midi_clear_buffer) midiClearBuffer = nullptr;
		decltype(&jack_midi_event_get) midiEventGet = nullptr;
		decltype(&jack_midi_event_write) midiEventWrite = nullptr;
		decltype(&jack_midi_get_event_count) midiGetEventCount = nullptr;
		decltype(&jack_on_info_shutdown) onInfoShutdown = nullptr;
		decltype(&jack_port_by_name) portByName = nullptr;
		decltype(&jack_port_get_buffer) portGetBuffer = nullptr;
		decltype(&jack_port_get_latency_range) portGetLatencyRange = nullptr;
		decltype(&jack_port_name) portName = nullptr;
		decltype(&jack_port_register) portRegister = nullptr;
		decltype(&jack_port_unregister) portUnregister = nullptr;
		decltype(&jack_set_buffer_size) setBufferSize = nullptr;
		decltype(&jack_set_error_function) setErrorFunction = nullptr;
		decltype(&jack_set_info_function) setInfoFunction = nullptr;
		decltype(&jack_set_latency_callback) setLatencyCallback = nullptr;
		decltype(&jack_set_port_registration_callback) setPortRegistrationCallback = nullptr;
		decltype(&jack_set_process_callback) setProcessCallback = nullptr;
		decltype(&jack_set_sample_rate_callback) setSampleRateCallback = nullptr;
		decltype(&jack_set_xrun_callback) setXrunCallback = nullptr;

		// Null where the library lacks them.
		decltype(&jack_get_cycle_times) getCycleTimes = nullptr;
		decltype(&jack_get_time) getTime = nullptr;

		// True for PipeWire's JACK layer (pipewire-jack), which says so in its version string.
		bool pipeWire = false;

		// The current cycle: when it started, on the host clock, and how long it lasts in nanoseconds.
		struct Cycle
		{
			HostTime start = 0;
			HostTime period = 0;
		};

		// The current cycle as the server's filtered clock has it (jack_get_cycle_times), free of the jitter of when
		// the process callback happens to run; or the time now and a period of 0 where the library doesn't tell
		// (#402). On the process thread, where it neither allocates nor locks.
		Cycle cycle(jack_client_t* client) const noexcept;

	private:
		template <typename Function> void load(Function& function, const char* name);
		template <typename Function> void loadOptional(Function& function, const char* name);

		void* m_handle = nullptr;
	};
}
