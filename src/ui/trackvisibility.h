// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Rml
{
	class Element;
	class ElementDocument;
}

// Track visibility (#166): what the project window's track list hides. docs/features/track-visibility.md describes it.
namespace gnubase::ui
{
	// The project window's Track Visibility Configurations and the Visibility tab's Track page (reference manual pp.
	// 58, 80-82), clients of trackVisibility.get and trackVisibility.set like any other. The document's button
	// #track-visibility-configurations shows the active configuration, with an asterisk while the tracks differ from
	// it, and opens #track-visibility-configurations-menu: Add, Update, Rename and Delete Configuration, then the
	// configurations, a click on one applies it. Add and Rename ask for the name in visibility-configuration.rml, next
	// to the document: OK there with a name that trackVisibility.set would refuse keeps the dialog open with the
	// reason (#906). Use it on the UI thread; the binding must outlive it.
	class TrackVisibility final : private Rml::EventListener
	{
	public:
		TrackVisibility(EngineBinding& binding, Rml::ElementDocument& document);
		~TrackVisibility() override;
		TrackVisibility(const TrackVisibility&) = delete;
		TrackVisibility& operator=(const TrackVisibility&) = delete;
		TrackVisibility(TrackVisibility&&) = delete;
		TrackVisibility& operator=(TrackVisibility&&) = delete;

		// Fills the project window's Visibility tab: the pages Track and Editor (.visibility-page[page]) with their
		// tabs at the bottom (button.visibility-tab[page], .checked for the one that shows; the other page has
		// .hidden). The Track page has a button.track-visibility-item[track][level] per track, .checked while it
		// shows: a click hides or shows it with the tracks inside it (and shows the folders around it), Shift-click
		// shows only it. Its #track-visibility-sync opens the menu #track-visibility-sync-menu, Sync Visibility of
		// Project and Mixer with one of the mixer windows. Returns the Editor page, for the lower zone editor's
		// visibility. It shows nothing more once the element goes.
		Rml::Element& showPanel(Rml::Element& element);

	private:
		void ProcessEvent(Rml::Event& event) override;
		void requestProject();
		void requestVisibility();
		void show();
		void showPanelList();
		void click(uint64_t track, bool exclusive);
		void choose(const std::string& item);
		void openDialog(bool rename);
		void dialogOk();
		void closedDialog(Rml::Event& event);
		void removeSyncMenu();
		std::vector<uint64_t> inside(uint64_t track) const;       // the track and the tracks in it, nested too
		std::set<uint64_t> current(std::set<uint64_t> ids) const; // the ids that are tracks as project.get answered
		engine::Json current(const engine::Json& ids) const;

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_document;
		Rml::ObserverPtr<Rml::Element> m_button; // #track-visibility-configurations
		Rml::ObserverPtr<Rml::Element> m_menu;   // its menu
		Rml::ObserverPtr<Rml::Element> m_panel;  // the Visibility tab's element
		Rml::ObserverPtr<Rml::Element> m_syncMenu;
		Rml::ObserverPtr<Rml::Element> m_dialog;
		bool m_renaming = false;

		engine::Json m_tracks = engine::Json::array(); // project.get's
		engine::Json m_visibility;                     // trackVisibility.get's
		std::set<uint64_t> m_hidden;
		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
	};
}
