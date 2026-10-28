// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginhost/formats.h"

#include "pluginhost/testplugins.h"
#include "pluginhost/vst3/vst3format.h"

#include <stdexcept>

namespace gnubase::pluginhost
{
	void Formats::add(std::unique_ptr<Format> format)
	{
		m_formats.push_back(std::move(format));
	}

	std::unique_ptr<plugin::Plugin> Formats::create(const std::string& id, const std::string& path, plugin::Host& host)
	{
		const std::string_view name = std::string_view(id).substr(0, id.find(':'));
		Format* format = nullptr;
		for (const std::unique_ptr<Format>& candidate : m_formats)
		{
			if (candidate->name() == name)
				format = candidate.get();
		}
		if (format == nullptr || name.size() == id.size())
			throw std::runtime_error("no plugin format for " + id);

		std::unique_ptr<plugin::Factory>& factory = m_factories[{std::string(name), path}];
		if (!factory)
			factory = format->load(path);
		std::unique_ptr<plugin::Plugin> plugin = factory ? factory->create(id, host) : nullptr;
		if (!plugin)
			throw std::runtime_error("no plugin " + id + (path.empty() ? std::string() : " in " + path));
		return plugin;
	}

	Formats builtinFormats()
	{
		Formats formats;
		formats.add(makeTestFormat());
		formats.add(makeVst3Format());
		return formats;
	}
}
