// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/mixer.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

// The project window's inspector (#89, reference manual pp. 76-79, 167-188): docs/features/inspector.md describes it.
namespace gnubase::ui
{
	// Fills an element with the inspector of a track: its name (<textdisplay class="inspector-name">) above the
	// sections of the set "inspector" (docs/features/docking.md): basics (mute, solo, record and monitor buttons, and
	// volume and pan value fields), routing (input and output drop-down lists, and a MIDI track's device, bank and
	// program by the user setting midi.devices, and a MIDI or instrument track's input transformer: Off, Project, Track
	// and, while it is on, Open Panel...), MIDI modifiers (MIDI and instrument tracks: bypass, value fields and
	// the random generators' and ranges' targets), MIDI inserts and sends (MIDI and instrument tracks: their slots'
	// effects and control panels), note expression (MIDI and instrument tracks: bypass, input assignments, MIDI Learn
	// and ranges), Arranger Track (the arranger track: its chain and events lists and jump mode, see
	// showArranger), inserts (the channel's plugin slots) and notepad (the channel's
	// notes in a <textarea>). A click on a section's header closes or opens it (.collapsed); what a track doesn't have
	// gets the class unavailable. The controls send engine commands. The element has the class empty without a track.
	// Beside the name, Edit Channel Settings (button.inspector-edit) runs editChannel with the track, for tracks with a
	// channel. Use it on the UI thread; the binding must outlive it.
	class Inspector final : private Rml::EventListener
	{
	public:
		// editChannel opens a track's channel settings, setupArticulations the articulation map setup (#175),
		// openInputTransformer the project input transformer (project) or the track's (#966), openArranger the
		// arranger editor (Open Arranger Editor on the Arranger Track section).
		Inspector(EngineBinding& binding, Rml::Element& root, std::function<void(uint64_t track)> editChannel = {},
			std::function<void(uint64_t track)> setupArticulations = {},
			std::function<void(uint64_t track, bool project)> openInputTransformer = {},
			std::function<void()> openArranger = {});
		~Inspector() override;
		Inspector(const Inspector&) = delete;
		Inspector& operator=(const Inspector&) = delete;
		Inspector(Inspector&&) = delete;
		Inspector& operator=(Inspector&&) = delete;

		// Shows the first of the tracks (none when it is empty) of the project as project.get returned it, the loads-th
		// answer, which the binding handed on as EngineBinding::handed() returned read; call it once per step, it does
		// nothing while neither changed. The Track Versions section works on every one of the tracks that has versions.
		// The project must stay until the next call.
		void update(const std::vector<uint64_t>& tracks, const engine::Json& project, uint64_t loads, uint64_t read);

	private:
		void ProcessEvent(Rml::Event& event) override;
		void show();
		void change(Rml::Element& control, const Rml::Event& event);
		// The MIDI Modifiers section (pp. 1099-1102): shows the channel's modifiers, and sends a control's change
		// (channel.set modifiers).
		void showModifiers(const engine::Json& modifiers);
		void changeModifier(const std::string& control, const Rml::Event& event);
		// The MIDI Inserts and MIDI Sends sections (pp. 1104-1106): show a section's slots, their effects and their
		// control panels, and send a control's change (channel.set midiInserts and midiSends).
		void showMidiEffects(const std::string& section, const engine::Json& rack);
		void changeMidiEffect(const std::string& control, const Rml::Event& event);
		// The Note Expression section (pp. 1300-1302): shows the channel's noteExpression with the expression selected
		// (m_expression) in Parameter Details, and sends a change of that one's input (noteExpression.set).
		void showNoteExpression(const engine::Json& setup);
		void sendExpressionInput(engine::Json change);
		// Turns MIDI Learn on or off, listening to the MIDI inputs (midi.activityListen) while it is on.
		void learn(bool on);
		// Takes the MIDI inputs that aren't among all MIDI inputs, which MIDI Learn leaves out, from midi.ports.
		void readPorts(const engine::Json& ports);
		// The Arranger Track section (pp. 405, 410-411): a row per step of the active chain and per arranger event,
		// each with its arrow, the one playing .current, and the jump mode. A step's name is a disabled textdisplay
		// that Alt and a double click open (p. 405).
		void showArranger();
		// Sends a step's repeats or repeat mode typed or chosen in the Current Arranger Chain list (arranger.setChain).
		void changeStep(Rml::Element& control, const std::string& name, const std::string& value);
		void followArranger(const engine::Json& status); // transport.get's or transport.changed's arranger
		void readArrangerPlaying();                      // transport.get, into followArranger
		// The Track Versions section (pp. 231-232): the active version's name and ID, and the list of them all.
		void showVersions(const engine::Json& track);
		// The selected tracks that have versions: a version command lists only those, since the engine refuses one
		// that names a track without any.
		std::vector<uint64_t> versioned() const;
		// A version command on every selected track that has versions, one undo step.
		void versionCommand(const char* command);
		// Shows the MIDI inserts' divider at a place, 0 above the first slot to 4 below the last.
		void showDivider(int gap);
		// A drag of the divider: it follows the mouse, and its end sends where it went (channel.set midiInserts).
		void dragDivider(Rml::Element& target, const Rml::Event& event);
		void saveNotes();
		// Sends a change of the channel (channel.set).
		void sendChannel(engine::Json parameters);
		// What a command does when it is done: if it was refused, the next update shows the channel again.
		std::function<void(const engine::Result&)> whenRefused();
		// Sends a value field's change as a step of its gesture (EngineBinding::sendStep) under key, with the value
		// that the field shows (shown), which the binding keeps with the step's record. Once the last step is answered,
		// refused or not, the next update shows the channel again.
		void sendFieldStep(
			int gesture, const std::string& key, float shown, std::string description, engine::Json commands);
		// What a value field shows for the project's value: while the project is older than the field's own steps for
		// that channel (#716), the value that the field sent last for it instead, also after another track showed in
		// the same field meanwhile (#1421).
		std::string fieldValue(const std::string& key, std::string value) const;
		// Ends what is being edited without taking it, such as notes being typed.
		void dropEdits();
		// Reads the open in-place editors into m_inPlace (editor.inPlace).
		void readInPlace();
		// Drops the step or arranger event pressed in the Arranger Track section's lists where the button went up:
		// steps dragged move, with Alt copy, and events go into the chain (reference manual pp. 409-410).
		void dropOnChain(const Rml::Event& event);

		EngineBinding& m_binding;
		const std::string m_stepOwner; // begins the whats of its steps (EngineBinding::stepOwner)
		Rml::ObserverPtr<Rml::Element> m_root;
		std::function<void(uint64_t track)> m_editChannel;
		std::function<void(uint64_t track)> m_setupArticulations;
		std::function<void(uint64_t track, bool project)> m_openInputTransformer;
		std::function<void()> m_openArranger;
		uint64_t m_track = 0;
		std::vector<uint64_t> m_selected; // the tracks the inspector shows, the first one's details
		uint64_t m_loads = 0;
		uint64_t m_read = 0;
		const engine::Json* m_project = nullptr;            // update's
		engine::Json m_devices = engine::Json::object();    // the user setting midi.devices
		engine::Json m_channel;                             // the track's, null without one
		engine::Json m_midiEffects = engine::Json::array(); // midiEffect.list's
		bool m_showing = false;  // while show sets the controls, whose changes aren't the user's
		bool m_dropping = false; // while dropEdits ends edits, which then aren't taken
		SoloHold m_soloHold;
		bool m_stale = false;          // a change was refused: show again
		int m_dividerTo = -1;          // while the divider is dragged, where it goes
		engine::Json m_dividerChannel; // and the id of the channel whose divider it is
		Subscription m_reset;          // project.reset
		Subscription m_devicesChanged; // settings.changed with midi.devices
		bool m_devicesKnown = false;   // settings.changed came before settings.get's answer
		std::set<uint64_t> m_inPlace;  // the tracks whose in-place editors are open (#131)
		Subscription m_inPlaceChanged; // editor.changed of in-place editors
		Subscription m_historyChanged; // history.changed, for m_inPlace
		engine::Json m_arranger;       // the last arranger.changed, which arranger.setMode sends without a project read
		uint64_t m_arrangerHanded = 0; // when it was handed on (EngineBinding::handed)
		engine::Json m_arrangerPlaying = engine::Json::object(); // transport.changed's arranger
		Subscription m_arrangerChanged;
		engine::Json m_arrangerChain = engine::Json::object(); // the active chain's id and steps, as the list shows
		Rml::ObserverPtr<Rml::Element> m_stepMenu;             // the chain list's context menu
		Rml::ObserverPtr<Rml::Element> m_retrospectiveMenu;    // Retrospective Recording's pop-up menu
		Rml::ObserverPtr<Rml::Element> m_expressionMenu;       // Note Expression's Show Additional Functions
		int m_touchedStep = -1;                                // the step right-clicked, for Remove Touched
		std::vector<int> m_selectedSteps; // the steps selected, by index, for Remove Selected; a chain edit clears them
		int m_stepAnchor = -1;            // the step clicked last, where Shift selects from
		// A step or an arranger event pressed in the lists, which a release over the chain list drops there.
		std::optional<int> m_stepPressed;
		std::optional<uint64_t> m_eventPressed;
		Subscription m_transportChanged;
		size_t m_expression = 0; // the Note Expression section's expression selected, by plugin::NoteExpression
		bool m_learning = false; // its MIDI Learn is on
		bool m_onlyUsed = false; // its Show only used Expressions is on (#2120)
		std::set<std::string> m_usedExpressions; // the expressions the track's notes have points of
		Subscription m_activity;                 // midi.activity, for MIDI Learn
		std::set<std::string> m_notInAllInputs;  // "backend id" of the MIDI inputs that aren't among all MIDI inputs
		Subscription m_portsChanged;             // midi.changed, for them
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
	};
}
