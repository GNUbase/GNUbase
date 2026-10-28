// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "plugin/plugin.h"

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The format adapters of the plugin host process: the only code that talks to a plugin format (docs/plugin-api.md).
namespace gnubase::pluginhost
{
	// Adapts one plugin format to the GNUbase plugin API. An adapter loads plugin files of its format and returns their
	// factory, whose plugins implement plugin::Plugin on top of the format. The CLAP (#67) and VST3 (#68) adapters,
	// and later AU, LV2 and FST, plug in here; for now the host has the test format of built-in test plugins.
	class Format
	{
	public:
		Format() = default;
		virtual ~Format() = default;
		Format(const Format&) = delete;
		Format& operator=(const Format&) = delete;

		// The format's name, which is also the prefix of its plugins' ids: "test" for "test:gain", "clap", "vst3".
		virtual std::string_view name() const = 0;

		// [main] The plugins of a plugin file, e.g. a .clap bundle or a .vst3 module. Formats without files ignore the
		// path. Throws std::runtime_error if the file can't be loaded.
		virtual std::unique_ptr<plugin::Factory> load(const std::string& path) = 0;
	};

	// The formats a host process has, and the plugin files it loaded: a file stays loaded while the process runs, so
	// its plugins' code stays valid.
	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	class Formats
	{
	public:
		void add(std::unique_ptr<Format> format);

		// [main] A new plugin from a file, by its id: the part before the first ':' names the format. Not initialized
		// yet. Throws std::runtime_error for an unknown format, file or plugin.
		std::unique_ptr<plugin::Plugin> create(const std::string& id, const std::string& path, plugin::Host& host);

	private:
		std::vector<std::unique_ptr<Format>> m_formats;
		std::map<std::pair<std::string, std::string>, std::unique_ptr<plugin::Factory>> m_factories; // format, path
	};

	// The formats built into this host.
	Formats builtinFormats();
}
