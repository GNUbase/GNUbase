// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

namespace gnubase::ui
{
	// The chord pads zone (#174, reference manual pp. 1346-1391, docs/features/chord-pads.md), a panel of the lower
	// zone: its toolbar (step input and Move Insert Mode, the root key with Transpose Pads, the presets with what to
	// load, Assign from Chord Track, the velocity of mouse clicks, the pad remote control with its range, the basic
	// player's octave offset and Show/Hide Editor/Assistant), its extended toolbar (Select All, Open Chord Editor,
	// Remove Assignment and the pad modifiers and pad state modifiers on the selected pads), the pads, each with its
	// chord, its degree in the root key and its remote key, and the chord assistant with its List and Circle of Fifths
	// pages. It shows chordPads.get, whichever client changes the pads, and changes them with the chordPads.* commands.
	//
	// A press on a pad selects it (Ctrl toggles it) and triggers it until the button goes up (chordPads.trigger), or
	// with step input on inserts its chord (chordPads.step); released over another pad it swaps the two, with Alt it
	// copies onto it (chordPads.move). A double click, or Open Chord Editor, opens the chord field, where Enter assigns
	// the chord typed. The wheel over a pad steps through its voicings, with Shift transposes it, with Alt changes its
	// tensions; over the root key it moves the key. With step input, Left and Right Arrow on a pad move the step
	// position by a bar. An assistant's chord plays while it is pressed; a double click assigns it to the first
	// unassigned pad. Use it on the UI thread; the binding must outlive it.
	class ChordPadsPanel final : private Rml::EventListener
	{
	public:
		// dropOn, if given, finds the track and the position (ticks) at a point of the window (pixels) where a pad
		// dragged out of the zone was released, such as on the event display's lanes.
		using DropTarget = std::function<std::optional<std::pair<uint64_t, int64_t>>(float x, float y)>;
		// selectedEvents, if given, gives the event display's selected events and parts, of which the first MIDI part
		// becomes the pattern with From Selected Part.
		using Selection = std::function<std::vector<uint64_t>()>;
		ChordPadsPanel(
			EngineBinding& binding, Rml::Element& element, DropTarget dropOn = {}, Selection selectedEvents = {});
		~ChordPadsPanel() override;
		ChordPadsPanel(const ChordPadsPanel&) = delete;
		ChordPadsPanel& operator=(const ChordPadsPanel&) = delete;
		ChordPadsPanel(ChordPadsPanel&&) = delete;
		ChordPadsPanel& operator=(ChordPadsPanel&&) = delete;

		// The selected pads, in order.
		const std::set<int>& selection() const { return m_selection; }

	private:
		void ProcessEvent(Rml::Event& event) override;
		void request();
		void show(const engine::Json& pads);
		void showSelection();
		void showAssistant();
		void press(int pad, const Rml::Event& event);
		void release(Rml::Element* target, const Rml::Event& event);
		void action(const std::string& name);
		void change(Rml::Element& target, const Rml::Event& event);
		void wheel(int pad, const Rml::Event& event);
		void wheelRootKey(const Rml::Event& event);
		void openField(int pad);
		void assignTyped();
		void assignFirstFree(const std::string& chord);
		void keyCommand(Rml::Event& event, const Rml::Element* target);
		void showDisplay();
		void showKeyboard();
		void showListColors();
		void requestPresets();
		void requestPorts();
		void requestTracks();
		void showPlayerTrack();
		void showEditor();
		void showRecent();
		void pressEditor(const Rml::Element& button);
		void modifyPressed(const std::string& name);
		static bool modifies(const std::string& name);
		std::vector<int> selected() const;
		void send(const std::string& command, engine::Json parameters);
		Rml::Element* find(const std::string& id) const;

		EngineBinding& m_binding;
		DropTarget m_dropOn;
		Selection m_selectedEvents;
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::ObserverPtr<Rml::Element> m_document; // for the mouse button going up outside the panel
		engine::Json m_pads = engine::Json::object();
		std::set<int> m_selection;
		int m_pressed = -1;                              // the pad the mouse button went down on, until it goes up
		std::string m_auditioned;                        // the assistant's chord that plays while pressed
		int m_editing = -1;                              // the pad whose chord the field types
		std::string m_page = "list";                     // the assistant's page: list or circle
		std::optional<int> m_rootKeySent;                // the root key the wheel sent last, until the pads show it
		int m_rootKeysSending = 0;                       // the wheel's root keys that the engine hasn't answered yet
		engine::Json m_display = engine::Json::object(); // the user setting ui.chordPads
		engine::Json m_presets = engine::Json::array();  // chordPads.presets
		engine::Json m_ports = engine::Json::array();    // the MIDI inputs, for the remote's MIDI Input
		widgets::Options m_tracks;                       // the MIDI and instrument tracks, for a player's track
		std::vector<std::string> m_recent;               // the last three chords auditioned in the assistant
		engine::Json m_listening;                        // what chordPads.listen listens for, or null
		std::map<int, std::vector<int>> m_sounding;      // the pitches each pad plays (-1: an audition)
		int m_hovered = -1;                              // the pad under the mouse
		int m_auditionPad = -1;         // the pad that a pad modifier button's press plays until the button goes up
		bool m_pressedModifier = false; // a pad modifier button acted on its press, so its click does nothing
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
		std::vector<Subscription> m_subscriptions;
	};
}
