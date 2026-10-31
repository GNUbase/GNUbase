// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/poolpanel.h"

#include "ui/widgets.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		// The toolbar's buttons and what they send: an engine command, or a UI command through run.
		struct Button
		{
			const char* id;
			const char* label;
			const char* command;
			bool ui;
		};

		constexpr Button g_buttons[] = {
			{"pool-import", "Import", "pool.import", true},
			{"pool-new-folder", "New Folder", "poolFolder.add", false},
			{"pool-remove-unused", "Remove Unused Media", "pool.removeUnused", false},
			{"pool-find-missing", "Find Missing Files", "pool.findMissing", true},
			{"pool-remove-missing", "Remove Missing Files", "media.removeMissing", false},
			{"pool-conform", "Conform Files", "media.conform", false},
		};

		constexpr const char* g_columns[] = {"used", "status", "info", "path"};

		std::string encode(const std::string& text)
		{
			return Rml::StringUtilities::EncodeRml(text);
		}

		void setText(Rml::Element& cell, const std::string& text)
		{
			const std::string rml = encode(text);
			if (cell.GetInnerRML() != rml)
				cell.SetInnerRML(rml);
		}

		// E.g. "44.1 kHz, Stereo, 1.500 s".
		std::string info(const Json& clip)
		{
			std::ostringstream text;
			text << std::setprecision(4) << clip.at("sampleRate").get<double>() / 1000.0 << " kHz, ";
			const int channels = clip.at("channels").get<int>();
			if (channels == 1)
				text << "Mono";
			else if (channels == 2)
				text << "Stereo";
			else
				text << channels << " Channels";
			const double seconds =
				static_cast<double>(clip.at("length").get<int64_t>()) / clip.at("sampleRate").get<double>();
			text << ", " << std::fixed << std::setprecision(3) << seconds << " s";
			return text.str();
		}

		std::string megabytes(uint64_t bytes)
		{
			std::ostringstream text;
			text << std::fixed << std::setprecision(2) << static_cast<double>(bytes) / (1024.0 * 1024.0) << " MB";
			return text.str();
		}

		// The value of the row an element is in, or empty.
		std::string rowValue(Rml::Element* element)
		{
			for (; element != nullptr && element->GetTagName() != "listview"; element = element->GetParentNode())
				if (element->GetTagName() == "listitem")
					return element->GetAttribute<Rml::String>("value", "");
			return {};
		}

		uint64_t idOf(const std::string& value)
		{
			return value.size() > 1 ? std::stoull(value.substr(1)) : 0;
		}
	}

	PoolPanel::PoolPanel(EngineBinding& binding, Rml::Element& element, Run run)
		: m_binding(binding), m_run(std::move(run)),
		  m_changed(binding.on("history.changed", [this](const engine::Event&) { request(); })),
		  m_saved(binding.on("project.saved", [this](const engine::Event&) { request(); }))
	{
		std::string rml = R"(<div class="pool"><div class="toolbar pool-toolbar">)";
		for (const Button& button : g_buttons)
			rml += std::string(R"(<button id=")") + button.id + R"(">)" + button.label + "</button>";
		rml +=
			R"(</div><div id="pool-info" class="pool-info"/><div class="pool-header"><span class="name">Media</span>)"
			R"(<span class="used">Used</span><span class="status">Status</span><span class="info">Info</span>)"
			R"(<span class="path">Path</span></div><listview id="pool" class="pool-list" multiple/></div>)";
		element.SetInnerRML(rml);
		Rml::Element* root = element.GetFirstChild();
		m_root = root->GetObserverPtr();
		m_view = root->GetElementById("pool")->GetObserverPtr();
		for (const Rml::EventId id :
			{Rml::EventId::Click, Rml::EventId::Change, Rml::EventId::Mousedown, Rml::EventId::Mouseup})
			root->AddEventListener(id, this);
		root->AddEventListener(Rml::EventId::Blur, this, true); // blur doesn't bubble
		root->AddEventListener(g_treeToggleEvent, this);
		request();
	}

	PoolPanel::~PoolPanel()
	{
		if (!m_root)
			return;
		for (const Rml::EventId id :
			{Rml::EventId::Click, Rml::EventId::Change, Rml::EventId::Mousedown, Rml::EventId::Mouseup})
			m_root->RemoveEventListener(id, this);
		m_root->RemoveEventListener(Rml::EventId::Blur, this, true);
		m_root->RemoveEventListener(g_treeToggleEvent, this);
	}

	void PoolPanel::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr)
			return;
		const bool field = target->GetTagName() == "input";
		if (event == Rml::EventId::Click && target->GetTagName() == "button")
		{
			const std::string id = target->GetId();
			for (const Button& button : g_buttons)
				if (id == button.id)
				{
					if (button.ui)
						m_run(button.command);
					else if (id == "pool-new-folder")
						addFolder();
					else
						m_binding.send(button.command);
				}
		}
		else if (field && (event == Rml::EventId::Blur || event.GetParameter<bool>("linebreak", false)))
		{
			rename(*target);
		}
		else if (event == g_treeToggleEvent)
		{
			const auto value = event.GetParameter<Rml::String>("value", "");
			if (event.GetParameter<bool>("expanded", true))
				m_collapsed.erase(value);
			else
				m_collapsed.insert(value);
		}
		else if (event == Rml::EventId::Mousedown && !field)
		{
			// Clips dragged onto a folder: the selected ones if the mouse went down on one of them.
			m_dragged.clear();
			const std::string value = rowValue(target);
			if (value.empty() || value[0] != 'c')
				return;
			const std::vector<std::string> selected = selection();
			if (std::find(selected.begin(), selected.end(), value) == selected.end())
				m_dragged = {value};
			else
				std::copy_if(selected.begin(), selected.end(), std::back_inserter(m_dragged),
					[](const std::string& one) { return one[0] == 'c'; });
		}
		else if (event == Rml::EventId::Mouseup)
		{
			const std::string value = rowValue(target);
			if (!m_dragged.empty() && !value.empty() && value[0] == 'f')
				drop(value);
			m_dragged.clear();
		}
	}

	std::vector<std::string> PoolPanel::selection() const
	{
		std::vector<std::string> values;
		if (!m_view)
			return values;
		std::istringstream list(m_view->GetAttribute<Rml::String>("value", ""));
		for (std::string value; std::getline(list, value, ',');)
			if (!value.empty())
				values.push_back(value);
		return values;
	}

	void PoolPanel::request()
	{
		m_binding.send("pool.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					show(result.value);
			});
	}

	namespace
	{
		struct Row
		{
			std::string value; // "f<id>" for a folder, "c<id>" for a clip; "f0" is the Audio folder
			int level = 0;
			std::string name;
			std::string used;
			std::string status;
			std::string info;
			std::string path;
			bool missing = false;
		};

		// The tree's rows: the Audio folder, then in each folder its folders and its clips.
		std::vector<Row> rows(const Json& pool)
		{
			// Folders first, then clips, each by name.
			std::multimap<uint64_t, const Json*> folders;
			std::multimap<uint64_t, const Json*> clips;
			for (const Json& folder : pool.at("folders"))
				folders.emplace(folder.at("parent").get<uint64_t>(), &folder);
			for (const Json& clip : pool.at("clips"))
				clips.emplace(clip.at("folder").get<uint64_t>(), &clip);
			const auto sorted = [](const std::multimap<uint64_t, const Json*>& from, uint64_t parent)
			{
				const auto [first, last] = from.equal_range(parent);
				std::vector<const Json*> items;
				for (auto item = first; item != last; ++item)
					items.push_back(item->second);
				std::stable_sort(items.begin(), items.end(), [](const Json* a, const Json* b)
					{ return a->at("name").get<std::string>() < b->at("name").get<std::string>(); });
				return items;
			};
			std::vector<Row> result;
			result.push_back({"f0", 0, "Audio", "", "", "", "", false});
			const std::function<void(uint64_t, int)> add = [&](uint64_t parent, int level)
			{
				for (const Json* folder : sorted(folders, parent))
				{
					const auto id = folder->at("id").get<uint64_t>();
					result.push_back({"f" + std::to_string(id), level, folder->at("name").get<std::string>(), "", "",
						"", "", false});
					add(id, level + 1);
				}
				for (const Json* clip : sorted(clips, parent))
				{
					const bool missing = clip->at("missing").get<bool>();
					const int used = clip->at("used").get<int>();
					result.push_back({"c" + std::to_string(clip->at("id").get<uint64_t>()), level,
						clip->at("name").get<std::string>(), used == 0 ? "" : std::to_string(used),
						missing ? "Missing" : (clip->at("external").get<bool>() ? "External" : ""), info(*clip),
						clip->at("path").get<std::string>(), missing});
				}
			};
			add(0, 1);
			return result;
		}
	}

	void PoolPanel::show(const Json& pool)
	{
		Rml::Element* view = m_view.get();
		if (view == nullptr)
			return;
		m_pool = pool;
		const std::vector<Row> all = rows(pool);
		std::vector<std::string> values;
		values.reserve(all.size());
		for (const Row& row : all)
			values.push_back(row.value);
		if (values != m_values)
		{
			std::string rml;
			for (const Row& row : all)
			{
				const bool folder = row.value[0] == 'f';
				rml += R"(<listitem value=")" + row.value + R"(" level=")" + std::to_string(row.level) + '"' +
					(folder && m_collapsed.count(row.value) == 0 ? " expanded" : "") + R"( class=")" +
					(folder ? "folder" : "clip") + R"("><span class="name">)" +
					(row.value == "f0" ? encode(row.name) : R"(<input type="text"/>)") +
					R"(</span><span class="used"/><span class="status"/><span class="info"/><span class="path"/></listitem>)";
			}
			view->SetInnerRML(rml);
			m_values = std::move(values);
		}
		for (size_t i = 0; i < all.size(); ++i)
		{
			Rml::Element* row = view->GetChild(static_cast<int>(i));
			const Row& data = all[i];
			row->SetClass("missing", data.missing);
			row->SetClass("unused", data.value[0] == 'c' && data.used.empty());
			Rml::Element* name = row->QuerySelector("span.name");
			// What the user is typing stays until it is sent.
			if (Rml::Element* field = name->GetFirstChild();
				field != nullptr && field->GetTagName() == "input" && !field->IsPseudoClassSet("focus"))
				field->SetAttribute("value", data.name);
			const std::string texts[] = {data.used, data.status, data.info, data.path};
			for (size_t column = 0; column < std::size(g_columns); ++column)
				setText(*row->QuerySelector(std::string("span.") + g_columns[column]), texts[column]);
		}
		if (Rml::Element* line = m_root ? m_root->GetElementById("pool-info") : nullptr)
		{
			const std::string rml = R"(<span class="audio-files">Audio Files: )" +
				std::to_string(pool.at("audioFiles").get<int>()) + R"(</span><span class="used-files">Used: )" +
				std::to_string(pool.at("usedFiles").get<int>()) + R"(</span><span class="total-size">Total Size: )" +
				megabytes(pool.at("totalSize").get<uint64_t>()) +
				R"(</span><span class="external-files">External Files: )" +
				std::to_string(pool.at("externalFiles").get<int>()) +
				R"(</span><span class="missing-files">Missing: )" + std::to_string(pool.at("missingFiles").get<int>()) +
				"</span>";
			if (line->GetInnerRML() != rml)
				line->SetInnerRML(rml);
		}
		// Answers arrive outside input handling: ask the window to update and draw.
		if (Rml::Context* context = view->GetContext())
			context->RequestNextUpdate(0);
	}

	void PoolPanel::rename(Rml::Element& field)
	{
		const std::string value = rowValue(&field);
		const auto name = field.GetAttribute<Rml::String>("value", "");
		if (value.size() < 2 || name.empty())
			return;
		const bool folder = value[0] == 'f';
		const uint64_t id = idOf(value);
		for (const Json& item : m_pool.value(folder ? "folders" : "clips", Json::array()))
			if (item.at("id").get<uint64_t>() == id && item.at("name").get<std::string>() != name)
				m_binding.send(
					folder ? "poolFolder.set" : "clip.set", {{folder ? "folder" : "clip", id}, {"name", name}});
	}

	void PoolPanel::drop(const std::string& folder)
	{
		const uint64_t target = idOf(folder);
		Json commands = Json::array();
		for (const std::string& clip : m_dragged)
			commands.push_back({{"command", "clip.set"}, {"parameters", {{"clip", idOf(clip)}, {"folder", target}}}});
		if (commands.size() == 1)
			m_binding.send("clip.set", commands[0].at("parameters"));
		else
			m_binding.send("history.transaction", {{"description", "Move clips"}, {"commands", commands}});
	}

	void PoolPanel::addFolder()
	{
		// Inside the selected folder, or the folder of the selected clip.
		uint64_t parent = 0;
		const std::vector<std::string> selected = selection();
		if (!selected.empty() && selected[0][0] == 'f')
			parent = idOf(selected[0]);
		else if (!selected.empty())
			for (const Json& clip : m_pool.value("clips", Json::array()))
				if (clip.at("id").get<uint64_t>() == idOf(selected[0]))
					parent = clip.at("folder").get<uint64_t>();
		m_binding.send("poolFolder.add", {{"parent", parent}});
	}
}
