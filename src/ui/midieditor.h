// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/audioparteditor.h"
#include "ui/enginebinding.h"
#include "ui/keyeditor.h"
#include "ui/listeditor.h"
#include "ui/patterneditor.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

// The editors' common framework (#126): the lower zone's editor and the editor window, for MIDI parts and audio
// parts.
// docs/features/midi-editors.md describes it.
namespace gnubase::ui
{
	// What a MidiEditor shows of its editor.
	enum class EditorPart : std::uint8_t
	{
		Editor,       // the toolbar and the events
		Visibility,   // the Visibility tab (reference manual pp. 1148-1153)
		GlobalTracks, // the inspector: Global Tracks (pp. 1144-1146), the editor window's only, and Chord Editing
	};

	// The project reads of the MIDI editors (#625): up to five of them (the lower zone's editor, both Visibility tabs,
	// the editor window's editor and its inspector) ask for the project after every change, and those that ask while
	// the binding hands on the same answer or event share one project.get. One asked later, while that read is out,
	// gets the next one, sent once it is answered, as what it asked for may have changed after the read went out. Use
	// it on the UI thread; the binding must outlive it.
	class ProjectReads
	{
	public:
		explicit ProjectReads(EngineBinding& binding) : m_binding(binding) {}

		// Calls done with project.get's result once a read answers that was sent while the binding handed on what it
		// hands on now (EngineBinding::handed), or later.
		void read(std::function<void(const engine::Result&)> done);

	private:
		void send();

		EngineBinding& m_binding;
		bool m_loading = false;
		uint64_t m_sentAt = 0; // EngineBinding::handed() when the read out was sent
		std::vector<std::function<void(const engine::Result&)>> m_current; // waiting for the read out
		std::vector<std::function<void(const engine::Result&)>> m_next;    // for the next one
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);      // for answers that come after it went
	};

	// Fills an element with a part of a MIDI editor and follows the engine's editor view (editor.get, editor.changed)
	// and the project, whichever client changes them. view is the editor's name in editor.* (lower or window). The
	// editor's toolbar has Keep Editor Contents, the Part Editing Mode menu, the Activate Part for Editing menu and
	// Open in Separate Window or Open in Lower Zone; under it the global tracks (window only) and the key editor
	// (ui/keyeditor.h), the list editor (ui/listeditor.h) or for audio parts the audio part editor
	// (ui/audioparteditor.h), as the view's content says, whose parts a click
	// activates. The inspector's Chord Editing section (pp. 1215-1218) shows the
	// chord of the selected notes and changes it with note.voicing. Every change goes to the engine as editor.*
	// commands. moved, if given, runs once Open in Separate Window or Open in Lower Zone has sent the parts to the
	// other editor. It reads the project through reads, shared with the other editors. Use it on the UI thread; the
	// binding and reads must outlive it.
	class MidiEditor final : private Rml::EventListener
	{
	public:
		MidiEditor(EngineBinding& binding, ProjectReads& reads, Rml::Element& root, std::string view,
			EditorPart part = EditorPart::Editor, std::function<void()> moved = {});
		~MidiEditor() override;
		MidiEditor(const MidiEditor&) = delete;
		MidiEditor& operator=(const MidiEditor&) = delete;
		MidiEditor(MidiEditor&&) = delete;
		MidiEditor& operator=(MidiEditor&&) = delete;

		// The editor's view as editor.get returns it, once it came.
		const engine::Json& view() const { return m_view; }
		// The key editor, of an EditorPart::Editor while it shows the key editor.
		KeyEditor* keyEditor() { return m_keys.get(); }
		// Runs when the drum editor's Map menu chooses Drum Map Setup (KeyEditor::setDrumMapSetup).
		void setDrumMapSetup(std::function<void()> open);
		// The list editor, of an EditorPart::Editor while it shows the list editor.
		ListEditor* listEditor() { return m_list.get(); }
		// The audio part editor, of an EditorPart::Editor while it shows audio parts.
		AudioPartEditor* audioPartEditor() { return m_audio.get(); }
		// The pattern editor, of an EditorPart::Editor while it shows a pattern event.
		PatternEditor* patternEditor() { return m_pattern.get(); }

	private:
		void ProcessEvent(Rml::Event& event) override;
		void requestProject();
		void requestView();
		void show();
		// Puts the key editor, the list editor or the audio part editor into the editor, as the view's content says.
		void showContent();
		bool showsAudio() const;
		// The project's parts of the kind the editor shows: audio parts or MIDI parts.
		engine::Json projectParts() const;
		// A row of the Visibility tab: a track, or a folder with tracks in it.
		struct VisibilityRow
		{
			uint64_t track = 0;
			int level = 0; // how deep in folders
			bool folder = false;
			std::string name;
			std::string state; // of its check: checked, partial or empty
			bool active = false;
			bool unavailable = false;
			bool disabled = false;
		};
		std::vector<VisibilityRow> visibilityRows() const;
		// Puts the rows into the Visibility tab: in place while it shows the same tracks with the same names, so that
		// a double click whose first click changed the view still reaches the row it started on.
		static void showVisibility(Rml::Element& list, const std::vector<VisibilityRow>& rows);
		std::string chordName() const;
		void voice(const std::string& key, const std::string& value);
		std::string globalTracksRml() const;
		void fillMenus();
		void set(engine::Json parameters);
		void toggleTrack(uint64_t track, bool exclusive);
		// The parts of a track, or of the tracks in a folder: how many there are and how many the editor shows.
		std::pair<size_t, size_t> partsShown(uint64_t track) const;
		std::vector<uint64_t> inside(uint64_t track) const; // the track and the tracks in it, nested too
		bool showable(uint64_t id) const;                   // whether editor.set's showTracks takes it
		bool hiddenInProject(uint64_t id) const;            // by itself or in a hidden folder
		void fold(Rml::Element& list) const;                // the Visibility tab's rows, from m_collapsed
		const engine::Json* track(uint64_t id) const;

		EngineBinding& m_binding;
		ProjectReads& m_reads;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus; // in the document's body
		std::string m_name;
		EditorPart m_part;
		std::function<void()> m_moved;
		std::function<void()> m_drumMapSetup;
		engine::Json m_project = engine::Json::object();
		uint64_t m_read = 0; // EngineBinding::handed() when m_project came
		engine::Json m_view = engine::Json::object();
		bool m_loading = false;
		bool m_stale = false;
		std::set<uint64_t> m_collapsed; // the Visibility tab's collapsed folders
		std::unique_ptr<KeyEditor> m_keys;
		std::unique_ptr<ListEditor> m_list;
		std::unique_ptr<AudioPartEditor> m_audio;
		std::unique_ptr<PatternEditor> m_pattern;
		LevelSteps m_levelSteps; // whether a channel-only step changed only levels and sends (#1881)
		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
	};
}
