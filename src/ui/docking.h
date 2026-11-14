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
#include <set>
#include <string>
#include <vector>

namespace Rml
{
	class Context;
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
	//   zone="main|channel|left|..."  a zone; every zone but main gets a <tabset> with its panels. size="240" and
	//                                 hidden give its default size in dp and visibility. Its size is its width, or
	//                                 its height in a flex column. tabbar="bottom" puts its tabs at its bottom.
	//   splitter for="left"           the splitter that sizes that zone or resizable element (resize="next" for one
	//                                 after it); hidden with the zone, and a double click hides the zone
	//   resizable="trackList"         an element inside a zone that a splitter sizes, such as the track list; size
	//                                 gives its default size in dp, its width, or its height in a flex column
	//   strip="infoLine"              a strip, such as the info line; hidden gives its default. The body has the
	//                                 class no-<strip> while it is hidden, for strips that are part of other elements
	//                                 such as the event display's overview line
	//   toggle="left|infoLine"        a button or menu item that shows or hides the zone or strip; checked follows it
	//   hide="lower"                  a button that hides the zone or strip, such as Close Lower Zone
	//   sections="channel"            a container of sections of that set, such as each of the mixer's channels; its
	//                                 children with section="eq" (label="EQ", default="hidden") are the sections, in
	//                                 the user's order and hidden with the class .hidden. The first container says
	//                                 which sections a set has and their default order; containers added later go
	//                                 through arrangeSections
	//   fit="room"                    a container of sections that shows only those it has room for: the shown
	//                                 sections before its .spacer that don't fit in its width get the state class
	//                                 .no-room, the last first, and show again once it has room for them. Its style
	//                                 must keep the widths they need, so they stay laid out, out of the flow
	//   fit="spare"                   an element in a section of a fit="room" container that goes before any
	//                                 section does, the last one first: it gets .no-room while the sections fit only
	//                                 without it, and its style keeps its width the same way
	//   setup="channel"               a setup pane's list of the set's sections, which the docking system fills with a
	//                                 toggle button per section (setup-section="eq"); a button dragged onto another
	//                                 moves its section there
	//   setup-all="channel"           a button that shows every section of the set (Show All)
	//   setup-reset="channel"         a button that gives the set its default order and hidden sections (Reset All)
	//   setup-presets="channel"       the set's presets from the user setting ui.setupPresets, which the docking system
	//                                 fills with a button per preset (setup-preset="name") that recalls it
	//   setup-preset-save="channel"   a button that names the set's order and hidden sections as a preset in a dialog
	//                                 (setup-preset.rml next to the panel window's document) and saves it
	//
	// Zones get the state class .hidden when hidden and .focused while the keyboard focus is in them; strips get
	// .hidden. A tab is dragged onto another to move it there; a right click on the tabs opens the zone's tab menu,
	// with Open in Separate Window and an item to show or hide each tab. A panel in a window of its own has a Dock
	// button (#dock), and closing the window docks it too. A docking window's or a panel window's document that live
	// reload loads again (ui/livereload.h) is attached in its place: what the panels' create returned for the old one
	// goes first, then they fill the new one. Use it on the UI thread; the engine, the binding and the shell must
	// outlive it.
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
		// name is a zone, or a resizable element.
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
		// Reset All: the set's default order and hidden sections.
		void resetSections(const std::string& window, const std::string& set);
		// The set's presets in ui.setupPresets, which all windows share: saving one replaces that of the same name;
		// recalling one that isn't there does nothing. Sections a preset lacks keep their defaults' order after its.
		void savePreset(const std::string& window, const std::string& set, const std::string& name);
		void recallPreset(const std::string& window, const std::string& set, const std::string& name);
		// Arranges containers of sections added or rebuilt after attach, such as a new mixer channel: element, and the
		// containers and setup panes in it, get the user's order and hidden sections. A set the window doesn't know yet
		// is learned from its first container there, which fills the set's setup panes. Call it once element is in a
		// docking window's document; anything else is left alone. A docked panel's content needn't: it is arranged when
		// the panel shows.
		void arrangeSections(Rml::Element& element);
		// Shows the panel as its zone's active tab, with the zone shown; a panel in a window of its own stays there,
		// and its window comes to the front with the keyboard focus, restored if it is minimized.
		void open(const std::string& id);

		// The window a panel shows in on its own, or null while it is docked.
		Window* windowOf(const std::string& panel);

		// A top-level window of the app that ui.layout keeps under windows, by name (#163): whether it is open (open,
		// closed by default), its bounds and its monitor's name. current gives its window, 0 or one that is gone while
		// it is closed; setOpen opens or closes it, and is empty for the main window, which is always open and takes
		// its bounds from ui.layout at once. ui.layout gets a window's bounds once it opens, moves or resizes, and
		// whether it is open when that changes. Another client's ui.layout, such as a workspace's
		// (workspace.activate), opens, closes and places the windows as it says.
		void addWindow(
			const std::string& name, std::function<WindowId()> current, std::function<void(bool open)> setOpen = {});
		// Where the window should open: its bounds in ui.layout, on a connected monitor (restore in ui/displays.h);
		// none if ui.layout has none.
		std::optional<Rect> bounds(const std::string& name) const;
		// Puts into ui.layout now, rather than at the next check twice a second, which windows are open and where they
		// and the panels in windows of their own are, e.g. right after a window opened or closed.
		void saveWindows();
		// The width the app fitted the window to, kept in its ui.layout entry as fittedWidth: a window whose bounds
		// still have that width wasn't sized by the user since, e.g. a transport panel that follows its sections
		// (#1527). None if ui.layout has none.
		std::optional<int> fittedWidth(const std::string& name) const;
		void setFittedWidth(const std::string& name, int width);

		// Whether a panel shows: in a window of its own, or as its zone's active tab while the zone shows. What create
		// returned stays while the panel's tab is hidden behind another one or its zone is, e.g. to refresh it only
		// once it shows again.
		bool showing(const std::string& id) const;

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
			std::vector<Rml::ObserverPtr<Rml::Element>> fits;     // its containers with fit="room"
		};

		void ProcessEvent(Rml::Event& event) override;
		// Gives the keyboard focus to the zone step zones on from the one from is in, among the shown zones of its
		// document, around the ends; false if the document has none.
		static bool focusZone(Rml::Element& from, int step);
		void listen(Rml::Element& document, bool on);
		void makeMenus(const std::string& name, Attached& attached);
		static std::vector<Rml::Element*> elements(Rml::Element& root, const std::string& selector);

		const Panel* panel(const std::string& id) const;
		engine::Json& zone(const std::string& window, const std::string& zone);
		// Where ui.layout has the size of a zone or a resizable element.
		engine::Json& size(const std::string& window, const std::string& name);
		Rml::Element* element(const std::string& window, const std::string& selector) const;
		std::string windowOf(const Rml::Element& element) const; // the docking window it is in
		std::string windowIn(Window& window) const;              // the docking window shown there
		std::vector<std::string> tabs(const std::string& window, const std::string& name) const; // in order
		std::vector<std::string> shownTabs(const std::string& window, const std::string& name) const;
		// The index of the zone's active tab in shown, its shownTabs: the one ui.layout names, or the first.
		size_t activeTab(
			const std::vector<std::string>& shown, const std::string& window, const std::string& name) const;
		const Section* section(const std::string& window, const std::string& set, const std::string& id) const;
		bool sectionHidden(const std::string& window, const std::string& set, const std::string& id) const;
		engine::Json& sectionsJson(const std::string& window, const std::string& set);
		// Learns the sets of sections that the containers in root, root too, are the first of, and fills their setup
		// panes. Whether it learned one.
		static bool learnSections(Attached& attached, Rml::Element& root);
		// The elements in root, root too, whose attribute name has the value, or any value if it is empty.
		static std::vector<Rml::Element*> elementsWith(Rml::Element& root, const char* name, const std::string& value);

		void apply();
		void applyWindow(const std::string& window, Attached& attached);
		void applyTabs(Attached& attached, Rml::Element& zone, const std::string& window, const std::string& zoneName);
		// Gives a zone or resizable element its size from ui.layout, or its default.
		static void applySize(Rml::Element& element, const engine::Json* saved);
		void applySections(const std::string& window, const Attached& attached, Rml::Element& root);
		// Gives the sections of the fit="room" containers in the context's shown documents the room they have, once
		// the context laid them out to draw them, and says whether that changed any; fit does one container.
		bool fitSections(Rml::Context& context);
		static bool fit(Rml::Element& container);
		// Runs change in the shell's next step, unless the docking system is gone by then.
		void later(std::function<void()> change);
		void applyFloating(bool open);
		// Makes the document the panel's window of its own: its title, its listeners and the panel's content in its
		// #panel-content. False if it has none, and then shows nothing.
		bool fillPanelWindow(const Panel& panel, Shown& shown, Rml::ElementDocument& document);
		// Puts loaded, a document that live reload loaded again, in the place of old, the docking window or panel
		// window it was.
		void reload(Rml::ElementDocument& old, Rml::ElementDocument& loaded);
		void save();
		// Opens, closes and places the app's windows as ui.layout says.
		void applyWindows();
		// Save Preset's dialog for the set, in the context of the setup pane's document; its close event saves it.
		void openPresetDialog(Rml::Context& context, const std::string& window, const std::string& set);
		void presetDialogClosed(Rml::Event& event);

		EngineBinding& m_binding;
		Shell& m_shell;
		std::filesystem::path m_panelWindow;
		bool m_hiddenWindows = false;
		Rml::String
			m_reloadEvent; // g_documentReloadEvent, made once: the destructor removes listeners without allocating
		Rml::String m_closeEvent;            // g_dialogCloseEvent, made once, as m_reloadEvent
		std::set<engine::RequestId> m_saves; // layouts sent to the engine that it hasn't answered yet
		engine::Json m_layout = engine::Json::object();
		engine::Json m_presets = engine::Json::object(); // ui.setupPresets
		Rml::ObserverPtr<Rml::Element> m_presetDialog;   // Save Preset's, while it is open
		std::string m_presetWindow;                      // and the window and set it saves
		std::string m_presetSet;
		std::vector<Panel> m_panels;
		std::map<std::string, Attached> m_windows;
		std::map<std::string, Shown> m_shown; // by panel
		// The app's windows that ui.layout keeps, with whether each was open and where, as saveWindows last saw it.
		struct AppWindow
		{
			std::string name;
			std::function<WindowId()> current;
			std::function<void(bool open)> setOpen;
			bool open = false;
			Rect bounds; // its normal bounds, also while it is maximized
			bool maximized = false;
		};
		// Whether ui.layout has the app's window maximized.
		bool savedMaximized(const std::string& name) const;
		std::vector<AppWindow> m_appWindows;
		std::string m_contextTab; // the tab last right-clicked
		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for the shell's tasks
	};
}
