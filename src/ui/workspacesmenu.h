// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <memory>
#include <vector>

namespace Rml
{
	class Element;
}

// The Workspaces menu's workspaces (#163, docs/features/workspaces.md, reference manual pp. 1576-1578).
namespace gnubase::ui
{
	// Fills a menu, after its last child that is there now (its commands, such as Add Workspace), with No Workspace
	// and an item per workspace from workspace.get, global ones (G) then project ones (P): menuitem[workspace-type]
	// [workspace=<name>], checked for the active one, No Workspace with an empty workspace-type. A click activates it
	// with workspace.activate. It follows workspace.changed and a new project. Use it on the UI thread; the binding
	// must outlive it.
	class WorkspacesMenu final : private Rml::EventListener
	{
	public:
		WorkspacesMenu(EngineBinding& binding, Rml::Element& menu);
		~WorkspacesMenu() override;
		WorkspacesMenu(const WorkspacesMenu&) = delete;
		WorkspacesMenu& operator=(const WorkspacesMenu&) = delete;
		WorkspacesMenu(WorkspacesMenu&&) = delete;
		WorkspacesMenu& operator=(WorkspacesMenu&&) = delete;

		// Whether a workspace is active, for Update Workspace.
		bool hasActive() const;

	private:
		void ProcessEvent(Rml::Event& event) override;
		void request();
		void show();

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_menu;
		int m_fixed = 0;                                    // the menu's own items, before the workspaces
		engine::Json m_workspaces = engine::Json::object(); // workspace.get's
		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
	};
}
