// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace gnubase::ui
{
	// The pool (#93, docs/features/pool.md): a toolbar, the info line and a tree of the pool's folders and clips from
	// pool.get, with the columns Media, Used, Status, Info and Path. The names of folders and clips are fields that
	// rename them (poolFolder.set, clip.set) with Enter or when they lose the focus; clips dragged onto a folder's row
	// move there. The toolbar's Import and Find Missing Files run the UI commands pool.import and pool.findMissing
	// through run, which own the file dialogs; its other buttons send engine commands. It follows history.changed and
	// project.saved, whichever client changed the pool. Use it on the UI thread; the binding must outlive it.
	class PoolPanel final : private Rml::EventListener
	{
	public:
		using Run = std::function<void(const std::string& command)>;

		PoolPanel(EngineBinding& binding, Rml::Element& element, Run run);
		~PoolPanel() override;
		PoolPanel(const PoolPanel&) = delete;
		PoolPanel& operator=(const PoolPanel&) = delete;
		PoolPanel(PoolPanel&&) = delete;
		PoolPanel& operator=(PoolPanel&&) = delete;

	private:
		void ProcessEvent(Rml::Event& event) override;
		void request();
		void show(const engine::Json& pool);
		void rename(Rml::Element& field);
		void drop(const std::string& folder);
		void addFolder();
		std::vector<std::string> selection() const;

		EngineBinding& m_binding;
		Run m_run;
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::ObserverPtr<Rml::Element> m_view; // the <listview>
		engine::Json m_pool = engine::Json::object();
		std::vector<std::string> m_values;  // the rows' values as shown
		std::set<std::string> m_collapsed;  // folders the user closed
		std::vector<std::string> m_dragged; // clips a mouse button went down on, until it goes up
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
		Subscription m_changed;
		Subscription m_saved;
	};
}
