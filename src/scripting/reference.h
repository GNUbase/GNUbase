// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"

#include <string>

namespace gnubase::scripting
{
	// The Lua API reference in Markdown, generated from the engine's command registry: docs/lua.md.
	std::string reference(const engine::Engine& engine);
}
