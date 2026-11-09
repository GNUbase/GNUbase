// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
	class ElementDocument;
}

// The tempo track in the UI (#155, reference manual pp. 162, 1447-1455): the tempo curve on the tempo track's lane in
// the event display and in the tempo track editor. The tempo is the engine's (tempo.* and project.set);
// docs/features/tempo-track.md describes both.
namespace gnubase::ui
{
	class Curve;
	class SignatureRow;

	// The project's tempo as project.get returns it.
	struct ProjectTempo
	{
		bool active = false;              // tempo track mode
		double fixed = 120.0;             // the fixed tempo, in BPM
		time::TempoMap tempo{120.0};      // what the project plays, which places everything on the timeline
		time::TempoMap tempoTrack{120.0}; // the tempo track's events
		uint64_t track = 0;               // the tempo track, 0 without one
		double length = 600.0;            // the project's, in seconds
		TimeContext time;                 // for positions as text
	};

	ProjectTempo readTempo(const engine::Json& project);

	// The tempos a tempo curve shows, lower and upper bound in BPM: from below the tempo track's slowest event to above
	// its fastest, in steps of 10 BPM.
	std::pair<double, double> visibleTempo(const ProjectTempo& tempo);

	// Current Tempo (reference manual pp. 162, 1446, 1449), which the transport bar's tempo field, the tempo track's
	// control and the tempo track editor show: the tempo the project plays at the project cursor, a position in
	// samples. That is the fixed tempo in fixed tempo mode.
	double currentTempo(const TimeContext& context, int64_t cursor);
	// The command and parameters that change Current Tempo to bpm: project.set's tempo in fixed tempo mode, else
	// tempo.set on the tempo event at or before the cursor, which is the first one in a project without tempo changes.
	std::pair<std::string, engine::Json> currentTempoCommand(
		const TimeContext& context, bool tempoTrackActive, int64_t cursor, double bpm);
	// A tempo as the tempo fields show it, "120.000".
	std::string tempoText(double bpm);

	// What a press on a tempo curve does (reference manual p. 1448).
	enum class TempoTool : uint8_t
	{
		Select, // selects and moves tempo events; a click on the curve adds one
		Draw,   // adds tempo events
		Erase,  // removes the tempo event clicked
	};

	// A tempo curve in an element of its own (<div class="tempo-curve">, .inactive in fixed tempo mode) that fills its
	// parent: the tempo track's events as a <curve class="tempo-line">, ramps and jumps, and a .tempo-point per event
	// (.selected when selected, attribute position: its ticks). In fixed tempo mode a second <curve class="tempo-line
	// fixed-tempo"> draws the fixed tempo as a horizontal line over the inactive events (reference manual pp. 1447,
	// 1456); in tempo track mode it has no points. With the select tool a click on an event selects it (Ctrl or Shift
	// adds it, or a click without moving takes a selected one out) and a drag moves the selected ones (tempo.set); a
	// drag elsewhere selects the events in a rectangle (.tool-rectangle.shown while dragged; Ctrl or Shift adds them to
	// the selection), and a click there adds an event (tempo.add, whose type follows editing.tempoType) (p. 1450). The
	// draw tool adds events where it presses, the erase tool removes them, but never the first. The parent places it;
	// the curve follows the view it is given. Use it on the UI thread; the binding must outlive it.
	class TempoCurve final : private Rml::EventListener
	{
	public:
		TempoCurve(EngineBinding& binding, Rml::Element& parent);
		~TempoCurve() override;
		TempoCurve(const TempoCurve&) = delete;
		TempoCurve& operator=(const TempoCurve&) = delete;
		TempoCurve(TempoCurve&&) = delete;
		TempoCurve& operator=(TempoCurve&&) = delete;

		Rml::Element* element() const { return m_element.get(); }
		const ProjectTempo& tempo() const { return m_tempo; }
		void setTempo(ProjectTempo tempo);
		// The time axis: seconds from the project start at the left edge, and seconds per dp.
		void setView(double start, double scale);
		void setTool(std::function<TempoTool()> tool) { m_tool = std::move(tool); }
		// The selected tempo events, by position in ticks. onSelect is told of the user's changes.
		const std::vector<int64_t>& selection() const { return m_selection; }
		void setSelection(std::vector<int64_t> positions);
		void onSelect(std::function<void()> handler) { m_onSelect = std::move(handler); }
		// Selects every tempo event (Select All Events, reference manual p. 1450).
		void selectAll();
		// Removes the selected tempo events but the first (Backspace and Delete in the tempo track editor).
		void removeSelected();

	private:
		// What a press with the select tool does until the mouse button goes up.
		struct Drag
		{
			Rml::Vector2f from; // where the press was, in pixels
			std::pair<double, double> range;
			bool onEvent = false;           // moves the selected events; else drags a rectangle or adds an event
			std::optional<int64_t> toggled; // a selected event pressed with Ctrl or Shift, out if it doesn't move
			std::vector<int64_t> base;      // the selection that the rectangle adds to
			bool moved = false;             // beyond g_dragStart
		};

		void ProcessEvent(Rml::Event& event) override;
		bool alive() const { return static_cast<bool>(m_element); }
		float ratio() const;
		double height() const; // dp
		void show();
		void place();
		void press(Rml::Event& event);
		void move(Rml::Event& event);
		void release(Rml::Event& event);
		void endDrag();
		// Adds a tempo event at a point in pixels, with the tempo the height there says.
		void addAt(Rml::Vector2f mouse);
		// The tempo event within reach of a point in pixels, by position.
		std::optional<int64_t> hit(Rml::Vector2f mouse) const;
		void select(std::vector<int64_t> positions);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_element;
		Curve* m_line = nullptr;
		Curve* m_fixed = nullptr;
		Rml::Element* m_points = nullptr;
		Rml::Element* m_rectangle = nullptr;
		ProjectTempo m_tempo;
		double m_start = 0.0;
		double m_scale = 0.01;
		std::function<TempoTool()> m_tool;
		std::vector<int64_t> m_selection;
		std::function<void()> m_onSelect;
		std::optional<Drag> m_drag;
	};

	// The tempo curve on the tempo track's lane in an event display, which follows the project and the display's view.
	// The curve takes the display's tools: draw draws, erase erases, the others select. Use it on the UI thread; the
	// display and the binding must outlive it.
	class TempoLane
	{
	public:
		TempoLane(EventDisplay& display, EngineBinding& binding);
		TempoLane(const TempoLane&) = delete;
		TempoLane& operator=(const TempoLane&) = delete;
		TempoLane(TempoLane&&) = delete;
		TempoLane& operator=(TempoLane&&) = delete;
		~TempoLane() = default;

		TempoCurve& curve() { return m_curve; }

	private:
		void requestProject();
		void place();

		EventDisplay& m_display;
		EngineBinding& m_binding;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for the binding's answers after it went
		TempoCurve m_curve;
		bool m_loading = false;
		bool m_stale = false;
		std::vector<Subscription> m_subscriptions;
	};

	// The tempo track editor (reference manual pp. 1447-1449, 1455): fills a document with the controls of
	// resources/ui/tempo-editor.rml and follows the project through the binding. #tempo-track-active switches the
	// tempo mode; the buttons [tempo-tool] pick the tool (select, draw, erase); #tempo-type is the type of new tempo
	// events (the user setting editing.tempoType); #current-tempo the fixed tempo, or in tempo track mode the tempo at
	// the project cursor. #tempo-curve-area holds the tempo curve of the whole project, #tempo-ruler a bars and beats
	// ruler over it and #tempo-scale the tempos, as .tempo-scale-label elements. The info line shows the first selected
	// event: #tempo-position, #tempo-value (its tempo, typed) and #tempo-ramp (ramp or jump). #signature-area under the
	// ruler is the time signature display, a ui::SignatureRow with the editor's tools (pp. 1468-1469); #signature-info
	// shows its first selected event instead: #signature-position (its bar) and #signature-value (its signature,
	// typed). Backspace and Delete remove the selected events. Use it on the UI thread; the binding must outlive it.
	class TempoEditor final : private Rml::EventListener
	{
	public:
		TempoEditor(EngineBinding& binding, Rml::ElementDocument& document);
		~TempoEditor() override;
		TempoEditor(const TempoEditor&) = delete;
		TempoEditor& operator=(const TempoEditor&) = delete;
		TempoEditor(TempoEditor&&) = delete;
		TempoEditor& operator=(TempoEditor&&) = delete;

		TempoCurve* curve() { return m_curve.get(); }
		SignatureRow* signatures() { return m_signatures.get(); }
		TempoTool tool() const { return m_tool; }

	private:
		void ProcessEvent(Rml::Event& event) override;
		void requestProject();
		void show();
		void place();

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_document;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
		std::unique_ptr<TempoCurve> m_curve;
		std::unique_ptr<SignatureRow> m_signatures;
		TempoTool m_tool = TempoTool::Select;
		std::string m_type = "auto";
		bool m_showing = false; // setting the fields' values
		int64_t m_cursor = 0;   // samples
		bool m_loading = false;
		bool m_stale = false;
		std::vector<Subscription> m_subscriptions;
	};
}
