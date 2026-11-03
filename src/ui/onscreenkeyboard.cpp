// SPDX-License-Identifier: GPL-3.0-or-later
// The on-screen keyboard window (ui/onscreenkeyboard.h).
#include "ui/onscreenkeyboard.h"

#include "ui/keycommands.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using namespace Rml::Input;

		constexpr std::array<Rml::EventId, 3> g_events = {
			Rml::EventId::Mousedown, Rml::EventId::Mouseup, Rml::EventId::Change};

		// The computer keys that play the keys, from the lowest: the lower row's octave, then the upper row's from
		// the next C up. Comma plays that C too.
		constexpr std::array<KeyIdentifier, OnScreenKeyboard::g_keys> g_computerKeys = {KI_Z, KI_S, KI_X, KI_D, KI_C,
			KI_V, KI_G, KI_B, KI_H, KI_N, KI_J, KI_M, KI_Q, KI_2, KI_W, KI_3, KI_E, KI_R, KI_5, KI_T, KI_6, KI_Y, KI_7,
			KI_U, KI_I, KI_9, KI_O, KI_0, KI_P};
		constexpr std::array<const char*, OnScreenKeyboard::g_keys> g_labels = {"Z", "S", "X", "D", "C", "V", "G", "B",
			"H", "N", "J", "M", "Q", "2", "W", "3", "E", "R", "5", "T", "6", "Y", "7", "U", "I", "9", "O", "0", "P"};
		constexpr int g_c = 12; // the key that comma plays too

		// The keys that keep their key commands while the window is open (reference manual p. 332).
		constexpr std::array<const char*, 9> g_kept = {
			"Mod+S", "Num*", "Space", "Num1", "Delete", "Backspace", "Num/", "F2", "Alt+K"};

		constexpr int g_modifiers = KM_CTRL | KM_ALT | KM_SHIFT | KM_META;

		bool black(int key)
		{
			const int pitch = key % 12;
			return pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
		}

		// The key that a computer key plays, or -1.
		int keyFor(const KeyPress& keys)
		{
			if ((keys.modifiers & (KM_CTRL | KM_ALT | KM_META)) != 0)
				return -1;
			if (keys.key == KI_OEM_COMMA)
				return g_c;
			const auto found = std::find(g_computerKeys.begin(), g_computerKeys.end(), keys.key);
			return found == g_computerKeys.end() ? -1 : static_cast<int>(found - g_computerKeys.begin());
		}

		std::string keyRml(int key)
		{
			return "<div class=\"" + std::string(black(key) ? "black-key" : "white-key") + "\" note=\"" +
				std::to_string(key) + "\"><span class=\"key-label\">" + g_labels[static_cast<size_t>(key)] +
				"</span></div>";
		}
	}

	OnScreenKeyboard::OnScreenKeyboard(EngineBinding& binding, Rml::ElementDocument& document, Settings& settings)
		: m_binding(binding), m_document(document.GetObserverPtr()), m_settings(settings),
		  m_keyElements(g_keys, nullptr)
	{
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
				"<div class=\"black-slot\">" + (key + 1 < g_keys && black(key + 1) ? keyRml(key + 1) : "") + "</div>";
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
		show();
	}

	OnScreenKeyboard::~OnScreenKeyboard()
	{
		while (!m_held.empty())
			noteOff(m_held.back().source);
		if (!m_document)
			return;
		for (const Rml::EventId id : g_events)
			m_document->RemoveEventListener(id, this);
	}

	bool OnScreenKeyboard::press(const KeyPress& keys)
	{
		if (const int key = keyFor(keys); key >= 0)
		{
			noteOn(keys.key, key);
			return true;
		}
		const KeyPress pressed{keys.key, keys.modifiers & g_modifiers};
		return std::none_of(
			g_kept.begin(), g_kept.end(), [&pressed](const char* kept) { return parseKeys(kept) == pressed; });
	}

	void OnScreenKeyboard::release(const KeyPress& keys)
	{
		if (keys.key != KI_UNKNOWN)
		{
			noteOff(keys.key);
			return;
		}
		while (!m_held.empty())
			noteOff(m_held.back().source);
	}

	void OnScreenKeyboard::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
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
			return;
		case Rml::EventId::Change:
		{
			const auto value = static_cast<int>(std::lround(event.GetParameter("value", 0.0F)));
			if (target->GetId() == "octave")
				m_settings.octave = std::clamp(value, 0, 6);
			else if (target->GetId() == "velocity")
				m_settings.velocity = std::clamp(value, 1, 127);
			else
				return;
			show();
			return;
		}
		default:
			return;
		}
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
