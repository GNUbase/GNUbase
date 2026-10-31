// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/projectfiles.h"

#include "platform/log.h"
#include "ui/widgets.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/StringUtilities.h>

#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		const std::vector<FileFilter>& projectFilters()
		{
			static const std::vector<FileFilter> filters{{"DAWproject", "dawproject"}, {"All files", "*"}};
			return filters;
		}

		// Logs a failed command, e.g. a file that can't be opened.
		void logFailure(const engine::Result& result)
		{
			if (!result.ok())
				logging::warning("Project handling: {}", result.error);
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
		  m_reset(binding.on("project.reset", [this](const engine::Event&) { readLocation(); }))
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
					m_binding.send("project.save", Json::object(), logFailure);
			}});
		keys.add({"file.saveAs", "Saves the project as a file chosen in a file dialog.",
			[this](Window& window, Rml::Element*) { saveAs(window); }});
		keys.add({"file.saveNewVersion", "Saves the project as the next numbered version of its file.",
			[this](Window& window, Rml::Element*)
			{
				if (m_file.empty())
					saveAs(window);
				else
					m_binding.send("project.saveNewVersion", Json::object(), logFailure);
			}});
		keys.add({"file.saveTemplate", "Saves the project as a template, named in a dialog.",
			[this](Window& window, Rml::Element*)
			{
				openDialog(window, "save-template.rml",
					[this](Rml::ElementDocument& dialog, const std::string& result)
					{
						auto* name = dynamic_cast<Rml::ElementFormControl*>(dialog.GetElementById("template-name"));
						if (result == "ok" && name != nullptr && !name->GetValue().empty())
							m_binding.send("project.saveTemplate", {{"name", name->GetValue()}}, logFailure);
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
					[this](const FileDialogResult& chosen)
					{
						if (!chosen.files.empty())
							m_binding.send("project.backup", {{"folder", chosen.files.front().u8string()}}, logFailure);
					});
			}});
		keys.add({"file.revert", "Goes back to the project as it was last saved.", [this](Window& window, Rml::Element*)
			{ unlessUnsaved(window, [this] { m_binding.send("project.revert", Json::object(), logFailure); }); }});
		keys.enableWhen("file.revert", [this](Window&, Rml::Element*) { return !m_file.empty(); });
		// Quitting closes every window, so it waits until the event that asked for it is done with them.
		keys.add({"file.quit", "Quits GNUbase.", [this](Window& window, Rml::Element*)
			{ unlessUnsaved(window, [this] { m_shell.post([this] { m_shell.quit(); }); }); }});

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
					unlessUnsaved(window, [this] { m_binding.send("project.new", Json::object(), logFailure); });
				}
				else if (result == "template")
				{
					if (const std::string name = selected(closed, "start-templates", m_templates); !name.empty())
						unlessUnsaved(
							window, [this, name] { m_binding.send("project.new", {{"template", name}}, logFailure); });
				}
				else if (result == "recent")
				{
					if (const std::string file = selected(closed, "start-recent", m_recent); !file.empty())
						unlessUnsaved(
							window, [this, file] { m_binding.send("project.load", {{"path", file}}, logFailure); });
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
		// Before the document closes, so that the callback can read its fields; m_dialog is free for another dialog.
		Closed closed = std::move(m_closed);
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

	void ProjectFiles::unlessUnsaved(Window& window, std::function<void()> action)
	{
		m_binding.send("project.revision", Json::object(),
			[this, id = window.id(), action = std::move(action)](const engine::Result& result)
			{
				if (!result.ok() || !result.value.at("modified").get<bool>())
				{
					action();
					return;
				}
				// The dialog that asked may still be closing.
				m_shell.post(
					[this, id, action]
					{
						if (Window* asking = m_shell.find(id))
							openDialog(*asking, "discard-changes.rml",
								[action](Rml::ElementDocument&, const std::string& answer)
								{
									if (answer == "ok")
										action();
								});
					});
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
				unlessUnsaved(*window, [this, file] { m_binding.send("project.load", {{"path", file}}, logFailure); });
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
			[this](const FileDialogResult& chosen)
			{
				if (chosen.files.empty())
					return;
				std::filesystem::path file = chosen.files.front();
				if (!file.has_extension())
					file += ".dawproject";
				m_binding.send("project.save", {{"path", file.u8string()}}, logFailure);
			});
	}
}
