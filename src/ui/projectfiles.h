// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/keycommands.h"
#include "ui/shell.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Rml
{
	class Context;
	class ElementDocument;
}

// Project handling in the UI (#77). docs/features/project-files.md describes it.
namespace gnubase::ui
{
	// The File menu's project commands as UI commands with the reference's keys: file.new (the start screen),
	// file.open, file.save, file.saveAs, file.saveNewVersion, file.saveTemplate, file.backup and file.revert, plus
	// file.quit. They ask through the OS's file dialogs (Shell::showFileDialog) and GNUbase's own (the documents
	// start-screen.rml, save-template.rml, backup-project.rml, backup-progress.rml, discard-changes.rml and
	// project-error.rml in folder), and do everything with engine commands. Opening another project, reverting and
	// quitting ask first if the project has unsaved changes, and a command that fails shows its error, as does an auto
	// save that fails. A dialog that comes while another is open, such as the unsaved changes question or an error,
	// waits for it instead of being dropped. A backup shows its progress, which Cancel stops, and whether it couldn't
	// read files or open the backup. Quitting asks however it is asked for: File > Quit, the main window's close button
	// or the OS (Shell::onQuitRequest). Use it on the UI thread; the key commands, the binding and the shell must
	// outlive it.
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
		using Shown = std::function<void(Rml::ElementDocument* dialog)>;
		struct WaitingError
		{
			WindowId window = 0;
			std::string error;
		};
		struct WaitingDialog
		{
			WindowId window = 0;
			const char* file = nullptr;
			Shown shown;
			Closed closed;
		};

		void ProcessEvent(Rml::Event& event) override;
		// Opens one of the dialogs in the window, modal: shown runs with its document once it opened, or with null if
		// it can't, and closed when it closes, with its result. One at a time: while another one is open it waits until
		// that closed or went with its window (openWaiting), unless that is a failed auto save's error, which makes
		// way; with wait false it doesn't open then.
		void openDialog(WindowId window, const char* file, Shown shown, Closed closed, bool wait = true);
		// Opens the dialogs that wait, in order, once no dialog is open; those whose window is gone don't open.
		void openWaiting();
		// Closes the open dialog, if any, with the result.
		void closeDialog(const std::string& result);
		// Asks the engine for the project's file (project.location).
		void readLocation();
		// A command's done that shows its error in a dialog in the window if it failed (project-error.rml), so that a
		// failed save isn't taken for done.
		std::function<void(const engine::Result&)> reportFailure(Window& window);
		// Logs the error and shows it in a dialog in the window (project-error.rml).
		void showError(WindowId window, const std::string& error);
		// Shows the error in project-error.rml in the window once the event that asked is done. A failed auto save's
		// error (autoSave) takes the place of an earlier one's but doesn't open over any other modal dialog in the
		// window: it waits for them to close (waitForDialogs). It counts as shown only once it opened and while no
		// other dialog took its place.
		void openError(WindowId window, const std::string& error, bool autoSave);
		// Keeps a failed auto save's error for the window until the modal dialogs in the context other than m_dialog,
		// which keep it from opening, closed: they tell ProcessEvent when one hides or unloads. It replaces the error
		// that waited before.
		void waitForDialogs(Rml::Context& context, WindowId window, const std::string& error);
		// Opens the error that waited, if any, once the dialog that closed is gone; it waits again for dialogs that
		// are still open.
		void showWaitingError();
		// Forgets the error that waited and stops watching the dialogs.
		void dropWaitingError();
		// An auto save succeeded or the project was saved or opened: a failed auto save's error that waits, or that
		// openError is still to open, is out of date.
		void autoSaveWorks();
		// Tells the user in the main window that the project can't be auto saved (project.autoSaveFailed): once, not
		// at every interval, until it fails for another reason or after an auto save succeeded or the project was
		// saved or opened, as the engine says with repeated.
		void autoSaveFailed(const engine::Json& failure);
		// Runs action now if the project has no unsaved changes, or else once the user agreed to discard them. asked,
		// if given, runs once the engine said whether to ask: with the question's document when it opened, else null.
		void unlessUnsaved(Window& window, std::function<void()> action,
			std::function<void(Rml::ElementDocument* question)> asked = {});
		// Quits GNUbase unless the user wants to keep unsaved changes, asking in the window. A dialog that is open
		// closes as cancelled first (closeDialog). Further quit requests while the question is coming or open do
		// nothing, so that one question answers them; once it is answered or gone, the next one asks again.
		void quit(Window& window);
		void open(Window& window);
		void saveAs(Window& window);
		// Asks for the backup's options in the window (backup-project.rml) and backs the project up into folder.
		void backup(Window& window, const std::string& folder);
		// Shows backup-project.rml in the window, with name as the Project Name, and backs up with its options on OK.
		void askBackupOptions(Window& window, const std::string& folder, const std::string& name);
		// project.backup, asked for in the window, returned: shows its error, or the job's progress in a dialog
		// (backup-progress.rml) whose Cancel stops it.
		void backupStarted(WindowId window, const engine::Result& result);
		// A backup's job ended (job.finished, or job.get for one that ended before backupStarted heard of it): if it is
		// one of those this UI started, closes its progress and shows in its window why it failed, which files it
		// couldn't read and why the backup wasn't opened, if it was to be. Other clients' backups show nothing (#944).
		void backupEnded(const engine::Json& job);
		// The open dialog if it is a backup's progress, or null.
		Rml::ElementDocument* progressDialog() const;
		void showStartScreen(Rml::ElementDocument& dialog, const engine::Json& templates, const engine::Json& recent);

		EngineBinding& m_binding;
		Shell& m_shell;
		std::filesystem::path m_folder;
		std::string m_file; // the project's file (project.location), empty for none
		Subscription m_saved;
		Subscription m_reset;
		Subscription m_jobProgress;
		Subscription m_jobFinished;
		Subscription m_autoSaveFailed;
		Subscription m_autoSaved;
		bool m_autoSaveFailureShown = false; // since the last auto save that failed for a new reason
		bool m_autoSaveErrorOpen = false;    // the open dialog is a failed auto save's error
		std::map<uint64_t, WindowId>
			m_backups;              // the running backups it started, by job, with where they were asked for
		uint64_t m_progressJob = 0; // the backup whose progress the open dialog shows, if any
		Rml::ObserverPtr<Rml::Element> m_dialog; // the open dialog's document
		Closed m_closed;
		std::deque<WaitingDialog> m_waitingDialogs;    // the dialogs that wait for it, oldest first
		bool m_quitting = false;                       // a quit request waits for the engine to say whether to ask
		Rml::ObserverPtr<Rml::Element> m_quitQuestion; // and then for the question, while it is m_dialog
		std::vector<std::string> m_templates;          // the start screen's templates, by name
		std::vector<std::string> m_recent;             // and its recent projects, by file

		// A failed auto save's error that modal dialogs keep away, and those dialogs, which ProcessEvent hears close.
		std::optional<WaitingError> m_waitingError;
		std::vector<Rml::ObserverPtr<Rml::Element>> m_keptAwayBy;
		uint64_t m_autoSaveWorked = 0; // how often autoSaveWorks ran, so that openError knows an error is out of date
	};
}
