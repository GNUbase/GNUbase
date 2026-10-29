// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The VST3 preset file (.vstpreset): the state of a VST3 plugin as the adapter saves and loads it, so a project's
// plugin state and a preset file are the same bytes (docs/plugin-api.md, "State and presets").
//
// Layout, little-endian: the header "VST3", version 1 (int32), the class id as 32 ASCII hex digits and the offset of
// the chunk list (int64); then the chunks' data; then the chunk list: "List", the entry count (int32) and per entry the
// chunk id (4 ASCII characters: "Comp" component state, "Cont" controller state), its offset and its size (int64).
namespace gnubase::pluginhost::vst3
{
	struct PresetFile
	{
		std::string classId; // 32 hex digits
		std::vector<uint8_t> component;
		std::vector<uint8_t> controller;
		bool hasController = false;
	};

	std::vector<uint8_t> writePresetFile(const PresetFile& preset);
	// False if the bytes are no preset file or have no component state.
	bool readPresetFile(const uint8_t* data, size_t size, PresetFile& preset);
}
