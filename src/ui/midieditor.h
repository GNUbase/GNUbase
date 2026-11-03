// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

// The MIDI editors' common framework (#126): the lower zone's editor and the editor window.
// docs/features/midi-editors.md describes it.
namespace gnubase::ui
{
	// What a MidiEditor shows of its editor.
	enum class EditorPart
	{
		Editor,       // the toolbar and the events
		Visibility,   // the Visibility tab (reference manual pp. 1148-1153)
		GlobalTracks, // the inspector's Global Tracks section (pp. 1144-1146), the editor window's only
	};

	// Fills an element with a part of a MIDI editor and follows the engine's editor view (editor.get, editor.changed)
	// and the project, whichever client changes them. view is the editor's name in editor.* (lower or window). The
	// editor's toolbar has Keep Editor Contents, the Part Editing Mode menu, the Activate Part for Editing menu and
	// Open in Separate Window or Open in Lower Zone; under it the global tracks (window only) and the parts it shows,
	// the stand-in for the key editor's note display (#127), where a click activates a part. Every change goes to the
	// engine as editor.* commands. Use it on the UI thread; the binding must outlive it.
	class MidiEditor final : private Rml::EventListener
	{
	public:
		MidiEditor(EngineBinding& binding, Rml::Element& root, std::string view, EditorPart part = EditorPart::Editor);
		~MidiEditor() override;
		MidiEditor(const MidiEditor&) = delete;
		MidiEditor& operator=(const MidiEditor&) = delete;
		MidiEditor(MidiEditor&&) = delete;
		MidiEditor& operator=(MidiEditor&&) = delete;

		// The editor's view as editor.get returns it, once it came.
		const engine::Json& view() const { return m_view; }

	private:
		void ProcessEvent(Rml::Event& event) override;
		void requestProject();
		void requestView();
		void show();
		std::string editorRml() const;
		std::string visibilityRml() const;
		std::string globalTracksRml() const;
		void fillMenus();
		void set(engine::Json parameters);
		void toggleTrack(uint64_t track, bool exclusive);
		// The parts of a track, or of the tracks in a folder: how many there are and how many the editor shows.
		std::pair<size_t, size_t> partsShown(uint64_t track) const;
		const engine::Json* track(uint64_t id) const;

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus; // in the document's body
		std::string m_name;
		EditorPart m_part;
		engine::Json m_project = engine::Json::object();
		engine::Json m_view = engine::Json::object();
		bool m_loading = false;
		bool m_stale = false;
		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
	};
}
