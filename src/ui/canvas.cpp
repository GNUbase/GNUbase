// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/canvas.h"

#include "engine/audio/automation.h"
#include "engine/audio/fades.h"
#include "ui/rmlui.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/RenderManager.h>
#include <RmlUi/Core/StyleSheetSpecification.h>
#include <RmlUi/Core/StyleSheetTypes.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using Colour = Rml::ColourbPremultiplied;

		// A rectangle from (x0, y0) to (x1, y1).
		void quad(Rml::Mesh& mesh, float x0, float y0, float x1, float y1, Colour colour)
		{
			const auto base = static_cast<int>(mesh.vertices.size());
			mesh.vertices.push_back({{x0, y0}, colour, {}});
			mesh.vertices.push_back({{x1, y0}, colour, {}});
			mesh.vertices.push_back({{x1, y1}, colour, {}});
			mesh.vertices.push_back({{x0, y1}, colour, {}});
			mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
		}

		// A rectangle filled with body inside an outline of the given width, or, too small for both, the outline's
		// colour only.
		void outlined(Rml::Mesh& mesh, float x0, float y0, float x1, float y1, Colour body, Colour outline, float width)
		{
			if (width <= 0.0F || x1 - x0 <= 2.0F * width || y1 - y0 <= 2.0F * width)
			{
				if (const Colour colour = width <= 0.0F ? body : outline; colour.alpha != 0)
					quad(mesh, x0, y0, x1, y1, colour);
				return;
			}
			// The outline as a ring: the outer corners, then the inner ones, two triangles per side.
			const auto base = static_cast<int>(mesh.vertices.size());
			const std::array<Rml::Vector2f, 8> corners{Rml::Vector2f{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1},
				{x0 + width, y0 + width}, {x1 - width, y0 + width}, {x1 - width, y1 - width}, {x0 + width, y1 - width}};
			for (const Rml::Vector2f& corner : corners)
				mesh.vertices.push_back({corner, outline, {}});
			for (int side = 0; side < 4; ++side)
			{
				const int next = (side + 1) % 4;
				mesh.indices.insert(mesh.indices.end(),
					{base + side, base + next, base + 4 + next, base + side, base + 4 + next, base + 4 + side});
			}
			if (body.alpha != 0) // a transparent body, as at an event opacity of 0, adds nothing
				quad(mesh, x0 + width, y0 + width, x1 - width, y1 - width, body);
		}

		// A line of the given width from a to b, with square ends, so that lines meeting at an angle leave no gap.
		void line(Rml::Mesh& mesh, Rml::Vector2<double> a, Rml::Vector2<double> b, Colour colour, double width)
		{
			const double length = std::hypot(b.x - a.x, b.y - a.y);
			if (length < 1e-9)
				return;
			const double half = width / 2.0;
			const double dx = (b.x - a.x) / length * half;
			const double dy = (b.y - a.y) / length * half;
			const auto vertex = [](double x, double y)
			{ return Rml::Vector2f(static_cast<float>(x), static_cast<float>(y)); };
			const auto base = static_cast<int>(mesh.vertices.size());
			mesh.vertices.push_back({vertex(a.x - dx - dy, a.y - dy + dx), colour, {}});
			mesh.vertices.push_back({vertex(b.x + dx - dy, b.y + dy + dx), colour, {}});
			mesh.vertices.push_back({vertex(b.x + dx + dy, b.y + dy - dx), colour, {}});
			mesh.vertices.push_back({vertex(a.x - dx + dy, a.y - dy - dx), colour, {}});
			mesh.indices.insert(mesh.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
		}

		// A number attribute; fallback if it is missing or not a finite number.
		double number(Rml::Element& element, const char* name, double fallback)
		{
			const auto value = element.GetAttribute<double>(name, fallback);
			return std::isfinite(value) ? value : fallback;
		}

		// A premultiplied colour scaled by a factor from 0 to 1, as if drawn that much more transparent.
		Colour scaled(Colour colour, double factor)
		{
			const auto part = [factor](uint8_t value)
			{ return static_cast<uint8_t>(std::lround(static_cast<double>(value) * factor)); };
			return {part(colour.red), part(colour.green), part(colour.blue), part(colour.alpha)};
		}

		// The lanes in a frame of lanes with these tops (dp, each lane's and the bottom of the last): the first whose
		// bottom is below the frame's top, and the one after the last whose top is above its bottom.
		std::pair<size_t, size_t> lanesIn(const std::vector<double>& tops, double top, double bottom, float dpRatio)
		{
			if (tops.size() < 2)
				return {0, 0};
			const auto first =
				static_cast<size_t>(std::upper_bound(tops.begin() + 1, tops.end(), top / dpRatio) - (tops.begin() + 1));
			size_t last = first;
			while (last + 1 < tops.size() && tops[last] * dpRatio < bottom)
				++last;
			return {first, last};
		}

		// The longest of the lengths, 0 for none.
		template <typename Items> double longestOf(const Items& items)
		{
			double longest = 0.0;
			for (const auto& item : items)
				longest = std::max(longest, item.length);
			return longest;
		}

		// The end of the text's first characters that fit in width pixels: 0 if none does. Cuts between UTF-8
		// characters only.
		size_t fitting(Rml::FontEngineInterface& fonts, Rml::FontFaceHandle font, const std::string& text,
			const Rml::TextShapingContext& shaping, float width)
		{
			if (static_cast<float>(fonts.GetStringWidth(font, text, shaping)) <= width)
				return text.size();
			std::vector<size_t> ends; // after each character
			for (size_t i = 1; i <= text.size(); ++i)
				if (i == text.size() || (static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U)
					ends.push_back(i);
			// The longest prefix that fits, by binary search over the characters' ends.
			size_t fits = 0;
			size_t low = 0;
			size_t high = ends.size();
			while (low < high)
			{
				const size_t middle = (low + high) / 2;
				if (static_cast<float>(fonts.GetStringWidth(font, text.substr(0, ends[middle]), shaping)) <= width)
				{
					fits = ends[middle];
					low = middle + 1;
				}
				else
				{
					high = middle;
				}
			}
			return fits;
		}
	}

	Colour brightened(Colour colour, double brightness)
	{
		const double level = std::isfinite(brightness) ? std::clamp(brightness, 0.0, 1.0) : 0.5;
		if (level <= 0.5)
			return scaled(colour, level / 0.5);
		// Toward white at the colour's own alpha: premultiplied, white is the alpha in every channel.
		const double toward = (level - 0.5) / 0.5;
		const auto part = [&colour, toward](uint8_t value)
		{
			return static_cast<uint8_t>(std::lround(static_cast<double>(value) +
				((static_cast<double>(colour.alpha) - static_cast<double>(value)) * toward)));
		};
		return {part(colour.red), part(colour.green), part(colour.blue), colour.alpha};
	}

	void registerCanvas(RmlUi& rmlui)
	{
		Rml::StyleSheetSpecification::RegisterProperty("stroke-color", "transparent", false).AddParser("color");
		Rml::StyleSheetSpecification::RegisterProperty("stroke-width", "1px", false).AddParser("length");
		Rml::StyleSheetSpecification::RegisterProperty("own-fill-opacity", "1", false).AddParser("number");
		Rml::StyleSheetSpecification::RegisterProperty("own-color-tint", "none", false)
			.AddParser("keyword", "none")
			.AddParser("color");
		Rml::StyleSheetSpecification::RegisterProperty("event-inset", "0px", false).AddParser("length");
		Rml::StyleSheetSpecification::RegisterProperty("handle-size", "6dp", false).AddParser("length");
		rmlui.registerElement<EventLanes>("event-lanes");
		rmlui.registerElement<EventNames>("event-names");
		rmlui.registerElement<PartContents>("part-contents");
		rmlui.registerElement<Waveform>("waveform");
		rmlui.registerElement<Curve>("curve");
		rmlui.registerElement<FadeLines>("fade-lines");
		rmlui.registerElement<Ruler>("ruler");
	}

	uint32_t spanColor(const std::string& text)
	{
		if (!engine::isColor(text))
			return 0;
		return (static_cast<uint32_t>(std::stoul(text.substr(1), nullptr, 16)) << 8U) | 0xffU;
	}

	float Canvas::Frame::x(double position) const
	{
		return static_cast<float>(std::round(position / unitsPerPixel) - left);
	}

	void Canvas::changed()
	{
		m_dirty = true;
		if (Rml::Context* context = GetContext())
			context->RequestNextUpdate(0);
	}

	Colour Canvas::fill()
	{
		const Rml::ComputedValues& values = GetComputedValues();
		return values.color().ToPremultiplied(values.opacity());
	}

	Colour Canvas::stroke()
	{
		const Rml::Property* colour = GetProperty("stroke-color");
		return colour == nullptr ? Colour()
								 : colour->Get<Rml::Colourb>().ToPremultiplied(GetComputedValues().opacity());
	}

	Canvas::OwnColors Canvas::ownColors()
	{
		OwnColors own;
		if (const Rml::Property* property = GetProperty("own-fill-opacity"))
		{
			const auto opacity = property->Get<float>();
			own.fillOpacity = std::isfinite(opacity) ? std::clamp(opacity, 0.0F, 1.0F) : 1.0F;
		}
		const Rml::Property* tint = GetProperty("own-color-tint");
		if (tint != nullptr && tint->unit == Rml::Unit::COLOUR)
			own.tint = tint->Get<Rml::Colourb>();
		own.opacity = GetComputedValues().opacity();
		return own;
	}

	Colour Canvas::OwnColors::fill(uint32_t rgba) const
	{
		const auto alpha = static_cast<uint8_t>(std::lround(static_cast<float>(rgba & 0xffU) * fillOpacity));
		return Rml::Colourb(static_cast<uint8_t>(rgba >> 24U), static_cast<uint8_t>(rgba >> 16U),
			static_cast<uint8_t>(rgba >> 8U), alpha)
			.ToPremultiplied(opacity);
	}

	std::optional<Colour> Canvas::OwnColors::line(uint32_t rgba) const
	{
		if (!tint)
			return std::nullopt;
		// The tint laid over the own color by the tint's alpha.
		const Rml::Colourb with = *tint;
		const auto over = [&with](uint32_t own, uint8_t to)
		{
			return static_cast<uint8_t>(std::lround(static_cast<float>(own) +
				((static_cast<float>(to) - static_cast<float>(own)) * static_cast<float>(with.alpha) / 255.0F)));
		};
		return Rml::Colourb(over((rgba >> 24U) & 0xffU, with.red), over((rgba >> 16U) & 0xffU, with.green),
			over((rgba >> 8U) & 0xffU, with.blue), static_cast<uint8_t>(rgba & 0xffU))
			.ToPremultiplied(opacity);
	}

	float Canvas::eventInset()
	{
		const Rml::Property* inset = GetProperty("event-inset");
		const float pixels = inset == nullptr ? 0.0F : ResolveLength(inset->GetNumericValue());
		return pixels > 0.0F ? std::round(pixels) : 0.0F;
	}

	float Canvas::strokeWidth()
	{
		const Rml::Property* width = GetProperty("stroke-width");
		const float pixels = width == nullptr ? 0.0F : ResolveLength(width->GetNumericValue());
		return pixels > 0.0F ? std::max(1.0F, std::round(pixels)) : 0.0F; // whole pixels stay sharp
	}

	void Canvas::OnRender()
	{
		const Rml::Context* context = GetContext();
		if (context == nullptr)
			return;
		const float dpRatio = context->GetDensityIndependentPixelRatio();
		const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
		const double unitsPerPixel = m_scale / dpRatio;
		const double left = std::round(m_start / unitsPerPixel);
		const double top = std::round(m_scrollTop * dpRatio);
		const bool inView = left >= m_built.left && left + size.x <= m_built.right && top >= m_built.top &&
			top + size.y <= m_built.bottom;
		if (m_dirty || !inView || unitsPerPixel != m_built.unitsPerPixel || size != m_built.size ||
			dpRatio != m_built.dpRatio)
		{
			m_built = Frame{
				unitsPerPixel, left - size.x, top - size.y, left + (2.0 * size.x), top + (2.0 * size.y), size, dpRatio};
			Rml::TexturedMeshList meshes;
			build(m_built, meshes);
			m_pieces.clear();
			for (Rml::TexturedMesh& mesh : meshes)
				if (!mesh.mesh.indices.empty())
					m_pieces.push_back({GetRenderManager()->MakeGeometry(std::move(mesh.mesh)), mesh.texture});
			m_dirty = false;
			++m_builds;
		}
		if (m_pieces.empty())
			return;

		Rml::RenderManager& render = *GetRenderManager();
		const Rml::Vector2f corner = GetAbsoluteOffset(Rml::BoxArea::Content).Round();
		const Rml::Rectanglei box =
			Rml::Rectanglei::FromPositionSize({static_cast<int>(corner.x), static_cast<int>(corner.y)},
				{static_cast<int>(std::round(size.x)), static_cast<int>(std::round(size.y))});
		const Rml::Rectanglei outer = render.GetScissorRegion();
		const Rml::Rectanglei clip = outer.Valid() ? box.Intersect(outer) : box;
		if (!clip.Valid())
			return; // outside its ancestors' clipping
		render.SetScissorRegion(clip);
		const Rml::Vector2f translation =
			corner + Rml::Vector2f(static_cast<float>(m_built.left - left), static_cast<float>(m_built.top - top));
		for (const Piece& piece : m_pieces)
			piece.geometry.Render(translation, piece.texture);
		render.SetScissorRegion(outer);
	}

	void Canvas::OnAttributeChange(const Rml::ElementAttributes& changed)
	{
		Rml::Element::OnAttributeChange(changed);
		if (drawsWith(changed))
			this->changed();
		if (changed.find("start") == changed.end() && changed.find("scale") == changed.end() &&
			changed.find("scroll-top") == changed.end())
			return;
		m_start = number(*this, "start", 0.0);
		const double scale = number(*this, "scale", 1.0);
		m_scale = scale > 0.0 ? scale : 1.0;
		m_scrollTop = number(*this, "scroll-top", 0.0);
		if (Rml::Context* context = GetContext())
			context->RequestNextUpdate(0);
	}

	void Canvas::OnPropertyChange(const Rml::PropertyIdSet& changed)
	{
		Rml::Element::OnPropertyChange(changed);
		m_dirty = true; // colors, widths or fonts
	}

	void Canvas::followFont()
	{
		// RmlUi gives fonts a new version when their glyphs must be generated again, e.g. after RmlUi released them.
		const Rml::FontFaceHandle font = GetFontFaceHandle();
		const int version = font == 0 ? -1 : Rml::GetFontEngineInterface()->GetVersion(font);
		if (version != m_fontVersion)
		{
			m_fontVersion = version;
			changed();
		}
	}

	void EventLanes::setLanes(std::vector<Lane> lanes)
	{
		m_lanes = std::move(lanes);
		m_tops.assign(1, 0.0);
		m_longest.clear();
		for (const Lane& lane : m_lanes)
		{
			m_tops.push_back(m_tops.back() + lane.height);
			double longest = 0.0;
			for (const TimeSpan& event : lane.events)
				longest = std::max(longest, event.length);
			m_longest.push_back(longest);
		}
		changed();
	}

	void EventLanes::build(const Frame& frame, Rml::TexturedMeshList& meshes)
	{
		if (m_lanes.empty())
			return;
		const double bodyOpacity = std::clamp(number(*this, "body-opacity", 1.0), 0.0, 1.0);
		const Colour body = scaled(fill(), bodyOpacity);
		const Colour outline = stroke();
		// Read once per build, not per event.
		const OwnColors own = ownColors();
		const float stroked = strokeWidth();
		const float width = outline.alpha == 0 ? 0.0F : stroked;
		// An own-color-tint outlines events with a color in stroke-width, also without a stroke-color.
		const float tinted = own.tint ? stroked : width;
		const float inset = eventInset();
		// Events reaching beyond the frame end just outside it, so their outline there stays out of view, whichever
		// width it has: the tinted one is never narrower.
		const float first = -(tinted + 1.0F);
		const float last = static_cast<float>(frame.right - frame.left) + tinted + 1.0F;
		const double from = frame.from();
		const double to = frame.to();
		Rml::Mesh mesh;
		// The first lane whose bottom is below the frame's top.
		auto lane = static_cast<size_t>(
			std::upper_bound(m_tops.begin() + 1, m_tops.end(), frame.top / frame.dpRatio) - (m_tops.begin() + 1));
		for (; lane < m_lanes.size() && m_tops[lane] * frame.dpRatio < frame.bottom; ++lane)
		{
			float y0 = frame.y(std::round(m_tops[lane] * frame.dpRatio));
			float y1 = frame.y(std::round(m_tops[lane + 1] * frame.dpRatio));
			if (y1 <= y0)
				continue;
			// Off the lane's edges, if it is high enough to keep a pixel between them.
			if (y1 - y0 > 2.0F * inset)
			{
				y0 += inset;
				y1 -= inset;
			}
			// By start, none longer than the longest: the first event reaching into the frame starts at most that
			// much before it.
			const std::vector<TimeSpan>& events = m_lanes[lane].events;
			auto event = std::lower_bound(events.begin(), events.end(), from - m_longest[lane],
				[](const TimeSpan& span, double position) { return span.start < position; });
			for (; event != events.end() && event->start < to; ++event)
			{
				if (event->end() <= from)
					continue;
				const float x0 = std::max(frame.x(event->start), first);
				const float x1 = std::max(std::min(frame.x(event->end()), last), x0 + 1.0F);
				if (event->color == 0)
				{
					outlined(mesh, x0, y0, x1, y1, body, outline, width);
					continue;
				}
				const std::optional<Colour> line = own.line(event->color);
				outlined(mesh, x0, y0, x1, y1, scaled(own.fill(event->color), bodyOpacity), line ? *line : outline,
					line ? tinted : width);
			}
		}
		meshes.push_back({std::move(mesh), {}});
	}

	bool EventLanes::drawsWith(const Rml::ElementAttributes& changed) const
	{
		return changed.count("body-opacity") != 0;
	}

	void EventNames::setLanes(std::vector<Lane> lanes)
	{
		m_lanes = std::move(lanes);
		m_tops.assign(1, 0.0);
		m_longest.clear();
		for (const Lane& lane : m_lanes)
		{
			m_tops.push_back(m_tops.back() + lane.height);
			m_longest.push_back(longestOf(lane.names));
		}
		changed();
	}

	bool EventNames::drawsWith(const Rml::ElementAttributes& changed) const
	{
		return changed.count("hide-truncated") != 0;
	}

	void EventNames::OnRender()
	{
		followFont();
		Canvas::OnRender();
	}

	void EventNames::build(const Frame& frame, Rml::TexturedMeshList& meshes)
	{
		const Rml::FontFaceHandle font = GetFontFaceHandle();
		if (m_lanes.empty() || font == 0)
			return;
		Rml::FontEngineInterface& fonts = *Rml::GetFontEngineInterface();
		const Rml::ComputedValues& computed = GetComputedValues();
		const Rml::TextShapingContext shaping{
			computed.language(), computed.direction(), computed.font_kerning(), computed.letter_spacing()};
		const Colour text = fill();
		const float inset = eventInset();
		const float margin = std::round(computed.font_size() / 4.0F);
		const Rml::FontMetrics& metrics = fonts.GetFontMetrics(font);
		const float lineHeight = std::ceil(metrics.ascent + metrics.descent);
		const bool hideTruncated = HasAttribute("hide-truncated");
		const double from = frame.from();
		const double to = frame.to();
		Rml::TexturedMeshList glyphs;
		const auto [firstLane, endLane] = lanesIn(m_tops, frame.top, frame.bottom, frame.dpRatio);
		for (size_t lane = firstLane; lane < endLane; ++lane)
		{
			const float top = frame.y(std::round(m_tops[lane] * frame.dpRatio));
			const float bottom = frame.y(std::round(m_tops[lane + 1] * frame.dpRatio));
			// Off the lane's top as events are, if it is high enough for the line then.
			const float y = bottom - top - (2.0F * inset) >= lineHeight ? top + inset : top;
			if (bottom - y < lineHeight)
				continue; // no room for a line of text
			const std::vector<Name>& names = m_lanes[lane].names;
			auto name = std::lower_bound(names.begin(), names.end(), from - m_longest[lane],
				[](const Name& item, double position) { return item.start < position; });
			for (; name != names.end() && name->start < to; ++name)
			{
				if (name->end() <= from || name->text.empty())
					continue;
				const float x0 = frame.x(name->start) + margin;
				const float room = frame.x(name->end()) - margin - x0;
				if (room <= 0.0F)
					continue;
				const size_t fits = fitting(fonts, font, name->text, shaping, room);
				if (fits == 0 || (hideTruncated && fits < name->text.size()))
					continue;
				fonts.GenerateString(*GetRenderManager(), font, 0, name->text.substr(0, fits),
					{x0, y + std::round(static_cast<float>(metrics.ascent))}, text, computed.opacity(), shaping,
					glyphs);
			}
		}
		std::move(glyphs.begin(), glyphs.end(), std::back_inserter(meshes));
	}

	void PartContents::setLanes(std::vector<Lane> lanes)
	{
		m_lanes = std::move(lanes);
		m_tops.assign(1, 0.0);
		m_longest.clear();
		m_ranges.clear();
		for (const Lane& lane : m_lanes)
		{
			m_tops.push_back(m_tops.back() + lane.height);
			m_longest.push_back(longestOf(lane.parts));
			std::vector<Range>& ranges = m_ranges.emplace_back();
			for (const Part& part : lane.parts)
			{
				Range range{127, 0, longestOf(part.notes)};
				for (const Note& note : part.notes)
				{
					range.lowest = std::min(range.lowest, note.pitch);
					range.highest = std::max(range.highest, note.pitch);
				}
				ranges.push_back(range);
			}
		}
		changed();
	}

	bool PartContents::drawsWith(const Rml::ElementAttributes& changed) const
	{
		return changed.count("brightness") != 0;
	}

	void PartContents::build(const Frame& frame, Rml::TexturedMeshList& meshes)
	{
		if (m_lanes.empty())
			return;
		const Colour notes = brightened(fill(), number(*this, "brightness", 0.5));
		const Colour bars = stroke();
		const float barWidth = std::max(1.0F, strokeWidth());
		const float inset = eventInset();
		const double from = frame.from();
		const double to = frame.to();
		Rml::Mesh mesh;
		const auto [firstLane, endLane] = lanesIn(m_tops, frame.top, frame.bottom, frame.dpRatio);
		for (size_t lane = firstLane; lane < endLane; ++lane)
		{
			float y0 = frame.y(std::round(m_tops[lane] * frame.dpRatio));
			float y1 = frame.y(std::round(m_tops[lane + 1] * frame.dpRatio));
			if (y1 - y0 > 2.0F * inset)
			{
				y0 += inset;
				y1 -= inset;
			}
			const float height = y1 - y0;
			if (height < 1.0F)
				continue;
			const std::vector<Part>& parts = m_lanes[lane].parts;
			const auto start = std::lower_bound(parts.begin(), parts.end(), from - m_longest[lane],
				[](const Part& part, double position) { return part.start < position; });
			for (auto part = start; part != parts.end() && part->start < to; ++part)
			{
				if (part->end() <= from)
					continue;
				const Range& range = m_ranges[lane][static_cast<size_t>(part - parts.begin())];
				const double left = std::max(part->start, from);
				const double right = std::min(part->end(), to);
				// Each pitch a row, the highest at the top: a line a pitch's share of the box high, at most a sixth.
				const int pitches = range.highest - range.lowest + 1;
				const float line = std::max(
					1.0F, std::round(std::min(height / static_cast<float>(std::max(pitches, 1)), height / 6.0F)));
				const float step = pitches > 1 ? (height - line) / static_cast<float>(pitches - 1) : 0.0F;
				if (notes.alpha != 0)
				{
					auto note = std::lower_bound(part->notes.begin(), part->notes.end(), left - range.longest,
						[](const Note& item, double position) { return item.start < position; });
					for (; note != part->notes.end() && note->start < right; ++note)
					{
						const double end = std::min(note->start + note->length, right);
						if (end <= left)
							continue;
						const float x0 = frame.x(std::max(note->start, left));
						const float x1 = std::max(frame.x(end), x0 + 1.0F);
						const float top = pitches > 1
							? std::round(y0 + (step * static_cast<float>(range.highest - note->pitch)))
							: std::round(y0 + ((height - line) / 2.0F));
						quad(mesh, x0, top, x1, top + line, notes);
					}
				}
				if (bars.alpha == 0)
					continue;
				const auto controller = std::lower_bound(part->controllers.begin(), part->controllers.end(), left,
					[](const Controller& item, double position) { return item.position < position; });
				for (auto item = controller; item != part->controllers.end() && item->position < right; ++item)
				{
					// Inside the part: a bar at its end ends there too.
					const float x =
						std::max(frame.x(left), std::min(frame.x(item->position), frame.x(right) - barWidth));
					const float high = std::max(
						1.0F, std::round(static_cast<float>(std::clamp(item->value, 0.0, 1.0)) * height / 4.0F));
					quad(mesh, x, y1 - high, x + barWidth, y1, bars);
				}
			}
		}
		meshes.push_back({std::move(mesh), {}});
	}

	void Waveform::setPeaks(TimeSpan span, std::vector<PeakColumns> channels)
	{
		m_span = span;
		m_channels = std::move(channels);
		changed();
	}

	void Waveform::setColor(uint32_t color)
	{
		if (m_span.color == color)
			return;
		m_span.color = color;
		changed();
	}

	void Waveform::setVolume(VolumeLine volume)
	{
		const auto samePoints = [](const engine::GainPoint& a, const engine::GainPoint& b)
		{ return a.position == b.position && a.gain == b.gain && a.curve == b.curve; };
		if (volume.start == m_volume.start && volume.end == m_volume.end && volume.rate == m_volume.rate &&
			volume.offset == m_volume.offset && volume.gain == m_volume.gain &&
			std::equal(
				volume.points.begin(), volume.points.end(), m_volume.points.begin(), m_volume.points.end(), samePoints))
			return; // the display sends it on every move of a drag
		m_volume = std::move(volume);
		changed();
	}

	void Waveform::build(const Frame& frame, Rml::TexturedMeshList& meshes)
	{
		if (m_channels.empty() || !(m_span.length > 0.0))
			return;
		const std::optional<Colour> own = m_span.color == 0 ? std::nullopt : ownColors().line(m_span.color);
		const Colour colour = brightened(own ? *own : fill(), number(*this, "brightness", 0.5));
		if (colour.alpha == 0)
			return;
		// event-inset off the lane's edges, inside the padding, as <event-lanes> keeps events, unless that would leave
		// the waveform no pixel. That includes the lanes no higher than twice the inset, where events drop it, as the
		// content box is no higher than the lane.
		const float inset = eventInset();
		float area = frame.size.y;
		float offset = 0.0F;
		if (frame.size.y - (2.0F * inset) >= 1.0F)
		{
			offset = inset;
			area = frame.size.y - (2.0F * inset);
		}
		const float height = area / static_cast<float>(m_channels.size());
		const double gain = number(*this, "gain", 1.0);
		const double first = std::max(frame.left, std::round(m_span.start / frame.unitsPerPixel));
		const double last = std::min(frame.right, std::round(m_span.end() / frame.unitsPerPixel));
		Rml::Mesh mesh;
		for (size_t channel = 0; channel < m_channels.size(); ++channel)
		{
			const PeakColumns& peaks = m_channels[channel];
			const size_t columns = std::min(peaks.low.size(), peaks.high.size());
			if (columns == 0)
				continue;
			const double perColumn = m_span.length / static_cast<double>(columns);
			const double middle = frame.y(0.0) + offset + (height * (static_cast<float>(channel) + 0.5F));
			const double half = height / 2.0;
			double volume = 1.0; // at the pixel's middle
			const auto y = [&](int16_t sample)
			{
				return static_cast<float>(
					std::round(middle - (std::clamp(sample / 32767.0 * gain * volume, -1.0, 1.0) * half)));
			};
			float previousTop = 0.0F;
			float previousBottom = 0.0F;
			float previousEnd = -1.0F;
			for (auto i = static_cast<int64_t>(first); i < static_cast<int64_t>(last); ++i)
			{
				const auto pixel = static_cast<double>(i);
				// The columns the pixel covers: at least one.
				const double start = (pixel * frame.unitsPerPixel) - m_span.start;
				const double end = start + frame.unitsPerPixel;
				const auto a = static_cast<std::ptrdiff_t>(
					std::clamp(std::floor(start / perColumn), 0.0, static_cast<double>(columns - 1)));
				const auto b = static_cast<std::ptrdiff_t>(
					std::clamp(std::ceil(end / perColumn), static_cast<double>(a + 1), static_cast<double>(columns)));
				if (m_volume.rate > 0.0)
					volume = m_volume.gain *
						engine::curveGain(m_volume.points,
							static_cast<double>(m_volume.offset) +
								((((pixel + 0.5) * frame.unitsPerPixel) - m_volume.start) * m_volume.rate));
				const int16_t low = *std::min_element(peaks.low.begin() + a, peaks.low.begin() + b);
				const int16_t high = *std::max_element(peaks.high.begin() + a, peaks.high.begin() + b);
				const float top = y(high);
				const float bottom = std::max(y(low), top + 1.0F); // silence is a line
				const auto x = static_cast<float>(pixel - frame.left);
				if (top == previousTop && bottom == previousBottom && x == previousEnd)
				{
					// The same as the pixel before: widen its quad.
					mesh.vertices[mesh.vertices.size() - 3].position.x = x + 1.0F;
					mesh.vertices[mesh.vertices.size() - 2].position.x = x + 1.0F;
				}
				else
				{
					quad(mesh, x, top, x + 1.0F, bottom, colour);
				}
				previousTop = top;
				previousBottom = bottom;
				previousEnd = x + 1.0F;
			}
		}
		meshes.push_back({std::move(mesh), {}});
	}

	bool Waveform::drawsWith(const Rml::ElementAttributes& changed) const
	{
		return changed.count("brightness") != 0 || changed.count("gain") != 0;
	}

	void Curve::setPoints(std::vector<CurvePoint> points, bool handles, bool dashed)
	{
		m_points = std::move(points);
		m_handles = handles;
		m_dashed = dashed;
		changed();
	}

	void Curve::setVirginTerritory(bool on)
	{
		if (std::exchange(m_virginTerritory, on) != on)
			changed();
	}

	void Curve::build(const Frame& frame, Rml::TexturedMeshList& meshes)
	{
		const Colour colour = stroke();
		const float width = strokeWidth();
		if (m_points.empty() || colour.alpha == 0 || width <= 0.0F)
			return;
		// The path in the frame's pixels: x from the left of the frame, y from its top. Values from 0 to 1 keep the
		// line inside the element.
		const double right = frame.right - frame.left;
		const auto x = [&](double position) { return std::round(position / frame.unitsPerPixel) - frame.left; };
		const auto y = [&](double value)
		{ return frame.y(((1.0 - std::clamp(value, 0.0, 1.0)) * (frame.size.y - width)) + (width / 2.0)); };
		// A gap lifts the pen: no line from the point before it to the one after it.
		const Rml::Vector2<double> gap{std::numeric_limits<double>::quiet_NaN(), 0.0};
		std::vector<Rml::Vector2<double>> path;
		auto point =
			static_cast<size_t>(std::lower_bound(m_points.begin(), m_points.end(), frame.from(),
									[](const CurvePoint& p, double position) { return p.position < position; }) -
				m_points.begin());
		if (point > 0)
			--point;
		else if (!m_virginTerritory)
			path.emplace_back(-1.0, y(m_points.front().value)); // level before the first point
		for (; point < m_points.size(); ++point)
		{
			const CurvePoint& from = m_points[point];
			const double x0 = x(from.position);
			path.emplace_back(x0, y(from.value));
			if (x0 > right)
				break;
			if (point + 1 == m_points.size())
			{
				if (!from.terminator && !m_virginTerritory)
					path.emplace_back(right + 1.0, y(from.value)); // level after the last point
				break;
			}
			if (from.terminator)
			{
				path.push_back(gap);
				continue;
			}
			const CurvePoint& to = m_points[point + 1];
			const double x1 = x(to.position);
			if (from.shape == engine::AutomationShape::Step)
			{
				path.emplace_back(x1, y(from.value));
			}
			else if (from.shape == engine::AutomationShape::Curve && x1 > x0)
			{
				// A piece every 2 pixels.
				const double begin = std::max(x0, -2.0);
				const auto pieces = static_cast<int64_t>(std::ceil((std::min(x1, right + 2.0) - begin) / 2.0));
				for (int64_t i = 1; i < pieces; ++i)
				{
					const double pixel = begin + (2.0 * static_cast<double>(i));
					path.emplace_back(pixel,
						y(from.value +
							((to.value - from.value) * engine::automationBend((pixel - x0) / (x1 - x0), from.curve))));
				}
			}
		}

		// Segments cut to the frame, so that a point far away doesn't make huge vertices.
		Rml::Mesh mesh;
		const double low = -width - 1.0;
		const double high = right + width + 1.0;
		for (size_t i = 1; i < path.size(); ++i)
		{
			Rml::Vector2<double> a = path[i - 1];
			Rml::Vector2<double> b = path[i];
			if (std::isnan(a.x) || std::isnan(b.x) || (a.x < low && b.x < low) || (a.x > high && b.x > high))
				continue;
			const auto cut = [](Rml::Vector2<double>& p, const Rml::Vector2<double>& q, double edge)
			{
				p.y += (q.y - p.y) * (edge - p.x) / (q.x - p.x);
				p.x = edge;
			};
			if (a.x < low)
				cut(a, b, low);
			if (b.x > high)
				cut(b, a, high);
			if (!m_dashed)
			{
				line(mesh, a, b, colour, width);
				continue;
			}
			// Dashes of 4 dp with gaps as long, counted from timeline position 0 so that they stay put when scrolling.
			const double dash = std::round(4.0 * frame.dpRatio);
			const double length = std::hypot(b.x - a.x, b.y - a.y);
			const auto at = [&](double along)
			{
				return Rml::Vector2<double>{a.x + ((b.x - a.x) * along / length), a.y + ((b.y - a.y) * along / length)};
			};
			for (double along = 0.0; along < length && dash > 0.0;)
			{
				const double phase =
					std::fmod(std::fmod(a.x + frame.left + along, 2.0 * dash) + (2.0 * dash), 2.0 * dash);
				if (phase < dash)
				{
					const double end = std::min(length, along + (dash - phase));
					line(mesh, at(along), at(end), colour, width);
					along = end;
				}
				else
				{
					along += (2.0 * dash) - phase;
				}
			}
		}
		if (m_handles)
		{
			const Colour body = fill();
			for (const CurvePoint& handle : m_points)
			{
				const double size = std::round((handle.selected ? 7.0 : 5.0) * frame.dpRatio);
				const double px = x(handle.position);
				if (px < -size || px > right + size)
					continue;
				const double py = y(handle.value);
				quad(mesh, static_cast<float>(px - (size / 2.0)), static_cast<float>(py - (size / 2.0)),
					static_cast<float>(px + (size / 2.0)), static_cast<float>(py + (size / 2.0)),
					handle.selected ? colour : body);
			}
		}
		meshes.push_back({std::move(mesh), {}});
	}

	void FadeLines::setLanes(std::vector<Lane> lanes)
	{
		m_lanes = std::move(lanes);
		changed();
	}

	void FadeLines::build(const Frame& frame, Rml::TexturedMeshList& meshes)
	{
		const Colour colour = stroke();
		const float width = strokeWidth();
		const Colour handle = fill();
		const double from = frame.from();
		const double to = frame.to();
		const double right = frame.right - frame.left;
		const Rml::Property* size = GetProperty("handle-size");
		const double handleSize = size == nullptr ? 0.0 : std::round(ResolveLength(size->GetNumericValue()));
		Rml::Mesh mesh;
		double top = 0.0; // dp
		for (const Lane& lane : m_lanes)
		{
			const double y0 = std::round(top * frame.dpRatio);
			top += lane.height;
			const double y1 = std::round(top * frame.dpRatio);
			if (y1 <= frame.top || y0 >= frame.bottom || y1 - y0 < 2.0)
				continue;
			const auto y = [&](double gain) { return frame.y(y1 - (gain * (y1 - y0 - width)) - (width / 2.0)); };
			if (colour.alpha != 0 && width > 0.0F)
				for (const FadeLine& fade : lane.fades)
				{
					if (!(fade.length > 0.0) || fade.start + fade.length <= from || fade.start >= to)
						continue;
					// A piece every 2 pixels, cut to the frame.
					const double x0 = std::round(fade.start / frame.unitsPerPixel) - frame.left;
					const double x1 = std::round((fade.start + fade.length) / frame.unitsPerPixel) - frame.left;
					const double begin = std::max(x0, -2.0);
					const double end = std::min(x1, right + 2.0);
					const auto gain = [&](double x)
					{
						const double at = x1 > x0 ? (x - x0) / (x1 - x0) : 1.0;
						return engine::fadeGain(fade.shape, fade.in ? at : 1.0 - at);
					};
					Rml::Vector2<double> last{begin, y(gain(begin))};
					const auto pieces = static_cast<int64_t>(std::ceil((end - begin) / 2.0));
					for (int64_t i = 1; i <= pieces; ++i)
					{
						const double x = std::min(begin + (2.0 * static_cast<double>(i)), end);
						const Rml::Vector2<double> next{x, y(gain(x))};
						line(mesh, last, next, colour, width);
						last = next;
					}
				}
			if (colour.alpha != 0 && width > 0.0F)
				for (const VolumeLine& volume : lane.volumes)
				{
					if (!(volume.rate > 0.0) || volume.end <= from || volume.start >= to)
						continue;
					const double x0 = std::round(volume.start / frame.unitsPerPixel) - frame.left;
					const double x1 = std::round(volume.end / frame.unitsPerPixel) - frame.left;
					const double begin = std::max(x0, -2.0);
					const double end = std::min(x1, right + 2.0);
					const auto gain = [&](double x)
					{
						const double seconds = (x + frame.left) * frame.unitsPerPixel;
						const double clipFrame =
							static_cast<double>(volume.offset) + ((seconds - volume.start) * volume.rate);
						return std::clamp(
							volume.gain * engine::curveGain(volume.points, clipFrame) / g_volumeLineTop, 0.0, 1.0);
					};
					// A piece every 2 pixels, and one to each point, so that jumps stay upright.
					std::vector<double> xs;
					for (int64_t i = 0; begin + (2.0 * static_cast<double>(i)) < end; ++i)
						xs.push_back(begin + (2.0 * static_cast<double>(i)));
					for (const engine::GainPoint& point : volume.points)
					{
						const double x =
							std::round(
								(volume.start + (static_cast<double>(point.position - volume.offset) / volume.rate)) /
								frame.unitsPerPixel) -
							frame.left;
						if (x > begin && x < end)
							xs.insert(xs.end(), {x - 0.01, x});
					}
					xs.push_back(end);
					std::sort(xs.begin(), xs.end());
					for (size_t i = 1; i < xs.size(); ++i)
						line(mesh, {xs[i - 1], y(gain(xs[i - 1]))}, {xs[i], y(gain(xs[i]))}, colour, width);
					if (handle.alpha == 0 || !volume.handles)
						continue;
					for (size_t i = 0; i < volume.points.size(); ++i)
					{
						const engine::GainPoint& point = volume.points[i];
						const double seconds =
							volume.start + (static_cast<double>(point.position - volume.offset) / volume.rate);
						if (seconds < volume.start || seconds > volume.end)
							continue;
						const double x = std::round(seconds / frame.unitsPerPixel) - frame.left;
						const double at = y(std::clamp(volume.gain * point.gain / g_volumeLineTop, 0.0, 1.0));
						quad(mesh, static_cast<float>(x - (handleSize / 2.0)),
							static_cast<float>(at - (handleSize / 2.0)), static_cast<float>(x + (handleSize / 2.0)),
							static_cast<float>(at + (handleSize / 2.0)),
							std::find(volume.selected.begin(), volume.selected.end(), i) != volume.selected.end()
								? colour
								: handle);
					}
				}
			// Fade handles at the top of the lane, event volume controls at its bottom.
			if (handle.alpha != 0)
				for (const auto& [places, squareTop] :
					{std::pair{&lane.handles, y0}, std::pair{&lane.faders, y1 - handleSize}})
					for (const double at : *places)
						if (at >= from - (handleSize * frame.unitsPerPixel) &&
							at <= to + (handleSize * frame.unitsPerPixel))
						{
							const double x = std::round(at / frame.unitsPerPixel) - frame.left;
							quad(mesh, static_cast<float>(x - (handleSize / 2.0)), frame.y(squareTop),
								static_cast<float>(x + (handleSize / 2.0)), frame.y(squareTop + handleSize), handle);
						}
		}
		meshes.push_back({std::move(mesh), {}});
	}

	std::pair<double, int> decimalStep(double minimum)
	{
		if (!(minimum > 0.0) || !std::isfinite(minimum))
			return {1.0, 5};
		const double power = std::pow(10.0, std::floor(std::log10(minimum)));
		for (const auto& [factor, parts] : {std::pair<double, int>{1.0, 5}, {2.0, 4}, {5.0, 5}})
			if (factor * power >= minimum * (1.0 - 1e-9))
				return {factor * power, parts};
		return {10.0 * power, 5};
	}

	std::string decimalLabel(double position, double step)
	{
		const int decimals = step > 0.0 ? std::clamp(-static_cast<int>(std::floor(std::log10(step) + 1e-9)), 0, 9) : 0;
		std::array<char, 64> text{};
		std::snprintf(text.data(), text.size(), "%.*f", decimals, position);
		std::string written = text.data();
		if (written.find_first_not_of("-0.") == std::string::npos && written[0] == '-')
			written.erase(0, 1); // no "-0"
		return written;
	}

	void evenMarks(double from, double to, double step, int parts, const std::function<std::string(double)>& label,
		std::vector<RulerMark>& marks)
	{
		if (!(step > 0.0) || parts < 1 || !((to - from) / step <= 10000.0))
			return; // a scale that asks for more ticks than pixels
		for (auto k = static_cast<int64_t>(std::ceil(from / step)); static_cast<double>(k) * step < to; ++k)
		{
			const double major = static_cast<double>(k) * step;
			marks.push_back({major, label(major)});
			for (int part = 1; part < parts; ++part)
				marks.push_back({major + (step * part / parts), {}});
		}
	}

	RulerScale::RulerScale()
		: marks(
			  [](double from, double to, double minimum, std::vector<RulerMark>& marks)
			  {
				  const auto [step, parts] = decimalStep(minimum);
				  evenMarks(
					  from, to, step, parts, [step = step](double position) { return decimalLabel(position, step); },
					  marks);
			  })
	{
	}

	void Ruler::setScale(RulerScale scale)
	{
		m_scale = std::move(scale);
		changed();
	}

	void Ruler::OnRender()
	{
		followFont();
		Canvas::OnRender();
	}

	void Ruler::build(const Frame& frame, Rml::TexturedMeshList& meshes)
	{
		if (!m_scale.marks)
			return;
		const Rml::FontFaceHandle font = GetFontFaceHandle();
		Rml::FontEngineInterface& fonts = *Rml::GetFontEngineInterface();
		const Rml::ComputedValues& computed = GetComputedValues();
		const Rml::TextShapingContext shaping{
			computed.language(), computed.direction(), computed.font_kerning(), computed.letter_spacing()};
		// Labels at least their width and an em apart: four ems at first, then as wide as the widest label asks.
		const double em = computed.font_size();
		std::vector<RulerMark> marks;
		double minimum = 4.0 * em * frame.unitsPerPixel;
		for (int attempt = 0; attempt < 3; ++attempt)
		{
			marks.clear();
			m_scale.marks(frame.from(), frame.to(), minimum, marks);
			double widest = 0.0;
			if (font != 0)
				for (const RulerMark& mark : marks)
					if (!mark.label.empty())
						widest = std::max(widest, static_cast<double>(fonts.GetStringWidth(font, mark.label, shaping)));
			const double needed = (widest + em) * frame.unitsPerPixel;
			if (needed <= minimum * (1.0 + 1e-9))
				break;
			minimum = needed;
		}

		const Colour ticks = stroke();
		const float width = strokeWidth();
		const Colour text = fill();
		const float y0 = frame.y(0.0);
		Rml::Mesh lines;
		Rml::TexturedMeshList labels;
		const float baseline = font == 0 ? 0.0F : static_cast<float>(fonts.GetFontMetrics(font).ascent);
		const bool drawTicks = width > 0.0F && ticks.alpha != 0;
		for (const RulerMark& mark : marks)
		{
			const float x = frame.x(mark.position);
			if (mark.label.empty())
			{
				if (drawTicks)
					quad(lines, x, y0 + std::round(frame.size.y * 0.75F), x + width, y0 + frame.size.y, ticks);
				continue;
			}
			if (drawTicks)
				quad(lines, x, y0, x + width, y0 + frame.size.y, ticks);
			if (font != 0)
				fonts.GenerateString(*GetRenderManager(), font, 0, mark.label,
					{x + width + std::round(static_cast<float>(em) / 4.0F), y0 + std::round(baseline)}, text,
					computed.opacity(), shaping, labels);
		}
		meshes.push_back({std::move(lines), {}});
		std::move(labels.begin(), labels.end(), std::back_inserter(meshes));
	}
}
