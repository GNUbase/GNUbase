// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/markers.h"
#include "ui/timeformat.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

// The project window's info line (#89, reference manual p. 72): docs/features/inspector.md describes it.
namespace gnubase::ui
{
	// The root keys, as midiPart.set, audioEvent.set and clip.set take them: none ("-"), then C to B. With differ,
	// first an empty one, g_differentRootKeys, which shows that the root keys of the events a list stands for differ
	// (#1644): choosing "-" there clears them.
	const widgets::Options& rootKeyOptions(bool differ = false);
	constexpr const char* g_differentRootKeys = "*";

	// Fills an element with the info line of the event display's selected events and parts: the first one's name,
	// start, end and length in the ruler's time format (<textdisplay field="start">), an audio event's volume
	// (<valuefield field="volume">) and mute and lock (toggle buttons), a MIDI part's or an audio event's transpose
	// (with what the project root key adds, rootKeyTranspose, in brackets), a MIDI part's velocity, the root key
	// (<select field="rootKey">) and Global Transpose (a toggle button, checked for Independent) of a MIDI part, an
	// audio event or an audio part's events, from event.get. The element has the class empty without a selection and
	// multiple with more than one selected. Edits go to every selected event or part as one undo step
	// (history.transaction): relative to each one's value, or with Ctrl (Cmd) held when Enter takes the value, the same
	// value for all. With followMarkers it shows the markers selected on the marker tracks too, when their selection
	// changed last (class markers): the first one's description, position and, for a cycle marker, end and length,
	// which marker.set edits the same way. With followArranger it shows the arranger events selected on the arranger
	// track's lane so too (class arranger, p. 405): the first one's name, start, end and length, which arranger.set
	// edits. When the selection of an in-place editor changed last (class notes), it shows the first of its selected
	// notes as that editor's own info line does (p. 1259): start, end and length in bars and beats, the pitch as a
	// name, the velocities 0-127 and the channel 1-16, which note.set edits the same way, relative to the first note or
	// with Ctrl (Cmd)+Enter the same value for all, the channel set for all. When the display's selected automation
	// points changed last (class points), it shows the first one in time: its position, value and Terminator (p. 881),
	// which automationPoint.set edits the same way. When the display's selection range changed last (class range), it
	// shows the range (pp. 287-288): Range Start, Range End and Range Length in the ruler's time format and its Top
	// Track and Bottom Track (<select field="topTrack">), which set the range (EventDisplay::setRange). Use it on the
	// UI thread; the display and the binding must outlive it.

	class InfoLine final : private Rml::EventListener
	{
	public:
		InfoLine(EventDisplay& display, EngineBinding& binding, Rml::Element& root);
		~InfoLine() override;
		InfoLine(const InfoLine&) = delete;
		InfoLine& operator=(const InfoLine&) = delete;
		InfoLine(InfoLine&&) = delete;
		InfoLine& operator=(InfoLine&&) = delete;

		// Follows the display's selection and project; call it once per step.
		void update();
		// The ids of the markers selected on the marker tracks, which update asks for.
		void followMarkers(std::function<std::vector<uint64_t>()> selection) { m_markerSource = std::move(selection); }
		// The ids of the arranger events selected on the arranger track's lane, which update asks for.
		void followArranger(std::function<std::vector<uint64_t>()> selection)
		{
			m_arrangerSource = std::move(selection);
		}

	private:
		// A note selected in an in-place editor, as the key editor's info line shows it (p. 1259).
		struct Note
		{
			uint64_t part = 0;
			uint64_t id = 0;
			int64_t start = 0; // project ticks
			int64_t length = 0;
			int pitch = 60;
			double velocity = 0.0;
			double releaseVelocity = 0.5;
			int channel = 0;
		};

		// A selected automation point, from the display's project.
		struct Point
		{
			uint64_t lane = 0;
			uint64_t id = 0;
			int64_t position = 0; // in its track's time base
			double seconds = 0.0;
			double value = 0.0;
			bool terminator = false;
			bool linear = false;       // its track's time base
			double minimum = -1.0e300; // its parameter's range, from automationLane.targets
			double maximum = 1.0e300;
		};

		void ProcessEvent(Rml::Event& event) override;
		// An editor.changed of an in-place editor: when its selection changed, the info line shows its notes.
		void followEditor(const engine::Json& view);
		// Shows the selection's values in the fields; changes the fields send meanwhile are no edits.
		void show();
		void showFields();
		// Edits the selected events; gesture is the value field's (ValueControl), 0 for others.
		void edit(const std::string& field, const std::string& text, float number, int gesture = 0);
		void showMarkers();
		void editMarkers(const std::string& field, const std::string& text);
		void showPoints();
		// Edits the selected automation points: position and value relative to the first one's, or with Ctrl+Enter the
		// same for all; Terminator for all.
		void editPoints(const std::string& field, const std::string& text);
		// The selected automation points, by position.
		std::vector<Point> pointsOf() const;
		void showRange();
		// Sets the display's range: its start, end or length, or its top or bottom track; a start past the end, an
		// end before the start or a top below the bottom track changes nothing.
		void editRange(const std::string& field, const std::string& text);
		void showNotes();
		// Edits the in-place editor's selected notes as the key editor's info line does (pp. 1140, 1259).
		void editNotes(const std::string& field, const std::string& text, float number, int gesture);
		bool shows() const
		{
			if (m_showsRange)
			{
				return m_range.has_value();
			}
			if (m_showsNotes)
			{
				return !m_notes.empty();
			}
			if (m_showsPoints)
			{
				return !m_points.empty();
			}
			return m_showsMarkers ? !m_markers.empty() : !m_events.empty();
		}
		// The selected event's volume in dB, from the display's project.
		double volumeOf(uint64_t event) const;
		// A selected MIDI part's transpose or velocity (key), or an audio event's transpose (#1555), from the
		// display's project.
		int partValue(const engine::Json& event, const char* key) const;
		// A MIDI part as the display's project has it, or null.
		const engine::Json* partOf(uint64_t part) const;
		// A selected MIDI part or audio event (of m_events) as the display's project has it, for its root key and
		// Global Transpose (#1368), or null. For an audio part, the first of its audio events by position, whose root
		// key and Global Transpose it shows unless they differ (#1555, #1644); null for an empty one.
		const engine::Json* transposableOf(const engine::Json& event) const;
		// The audio events of an audio part by position, from the display's project.
		std::vector<const engine::Json*> eventsOf(uint64_t part) const;
		// What a value field shows for the project's value of item, the first selected event or note (edits "events"
		// or "notes"): while the project is older than the field's own steps (#716), the value that the steps that
		// changed item gave it instead, also after other items showed in the field meanwhile (#1421), and when it was
		// dragged with others (#1471, #1550).
		std::string fieldValue(const char* edits, const std::string& field, uint64_t item, std::string value) const;
		// The number fieldValue shows for any item, not only the first: what a relative step goes from (#1632).
		double fieldNumber(const char* edits, const std::string& field, uint64_t item, double value) const;
		// What a relative step of gesture goes from for item: its number (fieldNumber's) where the gesture's first step
		// found it, so that an item a step holds at its limit doesn't drift from the others while the gesture goes on
		// (#870). A step outside a gesture (0) goes from now.
		double gestureBase(int gesture, const std::string& field, uint64_t item, double now);
		// The notes a selection of an in-place editor names, from the display's project, by start, then pitch.
		std::vector<Note> notesOf(const engine::Json& selection) const;
		// A part's start in project ticks, a linear track's position converted as the key editor does.
		int64_t partStart(const engine::Json& part) const;

		EventDisplay& m_display;
		EngineBinding& m_binding;
		const std::string m_stepOwner; // begins the whats of its steps (EngineBinding::stepOwner)
		Rml::ObserverPtr<Rml::Element> m_root;
		std::vector<uint64_t> m_selection;             // shown, or asked for
		uint64_t m_loads = 0;                          // the display's project shown (EventDisplay::answers)
		TimeFormat m_format = TimeFormat::BarsBeats;   // the display's ruler's
		engine::Json m_events = engine::Json::array(); // event.get's, the first one first
		bool m_absolute = false;                       // while edit applies an absolute take
		bool m_dropping = false; // while another project ends the edits in progress, which then aren't taken
		bool m_showing = false;  // while show sets the fields, whose changes aren't the user's (#1719)
		std::optional<std::pair<std::string, std::string>> m_absoluteTake; // field and text taken with Ctrl+Enter
		// gestureBase's: the gesture and field it is for, and each item's number where the gesture started.
		struct GestureBases
		{
			int gesture = 0;
			std::string field;
			std::map<uint64_t, double> numbers;
		} m_gestureBases;
		Subscription m_reset; // project.reset
		std::function<std::vector<uint64_t>()> m_markerSource;
		std::vector<uint64_t> m_markerSelection; // shown
		bool m_showsMarkers = false;             // the markers' selection changed last, or the arranger events'
		std::function<std::vector<uint64_t>()> m_arrangerSource;
		std::vector<uint64_t> m_arrangerSelection; // shown
		bool m_arranger = false;                   // m_markers are the selected arranger events
		std::vector<MarkerItem> m_markers; // the selected ones in timeline order, the first one first; arranger events
										   // as cycle markers on the arranger track
		Subscription m_editors;            // editor.changed
		std::map<std::string, engine::Json> m_viewSelections;    // each open in-place editor's selection seen
		std::string m_noteView;                                  // the editor whose notes show
		engine::Json m_noteSelection = engine::Json::array();    // its selection shown, or to show
		std::vector<Note> m_notes;                               // the selected ones, the first one first
		bool m_showsNotes = false;                               // an in-place editor's selection changed last
		bool m_notesDirty = false;                               // the notes wait to be read again
		std::map<uint64_t, std::set<uint64_t>> m_pointSelection; // the display's selected automation points, shown
		std::vector<Point> m_points;                             // the selected ones, the first one first
		bool m_showsPoints = false;                              // the points' selection changed last
		std::optional<RangeSelection> m_range;                   // the display's selection range, shown
		bool m_showsRange = false;                               // the range changed last
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
	};
}
