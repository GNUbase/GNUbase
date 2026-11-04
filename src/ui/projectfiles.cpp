// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/projectfiles.h"

#include "platform/log.h"
#include "ui/widgets.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/StringUtilities.h>

#include <filesystem>
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
	}

	ProjectFiles::ProjectFiles(KeyCommands& keys, EngineBinding& binding, Shell& shell, std::filesystem::path folder)
		: m_binding(binding), m_shell(shell), m_folder(std::move(folder)),
		  m_saved(binding.on("project.saved",
			  [this](const engine::Event& event) { m_file = event.data.at("file").get<std::string>(); })),
		  m_reset(binding.on("project.reset", [this](const engine::Event&) { readLocation(); })),
		  m_jobFinished(binding.on("job.finished",
			  [this](const engine::Event& event)
			  {
				  // A backup runs as a job, so most of its failures come after project.backup returned.
				  const Json& job = event.data.at("job");
				  if (m_backupWindow && job.at("kind") == "project.backup" && job.at("state") == "failed")
					  showError(*m_backupWindow, job.at("error").get<std::string>());
			  })),
		  m_autoSaveFailed(
			  binding.on("project.autoSaveFailed", [this](const engine::Event& event) { autoSaveFailed(event.data); }))
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
				openDialog(window, "save-template.rml",
					[this, done = reportFailure(window)](Rml::ElementDocument& dialog, const std::string& result)
					{
						auto* name = dynamic_cast<Rml::ElementFormControl*>(dialog.GetElementById("template-name"));
						if (result == "ok" && name != nullptr && !name->GetValue().empty())
							m_binding.send("project.saveTemplate", {{"name", name->GetValue()}}, done);
					});
			}});
		keys.add({"file.backup", "Backs the project up into a folder chosen in a file dialog.",
			[this](Window& window, Rml::Element*)
			{
				FileDialog dialog;
				dialog.kind = FileDialogKind::Folder;
				dialog.title = "Back up Project";
				dialog.parent = window.id();
				m_shell.showFileDialog(dialog,
					[this, id = window.id(), done = reportFailure(window)](const FileDialogResult& chosen)
					{
						if (chosen.files.empty())
							return;
						m_backupWindow = id;
						m_binding.send("project.backup", {{"folder", chosen.files.front().u8string()}}, done);
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
		if (Rml::Element* dialog = m_dialog.get())
		{
			dialog->RemoveEventListener(Rml::EventId::Dblclick, this);
			dialog->RemoveEventListener(g_dialogCloseEvent, this);
		}
	}

	void ProjectFiles::openStartScreen(Window& window)
	{
		Rml::ElementDocument* dialog = openDialog(window, "start-screen.rml",
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
							showStartScreen(*document, templates.value.at("templates"), recent.value.at("projects"));
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

	Rml::ElementDocument* ProjectFiles::openDialog(Window& window, const char* file, Closed closed)
	{
		// A failed auto save's error came on its own, not from what the user does: it makes way for the dialog of
		// something the user does, such as a failed save or the unsaved changes question, and shows again later.
		if (m_dialog && m_autoSaveErrorOpen)
			closeDialog(g_replaced);
		if (m_dialog)
			return nullptr; // one at a time
		const std::filesystem::path path = m_folder / file;
		Rml::ElementDocument* dialog = window.context().LoadDocument(path.generic_u8string());
		if (dialog == nullptr)
		{
			logging::warning("Cannot open the dialog {}", path.u8string());
			return nullptr;
		}
		dialog->AddEventListener(g_dialogCloseEvent, this);
		dialog->Show(Rml::ModalFlag::Modal);
		m_dialog = dialog->GetObserverPtr();
		m_closed = std::move(closed);
		return dialog;
	}

	// Double clicks in the start screen, which create from a template or open a recent project, and every dialog's
	// close event.
	void ProjectFiles::ProcessEvent(Rml::Event& event)
	{
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

	void ProjectFiles::openError(WindowId window, const std::string& error, bool autoSave)
	{
		// The dialog that asked may still be closing.
		m_shell.post(
			// NOLINTNEXTLINE(bugprone-exception-escape): only MSVC's std::string; out of memory only
			[this, window, error, autoSave]
			{
				Window* showing = m_shell.find(window);
				Rml::ElementDocument* dialog = showing != nullptr
					? openDialog(*showing, "project-error.rml",
						  [this, autoSave](Rml::ElementDocument&, const std::string& result)
						  {
							  // Shown again at the next failure if another dialog took its place.
							  if (autoSave && result == g_replaced)
								  m_autoSaveFailureShown = false;
						  })
					: nullptr;
				if (dialog == nullptr)
					return; // an auto save's error shows at the next failure instead
				if (Rml::Element* message = dialog->GetElementById("project-error-message"))
					message->SetInnerRML(Rml::StringUtilities::EncodeRml(error));
				if (autoSave)
				{
					m_autoSaveFailureShown = true;
					m_autoSaveErrorOpen = true;
				}
			});
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
				// The dialog that asked may still be closing.
				m_shell.post(
					[this, id, action, asked]
					{
						Window* asking = m_shell.find(id);
						Rml::ElementDocument* question = asking != nullptr
							? openDialog(*asking, "discard-changes.rml",
								  [action](Rml::ElementDocument&, const std::string& answer)
								  {
									  if (answer == "ok")
										  action();
								  })
							: nullptr;
						if (asked)
							asked(question);
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
