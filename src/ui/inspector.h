// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

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
	// effects and control panels), inserts (the channel's plugin slots) and notepad (the channel's
	// notes in a <textarea>). A click on a section's header closes or opens it (.collapsed); what a track doesn't have
	// gets the class unavailable. The controls send engine commands. The element has the class empty without a track.
	// Beside the name, Edit Channel Settings (button.inspector-edit) runs editChannel with the track, for tracks with a
	// channel. Use it on the UI thread; the binding must outlive it.
	class Inspector final : private Rml::EventListener
	{
	public:
		// editChannel opens a track's channel settings, setupArticulations the articulation map setup (#175),
		// openInputTransformer the project input transformer (project) or the track's (#966).
		Inspector(EngineBinding& binding, Rml::Element& root, std::function<void(uint64_t track)> editChannel = {},
			std::function<void(uint64_t track)> setupArticulations = {},
			std::function<void(uint64_t track, bool project)> openInputTransformer = {});
		~Inspector() override;
		Inspector(const Inspector&) = delete;
		Inspector& operator=(const Inspector&) = delete;
		Inspector(Inspector&&) = delete;
		Inspector& operator=(Inspector&&) = delete;

		// Shows a track (0: none) of the project as project.get returned it, the loads-th answer, which the binding
		// handed on as EngineBinding::handed() returned read; call it once per step, it does nothing while neither
		// changed. The project must stay until the next call.
		void update(uint64_t track, const engine::Json& project, uint64_t loads, uint64_t read);

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
		// Shows the MIDI inserts' divider at a place, 0 above the first slot to 4 below the last.
		void showDivider(int gap);
		// A drag of the divider: it follows the mouse, and its end sends where it went (channel.set midiInserts).
		void dragDivider(Rml::Element& target, const Rml::Event& event);
		void saveNotes();
		// Sends a change of the channel (channel.set).
		void sendChannel(engine::Json parameters);
		// What a command does when it is done: if it was refused, the next update shows the channel again.
		std::function<void(const engine::Result&)> whenRefused();
		// Sends a value field's change as a step of its gesture (EngineBinding::sendStep) under key, and keeps the
		// value that the field shows with it (shown).
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

		EngineBinding& m_binding;
		const std::string m_stepOwner; // begins the whats of its steps (EngineBinding::stepOwner)
		Rml::ObserverPtr<Rml::Element> m_root;
		std::function<void(uint64_t track)> m_editChannel;
		std::function<void(uint64_t track)> m_setupArticulations;
		std::function<void(uint64_t track, bool project)> m_openInputTransformer;
		uint64_t m_track = 0;
		uint64_t m_loads = 0;
		uint64_t m_read = 0;
		// sendFieldStep's values, by key. ponytail: never pruned, one short entry per field and channel the user
		// changed.
		std::map<std::string, std::string> m_sent;
		const engine::Json* m_project = nullptr;            // update's
		engine::Json m_devices = engine::Json::object();    // the user setting midi.devices
		engine::Json m_channel;                             // the track's, null without one
		engine::Json m_midiEffects = engine::Json::array(); // midiEffect.list's
		bool m_showing = false;        // while show sets the controls, whose changes aren't the user's
		bool m_dropping = false;       // while dropEdits ends edits, which then aren't taken
		bool m_stale = false;          // a change was refused: show again
		int m_dividerTo = -1;          // while the divider is dragged, where it goes
		engine::Json m_dividerChannel; // and the id of the channel whose divider it is
		Subscription m_reset;          // project.reset
		Subscription m_devicesChanged; // settings.changed with midi.devices
		bool m_devicesKnown = false;   // settings.changed came before settings.get's answer
		std::set<uint64_t> m_inPlace;  // the tracks whose in-place editors are open (#131)
		Subscription m_inPlaceChanged; // editor.changed of in-place editors
		Subscription m_historyChanged; // history.changed, for m_inPlace
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
	};
}
