// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "plugin/plugin.h"
#include "pluginhost/formats.h"

#include <memory>
#include <string_view>

// The plugin format "test": GNUbase's test plugins (tests/plugins) adapted to the plugin API, linked into the tests'
// plugin host (gnubase-testpluginhost) and into the test process, which compares hosted plugins with in-process ones.
// The shipped plugin host has no test plugins. Ids are "test:<name>": test:gain, test:synth, test:latency,
// test:paramstress, test:crash, test:hang, test:stateblob, with the parameters of tests/plugins/README.md. The
// format has no files: it ignores plugin paths.
//
// Like a format adapter, it passes on what a plugin asks of its host: a restart request, a changed latency when the
// plugin is activated, and a values rescan after a state was loaded.
namespace gnubase::testformat
{
	std::unique_ptr<pluginhost::Format> makeTestFormat();

	// A test plugin by id, not initialized, or nullptr.
	std::unique_ptr<plugin::Plugin> createTestPlugin(std::string_view id, plugin::Host& host);
}
