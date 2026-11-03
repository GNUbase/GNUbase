// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/keycommands.h"
#include "ui/shell.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Rml
{
	class ElementDocument;
}

// Project handling in the UI (#77). docs/features/project-files.md describes it.
namespace gnubase::ui
{
	// The File menu's project commands as UI commands with the reference's keys: file.new (the start screen),
	// file.open, file.save, file.saveAs, file.saveNewVersion, file.saveTemplate, file.backup and file.revert, plus
	// file.quit. They ask through the OS's file dialogs (Shell::showFileDialog) and GNUbase's own (the documents
	// start-screen.rml, save-template.rml, discard-changes.rml and project-error.rml in folder), and do everything with
	// engine commands. Opening another project, reverting and quitting ask first if the project has unsaved changes,
	// and a command that fails shows its error. Quitting asks however it is asked for: File > Quit, the main window's
	// close button or the OS (Shell::onQuitRequest). Use it on the UI thread; the key commands, the binding and the
	// shell must outlive it.
	class ProjectFiles final : private Rml::EventListener
	{
	public:
		ProjectFiles(KeyCommands& keys, EngineBinding& binding, Shell& shell, std::filesystem::path folder);
		~ProjectFiles() override;
		ProjectFiles(const ProjectFiles&) = delete;
		ProjectFiles& operator=(const ProjectFiles&) = delete;
		ProjectFiles(ProjectFiles&&) = delete;
		ProjectFiles& operator=(ProjectFiles&&) = delete;

		// Opens the start screen in the window, modal: Create Empty Project, the templates, the recent projects and
		// Open Other. File > New Project opens it, and the app at its start.
		void openStartScreen(Window& window);

	private:
		using Closed = std::function<void(Rml::ElementDocument& dialog, const std::string& result)>;

		void ProcessEvent(Rml::Event& event) override;
		// Opens one of the dialogs in the window, modal; closed runs when it closes, with its result.
		Rml::ElementDocument* openDialog(Window& window, const char* file, Closed closed);
		// Asks the engine for the project's file (project.location).
		void readLocation();
		// A command's done that shows its error in a dialog in the window if it failed (project-error.rml), so that a
		// failed save isn't taken for done.
		std::function<void(const engine::Result&)> reportFailure(Window& window);
		void showError(WindowId window, const std::string& error);
		// Runs action now if the project has no unsaved changes, or else once the user agreed to discard them; kept, if
		// given, runs instead when the user keeps them or the question can't open.
		void unlessUnsaved(Window& window, std::function<void()> action, std::function<void()> kept = {});
		// Quits GNUbase unless the user wants to keep unsaved changes, asking in the window. A dialog that is open
		// closes as cancelled first (closeDialog). Further quit requests while the question is coming or open do
		// nothing, so that one question answers them.
		void quit(Window& window);
		void open(Window& window);
		void saveAs(Window& window);
		void showStartScreen(Rml::ElementDocument& dialog, const engine::Json& templates, const engine::Json& recent);

		EngineBinding& m_binding;
		Shell& m_shell;
		std::filesystem::path m_folder;
		std::string m_file; // the project's file (project.location), empty for none
		Subscription m_saved;
		Subscription m_reset;
		Subscription m_jobFinished;
		std::optional<WindowId> m_backupWindow;  // where the last backup was asked for, which shows if it fails
		Rml::ObserverPtr<Rml::Element> m_dialog; // the open dialog's document
		Closed m_closed;
		bool m_quitting = false;              // a quit request waits for the unsaved changes question, or quits
		std::vector<std::string> m_templates; // the start screen's templates, by name
		std::vector<std::string> m_recent;    // and its recent projects, by file
	};
}
