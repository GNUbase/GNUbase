// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/enginebinding.h"
#include "ui/keycommands.h"
#include "ui/windowsystem.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

// The on-screen keyboard (#103, reference manual pp. 332-334, docs/features/onscreen-keyboard.md).
namespace gnubase::ui
{
	// The on-screen keyboard window: a piano keyboard of 29 keys, C to the E two octaves up, that the mouse and two
	// rows of the computer keyboard play, by the keys' places whatever the layout (on a US keyboard Z S X D C V G B H
	// N J M for the lower octave, Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P from the next C up; comma plays that C too), each
	// key labelled with what the layout has at its computer key's place, with the octave (#octave, 0 to 6: the lowest
	// key is C0 to C6, C3 being middle C) and the velocity (#velocity, 1 to 127) of the notes. It fills #white-keys and
	// #black-keys with the keys, .white-key and .black-key with the attribute note (0 to 28) and the state class
	// .pressed while they sound, and plays them through the engine command midi.keyboard, one of all MIDI inputs:
	// tracks that monitor or are record-enabled play and record them. Use it on the UI thread; the binding and the
	// settings must outlive it.
	class OnScreenKeyboard final : private Rml::EventListener
	{
	public:
		// The octave and velocity, which it sends as the user setting ui.onScreenKeyboard when they changed and the
		// mouse button or a key goes up, as a slider's drag ends, or the window closes.
		struct Settings
		{
			int octave = 3;
			int velocity = 100;
		};

		static constexpr int g_keys = 29;

		// label gives what the keyboard layout has at a computer key's place (Shell::keyLabel), for the keys' labels;
		// the US keyboard's where it gives nothing.
		OnScreenKeyboard(EngineBinding& binding, Rml::ElementDocument& document, Settings& settings,
			const std::function<std::string(Rml::Input::KeyIdentifier position)>& label = {});
		~OnScreenKeyboard() override; // ends the notes that sound
		OnScreenKeyboard(const OnScreenKeyboard&) = delete;
		OnScreenKeyboard& operator=(const OnScreenKeyboard&) = delete;
		OnScreenKeyboard(OnScreenKeyboard&&) = delete;
		OnScreenKeyboard& operator=(OnScreenKeyboard&&) = delete;

		// A key that no element used, while the window is open, with what the key commands would run for it
		// (KeyCommands::bound): a key of the two rows plays its note, and the window takes every other key but those of
		// the few commands that keep their keys (reference manual p. 332): Save, Start/Stop Record, Start/Stop
		// Playback, Jump to Left Locator, Delete, Cycle on/off, Show/Hide Transport Panel and Show/Hide On-Screen
		// Keyboard, by default Mod+S, Num*, Space, Num1, Delete and Backspace, Num/, F2 and Alt+K. A command the user
		// gave another key keeps that key, and its old one plays or does nothing. Returns whether it took the key.
		bool press(const KeyPress& keys, const std::optional<KeyBinding>& bound);
		// The key went up: ends its note. focusLost ends every note, as when the window lost the keyboard focus.
		void release(const KeyPress& keys);

		// Labels the keys again with what label gives, as after the keyboard layout changed (Shell::onKeymapChanged).
		void relabel();

		// The MIDI note the key plays at the octave now.
		int note(int key) const { return (12 * (m_settings.octave + 2)) + key; }

	private:
		static constexpr int g_mouse = -1; // the source of the note the mouse holds

		void ProcessEvent(Rml::Event& event) override;
		void noteOn(int source, int key);
		void noteOff(int source);
		void send(int status, int note, int velocity);
		void save(); // sends the settings if they changed since
		void show();

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_document;
		Settings& m_settings;
		std::function<std::string(Rml::Input::KeyIdentifier position)> m_label;
		bool m_unsaved = false;                   // the settings changed since they were last sent
		std::vector<Rml::Element*> m_keyElements; // by key
		struct Held
		{
			int source = 0; // a key identifier, or g_mouse
			int key = 0;
			int note = 0; // the MIDI note it plays, at the octave when it started
		};
		std::vector<Held> m_held; // the notes that sound
	};
}
