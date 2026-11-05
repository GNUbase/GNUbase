// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <memory>
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
	// program by the user setting midi.devices), inserts (the channel's plugin slots)
	// and notepad (the channel's notes in a <textarea>). A click on a section's header closes or opens it (.collapsed);
	// what a track doesn't have gets the class unavailable. The controls send engine commands. The element has the
	// class empty without a track. Beside the name, Edit Channel Settings (button.inspector-edit) runs editChannel
	// with the track, for tracks with a channel. Use it on the UI thread; the binding must outlive it.
	class Inspector final : private Rml::EventListener
	{
	public:
		Inspector(EngineBinding& binding, Rml::Element& root, std::function<void(uint64_t track)> editChannel = {});
		~Inspector() override;
		Inspector(const Inspector&) = delete;
		Inspector& operator=(const Inspector&) = delete;
		Inspector(Inspector&&) = delete;
		Inspector& operator=(Inspector&&) = delete;

		// Shows a track (0: none) of the project as project.get returned it, the loads-th answer; call it once per
		// step, it does nothing while neither changed. The project must stay until the next call.
		void update(uint64_t track, const engine::Json& project, uint64_t loads);

	private:
		void ProcessEvent(Rml::Event& event) override;
		void show();
		void change(Rml::Element& control, const Rml::Event& event);
		void saveNotes();
		// Sends a change of the channel (channel.set).
		void sendChannel(engine::Json parameters);
		// What a command does when it is done: if it was refused, the next update shows the channel again.
		std::function<void(const engine::Result&)> whenRefused();
		// Ends what is being edited without taking it, such as notes being typed.
		void dropEdits();

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::function<void(uint64_t track)> m_editChannel;
		uint64_t m_track = 0;
		uint64_t m_loads = 0;
		const engine::Json* m_project = nullptr;         // update's
		engine::Json m_devices = engine::Json::object(); // the user setting midi.devices
		engine::Json m_channel;                          // the track's, null without one
		bool m_showing = false;        // while show sets the controls, whose changes aren't the user's
		bool m_dropping = false;       // while dropEdits ends edits, which then aren't taken
		bool m_stale = false;          // a change was refused: show again
		Subscription m_reset;          // project.reset
		Subscription m_devicesChanged; // settings.changed with midi.devices
		bool m_devicesKnown = false;   // settings.changed came before settings.get's answer
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
	};
}
