// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <memory>

namespace gnubase::ui
{
	// The edit history (#95, docs/features/edit-history.md): a table of the engine's undo steps (history.get) with
	// their action, time, state and details, after a row for the initial state. The row of the last done step is
	// selected; selecting another row, with a click or the arrow keys, undoes or redoes up to it, and the details
	// typed into a row become its step's (history.describe) with Enter or when the field loses the focus. It follows
	// history.changed, whichever client changed the history. Use it on the UI thread; the binding must outlive it.
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
		void jump(int position);
		void describe(Rml::Element& field);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_view; // the <listview>
		engine::Json m_steps = engine::Json::array();
		int m_position = 0;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
		Subscription m_changed;
	};
}
