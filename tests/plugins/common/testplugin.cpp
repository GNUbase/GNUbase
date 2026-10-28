// SPDX-License-Identifier: GPL-3.0-or-later
#include "testplugin.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace gnubase::testplugins
{
	namespace
	{
		constexpr uint32_t g_stateMagic = 0x50544247; // "GBTP"

		template <typename T> void append(std::vector<uint8_t>& bytes, T value)
		{
			const size_t at = bytes.size();
			bytes.resize(at + sizeof(T));
			std::memcpy(bytes.data() + at, &value, sizeof(T));
		}

		template <typename T> bool read(const uint8_t* data, size_t size, size_t& at, T& value)
		{
			if (size - at < sizeof(T))
				return false;
			std::memcpy(&value, data + at, sizeof(T));
			at += sizeof(T);
			return true;
		}
	}

	TestPlugin::TestPlugin(std::vector<ParamInfo> params) : m_params(std::move(params)), m_values(m_params.size())
	{
		for (size_t i = 0; i < m_params.size(); ++i)
			m_values[i].store(m_params[i].defaultValue);
	}

	const ParamInfo* TestPlugin::findParam(uint32_t id) const
	{
		const auto it = std::find_if(m_params.begin(), m_params.end(), [id](const ParamInfo& p) { return p.id == id; });
		return it == m_params.end() ? nullptr : &*it;
	}

	bool TestPlugin::paramValue(uint32_t id, double& value) const
	{
		const ParamInfo* info = findParam(id);
		if (info == nullptr)
			return false;
		value = m_values[static_cast<size_t>(info - m_params.data())].load();
		return true;
	}

	void TestPlugin::setParam(uint32_t id, double value)
	{
		const ParamInfo* info = findParam(id);
		if (info == nullptr || std::isnan(value))
			return;
		value = std::clamp(value, info->minValue, info->maxValue);
		if (info->stepped)
			value = std::round(value);
		m_values[static_cast<size_t>(info - m_params.data())].store(value);
		paramChanged(id, value);
	}

	bool TestPlugin::valueToText(uint32_t id, double value, char* text, size_t size) const
	{
		const ParamInfo* info = findParam(id);
		if (info == nullptr || text == nullptr || size == 0)
			return false;
		int written = 0;
		if (info->valueNames != nullptr)
		{
			const double index = std::round(std::clamp(value, info->minValue, info->maxValue) - info->minValue);
			written = std::snprintf(text, size, "%s", info->valueNames[static_cast<size_t>(index)]);
		}
		else
		{
			const char* format = info->stepped ? "%.0f%s%s" : "%.3f%s%s";
			written = std::snprintf(text, size, format, value, *info->unit != 0 ? " " : "", info->unit);
		}
		return written >= 0;
	}

	bool TestPlugin::textToValue(uint32_t id, const char* text, double& value) const
	{
		const ParamInfo* info = findParam(id);
		if (info == nullptr || text == nullptr)
			return false;
		if (info->valueNames != nullptr)
		{
			const auto steps = static_cast<size_t>(std::round(info->maxValue - info->minValue)) + 1;
			for (size_t i = 0; i < steps; ++i)
				if (std::strcmp(text, info->valueNames[i]) == 0)
				{
					value = info->minValue + static_cast<double>(i);
					return true;
				}
			return false;
		}
		char* end = nullptr;
		const double parsed = std::strtod(text, &end);
		if (end == text)
			return false;
		value = parsed;
		return true;
	}

	void TestPlugin::activate(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
	}

	void TestPlugin::process(const ProcessBlock& block)
	{
		uint32_t from = 0;
		const uint32_t count = block.events != nullptr ? block.events->size() : 0;
		for (uint32_t i = 0; i < count; ++i)
		{
			Event event;
			if (!block.events->get(i, event))
				continue;
			const uint32_t time = std::min(event.time, block.frames);
			if (time > from)
			{
				render(block, from, time);
				from = time;
			}
			handleEvent(event);
		}
		if (from < block.frames)
			render(block, from, block.frames);
	}

	void TestPlugin::render(const ProcessBlock& block, uint32_t from, uint32_t to)
	{
		for (uint32_t c = 0; c < block.outputChannels; ++c)
		{
			float* out = block.outputs[c];
			if (block.inputs != nullptr && c < block.inputChannels)
				std::copy(block.inputs[c] + from, block.inputs[c] + to, out + from);
			else
				std::fill(out + from, out + to, 0.0f);
		}
	}

	void TestPlugin::handleEvent(const Event& event)
	{
		if (event.type == EventType::ParamValue)
			setParam(event.paramId, event.value);
	}

	std::vector<uint8_t> TestPlugin::saveState() const
	{
		std::vector<uint8_t> bytes;
		append(bytes, g_stateMagic);
		append(bytes, static_cast<uint32_t>(m_params.size()));
		for (size_t i = 0; i < m_params.size(); ++i)
		{
			append(bytes, m_params[i].id);
			append(bytes, m_values[i].load());
		}
		return bytes;
	}

	bool TestPlugin::loadState(const uint8_t* data, size_t size)
	{
		size_t at = 0;
		uint32_t magic = 0;
		uint32_t count = 0;
		if (!read(data, size, at, magic) || magic != g_stateMagic || !read(data, size, at, count))
			return false;
		if ((size - at) != count * (sizeof(uint32_t) + sizeof(double)))
			return false;
		for (uint32_t i = 0; i < count; ++i)
		{
			uint32_t id = 0;
			double value = 0.0;
			read(data, size, at, id);
			read(data, size, at, value);
			setParam(id, value); // unknown ids are skipped
		}
		return true;
	}

	void crashNow()
	{
#ifdef _WIN32
		RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
		std::raise(SIGSEGV);
#endif
		std::abort(); // only if a handler returned
	}

	void hangNow()
	{
		for (;;)
			std::this_thread::sleep_for(std::chrono::hours(1));
	}
}
