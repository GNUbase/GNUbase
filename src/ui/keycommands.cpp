// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/keycommands.h"

#include "engine/schema.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;
	using namespace Rml::Input;

	namespace
	{
		// The modifiers that make a key another one; lock keys don't.
		constexpr int g_modifiers = KM_CTRL | KM_ALT | KM_SHIFT | KM_META;

		struct NamedKey
		{
			const char* name;
			KeyIdentifier key;
		};

		// Keys with a name, the one keyName writes first. Letters, digits, Num0 to Num9 and F1 to F24 are counted.
		const std::vector<NamedKey>& namedKeys()
		{
			static const std::vector<NamedKey> keys = {{"Enter", KI_RETURN}, {"Return", KI_RETURN},
				{"Escape", KI_ESCAPE}, {"Esc", KI_ESCAPE}, {"Tab", KI_TAB}, {"Backspace", KI_BACK},
				{"Delete", KI_DELETE}, {"Del", KI_DELETE}, {"Insert", KI_INSERT}, {"Home", KI_HOME}, {"End", KI_END},
				{"PageUp", KI_PRIOR}, {"PageDown", KI_NEXT}, {"Up", KI_UP}, {"Down", KI_DOWN}, {"Left", KI_LEFT},
				{"Right", KI_RIGHT}, {"Space", KI_SPACE}, {".", KI_OEM_PERIOD}, {",", KI_OEM_COMMA},
				{"-", KI_OEM_MINUS}, {"+", KI_OEM_PLUS}, {"NumEnter", KI_NUMPADENTER}, {"Num*", KI_MULTIPLY},
				{"Num/", KI_DIVIDE}, {"Num+", KI_ADD}, {"Num-", KI_SUBTRACT}, {"Num.", KI_DECIMAL}};
			return keys;
		}

		std::string lowercase(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		// The number after prefix in name, e.g. 5 in "f5" with "f"; -1 if there's none.
		int numberAfter(const std::string& name, const std::string& prefix)
		{
			if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
				!std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
					[](unsigned char c) { return std::isdigit(c) != 0; }) ||
				name.size() > prefix.size() + 2)
				return -1;
			return std::stoi(name.substr(prefix.size()));
		}

		KeyIdentifier keyIdentifier(const std::string& name)
		{
			const std::string key = lowercase(name);
			for (const NamedKey& named : namedKeys())
				if (lowercase(named.name) == key)
					return named.key;
			if (key.size() == 1 && key[0] >= 'a' && key[0] <= 'z')
				return static_cast<KeyIdentifier>(KI_A + (key[0] - 'a'));
			if (key.size() == 1 && key[0] >= '0' && key[0] <= '9')
				return static_cast<KeyIdentifier>(KI_0 + (key[0] - '0'));
			if (const int digit = numberAfter(key, "num"); digit >= 0 && digit <= 9)
				return static_cast<KeyIdentifier>(KI_NUMPAD0 + digit);
			if (const int number = numberAfter(key, "f"); number >= 1 && number <= 24)
				return static_cast<KeyIdentifier>(KI_F1 + number - 1);
			throw std::invalid_argument("unknown key " + name);
		}

		std::string keyText(KeyIdentifier key)
		{
			for (const NamedKey& named : namedKeys())
				if (named.key == key)
					return named.name;
			if (key >= KI_A && key <= KI_Z)
				return {static_cast<char>('A' + (key - KI_A))};
			if (key >= KI_0 && key <= KI_9)
				return {static_cast<char>('0' + (key - KI_0))};
			if (key >= KI_NUMPAD0 && key <= KI_NUMPAD9)
				return "Num" + std::to_string(key - KI_NUMPAD0);
			if (key >= KI_F1 && key <= KI_F24)
				return "F" + std::to_string(key - KI_F1 + 1);
			return "Key" + std::to_string(key);
		}
	}

	Platform buildPlatform()
	{
#ifdef _WIN32
		return Platform::Windows;
#elif defined(__APPLE__)
		return Platform::Mac;
#else
		return Platform::Linux;
#endif
	}

	int parseModifier(const std::string& name, Platform platform)
	{
		const std::string modifier = lowercase(name);
		if (modifier == "mod")
			return platform == Platform::Mac ? KM_META : KM_CTRL;
		if (modifier == "ctrl" || modifier == "control")
			return KM_CTRL;
		if (modifier == "alt" || modifier == "opt" || modifier == "option")
			return KM_ALT;
		if (modifier == "shift")
			return KM_SHIFT;
		if (modifier == "meta" || modifier == "cmd" || modifier == "command")
			return KM_META;
		throw std::invalid_argument("unknown modifier " + name + "; use Mod, Ctrl, Alt, Shift or Meta");
	}

	KeyPress parseKeys(const std::string& keys, Platform platform)
	{
		if (keys.empty())
			throw std::invalid_argument("no key given");
		// The key follows the last +, unless it is + itself: "Ctrl++", "Shift+Num+".
		const size_t split = keys.size() < 2 ? std::string::npos : keys.rfind('+', keys.size() - 2);
		KeyPress pressed{keyIdentifier(split == std::string::npos ? keys : keys.substr(split + 1)), 0};
		std::istringstream modifiers(split == std::string::npos ? std::string() : keys.substr(0, split));
		for (std::string name; std::getline(modifiers, name, '+');)
			pressed.modifiers |= parseModifier(name, platform);
		return pressed;
	}

	std::string keyName(const KeyPress& keys, Platform platform)
	{
		const bool mac = platform == Platform::Mac;
		const std::array<std::pair<int, const char*>, 4> modifiers = {
			{{KM_CTRL, "Ctrl"}, {KM_ALT, mac ? "Opt" : "Alt"}, {KM_SHIFT, "Shift"}, {KM_META, mac ? "Cmd" : "Meta"}}};
		std::string name;
		for (const auto& [flag, text] : modifiers)
			if ((keys.modifiers & flag) != 0)
			{
				name += text;
				name += '+';
			}
		return name + keyText(keys.key);
	}

	std::vector<KeyBinding> defaultKeyMap()
	{
		std::vector<KeyBinding> keys = {
			// Transport
			{"Space", "transport.toggle"},
			{"NumEnter", "transport.play"},
			{"Num0", "transport.stop"},
			{"Num1", "transport.locate", {{"target", "leftLocator"}}},
			{"Num2", "transport.locate", {{"target", "rightLocator"}}},
			{"Num.", "transport.locate", {{"target", "projectStart"}}},
			{"Mod+Num1", "transport.set", {{"left", "cursor"}}},
			{"Mod+Num2", "transport.set", {{"right", "cursor"}}},
			{"Num/", "transport.toggle", {{"setting", "cycle"}}},
			{"Num*", "transport.toggle", {{"setting", "record"}}},
			{"C", "transport.toggle", {{"setting", "metronome"}}},
			{"I", "transport.toggle", {{"setting", "punchIn"}}},
			{"O", "transport.toggle", {{"setting", "punchOut"}}},
			{"Num-", "transport.locate", {{"by", -1.0}}},
			{"Num+", "transport.locate", {{"by", 1.0}}},
			{"Shift+Num-", "transport.locate", {{"by", -4.0}}},
			{"Shift+Num+", "transport.locate", {{"by", 4.0}}},
			{"Shift+N", "marker.locateNext"},
			{"Shift+B", "marker.locatePrevious"},
			// Edit
			{"Mod+Z", "history.undo"},
			{"Mod+Shift+Z", "history.redo"},
		};
		// To Marker 1 to 9: Shift with a digit, and from 3 on the keypad's digits too.
		for (int number = 1; number <= 9; ++number)
			keys.emplace_back("Shift+" + std::to_string(number), "marker.locate", Json{{"number", number}});
		for (int number = 3; number <= 9; ++number)
			keys.emplace_back("Num" + std::to_string(number), "marker.locate", Json{{"number", number}});
		return keys;
	}

	KeyCommands::KeyCommands(engine::Engine& engine, EngineBinding& binding, Platform platform)
		: m_binding(binding), m_platform(platform)
	{
		for (const engine::CommandInfo& info : engine.commands())
			m_engineCommands.emplace(info.name, info.parameters);
		for (const KeyBinding& keys : defaultKeyMap())
			bind(keys);
	}

	void KeyCommands::add(UiCommand command)
	{
		std::string id = command.id;
		if (m_engineCommands.count(id) != 0 || m_uiCommands.count(id) != 0)
			throw std::invalid_argument("there is a command " + id + " already");
		m_uiCommands.emplace(std::move(id), std::move(command));
	}

	void KeyCommands::bind(const KeyBinding& binding)
	{
		Bound bound{binding.zone, parseKeys(binding.keys, m_platform), binding.command, binding.parameters};
		if (const auto command = m_engineCommands.find(binding.command); command != m_engineCommands.end())
		{
			if (const std::string error = engine::validate(binding.parameters, command->second); !error.empty())
				throw std::invalid_argument(binding.command + ": " + error);
		}
		else if (m_uiCommands.count(binding.command) == 0)
		{
			throw std::invalid_argument("there is no command " + binding.command);
		}
		const auto same = std::find_if(m_bindings.begin(), m_bindings.end(),
			[&bound](const Bound& other) { return other.zone == bound.zone && other.keys == bound.keys; });
		if (same != m_bindings.end())
			*same = std::move(bound);
		else
			m_bindings.push_back(std::move(bound));
	}

	std::vector<std::string> KeyCommands::keysFor(const std::string& command, const Json& parameters) const
	{
		std::vector<std::string> names;
		for (const Bound& bound : m_bindings)
			if (bound.command == command && bound.parameters == parameters)
				names.push_back(keyName(bound.keys, m_platform));
		return names;
	}

	bool KeyCommands::press(Window& window, const KeyPress& keys)
	{
		const KeyPress pressed{keys.key, keys.modifiers & g_modifiers};
		Rml::Element* focus = window.context().GetFocusElement();
		if (focus != nullptr && focus->GetOwnerDocument() != nullptr && focus->GetOwnerDocument()->IsModal())
			return false;

		// A copy of the binding, since a UI command may bind keys.
		const auto bound = [this, &pressed](const std::string& zone) -> std::optional<Bound>
		{
			for (const Bound& binding : m_bindings)
				if (binding.zone == zone && binding.keys == pressed)
					return binding;
			return std::nullopt;
		};
		Rml::Element* innermost = nullptr;
		for (Rml::Element* element = focus; element != nullptr; element = element->GetParentNode())
		{
			const auto zone = element->GetAttribute<Rml::String>("focuszone", "");
			if (zone.empty())
				continue;
			if (innermost == nullptr)
				innermost = element;
			if (const std::optional<Bound> found = bound(zone))
			{
				run(found->command, found->parameters, window, element);
				return true;
			}
		}
		const std::optional<Bound> everywhere = bound({});
		if (!everywhere)
			return false;
		run(everywhere->command, everywhere->parameters, window, innermost);
		return true;
	}

	void KeyCommands::run(const std::string& command, const Json& parameters, Window& window, Rml::Element* zone)
	{
		if (const auto ui = m_uiCommands.find(command); ui != m_uiCommands.end())
		{
			if (ui->second.run)
				ui->second.run(window, zone);
			return;
		}
		m_binding.send(command, parameters);
	}
}
