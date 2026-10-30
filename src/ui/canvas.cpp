// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/canvas.h"

#include "engine/audio/automation.h"
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
#include <iterator>

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
				quad(mesh, x0, y0, x1, y1, width <= 0.0F ? body : outline);
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
	}

	void registerCanvas(RmlUi& rmlui)
	{
		Rml::StyleSheetSpecification::RegisterProperty("stroke-color", "transparent", false).AddParser("color");
		Rml::StyleSheetSpecification::RegisterProperty("stroke-width", "1px", false).AddParser("length");
		rmlui.registerElement<EventLanes>("event-lanes");
		rmlui.registerElement<Waveform>("waveform");
		rmlui.registerElement<Curve>("curve");
		rmlui.registerElement<Ruler>("ruler");
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
		const Colour body = fill();
		const Colour outline = stroke();
		const float width = outline.alpha == 0 ? 0.0F : strokeWidth();
		// Events reaching beyond the frame end just outside it, so their outline there stays out of view.
		const float first = -(width + 1.0F);
		const float last = static_cast<float>(frame.right - frame.left) + width + 1.0F;
		const double from = frame.from();
		const double to = frame.to();
		Rml::Mesh mesh;
		// The first lane whose bottom is below the frame's top.
		auto lane = static_cast<size_t>(
			std::upper_bound(m_tops.begin() + 1, m_tops.end(), frame.top / frame.dpRatio) - (m_tops.begin() + 1));
		for (; lane < m_lanes.size() && m_tops[lane] * frame.dpRatio < frame.bottom; ++lane)
		{
			const float y0 = frame.y(std::round(m_tops[lane] * frame.dpRatio));
			const float y1 = frame.y(std::round(m_tops[lane + 1] * frame.dpRatio));
			if (y1 <= y0)
				continue;
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
				outlined(mesh, x0, y0, x1, y1, body, outline, width);
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

	void Waveform::build(const Frame& frame, Rml::TexturedMeshList& meshes)
	{
		if (m_channels.empty() || !(m_span.length > 0.0))
			return;
		const Colour colour = fill();
		const float height = frame.size.y / static_cast<float>(m_channels.size());
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
			const double middle = frame.y(0.0) + (height * (static_cast<float>(channel) + 0.5F));
			const double half = height / 2.0;
			const auto y = [&](int16_t sample)
			{ return static_cast<float>(std::round(middle - (sample / 32767.0 * half))); };
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

	void Curve::setPoints(std::vector<CurvePoint> points)
	{
		m_points = std::move(points);
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
		std::vector<Rml::Vector2<double>> path;
		auto point =
			static_cast<size_t>(std::lower_bound(m_points.begin(), m_points.end(), frame.from(),
									[](const CurvePoint& p, double position) { return p.position < position; }) -
				m_points.begin());
		if (point > 0)
			--point;
		else
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
				path.emplace_back(right + 1.0, y(from.value)); // level after the last point
				break;
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
			if ((a.x < low && b.x < low) || (a.x > high && b.x > high))
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
			line(mesh, a, b, colour, width);
		}
		meshes.push_back({std::move(mesh), {}});
	}

	RulerScale::RulerScale()
		: step(
			  [](double minimum)
			  {
				  // The smallest of 1, 2 and 5 times a power of ten that is at least minimum.
				  if (!(minimum > 0.0) || !std::isfinite(minimum))
					  return std::pair<double, int>{1.0, 5};
				  const double power = std::pow(10.0, std::floor(std::log10(minimum)));
				  for (const auto& [factor, parts] : {std::pair<double, int>{1.0, 5}, {2.0, 4}, {5.0, 5}})
					  if (factor * power >= minimum * (1.0 - 1e-9))
						  return std::pair<double, int>{factor * power, parts};
				  return std::pair<double, int>{10.0 * power, 5};
			  }),
		  label(
			  [](double position, double step)
			  {
				  // As many decimals as the step needs.
				  const int decimals =
					  step > 0.0 ? std::clamp(-static_cast<int>(std::floor(std::log10(step) + 1e-9)), 0, 9) : 0;
				  std::array<char, 64> text{};
				  std::snprintf(text.data(), text.size(), "%.*f", decimals, position);
				  std::string written = text.data();
				  if (written.find_first_not_of("-0.") == std::string::npos && written[0] == '-')
					  written.erase(0, 1); // no "-0"
				  return written;
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
		// RmlUi gives fonts a new version when their glyphs must be generated again, e.g. after RmlUi released them.
		const Rml::FontFaceHandle font = GetFontFaceHandle();
		const int version = font == 0 ? -1 : Rml::GetFontEngineInterface()->GetVersion(font);
		if (version != m_fontVersion)
		{
			m_fontVersion = version;
			changed();
		}
		Canvas::OnRender();
	}

	void Ruler::build(const Frame& frame, Rml::TexturedMeshList& meshes)
	{
		const Rml::FontFaceHandle font = GetFontFaceHandle();
		Rml::FontEngineInterface& fonts = *Rml::GetFontEngineInterface();
		const Rml::ComputedValues& computed = GetComputedValues();
		const Rml::TextShapingContext shaping{
			computed.language(), computed.direction(), computed.font_kerning(), computed.letter_spacing()};
		// Labels at least their width and an em apart.
		const double em = computed.font_size();
		double widest = 0.0;
		if (font != 0)
			for (const double position : {frame.from(), frame.to()})
				widest = std::max(widest,
					static_cast<double>(
						fonts.GetStringWidth(font, m_scale.label(position, frame.unitsPerPixel), shaping)));
		const auto [step, parts] = m_scale.step((widest + em) * frame.unitsPerPixel);
		if (!(step > 0.0) || parts < 1)
			return;

		const Colour ticks = stroke();
		const float width = strokeWidth();
		const Colour text = fill();
		const float y0 = frame.y(0.0);
		Rml::Mesh lines;
		Rml::TexturedMeshList labels;
		const float baseline = font == 0 ? 0.0F : static_cast<float>(fonts.GetFontMetrics(font).ascent);
		if ((frame.to() - frame.from()) / step > 10000.0)
			return; // a scale that asks for more ticks than pixels
		for (auto k = static_cast<int64_t>(std::ceil(frame.from() / step)); static_cast<double>(k) * step < frame.to();
			++k)
		{
			const double major = static_cast<double>(k) * step;
			const float x = frame.x(major);
			if (width > 0.0F && ticks.alpha != 0)
			{
				quad(lines, x, y0, x + width, y0 + frame.size.y, ticks);
				for (int part = 1; part < parts; ++part)
				{
					const float minor = frame.x(major + (step * part / parts));
					quad(lines, minor, y0 + std::round(frame.size.y * 0.75F), minor + width, y0 + frame.size.y, ticks);
				}
			}
			if (font != 0)
				fonts.GenerateString(*GetRenderManager(), font, 0, m_scale.label(major, step),
					{x + width + std::round(static_cast<float>(em) / 4.0F), y0 + std::round(baseline)}, text,
					computed.opacity(), shaping, labels);
		}
		meshes.push_back({std::move(lines), {}});
		std::move(labels.begin(), labels.end(), std::back_inserter(meshes));
	}
}
