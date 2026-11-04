// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

// The track list (#81): the project window's tracks beside the event display's lanes, with their controls.
// docs/features/track-list.md describes it.
namespace gnubase::ui
{
	// Fills an element of a document with a row per track of the event display's project, at its lanes' heights and
	// scrolled with them: a <listview multiple> with a tree row (<listitem>) per track, whose value is the track's id,
	// with its name (.track-name) and its controls (button.toggle with a control attribute) in its controls area
	// (.track-controls), in the order and choice of the user setting ui.trackControls, and the name and the controls
	// area as wide as ui.trackWidths says. The controls send engine commands; whichever client changes the project,
	// the rows show it. Use it on the UI thread; the display and the binding must outlive it. If the element goes,
	// the list shows nothing more.
	//
	// The list view selects tracks (click, Ctrl-click, Shift-click, which deselects a selected track, the arrow keys)
	// and folds folders. A double click
	// on a name renames the track; a drag of a row moves the selected tracks there, into a folder when dropped on the
	// middle of a folder's row; a drag of a row's lower edge sets its track's height, with Ctrl every track's; the
	// wheel scrolls the display.
	class TrackList final : private Rml::EventListener
	{
	public:
		TrackList(EventDisplay& display, EngineBinding& binding, Rml::Element& root);
		~TrackList() override;
		TrackList(const TrackList&) = delete;
		TrackList& operator=(const TrackList&) = delete;
		TrackList(TrackList&&) = delete;
		TrackList& operator=(TrackList&&) = delete;

		// The selected tracks, top to bottom, and selecting them (the list view's value).
		std::vector<uint64_t> selection() const;
		void select(const std::vector<uint64_t>& tracks);
		// The selected automation tracks' lanes (#125): a click on an automation track's row, beside its buttons,
		// selects it, Ctrl or Shift adds or removes it, a click on a track's own row deselects them.
		std::vector<uint64_t> selectedLanes() const;
		// Whether a selected folder has a group channel (withGroupChannel) or has none: when Disable or Enable Group
		// Channel has something to do.
		bool selectsFolder(bool withGroupChannel) const;

		// Folding (reference manual p. 225): the tracks inside a folded folder are hidden, in the event display too.
		bool folded(uint64_t folder) const { return m_folded.count(folder) != 0; }
		void setFolded(uint64_t folder, bool folded);

		// Starts renaming a track: its name becomes a text field, which Enter or leaving it sends, Escape drops.
		void rename(uint64_t track);

		// The user settings as the list last heard of them: ui.removeTracksWarning (Remove Selected Tracks asks
		// before it removes tracks that aren't empty, in the document's remove-tracks.rml) and
		// ui.upDownSelectsTracksOnly (Up and Down leave the event display's selection alone).
		bool warnsBeforeRemoving() const { return m_warnBeforeRemoving; }
		bool upDownSelectsTracksOnly() const { return m_upDownTracksOnly; }

		// The commands that key commands and menus bind to, by id: track.selectPrevious and track.selectNext (Up and
		// Down, in the list too, which also select the first event on the new track while the event display has one
		// selected, unless ui.upDownSelectsTracksOnly), track.rename (the first selected track),
		// track.removeSelected (asks first for tracks that aren't empty, see warnsBeforeRemoving), track.removeEmpty,
		// track.duplicateSelected, track.disableSelected, track.enableSelected, track.toggleFolding (the selected
		// folders), track.foldAll, track.unfoldAll and track.flipFolding; track.freezeSelected (freezes the selected
		// audio and instrument tracks, or unfreezes them if every one is frozen) and track.renderSelected (renders them
		// in place); and for folder tracks track.enableSelectedGroupChannels, track.disableSelectedGroupChannels,
		// track.moveSelectedToNewFolder and track.moveSelectedToNewFolderWithGroupChannel; and for automation tracks
		// (#121) track.toggleAutomationSelected (shows the selected tracks' automation, or hides it if every one shows
		// it), track.showUsedAutomation and track.hideAllAutomation; and for copying automation to other tracks between
		// the locators (#125) track.copyAutomationAll, track.copyAutomationSelected,
		// track.copyAutomationSelectedParameters (the selected automation tracks), track.pasteAutomationAll,
		// track.pasteAutomationSelected and track.pasteAutomationSelectedParameters.
		//
		// A track with a channel has Show/Hide Automation (.automation-toggle, .checked while shown) in its lower
		// left corner, and below its own row a .automation-row[lane] per automation track the event display shows:
		// its parameter (.automation-parameter), which opens the parameter selector #automation-parameter-menu of the
		// document with a search field, Remove Parameter, Remove Unused Parameters, Show Only Automation Tracks of a
		// plugin and the track's parameters; Read (.automation-read) and Append Automation Track (.automation-append).
		const std::map<std::string, std::function<void()>>& actions() const { return m_actions; }

	private:
		// A track as the list shows it, from the event display's project.
		struct Row
		{
			uint64_t id = 0;
			std::string type;
			uint64_t parent = 0;
			int level = 0;
			uint64_t channel = 0;
			uint64_t output = 0; // where its channel plays into, 0 for none
			bool holds = false;  // events, parts, markers or automation
		};

		enum class Drag : uint8_t
		{
			None,
			Pressed, // on a row, not moved yet
			Move,    // moves the selected tracks
			Height,  // a row's lower edge
		};

		void ProcessEvent(Rml::Event& event) override;
		bool alive() const { return static_cast<bool>(m_root); }
		float dpRatio() const;

		void layout();
		void build();     // the rows, from the project
		void showState(); // names, controls and state classes
		void place();     // heights and scrolling, from the display's lanes
		void hide();      // tells the display which tracks folded folders hide
		void loadControls(const engine::Json& value);
		void loadWidths(const engine::Json& value);

		const Row* rowOf(Rml::Element* element) const;
		Rml::Element* rowElement(uint64_t track) const;
		const engine::Json* channelOf(const Row& row) const;
		std::vector<std::string> controlsOf(const Row& row) const;
		// A click on a track control; solo takes Ctrl (exclusive) and Alt (solo defeat), see soloClick.
		void control(const Row& row, const std::string& name, bool exclusive = false, bool defeat = false);
		void step(int direction);
		void reveal(uint64_t track); // scrolls the display so that the track's lane shows
		void finishRename(bool keep);

		void mouseDown(Rml::Event& event);
		void mouseMove(Rml::Event& event);
		void mouseUp(Rml::Event& event);
		// Where a drag at y (window pixels) drops: the folder to go into and the track to go before (0: the end).
		std::pair<uint64_t, uint64_t> dropAt(float y) const;
		void move(uint64_t parent, uint64_t before);

		// Whether a display lane is a track's own, not one of its lanes (#99) or automation tracks; the display lane
		// after a track's row, which reaches over its lanes and automation tracks.
		bool ownLane(size_t lane) const;
		size_t rowEnd(size_t lane) const;

		// Automation tracks (#121): the rows below a track's own, at its automation lanes' heights.
		void showAutomationRows(Rml::Element& row, size_t lane, size_t end); // display lanes lane+1 to end
		const engine::Json* laneOf(uint64_t lane) const;                     // as project.get has it
		const engine::Json* targetOf(const engine::Json& lane) const;        // its automationLane.targets entry
		void openParameters(uint64_t lane);                // fills the parameter selector for the automation track
		uint64_t laneAt(Rml::Element* element) const;      // the lane of the automation row it is in, or 0
		void selectLane(Rml::Event& event, uint64_t lane); // a mouse press on an automation row, or a track's (0)
		void showLaneSelection();
		void parameterEvent(Rml::Event& event);

		// The selected tracks without those inside other selected folders.
		std::vector<uint64_t> topSelection() const;
		// The tracks Remove Empty Tracks removes, with those that are only empty once the others went.
		std::set<uint64_t> emptyTracks() const;
		// Remove Selected Tracks: removes the tracks, after asking if they, or tracks inside them, hold anything.
		void removeSelected();
		void removeTracks(const std::vector<uint64_t>& tracks);
		void closedRemoveDialog(Rml::Event& event);
		// Runs the commands as one undo step.
		void transaction(const std::string& description, engine::Json commands);

		EventDisplay& m_display;
		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::Element* m_document = nullptr;
		Rml::Element* m_rows = nullptr; // clips the list view
		Rml::Element* m_view = nullptr; // the <listview>
		Rml::Element* m_dropLine = nullptr;
		Rml::Element* m_parameterMenu = nullptr; // the document's #automation-parameter-menu
		uint64_t m_parameterLane = 0;            // the automation lane it was opened for
		std::set<uint64_t> m_selectedLanes;      // the selected automation tracks' lanes
		std::vector<Row> m_tracks;               // every track in the project's order
		std::map<uint64_t, size_t> m_index;
		std::set<uint64_t> m_folded;
		engine::Json m_controls = engine::Json::object(); // ui.trackControls
		engine::Json m_widths = engine::Json::object();   // ui.trackWidths
		uint64_t m_loads = 0;                             // the display's, when the rows were last built
		bool m_layingOut = false;
		uint64_t m_renaming = 0;
		std::string m_renamed;                         // the name before renaming
		bool m_warnBeforeRemoving = true;              // ui.removeTracksWarning
		bool m_upDownTracksOnly = false;               // ui.upDownSelectsTracksOnly
		Rml::ObserverPtr<Rml::Element> m_removeDialog; // remove-tracks.rml while open
		std::vector<uint64_t> m_removing;              // the tracks it asks about

		Drag m_drag = Drag::None;
		uint64_t m_dragTrack = 0;
		float m_dragY = 0.0F;      // where it started, in the window's pixels
		double m_dragHeight = 0.0; // the track's height then, before zoom
		bool m_dragAll = false;    // Ctrl: every track's height
		uint64_t m_dropParent = 0; // where a move drops
		uint64_t m_dropBefore = 0;

		std::map<std::string, std::function<void()>> m_actions;
		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
	};
}
