// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/cursors.h"

#include "engine/schema.h"
#include "platform/log.h"

#include <lunasvg.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		std::optional<std::string> readText(const std::filesystem::path& file)
		{
			std::ifstream in(file, std::ios::binary);
			if (!in)
				return std::nullopt;
			return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		}
	}

	Cursors::Cursors(std::filesystem::path folder) : m_folder(std::move(folder))
	{
		if (m_folder.empty())
			return;
		const std::filesystem::path file = m_folder / "cursors.json";
		const std::optional<std::string> text = readText(file);
		const engine::Json manifest = text ? engine::Json::parse(*text, nullptr, false) : engine::Json();
		if (!manifest.is_object())
		{
			logging::warning("The mouse pointers' manifest {} can't be read", file.u8string());
			return;
		}
		for (const auto& [name, cursor] : manifest.items())
		{
			const engine::Json* hotspot =
				cursor.is_object() && cursor.contains("hotspot") ? &cursor["hotspot"] : nullptr;
			if (hotspot == nullptr || !hotspot->is_array() || hotspot->size() != 2 || !(*hotspot)[0].is_number() ||
				!(*hotspot)[1].is_number() || !std::isfinite((*hotspot)[0].get<double>()) ||
				!std::isfinite((*hotspot)[1].get<double>()))
			{
				logging::warning("The mouse pointer {} in {} has no hotspot [x, y]", name, file.u8string());
				continue;
			}
			// Inside the largest picture; load() clamps it to the picture's own size.
			const auto inside = [](const engine::Json& value)
			{ return static_cast<float>(std::clamp(value.get<double>(), 0.0, double{g_maxCursorSize})); };
			m_hotspots[name] = {inside((*hotspot)[0]), inside((*hotspot)[1])};
		}
	}

	const Cursor& Cursors::get(const std::string& name)
	{
		if (const auto found = m_loaded.find(name); found != m_loaded.end())
			return found->second;
		Cursor cursor{name, {}};
		if (has(name))
		{
			if (std::optional<Cursor> loaded = load(name))
				cursor = std::move(*loaded);
			else
				cursor.name.clear(); // the OS's arrow
		}
		return m_loaded.emplace(name, std::move(cursor)).first->second;
	}

	std::optional<Cursor> Cursors::load(const std::string& name) const
	{
		const std::filesystem::path file = m_folder / std::filesystem::u8path(name + ".svg");
		const std::optional<std::string> text = readText(file);
		const std::unique_ptr<lunasvg::Document> document = text ? lunasvg::Document::loadFromData(*text) : nullptr;
		if (document == nullptr || !(document->width() > 0.0F) || !(document->height() > 0.0F) ||
			document->width() > g_maxCursorSize || document->height() > g_maxCursorSize)
		{
			logging::warning("The mouse pointer {} can't be read", file.u8string());
			return std::nullopt;
		}
		const std::array<float, 2>& hotspot = m_hotspots.at(name);
		Cursor cursor{name, {}};
		for (const float scale : g_cursorScales)
		{
			CursorImage image;
			image.scale = scale;
			image.width = std::max(1, static_cast<int>(std::lround(document->width() * scale)));
			image.height = std::max(1, static_cast<int>(std::lround(document->height() * scale)));
			lunasvg::Bitmap bitmap = document->renderToBitmap(image.width, image.height);
			if (bitmap.isNull())
			{
				logging::warning("The mouse pointer {} can't be drawn", file.u8string());
				return std::nullopt;
			}
			bitmap.convertToRGBA();
			const auto row = static_cast<size_t>(image.width) * 4;
			image.pixels.resize(row * static_cast<size_t>(image.height));
			for (int y = 0; y < image.height; ++y)
				std::memcpy(image.pixels.data() + (row * static_cast<size_t>(y)),
					bitmap.data() + (static_cast<ptrdiff_t>(bitmap.stride()) * y), row);
			image.hotspot = {std::clamp(static_cast<int>(std::floor(hotspot[0] * scale)), 0, image.width - 1),
				std::clamp(static_cast<int>(std::floor(hotspot[1] * scale)), 0, image.height - 1)};
			cursor.images.push_back(std::move(image));
		}
		return cursor;
	}
}
