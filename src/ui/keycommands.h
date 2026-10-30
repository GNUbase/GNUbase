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
	// Left, Right, Space, the punctuation keys . , - +, and on the numeric keypad Num0 to Num9, NumEnter, Num* Num/
	// Num+ Num- and Num. . Throws std::invalid_argument, naming what it can't read.
	KeyPress parseKeys(const std::string& keys, Platform platform = buildPlatform());

	// The keys as the platform writes them, modifiers first in the order Ctrl, Alt, Shift, Cmd: "Ctrl+Shift+Z",
	// "Shift+Cmd+Z" on macOS.
	std::string keyName(const KeyPress& keys, Platform platform = buildPlatform());

	// A command of the UI's own, such as zooming or opening a window. run gets the window the key went to and the
	// focus zone (see KeyCommands::press), or null.
	struct UiCommand
	{
		std::string id; // "area.action" like the engine's commands, e.g. "view.zoomIn"
		std::string description;
		std::function<void(Window& window, Rml::Element* zone)> run;
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

		// Binds the keys to a command in a zone, replacing what they did there. Throws std::invalid_argument for keys
		// it can't read, a command it doesn't know, or parameters that the engine's command doesn't take.
		void bind(const KeyBinding& binding);

		// The keys bound to the command with these parameters, in every zone, as the platform writes them, e.g. for a
		// menu item.
		std::vector<std::string> keysFor(
			const std::string& command, const engine::Json& parameters = engine::Json::object()) const;

		// Runs the command that the keys, pressed in the window, are bound to. The focus zones are the elements with a
		// focuszone attribute around the focused element: the innermost zone's binding counts first, then the outer
		// zones', then the one for everywhere. A UI command gets the zone of its binding, or for one bound everywhere
		// the innermost zone. Lock keys don't count, and nothing runs while a modal document has the focus. Returns
		// whether a command ran.
		bool press(Window& window, const KeyPress& keys);

		// Runs a command as a key would, e.g. for a menu item.
		void run(const std::string& command, const engine::Json& parameters, Window& window, Rml::Element* zone);

	private:
		struct Bound
		{
			std::string zone;
			KeyPress keys;
			std::string command;
			engine::Json parameters;
		};

		EngineBinding& m_binding;
		Platform m_platform;
		std::map<std::string, engine::Json> m_engineCommands; // their parameter schemas, by name
		std::map<std::string, UiCommand> m_uiCommands;
		std::vector<Bound> m_bindings; // in the order they were bound
	};
}
