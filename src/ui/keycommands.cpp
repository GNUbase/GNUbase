// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/keycommands.h"

#include "engine/schema.h"
#include "platform/log.h"

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

		bool isTextField(const Rml::Element& element)
		{
			const auto type = element.GetAttribute<Rml::String>("type", "text");
			return element.GetTagName() == "textarea" ||
				(element.GetTagName() == "input" && (type == "text" || type == "password" || type == "search"));
		}

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
				{"Right", KI_RIGHT}, {"Space", KI_SPACE}, {"Alt", KI_LMENU}, {".", KI_OEM_PERIOD}, {",", KI_OEM_COMMA},
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
		if (pressed.key == KI_LMENU)
			pressed.modifiers |= KM_ALT; // held while it's pressed, as the keyboard sends it
		return pressed;
	}

	std::string keyName(const KeyPress& keys, Platform platform)
	{
		const bool mac = platform == Platform::Mac;
		const std::array<std::pair<int, const char*>, 4> modifiers = {
			{{KM_CTRL, "Ctrl"}, {KM_ALT, mac ? "Opt" : "Alt"}, {KM_SHIFT, "Shift"}, {KM_META, mac ? "Cmd" : "Meta"}}};
		std::string name;
		for (const auto& [flag, text] : modifiers)
			if ((keys.modifiers & flag) != 0 && (flag != KM_ALT || keys.key != KI_LMENU)) // Alt, not Alt+Alt
			{
				name += text;
				name += '+';
			}
		return name + keyText(keys.key);
	}

	std::string keySetting(const KeyPress& keys, Platform platform)
	{
		const int command = platform == Platform::Mac ? KM_META : KM_CTRL;
		const std::array<std::pair<int, const char*>, 5> modifiers = {{{command, "Mod"}, {KM_CTRL & ~command, "Ctrl"},
			{KM_ALT, "Alt"}, {KM_SHIFT, "Shift"}, {KM_META & ~command, "Meta"}}};
		std::string name;
		for (const auto& [flag, text] : modifiers)
			if (flag != 0 && (keys.modifiers & flag) != 0 && (flag != KM_ALT || keys.key != KI_LMENU))
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
			// Num- and Num+ (Rewind, Forward) wind while held: the app binds its own commands to them.
			{"Shift+N", "marker.locateNext"},
			{"Shift+B", "marker.locatePrevious"},
			{"Insert", "marker.add"},
			// Edit
			{"Mod+Z", "history.undo"},
			{"Mod+Shift+Z", "history.redo"},
		};
		// To Marker 1 to 9: Shift with a digit, and from 3 on the keypad's digits too.
		for (int number = 1; number <= 9; ++number)
			keys.emplace_back("Shift+" + std::to_string(number), "marker.locate", Json{{"number", number}});
		for (int number = 3; number <= 9; ++number)
			keys.emplace_back("Num" + std::to_string(number), "marker.locate", Json{{"number", number}});
		// Set Marker 1 to 9 the same way with Mod, and Recall Cycle Marker 1 to 9 with Shift and the keypad.
		for (int number = 1; number <= 9; ++number)
			keys.emplace_back("Mod+" + std::to_string(number), "marker.put", Json{{"number", number}});
		for (int number = 3; number <= 9; ++number)
			keys.emplace_back("Mod+Num" + std::to_string(number), "marker.put", Json{{"number", number}});
		for (int number = 1; number <= 9; ++number)
			keys.emplace_back("Shift+Num" + std::to_string(number), "marker.setLocators", Json{{"number", number}});
		return keys;
	}

	KeyCommands::KeyCommands(engine::Engine& engine, EngineBinding& binding, Platform platform)
		: m_binding(binding), m_platform(platform)
	{
		for (const engine::CommandInfo& info : engine.commands())
			m_engineCommands.emplace(info.name, info);
		for (const KeyBinding& keys : defaultKeyMap())
			bind(keys);
	}

	void KeyCommands::add(UiCommand command)
	{
		std::string id = command.id;
		if (m_engineCommands.count(id) != 0 || m_uiCommands.count(id) != 0 || id.rfind("macro.", 0) == 0)
			throw std::invalid_argument("there is a command " + id + " already");
		m_uiCommands.emplace(std::move(id), std::move(command));
	}

	void KeyCommands::remove(const std::string& id)
	{
		if (m_uiCommands.erase(id) == 0)
			return;
		m_enabled.erase(id);
		m_bindings.erase(std::remove_if(m_bindings.begin(), m_bindings.end(),
							 [&id](const Bound& bound) { return bound.command == id; }),
			m_bindings.end());
		m_custom.erase(std::remove_if(m_custom.begin(), m_custom.end(),
						   [&id](const Customized& custom) { return custom.command == id; }),
			m_custom.end());
		rebuild();
	}

	void KeyCommands::check(const std::string& command, const Json& parameters) const
	{
		if (!parameters.is_object())
			throw std::invalid_argument(command + ": the parameters must be a JSON object, not " + parameters.dump());
		if (const auto schema = m_engineCommands.find(command); schema != m_engineCommands.end())
		{
			if (const std::string error = engine::validate(parameters, schema->second.parameters); !error.empty())
				throw std::invalid_argument(command + ": " + error);
		}
		else if (m_uiCommands.count(command) == 0 && !isMacro(command))
		{
			throw std::invalid_argument("there is no command " + command);
		}
		else if (!parameters.empty())
		{
			throw std::invalid_argument(command + " takes no parameters, but got " + parameters.dump());
		}
	}

	void KeyCommands::bind(const KeyBinding& binding)
	{
		Bound bound{binding.zone, parseKeys(binding.keys, m_platform), binding.command, binding.parameters};
		check(binding.command, binding.parameters);
		const auto same = std::find_if(m_bindings.begin(), m_bindings.end(),
			[&bound](const Bound& other) { return other.zone == bound.zone && other.keys == bound.keys; });
		if (same != m_bindings.end())
			*same = std::move(bound);
		else
			m_bindings.push_back(std::move(bound));
		rebuild();
	}

	bool KeyCommands::isMacro(const std::string& command) const
	{
		return m_macros.count(command) != 0;
	}

	void KeyCommands::customize(const Json& keyCommands, const Json& macros)
	{
		m_macros.clear();
		for (const Json& macro : macros.is_array() ? macros : Json::array())
		{
			auto& commands = m_macros["macro." + macro.value("name", std::string())];
			for (const Json& command : macro.value("commands", Json::array()))
				commands.emplace_back(
					command.value("command", std::string()), command.value("parameters", Json::object()));
		}
		m_custom.clear();
		for (const Json& entry : keyCommands.is_array() ? keyCommands : Json::array())
		{
			Customized customized{entry.value("command", std::string()), entry.value("parameters", Json::object()), {}};
			try
			{
				check(customized.command, customized.parameters);
				for (const Json& keys : entry.value("keys", Json::array()))
					customized.keys.push_back(parseKeys(keys.get<std::string>(), m_platform));
			}
			catch (const std::exception& error)
			{
				logging::warning("A key command of the user settings is left out: {}", error.what());
				continue;
			}
			m_custom.push_back(std::move(customized));
		}
		rebuild();
	}

	void KeyCommands::rebuild()
	{
		const auto customized = [this](const std::string& command, const Json& parameters)
		{
			return std::any_of(m_custom.begin(), m_custom.end(),
				[&](const Customized& custom) { return custom.command == command && custom.parameters == parameters; });
		};
		m_effective.clear();
		for (const Bound& bound : m_bindings)
			if (!customized(bound.command, bound.parameters))
				m_effective.push_back(bound);
		for (const Customized& custom : m_custom)
		{
			const auto first = std::find_if(m_bindings.begin(), m_bindings.end(), [&custom](const Bound& bound)
				{ return bound.command == custom.command && bound.parameters == custom.parameters; });
			const std::string zone = first != m_bindings.end() ? first->zone : std::string();
			for (const KeyPress& keys : custom.keys)
			{
				Bound bound{zone, keys, custom.command, custom.parameters};
				const auto same = std::find_if(m_effective.begin(), m_effective.end(),
					[&bound](const Bound& other) { return other.zone == bound.zone && other.keys == bound.keys; });
				if (same != m_effective.end())
					*same = std::move(bound);
				else
					m_effective.push_back(std::move(bound));
			}
		}
	}

	Json KeyCommands::commands() const
	{
		Json found = Json::array();
		const auto add = [this, &found](const std::string& command, const Json& parameters, const std::string& about)
		{
			for (const Json& listed : found)
				if (listed.at("command") == command && listed.at("parameters") == parameters)
					return;
			Json keys = Json::array();
			std::string zone;
			for (const Bound& binding : m_bindings)
				if (binding.command == command && binding.parameters == parameters)
				{
					if (keys.empty())
						zone = binding.zone;
					keys.push_back(keySetting(binding.keys, m_platform));
				}
			found.push_back({{"command", command}, {"parameters", parameters}, {"description", about}, {"zone", zone},
				{"keys", keys}});
		};
		for (const auto& [id, command] : m_uiCommands)
			add(id, Json::object(), command.description);
		for (const auto& [name, info] : m_engineCommands)
			if (!info.query && engine::validate(Json::object(), info.parameters).empty())
				add(name, Json::object(), info.description);
		for (const Bound& binding : m_bindings)
		{
			const auto engineCommand = m_engineCommands.find(binding.command);
			const auto ui = m_uiCommands.find(binding.command);
			std::string about;
			if (engineCommand != m_engineCommands.end())
				about = engineCommand->second.description;
			else if (ui != m_uiCommands.end())
				about = ui->second.description;
			add(binding.command, binding.parameters, about);
		}
		std::stable_sort(found.begin(), found.end(), [](const Json& a, const Json& b)
			{ return a.at("command").get<std::string>() < b.at("command").get<std::string>(); });
		return found;
	}

	std::vector<std::string> KeyCommands::keysFor(const std::string& command, const Json& parameters) const
	{
		std::vector<std::string> names;
		for (const Bound& bound : m_effective)
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
		if (focus != nullptr && pressed.modifiers == 0 && isTextField(*focus) &&
			(pressed.key == KI_UP || pressed.key == KI_DOWN || pressed.key == KI_LEFT || pressed.key == KI_RIGHT))
			return false;

		// A copy of the binding, since a UI command may bind keys.
		const auto bound = [this, &pressed](const std::string& zone) -> std::optional<Bound>
		{
			for (const Bound& binding : m_effective)
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
				hold(pressed.key, found->command);
				run(found->command, found->parameters, window, element);
				return true;
			}
		}
		const std::optional<Bound> everywhere = bound({});
		if (!everywhere)
			return false;
		hold(pressed.key, everywhere->command);
		run(everywhere->command, everywhere->parameters, window, innermost);
		return true;
	}

	void KeyCommands::hold(int key, const std::string& command)
	{
		const auto ui = m_uiCommands.find(command);
		if (ui != m_uiCommands.end() && ui->second.release)
			m_held.emplace_back(key, command);
	}

	void KeyCommands::released(const KeyPress& keys)
	{
		// A copy, since a command may press or release others.
		std::vector<std::string> ending;
		const auto ends = [&keys](const std::pair<int, std::string>& held)
		{ return keys.key == Rml::Input::KI_UNKNOWN || held.first == keys.key; };
		for (const auto& held : m_held)
			if (ends(held))
				ending.push_back(held.second);
		m_held.erase(std::remove_if(m_held.begin(), m_held.end(), ends), m_held.end());
		for (const std::string& command : ending)
			release(command);
	}

	void KeyCommands::release(const std::string& command)
	{
		if (const auto ui = m_uiCommands.find(command); ui != m_uiCommands.end() && ui->second.release)
			ui->second.release();
	}

	void KeyCommands::run(const std::string& command, const Json& parameters, Window& window, Rml::Element* zone)
	{
		if (const auto macro = m_macros.find(command); macro != m_macros.end())
		{
			// A copy, since a command may change the macros.
			const std::vector<std::pair<std::string, Json>> commands = macro->second;
			for (const auto& [each, eachParameters] : commands)
			{
				if (isMacro(each))
					continue;
				try
				{
					check(each, eachParameters);
				}
				catch (const std::invalid_argument& error)
				{
					logging::warning("The macro {} skips a command: {}", command, error.what());
					continue;
				}
				run(each, eachParameters, window, zone);
			}
			return;
		}
		if (const auto ui = m_uiCommands.find(command); ui != m_uiCommands.end())
		{
			// A copy, since a command may remove itself, e.g. a script that finds the scripts again.
			const auto runCommand = ui->second.run;
			if (runCommand)
				runCommand(window, zone);
			return;
		}
		m_binding.send(command, parameters);
	}

	void KeyCommands::enableWhen(
		const std::string& command, std::function<bool(Window& window, Rml::Element* zone)> enabled)
	{
		if (m_engineCommands.count(command) == 0 && m_uiCommands.count(command) == 0)
			throw std::invalid_argument("there is no command " + command);
		m_enabled[command] = std::move(enabled);
	}

	bool KeyCommands::canRun(const std::string& command, Window& window, Rml::Element* zone) const
	{
		if (m_engineCommands.count(command) == 0 && m_uiCommands.count(command) == 0)
			return isMacro(command);
		const auto enabled = m_enabled.find(command);
		return enabled == m_enabled.end() || !enabled->second || enabled->second(window, zone);
	}
}
