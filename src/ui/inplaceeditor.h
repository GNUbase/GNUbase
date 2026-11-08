// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/keyeditor.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	class EventDisplay;
}

// The in-place editor (#131, reference manual pp. 1259-1261): docs/features/in-place-editor.md describes it.
namespace gnubase::ui
{
	// Shows the project window's in-place editors: for each MIDI or instrument track whose editor inPlace:<track> is
	// open (editor.inPlace), whichever client opened it, a key editor (ui/keyeditor.h) over the track's lane in the
	// event display, which makes the lane at least g_inPlaceHeight high. The key editor's notes lie on the display's
	// timeline, under its ruler; its toolbar and info line show while the editor's toggle button
	// .in-place-toolbar-toggle has them shown. A click on a part in its row of parts activates it. Presses in an
	// in-place editor stay there: the event display doesn't select or edit what is under it. Use it on the UI thread;
	// the display and the binding must outlive it.
	class InPlaceEditors final : private Rml::EventListener
	{
	public:
		InPlaceEditors(EventDisplay& display, EngineBinding& binding);
		~InPlaceEditors() override;
		InPlaceEditors(const InPlaceEditors&) = delete;
		InPlaceEditors& operator=(const InPlaceEditors&) = delete;
		InPlaceEditors(InPlaceEditors&&) = delete;
		InPlaceEditors& operator=(InPlaceEditors&&) = delete;

		// Places the editors over their tracks' lanes and on the timeline as the display shows them now, with the
		// project the display read last. Call it once a frame.
		void update();

		// The tracks whose in-place editors are open.
		std::set<uint64_t> tracks() const;
		// A track's in-place key editor, while it is open.
		KeyEditor* keyEditor(uint64_t track);

	private:
		struct Editor
		{
			Rml::ObserverPtr<Rml::Element> element; // .in-place-editor[track]
			std::unique_ptr<KeyEditor> keys;
			engine::Json view;                              // editor.get's
			std::optional<std::pair<double, double>> place; // top and height as last set, dp
		};

		void ProcessEvent(Rml::Event& event) override;
		void follow(const engine::Json& view); // an editor.changed of an in-place editor
		void requestView(uint64_t track);
		void close(uint64_t track);
		void place(uint64_t track, Editor& editor);

		EventDisplay& m_display;
		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_area; // .in-place-editors in the event area
		std::map<uint64_t, Editor> m_editors;
		uint64_t m_loads = 0; // the display's project.get answers shown
		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it's gone
	};
}
