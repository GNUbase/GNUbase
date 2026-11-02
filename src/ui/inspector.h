// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <memory>
#include <string>

namespace Rml
{
	class Element;
}

// The project window's inspector (#89, reference manual pp. 76-79, 167-188): docs/features/inspector.md describes it.
namespace gnubase::ui
{
	// Fills an element with the inspector of a track: its name (<textdisplay class="inspector-name">) above the
	// sections of the set "inspector" (docs/features/docking.md): basics (mute, solo, record and monitor buttons, and
	// volume and pan value fields), routing (input and output drop-down lists), inserts (the channel's plugin slots)
	// and notepad (the channel's notes in a <textarea>). A click on a section's header closes or opens it (.collapsed);
	// what a track doesn't have gets the class unavailable. The controls send engine commands. The element has the
	// class empty without a track. Use it on the UI thread; the binding must outlive it.
	class Inspector final : private Rml::EventListener
	{
	public:
		Inspector(EngineBinding& binding, Rml::Element& root);
		~Inspector() override;
		Inspector(const Inspector&) = delete;
		Inspector& operator=(const Inspector&) = delete;
		Inspector(Inspector&&) = delete;
		Inspector& operator=(Inspector&&) = delete;

		// Shows a track (0: none) of the project as project.get returned it, the loads-th answer; call it once per
		// step, it does nothing while neither changed.
		void update(uint64_t track, const engine::Json& project, uint64_t loads);

	private:
		void ProcessEvent(Rml::Event& event) override;
		void show();
		void change(Rml::Element& control, const Rml::Event& event);
		void saveNotes();

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		uint64_t m_track = 0;
		uint64_t m_loads = 0;
		engine::Json m_project;
		engine::Json m_channel; // the track's, null without one
		bool m_showing = false; // while show sets the controls, whose changes aren't the user's
	};
}
