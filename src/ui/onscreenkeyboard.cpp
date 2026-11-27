// SPDX-License-Identifier: GPL-3.0-or-later
// The on-screen keyboard window (ui/onscreenkeyboard.h).
#include "ui/onscreenkeyboard.h"

#include "platform/log.h"
#include "ui/keycommands.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <string>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using namespace Rml::Input;

		constexpr std::array<Rml::EventId, 5> g_events = {Rml::EventId::Mousedown, Rml::EventId::Mouseup,
			Rml::EventId::Mouseout, Rml::EventId::Change, Rml::EventId::Keyup};

		// The computer keys that play the keys, from the lowest, by their places on a US keyboard (KeyPress::position):
		// the lower row's octave, then the upper row's from the next C up. Comma plays that C too.
		constexpr std::array<KeyIdentifier, OnScreenKeyboard::g_keys> g_computerKeys = {KI_Z, KI_S, KI_X, KI_D, KI_C,
			KI_V, KI_G, KI_B, KI_H, KI_N, KI_J, KI_M, KI_Q, KI_2, KI_W, KI_3, KI_E, KI_R, KI_5, KI_T, KI_6, KI_Y, KI_7,
			KI_U, KI_I, KI_9, KI_O, KI_0, KI_P};
		constexpr std::array<const char*, OnScreenKeyboard::g_keys> g_labels = {"Z", "S", "X", "D", "C", "V", "G", "B",
			"H", "N", "J", "M", "Q", "2", "W", "3", "E", "R", "5", "T", "6", "Y", "7", "U", "I", "9", "O", "0", "P"};
		constexpr int g_c = 12; // the key that comma plays too

		// The commands that keep their keys while the window is open (reference manual p. 332), with their parameters.
		struct Kept
		{
			const char* command;
			const char* parameters;
		};
		constexpr std::array<Kept, 10> g_kept = {{{"file.save", "{}"}, {"transport.toggle", R"({"setting": "record"})"},
			{"transport.toggle", "{}"}, {"transport.locate", R"({"target": "leftLocator"})"}, {"edit.delete", "{}"},
			{"sampleEditor.delete", "{}"}, {"keyEditor.delete", "{}"}, {"transport.toggle", R"({"setting": "cycle"})"},
			{"window.transportPanel", "{}"}, {"window.onScreenKeyboard", "{}"}}};

		bool black(int key)
		{
			const int pitch = key % 12;
			return pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
		}

		// The computer key's place, the key a US keyboard has there: by its key without one.
		KeyIdentifier placeOf(const KeyPress& keys)
		{
			return keys.position != KI_UNKNOWN ? keys.position : keys.key;
		}

		// The key that a computer key plays, or -1.
		int keyFor(const KeyPress& keys)
		{
			if ((keys.modifiers & (KM_CTRL | KM_ALT | KM_META)) != 0)
				return -1;
			const KeyIdentifier place = placeOf(keys);
			if (place == KI_OEM_COMMA)
				return g_c;
			const auto index = std::find(g_computerKeys.begin(), g_computerKeys.end(), place) - g_computerKeys.begin();
			return index == static_cast<std::ptrdiff_t>(g_computerKeys.size()) ? -1 : static_cast<int>(index);
		}
	}

	OnScreenKeyboard::OnScreenKeyboard(EngineBinding& binding, Rml::ElementDocument& document, Settings& settings,
		const std::function<std::string(KeyIdentifier position)>& label)
		: m_binding(binding), m_document(document.GetObserverPtr()), m_settings(settings), m_label(label),
		  m_keyElements(g_keys, nullptr)
	{
		const auto keyRml = [](int key)
		{
			return R"(<div class=")" + std::string(black(key) ? "black-key" : "white-key") + R"(" note=")" +
				std::to_string(key) + R"("><span class="key-label"></span></div>)";
		};
		// The white keys side by side; a black key in a slot as wide as a white key, between the two it lies
		// between, and an empty slot where there is no black key.
		std::string white;
		std::string blacks;
		for (int key = 0; key < g_keys; ++key)
		{
			if (black(key))
				continue;
			white += keyRml(key);
			blacks +=
				R"(<div class="black-slot">)" + (key + 1 < g_keys && black(key + 1) ? keyRml(key + 1) : "") + "</div>";
		}
		if (Rml::Element* keys = document.GetElementById("white-keys"); keys != nullptr)
			keys->SetInnerRML(white);
		if (Rml::Element* keys = document.GetElementById("black-keys"); keys != nullptr)
			keys->SetInnerRML(blacks);
		Rml::ElementList elements;
		document.QuerySelectorAll(elements, "[note]");
		for (Rml::Element* element : elements)
			m_keyElements[static_cast<size_t>(element->GetAttribute<int>("note", 0))] = element;
		for (const Rml::EventId id : g_events)
			document.AddEventListener(id, this);
		relabel();
		show();
	}

	void OnScreenKeyboard::relabel()
	{
		// Each key labelled with what the layout has at its computer key's place (reference manual p. 333).
		for (size_t key = 0; key < m_keyElements.size(); ++key)
		{
			Rml::Element* text =
				m_keyElements[key] != nullptr ? m_keyElements[key]->QuerySelector(".key-label") : nullptr;
			if (text == nullptr)
				continue;
			std::string shown = m_label ? m_label(g_computerKeys[key]) : std::string();
			if (shown.empty())
				shown = g_labels[key];
			text->SetInnerRML(Rml::StringUtilities::EncodeRml(shown));
		}
	}

	OnScreenKeyboard::~OnScreenKeyboard()
	{
		try
		{
			while (!m_held.empty())
				noteOff(m_held.back().source);
			save();
		}
		catch (const std::exception& error)
		{
			logging::warning("The on-screen keyboard cannot end its notes or save its settings: {}", error.what());
		}
		if (!m_document)
			return;
		for (const Rml::EventId id : g_events)
			m_document->RemoveEventListener(id, this);
	}

	bool OnScreenKeyboard::press(const KeyPress& keys, const std::optional<KeyBinding>& bound)
	{
		if (const int key = keyFor(keys); key >= 0)
		{
			noteOn(placeOf(keys), key);
			return true;
		}
		return !bound ||
			std::none_of(g_kept.begin(), g_kept.end(),
				[&bound](const Kept& kept)
				{
					return bound->command == kept.command && bound->parameters == engine::Json::parse(kept.parameters);
				});
	}

	void OnScreenKeyboard::release(const KeyPress& keys)
	{
		if (keys.focusLost)
		{
			while (!m_held.empty())
				noteOff(m_held.back().source);
			return;
		}
		if (keys.key != KI_UNKNOWN || keys.position != KI_UNKNOWN)
			noteOff(placeOf(keys));
	}

	void OnScreenKeyboard::ProcessEvent(Rml::Event& event)
	{
		const Rml::Element* target = event.GetTargetElement();
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
			if (event.GetParameter("button", 0) != 0)
				return;
			for (; target != nullptr; target = target->GetParentNode())
				if (target->HasAttribute("note"))
				{
					noteOn(g_mouse, target->GetAttribute<int>("note", 0));
					return;
				}
			return;
		case Rml::EventId::Mouseup:
			if (event.GetParameter("button", 0) == 0)
				noteOff(g_mouse);
			save(); // a slider's drag ended
			return;
		case Rml::EventId::Keyup:
			save(); // a slider moved by keys
			return;
		case Rml::EventId::Mouseout:
			// The mouse leaving the window ends its note: the button going up out there sends no mouseup.
			if (target == m_document.get())
				noteOff(g_mouse);
			return;
		case Rml::EventId::Change:
		{
			const auto value = static_cast<int>(std::lround(event.GetParameter("value", 0.0F)));
			const Settings was = m_settings;
			if (target->GetId() == "octave")
				m_settings.octave = std::clamp(value, 0, 6);
			else if (target->GetId() == "velocity")
				m_settings.velocity = std::clamp(value, 1, 127);
			else
				return;
			// User settings, which last across restarts: written once the drag ends, not for each of its steps.
			m_unsaved = m_unsaved || m_settings.octave != was.octave || m_settings.velocity != was.velocity;
			show();
			return;
		}
		default:
			return;
		}
	}

	void OnScreenKeyboard::save()
	{
		if (!m_unsaved)
			return;
		m_unsaved = false;
		m_binding.send("settings.set",
			{{"key", "ui.onScreenKeyboard"},
				{"value", {{"octave", m_settings.octave}, {"velocity", m_settings.velocity}}}});
	}

	void OnScreenKeyboard::noteOn(int source, int key)
	{
		// A key held down again, e.g. the mouse pressed twice: its note starts again.
		noteOff(source);
		const int played = note(key);
		const bool sounding =
			std::any_of(m_held.begin(), m_held.end(), [played](const Held& held) { return held.note == played; });
		m_held.push_back({source, key, played});
		if (!sounding) // comma and Q play the same note
			send(0x90, played, m_settings.velocity);
		show();
	}

	void OnScreenKeyboard::noteOff(int source)
	{
		const auto found =
			std::find_if(m_held.begin(), m_held.end(), [source](const Held& held) { return held.source == source; });
		if (found == m_held.end())
			return;
		const int played = found->note;
		m_held.erase(found);
		if (std::none_of(m_held.begin(), m_held.end(), [played](const Held& held) { return held.note == played; }))
			send(0x80, played, 64);
		show();
	}

	void OnScreenKeyboard::send(int status, int note, int velocity)
	{
		const engine::Json message = {{"bytes", engine::Json::array({status, note, velocity})}};
		m_binding.send("midi.keyboard", {{"messages", engine::Json::array({message})}});
	}

	void OnScreenKeyboard::show()
	{
		if (!m_document)
			return;
		for (int key = 0; key < g_keys; ++key)
			if (Rml::Element* element = m_keyElements[static_cast<size_t>(key)]; element != nullptr)
				element->SetClass("pressed",
					std::any_of(m_held.begin(), m_held.end(), [key](const Held& held) { return held.key == key; }));
		const std::array<std::pair<const char*, int>, 2> values = {
			{{"octave", m_settings.octave}, {"velocity", m_settings.velocity}}};
		for (const auto& [id, value] : values)
		{
			if (Rml::Element* slider = m_document->GetElementById(id); slider != nullptr)
				slider->SetAttribute("value", value);
			if (Rml::Element* text = m_document->GetElementById(std::string(id) + "-value"); text != nullptr)
				text->SetInnerRML(std::to_string(value));
		}
	}
}
