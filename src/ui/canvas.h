// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/Texture.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// The canvas elements (#55): what editors show on a timeline, drawn as geometry built from data, with only what is in
// view built. docs/features/canvas.md has the reasoning and the numbers.
namespace gnubase::ui
{
	class RmlUi;

	// Registers <event-lanes>, <waveform>, <curve> and <ruler>, and the style properties they draw with (stroke-color
	// and stroke-width). ui::RmlUi does it when it starts, so every document has them.
	void registerCanvas(RmlUi& rmlui);

	// A stretch of a timeline, in the units of whoever fills the element: ticks, samples or seconds. The elements only
	// map them to the screen.
	struct TimeSpan
	{
		double start = 0.0;
		double length = 0.0;

		double end() const { return start + length; }
	};

	// Base of the canvas elements. The attributes start (the timeline position at the left edge, default 0) and scale
	// (units per dp, default 1) are the time axis, scroll-top (dp, default 0) scrolls vertically. Geometry is built
	// for the view and one view's width and height around it; scrolling within that only moves it, so it is rebuilt
	// once per view's width scrolled, and when the data, the scale, the size or the style changes. Positions are
	// snapped to whole pixels. The content box clips what is drawn (by scissor, so not inside a transform).
	class Canvas : public Rml::Element
	{
	public:
		explicit Canvas(const Rml::String& tag) : Rml::Element(tag) {}

		// How often the element built its geometry.
		uint64_t builds() const { return m_builds; }

	protected:
		// What a build covers, in pixels: x counts from timeline position 0 (a position divided by unitsPerPixel),
		// y from the top of what scroll-top scrolls. Vertices are relative to (left, top), so they stay small.
		struct Frame
		{
			double unitsPerPixel = 1.0;
			double left = 0.0;
			double top = 0.0;
			double right = 0.0;
			double bottom = 0.0;
			Rml::Vector2f size; // the content box
			float dpRatio = 1.0F;

			// The timeline positions the build covers.
			double from() const { return left * unitsPerPixel; }
			double to() const { return right * unitsPerPixel; }
			// A timeline position as a vertex's x, on a whole pixel.
			float x(double position) const;
			// A height in pixels from the top of the scrolled content as a vertex's y.
			float y(double pixels) const { return static_cast<float>(pixels - top); }
		};

		// Adds the meshes for the frame. Untextured meshes go with an empty texture.
		virtual void build(const Frame& frame, Rml::TexturedMeshList& meshes) = 0;

		// The data changed: builds again and draws the window.
		void changed();

		// The colors and widths of the style sheets: color fills, stroke-color and stroke-width (in pixels) draw lines
		// and outlines.
		Rml::ColourbPremultiplied fill();
		Rml::ColourbPremultiplied stroke();
		float strokeWidth();

		void OnRender() override;
		void OnAttributeChange(const Rml::ElementAttributes& changed) override;
		void OnPropertyChange(const Rml::PropertyIdSet& changed) override;

	private:
		struct Piece
		{
			Rml::Geometry geometry;
			Rml::Texture texture;
		};

		double m_start = 0.0;
		double m_scale = 1.0;
		double m_scrollTop = 0.0;
		bool m_dirty = true;
		Frame m_built;
		std::vector<Piece> m_pieces;
		uint64_t m_builds = 0;
	};

	// <event-lanes>: rows of rectangles, such as the events of tracks or the notes of a key editor (one lane per
	// pitch). Fills them with color, outlined by stroke-color.
	class EventLanes final : public Canvas
	{
	public:
		struct Lane
		{
			float height = 0.0F;          // dp
			std::vector<TimeSpan> events; // by start
		};

		using Canvas::Canvas;

		void setLanes(std::vector<Lane> lanes);
		const std::vector<Lane>& lanes() const { return m_lanes; }

	protected:
		void build(const Frame& frame, Rml::TexturedMeshList& meshes) override;

	private:
		std::vector<Lane> m_lanes;
		std::vector<double> m_tops;    // each lane's top in dp, and the bottom of the last
		std::vector<double> m_longest; // each lane's longest event
	};

	// One channel's peak columns as media.peaks returns them: the smallest and largest sample of each column, -32767
	// to 32767.
	struct PeakColumns
	{
		std::vector<int16_t> low;
		std::vector<int16_t> high;
	};

	// <waveform>: the peaks of one or more channels, one above the other, in color. A pixel shows the columns it
	// covers; columns wider than a pixel are repeated.
	class Waveform final : public Canvas
	{
	public:
		using Canvas::Canvas;

		// The columns cover span evenly.
		void setPeaks(TimeSpan span, std::vector<PeakColumns> channels);

	protected:
		void build(const Frame& frame, Rml::TexturedMeshList& meshes) override;

	private:
		TimeSpan m_span;
		std::vector<PeakColumns> m_channels;
	};

	// A point of a curve: value from 0 (bottom) to 1 (top), and the shape of the segment that starts at it, as the
	// engine plays automation (engine::AutomationPoint).
	struct CurvePoint
	{
		double position = 0.0;
		double value = 0.0;
		engine::AutomationShape shape = engine::AutomationShape::Linear;
		double curve = 0.0;
	};

	// <curve>: an automation curve, a line in stroke-color, level with the first point before it and with the last
	// one after it.
	class Curve final : public Canvas
	{
	public:
		using Canvas::Canvas;

		// Points by position.
		void setPoints(std::vector<CurvePoint> points);

	protected:
		void build(const Frame& frame, Rml::TexturedMeshList& meshes) override;

	private:
		std::vector<CurvePoint> m_points;
	};

	// A tick of a ruler: labelled ticks reach to the top, the others are a quarter as high.
	struct RulerMark
	{
		double position = 0.0;
		std::string label; // empty for a minor tick
	};

	// How a ruler divides the timeline: marks adds the ticks from `from` up to `to`, by position, with the labelled
	// ones at least minimum units apart. The default counts in decimal steps of 1, 2 and 5; ui/timeformat.h has bars
	// and beats, seconds, timecode and samples.
	struct RulerScale
	{
		std::function<void(double from, double to, double minimum, std::vector<RulerMark>& marks)> marks;

		RulerScale();
	};

	// The smallest of 1, 2 and 5 times a power of ten that is at least minimum, and how many parts minor ticks divide
	// it into.
	std::pair<double, int> decimalStep(double minimum);
	// A position with as many decimals as the step needs.
	std::string decimalLabel(double position, double step);
	// Marks every step from `from` up to `to`, labelled by label, each divided by minor ticks into parts. Adds nothing
	// for more than 10,000 steps.
	void evenMarks(double from, double to, double step, int parts, const std::function<std::string(double)>& label,
		std::vector<RulerMark>& marks);

	// <ruler>: ticks in stroke-color along the bottom, labelled major ticks reaching to the top, labels in color and
	// the element's font right of their tick.
	class Ruler final : public Canvas
	{
	public:
		using Canvas::Canvas;

		void setScale(RulerScale scale);

	protected:
		void build(const Frame& frame, Rml::TexturedMeshList& meshes) override;
		void OnRender() override;

	private:
		RulerScale m_scale;
		int m_fontVersion = -1;
	};
}
