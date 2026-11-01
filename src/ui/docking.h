// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/shell.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rml
{
	class ElementDocument;
}

// The docking system (#56): windows built from zones, tab groups, splitters and strips, and panels that dock in a zone
// or open in a window of their own. docs/features/docking.md describes it.
namespace gnubase::ui
{
	class KeyCommands;

	// What a docking window can show in a zone's tab group or in a window of its own, e.g. the mixer.
	struct Panel
	{
		std::string id;     // e.g. "mixer"
		std::string title;  // its tab's and its window's title
		std::string window; // the docking window it docks in, e.g. "project"
		std::string zone;   // its zone there, e.g. "lower"
		// Fills the element the panel shows in, wherever it is shown; what it returns lives as long as the panel shows
		// there. Optional.
		std::function<std::shared_ptr<void>(Rml::Element& element)> create;
	};

	// The layout of every docking window and panel, kept in the user setting ui.layout: the UI changes it with
	// settings.set through the binding and follows settings.changed, so MCP and Lua change the layout the same way.
	// A document becomes a docking window with attach; its elements say what they are by attributes:
	//
	//   zone="main|left|right|lower"  a zone; every zone but main gets a <tabset> with its panels. size="240" and
	//                                 hidden give its default size in dp and visibility. Its size is its width, or
	//                                 its height in a flex column.
	//   splitter for="left"           the splitter that sizes that zone (resize="next" for zones after it); hidden
	//                                 with the zone, and a double click hides the zone
	//   strip="infoLine"              a strip, such as the info line; hidden gives its default. The body has the
	//                                 class no-<strip> while it is hidden, for strips that are part of other elements
	//                                 such as the event display's overview line
	//   toggle="left|infoLine"        a button or menu item that shows or hides the zone or strip; checked follows it
	//   sections="channel"            a container of sections of that set, such as each of the mixer's channels; its
	//                                 children with section="eq" (label="EQ", default="hidden") are the sections, in
	//                                 the user's order and hidden with the class .hidden. The first container says
	//                                 which sections a set has and their default order
	//   setup="channel"               a setup pane's list of the set's sections, which the docking system fills with a
	//                                 toggle button per section (setup-section="eq"); a button dragged onto another
	//                                 moves its section there
	//
	// Zones get the state class .hidden when hidden and .focused while the keyboard focus is in them; strips get
	// .hidden. A tab is dragged onto another to move it there; a right click on the tabs opens the zone's tab menu,
	// with Open in Separate Window and an item to show or hide each tab. A panel in a window of its own has a Dock
	// button (#dock), and closing the window docks it too. Use it on the UI thread; the engine, the binding and the
	// shell must outlive it.
	class Docking final : private Rml::EventListener
	{
	public:
		// panelWindow is the document for panels in windows of their own, with an element #panel-content; hiddenWindows
		// opens those windows hidden, for tests.
		Docking(engine::Engine& engine, EngineBinding& binding, Shell& shell, std::filesystem::path panelWindow,
			bool hiddenWindows = false);
		~Docking() override;
		Docking(const Docking&) = delete;
		Docking& operator=(const Docking&) = delete;
		Docking(Docking&&) = delete;
		Docking& operator=(Docking&&) = delete;

		// Adds a panel, after those of its zone that are there already. Throws std::invalid_argument if a panel has
		// its id.
		void add(Panel panel);

		// Makes the document the docking window of that name and lays it out.
		void attach(const std::string& name, Rml::ElementDocument& document);

		// The UI commands that show and hide zones and strips and go through tabs, with the reference's keys.
		void addKeyCommands(KeyCommands& keys);

		// The layout as ui.layout has it: what the user changed, without the defaults.
		const engine::Json& layout() const { return m_layout; }

		// Changes to the layout, applied at once and saved in ui.layout.
		// name is a zone, or a strip.
		void setVisible(const std::string& window, const std::string& name, bool visible);
		bool visible(const std::string& window, const std::string& name) const;
		void setSize(const std::string& window, const std::string& name, double dp);
		void showTab(const std::string& id);
		void stepTab(const std::string& window, const std::string& name, int step); // +1 the next, -1 the previous
		void moveTab(const std::string& id, const std::string& before);             // to that panel's place
		void setTabHidden(const std::string& id, bool hidden);
		void setFloating(const std::string& id, bool floating);
		// set is a set of sections, such as "channel"; before is the section whose place it takes.
		void moveSection(
			const std::string& window, const std::string& set, const std::string& id, const std::string& before);
		void setSectionHidden(const std::string& window, const std::string& set, const std::string& id, bool hidden);
		std::vector<std::string> sections(const std::string& window, const std::string& set) const; // in order
		// Shows the panel as its zone's active tab, with the zone shown, unless it is in a window of its own.
		void open(const std::string& id);

		// The window a panel shows in on its own, or null while it is docked.
		Window* windowOf(const std::string& panel);

	private:
		// Where a panel shows: a tab of its zone, or a window of its own.
		struct Shown
		{
			Rml::Element* element = nullptr;         // what create filled
			Rml::ElementPtr parked;                  // the element while its tab group is rebuilt
			std::shared_ptr<void> content;           // what create returned
			WindowId window = 0;                     // its window, if it has one
			Rml::ObserverPtr<Rml::Element> document; // that window's document
		};

		struct Section
		{
			std::string id;
			std::string label;
			bool hidden = false; // by default
		};

		struct Attached
		{
			Rml::ObserverPtr<Rml::Element> document;
			std::map<std::string, std::vector<std::string>> tabs; // the tabs each zone's tab group shows, in order
			std::map<std::string, std::vector<Section>> sections; // each set's, in the document's order
		};

		void ProcessEvent(Rml::Event& event) override;
		void listen(Rml::Element& document, bool on);
		void makeMenus(const std::string& name, Attached& attached);
		static std::vector<Rml::Element*> elements(Rml::Element& root, const std::string& selector);

		const Panel* panel(const std::string& id) const;
		engine::Json& zone(const std::string& window, const std::string& zone);
		Rml::Element* element(const std::string& window, const std::string& selector) const;
		std::string windowOf(const Rml::Element& element) const; // the docking window it is in
		std::string windowIn(Window& window) const;              // the docking window shown there
		std::vector<std::string> tabs(const std::string& window, const std::string& name) const; // in order
		std::vector<std::string> shownTabs(const std::string& window, const std::string& name) const;
		const Section* section(const std::string& window, const std::string& set, const std::string& id) const;
		bool sectionHidden(const std::string& window, const std::string& set, const std::string& id) const;
		engine::Json& sectionsJson(const std::string& window, const std::string& set);

		void apply();
		void applyWindow(const std::string& window, Attached& attached);
		void applyTabs(Attached& attached, Rml::Element& zone, const std::string& window, const std::string& zoneName);
		// Runs change in the shell's next step, unless the docking system is gone by then.
		void later(std::function<void()> change);
		void applyFloating(bool open);
		void save();
		void saveBounds();

		EngineBinding& m_binding;
		Shell& m_shell;
		std::filesystem::path m_panelWindow;
		bool m_hiddenWindows = false;
		int m_saving = 0; // layouts sent to the engine that it hasn't answered yet
		engine::Json m_layout = engine::Json::object();
		std::vector<Panel> m_panels;
		std::map<std::string, Attached> m_windows;
		std::map<std::string, Shown> m_shown; // by panel
		std::string m_contextTab;             // the tab last right-clicked
		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for the shell's tasks
	};
}
