// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/jacklibrary.h"

#include "platform/audio/device.h"
#include "platform/log.h"

#include <dlfcn.h>

#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace gnubase
{
	template <typename Function> void JackLibrary::load(Function& function, const char* name)
	{
		function = reinterpret_cast<Function>(dlsym(m_handle, name));
		if (function == nullptr)
			throw audio::DeviceError(std::string("JACK: libjack has no ") + name);
	}

	template <typename Function> void JackLibrary::loadOptional(Function& function, const char* name)
	{
		function = reinterpret_cast<Function>(dlsym(m_handle, name));
	}

	JackLibrary::JackLibrary(const std::string& path)
	{
		m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (m_handle == nullptr)
		{
			const char* error = dlerror();
			throw audio::DeviceError("JACK: cannot load " + path + " (" + (error != nullptr ? error : "") + ")");
		}
		try
		{
			load(activate, "jack_activate");
			load(clientClose, "jack_client_close");
			load(clientOpen, "jack_client_open");
			load(connect, "jack_connect");
			load(deactivate, "jack_deactivate");
			load(free, "jack_free");
			load(getBufferSize, "jack_get_buffer_size");
			load(getClientName, "jack_get_client_name");
			load(getPorts, "jack_get_ports");
			load(getSampleRate, "jack_get_sample_rate");
			load(midiClearBuffer, "jack_midi_clear_buffer");
			load(midiEventGet, "jack_midi_event_get");
			load(midiEventWrite, "jack_midi_event_write");
			load(midiGetEventCount, "jack_midi_get_event_count");
			load(onInfoShutdown, "jack_on_info_shutdown");
			load(portByName, "jack_port_by_name");
			load(portGetBuffer, "jack_port_get_buffer");
			load(portGetLatencyRange, "jack_port_get_latency_range");
			load(portName, "jack_port_name");
			load(portRegister, "jack_port_register");
			load(portUnregister, "jack_port_unregister");
			load(setBufferSize, "jack_set_buffer_size");
			load(setErrorFunction, "jack_set_error_function");
			load(setInfoFunction, "jack_set_info_function");
			load(setLatencyCallback, "jack_set_latency_callback");
			load(setPortRegistrationCallback, "jack_set_port_registration_callback");
			load(setProcessCallback, "jack_set_process_callback");
			load(setSampleRateCallback, "jack_set_sample_rate_callback");
			load(setXrunCallback, "jack_set_xrun_callback");
			loadOptional(getCycleTimes, "jack_get_cycle_times");
			loadOptional(getTime, "jack_get_time");
		}
		catch (...)
		{
			dlclose(m_handle);
			throw;
		}
		setErrorFunction([](const char* message) { logging::debug("libjack: {}", message); });
		setInfoFunction([](const char* message) { logging::debug("libjack: {}", message); });
		decltype(&jack_get_version_string) version = nullptr;
		loadOptional(version, "jack_get_version_string");
		pipeWire = version != nullptr && std::strstr(version(), "PipeWire") != nullptr;
	}

	JackLibrary::Cycle JackLibrary::cycle(jack_client_t* client) const noexcept
	{
		const HostTime now = hostTime();
		jack_nframes_t frames = 0;
		jack_time_t current = 0;
		jack_time_t next = 0;
		float period = 0.0f;
		if (getCycleTimes == nullptr || getTime == nullptr ||
			getCycleTimes(client, &frames, &current, &next, &period) != 0)
			return {now, 0};
		// The server's clock, read right after the host's: their difference converts one to the other.
		return toHostClock(now, getTime(), current, period);
	}

	JackLibrary::Cycle JackLibrary::toHostClock(
		HostTime hostNow, jack_time_t jackNow, jack_time_t start, float period) noexcept
	{
		const int64_t ago = static_cast<int64_t>(jackNow) - static_cast<int64_t>(start); // negative: ahead
		if (period <= 0.0f || static_cast<float>(-ago) > period || ago > 1000000)
			return {hostNow, 0};
		return {hostNow - (ago * 1000), static_cast<HostTime>(std::lround(period * 1e3))};
	}

	const JackLibrary& JackLibrary::get(const std::string& path)
	{
		static std::mutex mutex;
		static std::map<std::string, std::unique_ptr<JackLibrary>> libraries;
		const std::scoped_lock lock(mutex);
		std::unique_ptr<JackLibrary>& library = libraries[path];
		if (!library)
			library = std::make_unique<JackLibrary>(path);
		return *library;
	}

}
