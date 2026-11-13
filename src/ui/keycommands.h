// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/shell.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

// Key commands (#57): keys bound to commands, which are the engine's commands by name plus the UI's own, run for the
// keys that no element used, by the focus zone they were pressed in. docs/features/key-commands.md describes them.
namespace gnubase::ui
{
	enum class Platform : uint8_t
	{
		Windows,
		Mac,
		Linux,
	};

	// The platform GNUbase was built for.
	Platform buildPlatform();

	// Reads a modifier as parseKeys does, e.g. "Mod" or "shift": an Rml::Input::KeyModifier flag. Throws
	// std::invalid_argument for one it doesn't know.
	int parseModifier(const std::string& name, Platform platform = buildPlatform());

	// Reads keys written like "Space", "Mod+S", "Shift+Alt+F5" or "Ctrl+Num+", in any case. Modifiers: Mod, the
	// platform's command key (Cmd on macOS, Ctrl elsewhere), Ctrl, Alt (or Opt), Shift and Meta (or Cmd). Keys:
	// letters, digits, F1 to F24, Enter, Escape, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown, Up, Down,
	// Left, Right, Space, the punctuation keys . , - +, Alt on its own, and on the numeric keypad Num0 to Num9,
	// NumEnter, Num* Num/ Num+ Num- and Num. . Throws std::invalid_argument, naming what it can't read.
	KeyPress parseKeys(const std::string& keys, Platform platform = buildPlatform());

	// The keys as the platform writes them, modifiers first in the order Ctrl, Alt, Shift, Cmd: "Ctrl+Shift+Z",
	// "Shift+Cmd+Z" on macOS.
	std::string keyName(const KeyPress& keys, Platform platform = buildPlatform());

	// The keys as the user settings write them (ui.keyCommands), the same on every platform: Mod for the platform's
	// command key, modifiers first in the order Mod, Ctrl, Alt, Shift, Meta, e.g. "Mod+Shift+K"; parseKeys reads it.
	std::string keySetting(const KeyPress& keys, Platform platform = buildPlatform());

	// A command of the UI's own, such as zooming or opening a window. run gets the window the key went to and the
	// focus zone (see KeyCommands::press), or null.
	struct UiCommand
	{
		UiCommand(std::string id, std::string description, std::function<void(Window& window, Rml::Element* zone)> run,
			std::function<void()> release = {})
			: id(std::move(id)), description(std::move(description)), run(std::move(run)), release(std::move(release))
		{
		}

		std::string id; // "area.action" like the engine's commands, e.g. "view.zoomIn"
		std::string description;
		std::function<void(Window& window, Rml::Element* zone)> run;
		// For a command that runs while its key or button is held, such as winding: when it goes up. Optional.
		std::function<void()> release;
	};

	// Keys bound to a command, with the parameters an engine command gets, in a focus zone or, without one, everywhere.
	struct KeyBinding
	{
		KeyBinding(std::string keys, std::string command, engine::Json parameters = engine::Json::object(),
			std::string zone = {})
			: keys(std::move(keys)), command(std::move(command)), parameters(std::move(parameters)),
			  zone(std::move(zone))
		{
		}

		std::string keys; // as parseKeys reads them
		std::string command;
		engine::Json parameters;
		std::string zone;
	};

	// GNUbase's default key map, the reference's (manual pp. 1560-1573) for the commands that exist so far.
	std::vector<KeyBinding> defaultKeyMap();

	// The commands that keys run, and the key map. Used on the UI thread only.
	class KeyCommands
	{
	public:
		// The engine's commands, which run through the binding, with the default key map.
		KeyCommands(engine::Engine& engine, EngineBinding& binding, Platform platform = buildPlatform());

		// Adds a command of the UI's own. Throws std::invalid_argument if a command has its id.
		void add(UiCommand command);
		// Removes a command of the UI's own with its keys, e.g. a user script that is gone. Does nothing for a command
		// it doesn't have.
		void remove(const std::string& id);

		// Checks that a key, a menu item or a button can run the command with these parameters: throws
		// std::invalid_argument, naming the command, for a command it doesn't know, parameters that the engine's
		// command doesn't take (its parameter schema), or parameters other than an empty object for a UI command,
		// which takes none.
		void check(const std::string& command, const engine::Json& parameters) const;

		// Binds the keys to a command in a zone, replacing what they did there. Throws std::invalid_argument for keys
		// it can't read, or a command and parameters that check refuses.
		void bind(const KeyBinding& binding);

		// The keys bound to the command with these parameters, in every zone, as the platform writes them, e.g. for a
		// menu item.
		std::vector<std::string> keysFor(
			const std::string& command, const engine::Json& parameters = engine::Json::object()) const;

		// Runs the command that the keys, pressed in the window, are bound to. The focus zones are the elements with a
		// focuszone attribute around the focused element: the innermost zone's binding counts first, then the outer
		// zones', then the one for everywhere. A UI command gets the zone of its binding, or for one bound everywhere
		// the innermost zone. Lock keys don't count, and nothing runs while a modal document has the focus. A focused
		// text field keeps the arrow keys without modifiers, also those it can't use at its first or last line or
		// character, which the text field lets through. Returns whether a command ran.
		bool press(Window& window, const KeyPress& keys);
		// The key went up: ends the commands that its presses run while it is held (UiCommand::release), whatever
		// the modifiers are now. KI_UNKNOWN ends every one, as when the window lost the keyboard focus.
		void released(const KeyPress& keys);

		// Runs a command as a key would, e.g. for a menu item.
		void run(const std::string& command, const engine::Json& parameters, Window& window, Rml::Element* zone);
		// Ends a command that runs while held, as its key going up would, e.g. for a button released.
		void release(const std::string& command);

		// The user's key commands and macros, as the user settings ui.keyCommands and ui.macros have them: each entry
		// of keyCommands gives a command, with its parameters, all its keys, which replace its default keys in the zone
		// of its first default binding (everywhere if it has none) and win over other commands' keys there. A macro
		// runs its commands one after the other as the command "macro.<name>"; macros in macros don't run. Replaces
		// what an earlier call gave. Entries with a command check refuses or keys parseKeys can't read are left out
		// and logged, so that settings from another GNUbase version never stop the UI.
		void customize(const engine::Json& keyCommands, const engine::Json& macros);

		// Every command a key can run, for the key commands dialog, by area: the UI's commands, the engine's that
		// change something and run without parameters, and each command with the parameters of a default binding.
		// Each is {command, parameters, description, zone, keys}, keys being the default keys as keySetting writes
		// them; macros aren't in it.
		engine::Json commands() const;

		// Every key that runs a command now, for the key commands assignments export: the effective bindings (the
		// defaults with the user's key commands over them), grouped by command and parameters and sorted by command.
		// Each is {command, parameters, description, zone, keys}, keys being the key names as the platform writes them
		// (keyName), and a macro's description is "Runs the macro <name>.".
		engine::Json assignments() const;

		// Says when the command can run, for menu items (ui/mainmenu.h): enabled gets the window and the zone it
		// would run in. Throws std::invalid_argument for a command it doesn't know.
		void enableWhen(const std::string& command, std::function<bool(Window& window, Rml::Element* zone)> enabled);

		// Whether the command exists and can run in the window and zone now: what enableWhen says, or true.
		bool canRun(const std::string& command, Window& window, Rml::Element* zone) const;

	private:
		// Remembers a key that runs a command while it is held.
		void hold(int key, const std::string& command);

		struct Bound
		{
			std::string zone;
			KeyPress keys;
			std::string command;
			engine::Json parameters;
		};

		struct Customized
		{
			std::string command;
			engine::Json parameters;
			std::vector<KeyPress> keys;
		};

		// The default bindings with the user's key commands over them, into m_effective.
		void rebuild();
		bool isMacro(const std::string& command) const;

		EngineBinding& m_binding;
		Platform m_platform;
		std::map<std::string, engine::CommandInfo> m_engineCommands; // by name
		std::map<std::string, UiCommand> m_uiCommands;
		std::map<std::string, std::function<bool(Window&, Rml::Element*)>> m_enabled; // by command
		std::vector<Bound> m_bindings;    // the defaults, in the order they were bound
		std::vector<Customized> m_custom; // the user's key commands
		std::map<std::string, std::vector<std::pair<std::string, engine::Json>>> m_macros; // by command id
		std::vector<Bound> m_effective;                  // what the keys run: the defaults with the user's key commands
		std::vector<std::pair<int, std::string>> m_held; // the keys down with the commands they run while held
	};
}
