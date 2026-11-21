// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/keycommands.h"
#include "ui/shell.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <optional>
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
	// command can't run (KeyCommands::canRun), and checked while KeyCommands::checked says so. Use it on the UI thread;
	// the key commands, the window and the document must outlive it.
	class MainMenu final : private Rml::EventListener
	{
	public:
		// Throws std::invalid_argument, leaving the document as it was, for an item whose parameters aren't a JSON
		// object, or whose command and parameters KeyCommands::check refuses: a command that doesn't exist, or
		// parameters the command doesn't take.
		MainMenu(KeyCommands& keys, Window& window, Rml::ElementDocument& document);
		~MainMenu() override;
		MainMenu(const MainMenu&) = delete;
		MainMenu& operator=(const MainMenu&) = delete;
		MainMenu(MainMenu&&) = delete;
		MainMenu& operator=(MainMenu&&) = delete;

		// Disables the items whose command can't run now and enables the others, and shows their keys. The app calls
		// it in every step of the shell and after each input of ui::Automation (Shell::onInputHandled), so the DOM
		// that MCP's UI tools read is up to date right after a click or a key, whatever changed (#1173). It asks each
		// command once whether it can run in an item's zone, and works out the keys only when the bindings changed
		// (KeyCommands::bindingsRevision), writing only those that differ from what it wrote (#1901). When a click
		// reaches its document, the menu asks the clicked item's command again before it runs it, for what the click's
		// listeners changed (#1120). That sees the listeners that ran before the menu's own on the document: those of
		// the elements below the document, the document's capture phase listeners and its listeners added before the
		// menu's. A bubble phase click listener added to the document after the menu runs after that, and what it
		// changes shows only in the update after the input.
		void update();

	private:
		struct Item
		{
			Rml::ObserverPtr<Rml::Element> element;
			Rml::ObserverPtr<Rml::Element> keys; // its .menu-keys
			std::string command;
			engine::Json parameters;
			std::string shownKeys; // the RML last written into keys
		};

		// Enables or disables the item's element, if it changed.
		static void enable(Rml::Element& element, bool enabled);

		void ProcessEvent(Rml::Event& event) override;

		KeyCommands& m_keys;
		Window& m_window;
		Rml::ObserverPtr<Rml::Element> m_document;
		std::vector<Item> m_items;
		std::optional<uint64_t> m_keysRevision; // of the bindings the items show the keys of
	};
}
