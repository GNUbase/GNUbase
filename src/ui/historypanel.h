// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <memory>
#include <vector>

namespace gnubase::ui
{
	// The edit history (#95, docs/features/edit-history.md): a table of the engine's undo steps (history.get) with
	// their action, time, state and details, after a row for the initial state. A separator follows the row of the
	// last done step, which is selected; dragging the separator up or down, also past the list's edges, which scrolls
	// it, or selecting another row with a click or the arrow keys, undoes or redoes up to there, and the details typed
	// into a row become its step's (history.describe) with Enter or when the field loses the focus. It follows
	// history.changed, whichever client changed the history; details being typed stay with their step, or go with it.
	// Use it on the UI thread; the binding must outlive it.
	class HistoryPanel final : private Rml::EventListener
	{
	public:
		HistoryPanel(EngineBinding& binding, Rml::Element& element);
		~HistoryPanel() override;
		HistoryPanel(const HistoryPanel&) = delete;
		HistoryPanel& operator=(const HistoryPanel&) = delete;
		HistoryPanel(HistoryPanel&&) = delete;
		HistoryPanel& operator=(HistoryPanel&&) = delete;

	private:
		void ProcessEvent(Rml::Event& event) override;
		void request();
		void show(const engine::Json& history);
		void place(int position);
		void drag(const Rml::Event& event);
		void follow();
		void cancelDrag();
		void jump(int position);
		void describe(Rml::Element& field);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_view;      // the <listview>
		Rml::ObserverPtr<Rml::Element> m_separator; // between the done and the undone steps: what is dragged
		engine::Json m_steps = engine::Json::array();
		int m_position = 0;
		// While the separator is dragged: where it is, the mouse's last y in the window, and the middles of the
		// steps' rows in the list's content, which scrolls, when the drag started.
		bool m_dragging = false;
		int m_dragTo = 0;
		float m_mouse = 0.0F;
		std::vector<float> m_middles;
		bool m_placing = false; // rows are moving across the separator or are made again: a lost focus sends nothing
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
		Subscription m_changed;
	};
}
