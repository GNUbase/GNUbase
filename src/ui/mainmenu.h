// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/keycommands.h"
#include "ui/shell.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <string>
#include <vector>

namespace Rml
{
	class ElementDocument;
}

// The main menu (#392): menu items that run engine and UI commands as keys do. docs/features/main-menu.md describes it.
namespace gnubase::ui
{
	// Gives a document's <menuitem command="..."> elements their command: a click runs it as a key would
	// (KeyCommands::run), with the JSON object in the item's parameters attribute, in the window and the focus zone
	// around the item. Each item shows the command's first key in a .menu-keys element and is disabled while the
	// command can't run (KeyCommands::canRun). Use it on the UI thread; the key commands, the window and the
	// document must outlive it.
	class MainMenu final : private Rml::EventListener
	{
	public:
		// Throws std::invalid_argument for an item whose parameters aren't a JSON object.
		MainMenu(KeyCommands& keys, Window& window, Rml::ElementDocument& document);
		~MainMenu() override;
		MainMenu(const MainMenu&) = delete;
		MainMenu& operator=(const MainMenu&) = delete;
		MainMenu(MainMenu&&) = delete;
		MainMenu& operator=(MainMenu&&) = delete;

		// Disables the items whose command can't run now and enables the others, and shows their keys. The app calls
		// it in every step of the shell, so the DOM that MCP's UI tools read is up to date too.
		void update();

	private:
		struct Item
		{
			Rml::ObserverPtr<Rml::Element> element;
			Rml::ObserverPtr<Rml::Element> keys; // its .menu-keys
			std::string command;
			engine::Json parameters;
		};

		void ProcessEvent(Rml::Event& event) override;

		KeyCommands& m_keys;
		Window& m_window;
		Rml::ObserverPtr<Rml::Element> m_document;
		std::vector<Item> m_items;
	};
}
