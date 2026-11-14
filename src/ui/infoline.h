// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/markers.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
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
	// Fills an element with the info line of the event display's selected events and parts: the first one's name,
	// start, end and length in the ruler's time format (<textdisplay field="start">), an audio event's volume
	// (<valuefield field="volume">) and mute and lock (toggle buttons), a MIDI part's transpose (with what the project
	// root key adds, rootKeyTranspose, in brackets) and velocity, its root key (<select field="rootKey">) and Global
	// Transpose (a toggle button, checked for Independent), from event.get. The element has the class empty without a
	// selection and multiple with more than one selected. Edits go to every selected event or part as one undo step
	// (history.transaction): relative to each one's value, or with Ctrl (Cmd) held when Enter takes the value, the same
	// value for all. With followMarkers it shows the markers selected on the marker tracks too, when their selection
	// changed last (class markers): the first one's description, position and, for a cycle marker, end and length,
	// which marker.set edits the same way. When the selection of an in-place editor changed last (class notes), it
	// shows the first of its selected notes as that editor's own info line does (p. 1259): start, end and length in
	// bars and beats, the pitch as a name, the velocities 0-127 and the channel 1-16, which note.set edits the same
	// way, relative to the first note or with Ctrl (Cmd)+Enter the same value for all, the channel set for all. Use
	// it on the UI thread; the display and the binding must outlive it.
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

		void ProcessEvent(Rml::Event& event) override;
		// An editor.changed of an in-place editor: when its selection changed, the info line shows its notes.
		void followEditor(const engine::Json& view);
		void show();
		// Edits the selected events; gesture is the value field's (ValueControl), 0 for others.
		void edit(const std::string& field, const std::string& text, float number, int gesture = 0);
		void showMarkers();
		void editMarkers(const std::string& field, const std::string& text);
		void showNotes();
		// Edits the in-place editor's selected notes as the key editor's info line does (pp. 1140, 1259).
		void editNotes(const std::string& field, const std::string& text, float number, int gesture);
		bool shows() const
		{
			if (m_showsNotes)
			{
				return !m_notes.empty();
			}
			return m_showsMarkers ? !m_markers.empty() : !m_events.empty();
		}
		// The selected event's volume in dB, from the display's project.
		double volumeOf(uint64_t event) const;
		// A MIDI part's transpose or velocity (key), from the display's project.
		int partValue(uint64_t part, const char* key) const;
		// A MIDI part as the display's project has it, or null.
		const engine::Json* partOf(uint64_t part) const;
		// A selected MIDI part or audio event (of m_events) as the display's project has it, for its root key and
		// Global Transpose (#1368), or null.
		const engine::Json* transposableOf(const engine::Json& event) const;
		// What a value field shows for the project's value of event, the first selected: while the project is older
		// than the field's own steps (#716), the value that the steps that changed event gave it instead, also after
		// other events showed in the field meanwhile (#1421), and when it was dragged with others (#1471).
		std::string fieldValue(const std::string& field, uint64_t event, std::string value) const;
		// The notes a selection of an in-place editor names, from the display's project, by start, then pitch.
		std::vector<Note> notesOf(const engine::Json& selection) const;
		// A part's start in project ticks, a linear track's position converted as the key editor does.
		int64_t partStart(const engine::Json& part) const;

		EventDisplay& m_display;
		EngineBinding& m_binding;
		const std::string m_stepOwner; // begins the whats of its steps (EngineBinding::stepOwner)
		Rml::ObserverPtr<Rml::Element> m_root;
		std::vector<uint64_t> m_selection;             // shown, or asked for
		uint64_t m_loads = 0;                          // the display's project shown
		TimeFormat m_format = TimeFormat::BarsBeats;   // the display's ruler's
		engine::Json m_events = engine::Json::array(); // event.get's, the first one first
		bool m_absolute = false;                       // while edit applies an absolute take
		bool m_dropping = false; // while another project ends the edits in progress, which then aren't taken
		std::optional<std::pair<std::string, std::string>> m_absoluteTake; // field and text taken with Ctrl+Enter
		Subscription m_reset;                                              // project.reset
		std::function<std::vector<uint64_t>()> m_markerSource;
		std::vector<uint64_t> m_markerSelection; // shown
		bool m_showsMarkers = false;             // the markers' selection changed last
		std::vector<MarkerItem> m_markers;       // the selected ones in timeline order, the first one first
		Subscription m_editors;                  // editor.changed
		std::map<std::string, engine::Json> m_viewSelections; // each open in-place editor's selection seen
		std::string m_noteView;                               // the editor whose notes show
		engine::Json m_noteSelection = engine::Json::array(); // its selection shown, or to show
		std::vector<Note> m_notes;                            // the selected ones, the first one first
		bool m_showsNotes = false;                            // an in-place editor's selection changed last
		bool m_notesDirty = false;                            // the notes wait to be read again
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
	};
}
