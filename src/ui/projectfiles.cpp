// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/projectfiles.h"

#include "platform/log.h"
#include "ui/widgets.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		// The result of a dialog that closed to make way for another one.
		constexpr const char* g_replaced = "replaced";
		// The result with which a backup's progress dialog closes once the backup ended.
		constexpr const char* g_backupDone = "done";

		const std::vector<FileFilter>& projectFilters()
		{
			static const std::vector<FileFilter> filters{{"DAWproject", "dawproject"}, {"All files", "*"}};
			return filters;
		}

		// A file chosen to save a project as, with the DAWproject extension unless it has it already, in any case:
		// "Song.DAWPROJECT" stays as it is and "Song v1.2" becomes "Song v1.2.dawproject". The dots and spaces at the
		// end of the name, which Windows drops and the dialogs of Linux return as typed, go first, so that "Song." and
		// "Song.dawproject." are "Song.dawproject" rather than "Song..dawproject" and "Song.dawproject.dawproject"
		// (#706). A name of nothing else is "Untitled".
		std::filesystem::path withProjectExtension(const std::filesystem::path& file)
		{
			std::string name = file.filename().u8string();
			while (!name.empty() && (name.back() == '.' || name.back() == ' '))
				name.pop_back();
			if (name.empty())
				name = "Untitled";
			if (Rml::StringUtilities::ToLower(std::filesystem::u8path(name).extension().u8string()) != ".dawproject")
				name += ".dawproject";
			return file.parent_path() / std::filesystem::u8path(name);
		}

		// The rows of a start screen list: value is the row's index in the list, text what it shows.
		std::string listItems(const std::vector<std::string>& texts, const std::vector<std::string>& titles)
		{
			std::string rml;
			for (size_t i = 0; i < texts.size(); ++i)
				rml += "<listitem value=\"" + std::to_string(i) + "\" title=\"" +
					Rml::StringUtilities::EncodeRml(titles[i]) + "\">" + Rml::StringUtilities::EncodeRml(texts[i]) +
					"</listitem>";
			return rml;
		}

		// What the start screen's list selects: one of the items, or nothing.
		std::string selected(Rml::ElementDocument& dialog, const char* list, const std::vector<std::string>& items)
		{
			const Rml::Element* view = dialog.GetElementById(list);
			const std::string value = view != nullptr ? view->GetAttribute<Rml::String>("value", "") : std::string();
			for (size_t i = 0; i < items.size(); ++i)
				if (value == std::to_string(i))
					return items[i];
			return {};
		}

		bool inside(const Rml::Element* element, const char* id)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->GetId() == id)
					return true;
			return false;
		}

		// Whether a modal document other than own is open in the context, such as the fade editor.
		bool otherModalOpen(Rml::Context& context, const Rml::Element* own)
		{
			for (int i = 0; i < context.GetNumDocuments(); ++i)
			{
				const Rml::ElementDocument* document = context.GetDocument(i);
				if (document != own && document->IsModal())
					return true;
			}
			return false;
		}
	}

	ProjectFiles::ProjectFiles(KeyCommands& keys, EngineBinding& binding, Shell& shell, std::filesystem::path folder)
		: m_binding(binding), m_shell(shell), m_folder(std::move(folder)),
		  m_saved(binding.on("project.saved",
			  [this](const engine::Event& event)
			  {
				  m_file = event.data.at("file").get<std::string>();
				  autoSaveWorks();
			  })),
		  m_reset(binding.on("project.reset",
			  [this](const engine::Event&)
			  {
				  autoSaveWorks();
				  readLocation();
			  })),
		  m_jobProgress(binding.on("job.progress",
			  [this](const engine::Event& event)
			  {
				  const auto backup = m_backups.find(event.data.at("job").get<uint64_t>());
				  auto* dialog = backup != m_backups.end()
					  ? dynamic_cast<Rml::ElementDocument*>(backup->second.progress.get())
					  : nullptr;
				  if (dialog == nullptr)
					  return;
				  if (Rml::Element* bar = dialog->GetElementById("backup-progress"))
					  bar->SetAttribute("value", event.data.at("progress").get<double>());
				  if (Rml::Element* file = dialog->GetElementById("backup-progress-file"))
					  file->SetInnerRML(Rml::StringUtilities::EncodeRml(event.data.at("detail").get<std::string>()));
			  })),
		  m_jobFinished(
			  binding.on("job.finished", [this](const engine::Event& event) { backupEnded(event.data.at("job")); })),
		  m_autoSaveFailed(
			  binding.on("project.autoSaveFailed", [this](const engine::Event& event) { autoSaveFailed(event.data); })),
		  m_autoSaved(binding.on("project.autoSaved", [this](const engine::Event&) { autoSaveWorks(); }))
	{
		readLocation();

		keys.add({"file.new", "Opens the start screen, where new projects are created and recent ones opened.",
			[this](Window& window, Rml::Element*) { openStartScreen(window); }});
		keys.add({"file.open", "Opens a project file, chosen in a file dialog.",
			[this](Window& window, Rml::Element*) { open(window); }});
		keys.add({"file.save", "Saves the project to its file, or asks for one if it has none.",
			[this](Window& window, Rml::Element*)
			{
				if (m_file.empty())
					saveAs(window);
				else
					m_binding.send("project.save", Json::object(), reportFailure(window));
			}});
		keys.add({"file.saveAs", "Saves the project as a file chosen in a file dialog.",
			[this](Window& window, Rml::Element*) { saveAs(window); }});
		keys.add({"file.saveNewVersion", "Saves the project as the next numbered version of its file.",
			[this](Window& window, Rml::Element*)
			{
				if (m_file.empty())
					saveAs(window);
				else
					m_binding.send("project.saveNewVersion", Json::object(), reportFailure(window));
			}});
		keys.add({"file.saveTemplate", "Saves the project as a template, named in a dialog.",
			[this](Window& window, Rml::Element*)
			{
				openDialog(window.id(), "save-template.rml", {},
					[this, done = reportFailure(window)](Rml::ElementDocument& dialog, const std::string& result)
					{
						auto* name = dynamic_cast<Rml::ElementFormControl*>(dialog.GetElementById("template-name"));
						if (result == "ok" && name != nullptr && !name->GetValue().empty())
							m_binding.send("project.saveTemplate", {{"name", name->GetValue()}}, done);
					});
			}});
		keys.add({"file.backup",
			"Backs the project up into a folder chosen in a file dialog, with the options of a dialog that follows.",
			[this](Window& window, Rml::Element*)
			{
				FileDialog dialog;
				dialog.kind = FileDialogKind::Folder;
				dialog.title = "Back up Project";
				dialog.parent = window.id();
				m_shell.showFileDialog(dialog,
					[this, id = window.id()](const FileDialogResult& chosen)
					{
						if (Window* asked = m_shell.find(id); asked != nullptr && !chosen.files.empty())
							backup(*asked, chosen.files.front().u8string());
					});
			}});
		keys.add({"file.revert", "Goes back to the project as it was last saved.", [this](Window& window, Rml::Element*)
			{
				unlessUnsaved(window,
					[this, done = reportFailure(window)] { m_binding.send("project.revert", Json::object(), done); });
			}});
		keys.enableWhen("file.revert", [this](Window&, Rml::Element*) { return !m_file.empty(); });
		keys.add({"file.quit", "Quits GNUbase.", [this](Window& window, Rml::Element*) { quit(window); }});
		m_shell.onQuitRequest([this](Window& main) { quit(main); });

		// The reference's keys (manual pp. 1564-1565).
		keys.bind({"Mod+N", "file.new"});
		keys.bind({"Mod+O", "file.open"});
		keys.bind({"Mod+S", "file.save"});
		keys.bind({"Mod+Shift+S", "file.saveAs"});
		keys.bind({"Mod+Alt+S", "file.saveNewVersion"});
		keys.bind({"Mod+Q", "file.quit"});
	}

	ProjectFiles::~ProjectFiles()
	{
		m_shell.onQuitRequest({});
		dropWaitingError();
		if (Rml::Element* dialog = m_dialog.get())
		{
			dialog->RemoveEventListener(Rml::EventId::Dblclick, this);
			dialog->RemoveEventListener(g_dialogCloseEvent, this);
			dialog->RemoveEventListener(Rml::EventId::Unload, this);
		}
		for (const auto& backup : m_backups)
			if (Rml::Element* progress = backup.second.progress.get())
				progress->RemoveEventListener(g_dialogCloseEvent, this);
	}

	void ProjectFiles::openStartScreen(Window& window)
	{
		openDialog(
			window.id(), "start-screen.rml",
			[this](Rml::ElementDocument* dialog)
			{
				if (dialog == nullptr)
					return;
				dialog->AddEventListener(Rml::EventId::Dblclick, this);
				m_templates.clear();
				m_recent.clear();
				const Rml::ObserverPtr<Rml::Element> shown = dialog->GetObserverPtr();
				m_binding.send("project.templates", Json::object(),
					[this, shown](const engine::Result& templates)
					{
						m_binding.send("project.recent", Json::object(),
							[this, shown, templates](const engine::Result& recent)
							{
								if (auto* document = dynamic_cast<Rml::ElementDocument*>(shown.get());
									document != nullptr && templates.ok() && recent.ok())
									showStartScreen(
										*document, templates.value.at("templates"), recent.value.at("projects"));
							});
					});
			},
			[this, &window](Rml::ElementDocument& closed, const std::string& result)
			{
				if (result == "empty")
				{
					unlessUnsaved(window,
						[this, done = reportFailure(window)] { m_binding.send("project.new", Json::object(), done); });
				}
				else if (result == "template")
				{
					if (const std::string name = selected(closed, "start-templates", m_templates); !name.empty())
						unlessUnsaved(window, [this, name, done = reportFailure(window)]
							{ m_binding.send("project.new", {{"template", name}}, done); });
				}
				else if (result == "recent")
				{
					if (const std::string file = selected(closed, "start-recent", m_recent); !file.empty())
						unlessUnsaved(window, [this, file, done = reportFailure(window)]
							{ m_binding.send("project.load", {{"path", file}}, done); });
				}
				else if (result == "open")
				{
					open(window);
				}
			});
	}

	void ProjectFiles::newProject(Window& window, std::function<void()> then)
	{
		unlessUnsaved(window,
			[this, done = reportFailure(window), then = std::move(then)]
			{
				m_binding.send("project.new", Json::object(),
					[done, then](const engine::Result& result)
					{
						done(result);
						if (result.ok())
							then();
					});
			});
	}

	// The templates by name, and the recent projects that are still there by file name.
	void ProjectFiles::showStartScreen(Rml::ElementDocument& dialog, const Json& templates, const Json& recent)
	{
		std::vector<std::string> files;
		for (const Json& item : templates)
		{
			m_templates.push_back(item.at("name").get<std::string>());
			files.push_back(item.at("file").get<std::string>());
		}
		if (Rml::Element* list = dialog.GetElementById("start-templates"))
			list->SetInnerRML(listItems(m_templates, files));
		std::vector<std::string> names;
		for (const Json& item : recent)
			if (item.at("exists").get<bool>())
			{
				m_recent.push_back(item.at("file").get<std::string>());
				names.push_back(std::filesystem::u8path(m_recent.back()).stem().u8string());
			}
		if (Rml::Element* list = dialog.GetElementById("start-recent"))
			list->SetInnerRML(listItems(names, m_recent));
	}

	void ProjectFiles::backup(Window& window, const std::string& folder)
	{
		// The name starts as the engine's default name for the backup (project.location); left as it is, or empty,
		// the engine names the backup on its own.
		m_binding.send("project.location", Json::object(),
			[this, id = window.id(), folder](const engine::Result& location)
			{
				if (Window* asked = m_shell.find(id); asked != nullptr && location.ok())
					askBackupOptions(*asked, folder, location.value.at("name").get<std::string>());
			});
	}

	void ProjectFiles::askBackupOptions(Window& window, const std::string& folder, const std::string& name)
	{
		openDialog(
			window.id(), "backup-project.rml",
			[name](Rml::ElementDocument* dialog)
			{
				if (auto* field = dialog != nullptr
						? dynamic_cast<Rml::ElementFormControl*>(dialog->GetElementById("backup-name"))
						: nullptr)
					field->SetValue(name);
			},
			[this, id = window.id(), folder, name](Rml::ElementDocument& options, const std::string& result)
			{
				if (result != "ok")
					return;
				const auto checked = [&options](const char* option)
				{
					const Rml::Element* element = options.GetElementById(option);
					return element != nullptr && element->HasAttribute("checked");
				};
				Json parameters = {{"folder", folder}, {"activate", !checked("backup-keep-active")},
					{"minimize", checked("backup-minimize")}, {"currentVersionsOnly", checked("backup-offline")},
					{"removeUnused", checked("backup-remove-unused")}, {"skipVideo", checked("backup-skip-video")},
					{"skipMixdown", checked("backup-skip-mixdown")}};
				if (auto* field = dynamic_cast<Rml::ElementFormControl*>(options.GetElementById("backup-name")))
					if (const Rml::String typed = field->GetValue(); !typed.empty() && typed != name)
						parameters["name"] = typed;
				m_binding.send("project.backup", parameters,
					[this, id](const engine::Result& started) { backupStarted(id, started); });
			});
	}

	void ProjectFiles::backupStarted(WindowId window, const engine::Result& result)
	{
		if (!result.ok())
		{
			showError(window, result.error);
			return;
		}
		const Json& job = result.value.at("job");
		const auto id = job.at("id").get<uint64_t>();
		Backup& backup = m_backups[id];
		backup.window = window;
		// The progress isn't one of the dialogs that take turns (openDialog): while the backup runs, the other project
		// dialogs still open, over it, and quitting closes them, not the backup (#1001). Each window shows one backup's
		// progress at a time, so that a backup asked for in another window meanwhile shows its own (#1109). Closed
		// before the backup ended, by Cancel or otherwise, it stops it (ProcessEvent); the backup may end before
		// job.cancel comes.
		const bool showing = std::any_of(m_backups.begin(), m_backups.end(),
			[window](const auto& other) { return other.second.window == window && other.second.progress; });
		if (Rml::ElementDocument* dialog = showing ? nullptr : loadDialog(window, "backup-progress.rml"))
		{
			backup.progress = dialog->GetObserverPtr();
			if (Rml::Element* bar = dialog->GetElementById("backup-progress"))
				bar->SetAttribute("value", job.at("progress").get<double>());
		}
		// The job may have ended before its command's result came: job.get tells.
		m_binding.send("job.get", {{"job", id}},
			[this](const engine::Result& state)
			{
				if (state.ok() && state.value.at("job").at("state") != "running")
					backupEnded(state.value.at("job"));
			});
	}

	void ProjectFiles::backupEnded(const Json& job)
	{
		const auto id = job.at("id").get<uint64_t>();
		const auto found = m_backups.find(id);
		if (found == m_backups.end())
			return; // another client's, or reported already
		const WindowId window = found->second.window;
		auto* progress = dynamic_cast<Rml::ElementDocument*>(found->second.progress.get());
		m_backups.erase(found);
		if (progress != nullptr)
		{
			progress->RemoveEventListener(g_dialogCloseEvent, this);
			ui::closeDialog(*progress, g_backupDone);
		}
		// A backup runs as a job, so most of its failures come after project.backup returned. So do the files it
		// couldn't read, and the reason why a backup that was to be opened wasn't, such as an edit while the job ran.
		const Json& state = job.at("state");
		if (state == "failed")
		{
			showError(window, job.at("error").get<std::string>());
			return;
		}
		if (state != "done")
			return; // cancelled: as the user asked
		const Json& result = job.at("result");
		std::string problems;
		if (const Json& missing = result.at("missing"); !missing.empty())
		{
			// A freeze file that several tracks play is listed once for each: files count by path, so that two of the
			// same name in different folders are two (#1001), and show by name, as the Mixdown folder's folders that
			// couldn't be listed do, which count as folders (#1109).
			std::set<std::string> files;
			std::set<std::string> folders;
			for (const Json& entry : missing)
				(entry.value("folder", false) ? folders : files).insert(entry.at("path").get<std::string>());
			const auto counted = [](size_t count, const char* what)
			{ return count == 0 ? std::string() : std::to_string(count) + " " + what + (count == 1 ? "" : "s"); };
			const std::string fileCount = counted(files.size(), "file");
			const std::string folderCount = counted(folders.size(), "folder");
			std::string list;
			for (const std::set<std::string>* paths : {&files, &folders})
				for (const std::string& path : *paths)
					list += (list.empty() ? "" : ", ") + std::filesystem::u8path(path).filename().u8string();
			problems = "The project was backed up without " + fileCount +
				(fileCount.empty() || folderCount.empty() ? "" : " and ") + folderCount +
				" that couldn't be read: " + list + ".";
		}
		if (const Json& activation = result.value("activationError", Json()); activation.is_string())
			problems += (problems.empty() ? "" : " ") + activation.get<std::string>();
		if (!problems.empty())
			showError(window, problems);
	}

	void ProjectFiles::openDialog(WindowId window, const char* file, Shown shown, Closed closed, bool wait)
	{
		// A failed auto save's error came on its own, not from what the user does: it makes way for the dialog of
		// something the user does, such as a failed save or the unsaved changes question, and shows again later.
		if (m_dialog && m_autoSaveErrorOpen)
			closeDialog(g_replaced);
		if (!wait && (m_dialog || !m_waitingDialogs.empty()))
		{
			if (shown)
				shown(nullptr);
			return;
		}
		// One at a time, and the others wait, so that nothing the user is to answer or learn is dropped (#485).
		m_waitingDialogs.push_back({window, file, std::move(shown), std::move(closed)});
		openWaiting();
	}

	void ProjectFiles::openWaiting()
	{
		while (!m_dialog && !m_waitingDialogs.empty())
		{
			WaitingDialog next = std::move(m_waitingDialogs.front());
			m_waitingDialogs.pop_front();
			Rml::ElementDocument* dialog = loadDialog(next.window, next.file);
			if (dialog == nullptr)
			{
				if (next.shown)
					next.shown(nullptr);
				continue;
			}
			// Closed or gone with its window, it makes way for the next one.
			dialog->AddEventListener(Rml::EventId::Unload, this);
			m_dialog = dialog->GetObserverPtr();
			m_closed = std::move(next.closed);
			m_autoSaveErrorOpen = false;
			if (next.shown)
				next.shown(dialog);
		}
	}

	Rml::ElementDocument* ProjectFiles::loadDialog(WindowId window, const char* file)
	{
		const Window* shown = m_shell.find(window);
		const std::filesystem::path path = m_folder / file;
		Rml::ElementDocument* dialog =
			shown != nullptr ? shown->context().LoadDocument(path.generic_u8string()) : nullptr;
		if (dialog == nullptr)
		{
			if (shown != nullptr)
				logging::warning("Cannot open the dialog {}", path.u8string());
			return nullptr;
		}
		dialog->AddEventListener(g_dialogCloseEvent, this);
		dialog->Show(Rml::ModalFlag::Modal);
		return dialog;
	}

	// Double clicks in the start screen, which create from a template or open a recent project, every dialog's close
	// event and unloading, and the dialogs that keep a failed auto save's error away hiding or unloading.
	void ProjectFiles::ProcessEvent(Rml::Event& event)
	{
		if (event.GetId() == Rml::EventId::Hide || event.GetId() == Rml::EventId::Unload)
		{
			// The dialog is still in its context while it unloads.
			m_shell.post(
				[this]
				{
					openWaiting();
					showWaitingError();
				});
			return;
		}
		auto* dialog = dynamic_cast<Rml::ElementDocument*>(event.GetCurrentElement());
		if (dialog == nullptr)
			return;
		if (event.GetId() == Rml::EventId::Dblclick)
		{
			const Rml::Element* target = event.GetTargetElement();
			Rml::Element* button = nullptr;
			if (inside(target, "start-templates"))
				button = dialog->GetElementById("create-template");
			else if (inside(target, "start-recent"))
				button = dialog->GetElementById("open-recent");
			if (button != nullptr)
				button->Click();
			return;
		}
		if (event.GetType() != g_dialogCloseEvent)
			return;
		const auto result = event.GetParameter<Rml::String>("result", "");
		// A backup's progress that closes before the backup ended (backupEnded closes it unheard).
		for (auto& backup : m_backups)
			if (backup.second.progress.get() == dialog)
			{
				dialog->RemoveEventListener(g_dialogCloseEvent, this);
				backup.second.progress.reset();
				m_binding.send("job.cancel", {{"job", backup.first}}, [](const engine::Result&) {});
				return;
			}
		dialog->RemoveEventListener(Rml::EventId::Dblclick, this);
		dialog->RemoveEventListener(g_dialogCloseEvent, this);
		m_dialog.reset();
		m_autoSaveErrorOpen = false;
		// Before the document closes, so that the callback can read its fields; m_dialog is free for another dialog.
		const Closed closed = std::move(m_closed);
		closed(*dialog, result);
	}

	void ProjectFiles::readLocation()
	{
		m_binding.send("project.location", Json::object(),
			[this](const engine::Result& result)
			{
				if (result.ok())
					m_file = result.value.at("file").is_string() ? result.value.at("file").get<std::string>()
																 : std::string();
			});
	}

	std::function<void(const engine::Result&)> ProjectFiles::reportFailure(Window& window)
	{
		return [this, id = window.id()](const engine::Result& result)
		{
			if (!result.ok())
				showError(id, result.error);
		};
	}

	void ProjectFiles::showError(WindowId window, const std::string& error)
	{
		logging::warning("Project handling: {}", error);
		openError(window, error, false);
	}

	void ProjectFiles::showNotice(WindowId window, const std::string& title, const std::string& message)
	{
		// The dialog that asked may still be closing.
		m_shell.post(
			// NOLINTNEXTLINE(bugprone-exception-escape): only MSVC's std::string; out of memory only
			[this, window, title, message]
			{
				openDialog(
					window, "project-error.rml",
					[title, message](Rml::ElementDocument* dialog)
					{
						if (dialog == nullptr)
							return;
						if (Rml::Element* heading = dialog->GetElementById("project-error-title"))
							heading->SetInnerRML(Rml::StringUtilities::EncodeRml(title));
						if (Rml::Element* text = dialog->GetElementById("project-error-message"))
							text->SetInnerRML(Rml::StringUtilities::EncodeRml(message));
					},
					[](Rml::ElementDocument&, const std::string&) {});
			});
	}

	void ProjectFiles::openError(WindowId window, const std::string& error, bool autoSave)
	{
		// The dialog that asked may still be closing.
		m_shell.post(
			// NOLINTNEXTLINE(bugprone-exception-escape): only MSVC's std::string; out of memory only
			[this, window, error, autoSave, worked = m_autoSaveWorked]
			{
				if (autoSave && worked != m_autoSaveWorked)
					return; // an auto save succeeded or the project was saved or opened since it failed
				const Window* showing = m_shell.find(window);
				// A failed auto save's error doesn't open over another modal dialog, such as the fade editor, which the
				// user works in: it waits until that closed. The project dialog that is open (m_dialog) is left to
				// openDialog, where an auto save's error makes way or else shows at the next failure.
				if (autoSave && showing != nullptr && otherModalOpen(showing->context(), m_dialog.get()))
				{
					waitForDialogs(showing->context(), window, error);
					return;
				}
				// Any other error waits for the project dialog that is open.
				openDialog(
					window, "project-error.rml",
					[this, error, autoSave](Rml::ElementDocument* dialog)
					{
						if (dialog == nullptr)
							return; // an auto save's error shows at the next failure instead
						if (Rml::Element* message = dialog->GetElementById("project-error-message"))
							message->SetInnerRML(Rml::StringUtilities::EncodeRml(error));
						if (autoSave)
						{
							m_autoSaveFailureShown = true;
							m_autoSaveErrorOpen = true;
							dropWaitingError(); // an older one
						}
					},
					[this, autoSave](Rml::ElementDocument&, const std::string& result)
					{
						// Shown again at the next failure if another dialog took its place.
						if (autoSave && result == g_replaced)
							m_autoSaveFailureShown = false;
					},
					!autoSave);
			});
	}

	void ProjectFiles::waitForDialogs(Rml::Context& context, WindowId window, const std::string& error)
	{
		dropWaitingError();
		m_waitingError = WaitingError{window, error};
		for (int i = 0; i < context.GetNumDocuments(); ++i)
		{
			Rml::ElementDocument* document = context.GetDocument(i);
			if (document == m_dialog.get() || !document->IsModal())
				continue;
			document->AddEventListener(Rml::EventId::Hide, this);
			document->AddEventListener(Rml::EventId::Unload, this);
			m_keptAwayBy.push_back(document->GetObserverPtr());
		}
	}

	void ProjectFiles::showWaitingError()
	{
		if (!m_waitingError)
			return;
		const WaitingError waiting = std::move(*m_waitingError);
		dropWaitingError();
		openError(waiting.window, waiting.error, true);
	}

	void ProjectFiles::autoSaveWorks()
	{
		++m_autoSaveWorked;
		dropWaitingError();
	}

	void ProjectFiles::dropWaitingError()
	{
		m_waitingError.reset();
		for (const Rml::ObserverPtr<Rml::Element>& dialog : m_keptAwayBy)
			if (Rml::Element* document = dialog.get())
			{
				document->RemoveEventListener(Rml::EventId::Hide, this);
				document->RemoveEventListener(Rml::EventId::Unload, this);
			}
		m_keptAwayBy.clear();
	}

	void ProjectFiles::closeDialog(const std::string& result)
	{
		// As its buttons close it, so that it closes once (ui::closeDialog).
		if (auto* dialog = dynamic_cast<Rml::ElementDocument*>(m_dialog.get()))
			ui::closeDialog(*dialog, result);
	}

	void ProjectFiles::autoSaveFailed(const Json& failure)
	{
		// The engine logged the failure already, if it was new.
		if (!failure.value("repeated", false))
			m_autoSaveFailureShown = false;
		if (m_autoSaveFailureShown)
			return;
		for (const Window* window : m_shell.windows())
			if (window->main())
			{
				openError(
					window->id(), "The project can't be auto saved: " + failure.at("error").get<std::string>(), true);
				return;
			}
	}

	void ProjectFiles::unlessUnsaved(
		Window& window, std::function<void()> action, std::function<void(Rml::ElementDocument*)> asked)
	{
		m_binding.send("project.revision", Json::object(),
			[this, id = window.id(), action = std::move(action), asked = std::move(asked)](const engine::Result& result)
			{
				if (!result.ok() || !result.value.at("modified").get<bool>())
				{
					if (asked)
						asked(nullptr);
					action();
					return;
				}
				// The dialog that asked may still be closing. The question waits for a project dialog that is open.
				m_shell.post(
					[this, id, action, asked]
					{
						openDialog(id, "discard-changes.rml", asked,
							[action](Rml::ElementDocument&, const std::string& answer)
							{
								if (answer == "ok")
									action();
							});
					});
			});
	}

	void ProjectFiles::quit(Window& window)
	{
		// SDL follows the main window's close request with its own quit request when that is the last window, and
		// the user may ask again while the question is open: the question that is coming or open answers them all.
		// A question that was answered, or went with its window, is no longer the open dialog: the request asks again.
		if (m_quitting || (m_quitQuestion.get() != nullptr && m_quitQuestion.get() == m_dialog.get()))
			return;
		// A dialog that is open, such as the start screen, would keep the question from opening: quitting cancels it.
		closeDialog("cancel");
		m_quitting = true;
		// Quitting closes every window, so it waits until the event that asked for it is done with them.
		unlessUnsaved(
			window, [this] { m_shell.post([this] { m_shell.quit(); }); },
			[this](Rml::ElementDocument* question)
			{
				m_quitting = false;
				m_quitQuestion = question != nullptr ? question->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
			});
	}

	void ProjectFiles::open(Window& window)
	{
		FileDialog dialog;
		dialog.title = "Open Project";
		dialog.filters = projectFilters();
		dialog.parent = window.id();
		if (!m_file.empty())
			dialog.location = std::filesystem::u8path(m_file).parent_path();
		m_shell.showFileDialog(dialog,
			[this, id = window.id()](const FileDialogResult& chosen)
			{
				Window* window = m_shell.find(id);
				if (chosen.files.empty() || window == nullptr)
					return;
				const std::string file = chosen.files.front().u8string();
				unlessUnsaved(*window, [this, file, done = reportFailure(*window)]
					{ m_binding.send("project.load", {{"path", file}}, done); });
			});
	}

	void ProjectFiles::saveAs(Window& window)
	{
		FileDialog dialog;
		dialog.kind = FileDialogKind::Save;
		dialog.title = "Save Project As";
		dialog.filters = projectFilters();
		dialog.parent = window.id();
		dialog.location =
			m_file.empty() ? std::filesystem::path("Untitled.dawproject") : std::filesystem::u8path(m_file);
		m_shell.showFileDialog(dialog,
			[this, done = reportFailure(window)](const FileDialogResult& chosen)
			{
				if (chosen.files.empty())
					return;
				m_binding.send("project.save", {{"path", withProjectExtension(chosen.files.front()).u8string()}}, done);
			});
	}
}
