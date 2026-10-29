// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginhost/vst3/presetfile.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace gnubase::pluginhost::vst3
{
	namespace
	{
		constexpr size_t g_headerSize = 48;
		constexpr size_t g_classIdSize = 32;
		constexpr size_t g_entrySize = 20;

		void put(std::vector<uint8_t>& out, uint64_t value, size_t bytes)
		{
			for (size_t i = 0; i < bytes; ++i)
				out.push_back(static_cast<uint8_t>(value >> (8 * i)));
		}

		void putId(std::vector<uint8_t>& out, const char* id)
		{
			out.insert(out.end(), id, id + 4);
		}

		uint64_t get(const uint8_t* data, size_t bytes)
		{
			uint64_t value = 0;
			for (size_t i = 0; i < bytes; ++i)
				value |= uint64_t{data[i]} << (8 * i);
			return value;
		}
	}

	std::vector<uint8_t> writePresetFile(const PresetFile& preset)
	{
		std::vector<uint8_t> out;
		putId(out, "VST3");
		put(out, 1, 4);
		std::array<char, g_classIdSize> classId{};
		std::memcpy(classId.data(), preset.classId.data(), std::min(preset.classId.size(), classId.size()));
		out.insert(out.end(), classId.begin(), classId.end());
		const size_t listOffsetAt = out.size();
		put(out, 0, 8);

		struct Entry
		{
			const char* id;
			size_t offset;
			size_t size;
		};
		std::vector<Entry> entries;
		entries.push_back({"Comp", out.size(), preset.component.size()});
		out.insert(out.end(), preset.component.begin(), preset.component.end());
		if (preset.hasController)
		{
			entries.push_back({"Cont", out.size(), preset.controller.size()});
			out.insert(out.end(), preset.controller.begin(), preset.controller.end());
		}

		const size_t listOffset = out.size();
		putId(out, "List");
		put(out, entries.size(), 4);
		for (const Entry& entry : entries)
		{
			putId(out, entry.id);
			put(out, entry.offset, 8);
			put(out, entry.size, 8);
		}
		for (size_t i = 0; i < 8; ++i)
			out[listOffsetAt + i] = static_cast<uint8_t>(listOffset >> (8 * i));
		return out;
	}

	bool readPresetFile(const uint8_t* data, size_t size, PresetFile& preset)
	{
		if (size < g_headerSize || std::memcmp(data, "VST3", 4) != 0)
			return false;
		preset = PresetFile{};
		preset.classId.assign(reinterpret_cast<const char*>(data + 8), g_classIdSize);
		const uint64_t listOffset = get(data + 40, 8);
		if (listOffset > size || size - listOffset < 8 || std::memcmp(data + listOffset, "List", 4) != 0)
			return false;
		const uint64_t count = get(data + listOffset + 4, 4);
		if ((size - listOffset - 8) / g_entrySize < count)
			return false;
		bool hasComponent = false;
		for (uint64_t i = 0; i < count; ++i)
		{
			const uint8_t* entry = data + listOffset + 8 + (i * g_entrySize);
			const uint64_t offset = get(entry + 4, 8);
			const uint64_t length = get(entry + 12, 8);
			if (offset > size || length > size - offset)
				return false;
			const uint8_t* begin = data + offset;
			if (std::memcmp(entry, "Comp", 4) == 0)
			{
				preset.component.assign(begin, begin + length);
				hasComponent = true;
			}
			else if (std::memcmp(entry, "Cont", 4) == 0)
			{
				preset.controller.assign(begin, begin + length);
				preset.hasController = true;
			}
		}
		return hasComponent;
	}
}
