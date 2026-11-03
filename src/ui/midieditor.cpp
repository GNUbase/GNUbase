// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/midieditor.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		// The Part Editing Mode pane's options (reference manual pp. 1142-1143), as editor.set names them.
		constexpr std::array<std::pair<const char*, const char*>, 3> g_editingModes = {{
			{"allParts", "All Parts"},
			{"activePart", "Active Part"},
			{"activeTrack", "All Parts on Active Track"},
		}};

		constexpr std::array<Rml::EventId, 2> g_events = {Rml::EventId::Click, Rml::EventId::Dblclick};

		std::string encode(const std::string& text)
		{
			return Rml::StringUtilities::EncodeRml(text);
		}

		std::string attribute(const Rml::Element& element, const char* name)
		{
			return element.GetAttribute<Rml::String>(name, "");
		}

		// The id in the attribute of the element or its closest parent that has it, up to stop; 0 for none.
		uint64_t idOf(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->HasAttribute(name))
				{
					const std::string value = attribute(*element, name);
					return value.empty() ? 0 : std::stoull(value);
				}
			return 0;
		}

		Rml::Element* closest(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->IsClassSet(name))
					return element;
			return nullptr;
		}

		void setInner(Rml::Element* element, const std::string& rml)
		{
			if (element != nullptr && element->GetInnerRML() != rml)
				element->SetInnerRML(rml);
		}

		bool contains(const Json& ids, uint64_t id)
		{
			return std::find(ids.begin(), ids.end(), Json(id)) != ids.end();
		}

		bool holdsParts(const Json& track)
		{
			const std::string type = track.at("type").get<std::string>();
			return type == "midi" || type == "instrument";
		}

		std::string idText(uint64_t value)
		{
			return std::to_string(value);
		}
	}

	MidiEditor::MidiEditor(
		EngineBinding& binding, Rml::Element& root, std::string view, EditorPart part, std::function<void()> moved)
		: m_binding(binding), m_root(root.GetObserverPtr()), m_name(std::move(view)), m_part(part),
		  m_moved(std::move(moved))
	{
		if (m_part == EditorPart::Editor)
		{
			// The toolbar (reference manual pp. 1142-1153, 1194-1196). Its menus go into the body, so that no zone
			// clips them.
			const bool window = m_name == "window";
			root.SetInnerRML(R"(<div class="midi-editor" editor=")" + m_name +
				R"("><div class="toolbar midi-editor-toolbar">)"
				R"x(<button id="editor-keep-contents" title="Keep Editor Contents (Don't follow Project window event )x"
				R"x(selection)">Keep Editor Contents</button>)x"
				R"(<button id="editor-editing-mode" menu="editor-editing-mode-menu" title="Part Editing Mode">)"
				R"(Part Editing Mode</button>)"
				R"(<button id="editor-active-part" menu="editor-active-part-menu" title="Activate Part for Editing"/>)"
				R"(<div class="spacer"/>)" +
				(window ? R"(<button id="editor-move" title="Open in Lower Zone">Open in Lower Zone</button>)"
						: R"(<button id="editor-move" title="Open in Separate Window">Open in Separate Window</button>)") +
				R"(</div>)" + (window ? R"(<div class="editor-global-tracks"/>)" : "") +
				R"(<div class="editor-parts"/></div>)");
			Rml::ElementDocument* document = root.GetOwnerDocument();
			for (const char* menu : {"editor-editing-mode-menu", "editor-active-part-menu"})
			{
				const Rml::ElementPtr holder = document->CreateElement("div");
				holder->SetInnerRML(std::string(R"(<menu id=")") + menu + R"("/>)");
				Rml::Element* added = document->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
				added->AddEventListener(Rml::EventId::Click, this);
				m_menus.push_back(added->GetObserverPtr());
			}
		}
		else if (m_part == EditorPart::Visibility)
		{
			root.SetInnerRML(R"(<div class="editor-visibility"/>)");
		}
		else
		{
			root.SetInnerRML(R"(<div class="editor-global-tracks-section"><div class="section-header">Global )"
							 R"(Tracks</div><div class="editor-global-track-list"/></div>)");
		}
		for (const Rml::EventId event : g_events)
			root.AddEventListener(event, this);

		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event,
				[this, reset = std::string(event) == "project.reset"](const engine::Event&)
				{
					requestProject();
					if (reset)
						requestView(); // a new project closes the editors
				}));
		m_subscriptions.push_back(m_binding.on("editor.changed",
			[this](const engine::Event& event)
			{
				if (event.data.at("editor") != m_name)
					return;
				m_view = event.data;
				show();
			}));
		requestView();
		requestProject();
	}

	MidiEditor::~MidiEditor()
	{
		if (Rml::Element* root = m_root.get())
			for (const Rml::EventId event : g_events)
				root->RemoveEventListener(event, this);
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (Rml::Element* element = menu.get())
			{
				element->RemoveEventListener(Rml::EventId::Click, this);
				if (Rml::Element* parent = element->GetParentNode())
					parent->RemoveChild(element);
			}
	}

	// ponytail: reads the whole project after every change, as the event display does.
	void MidiEditor::requestProject()
	{
		if (m_loading)
		{
			m_stale = true;
			return;
		}
		m_loading = true;
		m_binding.send("project.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_loading = false;
				if (result.ok())
				{
					m_project = result.value;
					// The view leaves out parts that are gone; ask again, so that it has their removal too.
					requestView();
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	void MidiEditor::requestView()
	{
		m_binding.send("editor.get", {{"editor", m_name}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_view = result.value;
				show();
			});
	}

	const Json* MidiEditor::track(uint64_t id) const
	{
		if (!m_project.contains("tracks"))
			return nullptr;
		for (const Json& track : m_project.at("tracks")) // in the project, not a copy
			if (track.at("id") == id)
				return &track;
		return nullptr;
	}

	std::pair<size_t, size_t> MidiEditor::partsShown(uint64_t track) const
	{
		std::vector<uint64_t> tracks = {track};
		for (size_t i = 0; i < tracks.size(); ++i) // the folder's tracks, and theirs
			for (const Json& child : m_project.value("tracks", Json::array()))
				if (child.at("parent") == tracks[i])
					tracks.push_back(child.at("id").get<uint64_t>());
		std::pair<size_t, size_t> counts;
		const Json parts = m_view.value("parts", Json::array());
		for (const Json& part : m_project.value("midiParts", Json::array()))
			if (std::find(tracks.begin(), tracks.end(), part.at("track").get<uint64_t>()) != tracks.end())
			{
				++counts.first;
				counts.second += contains(parts, part.at("id").get<uint64_t>()) ? 1 : 0;
			}
		return counts;
	}

	void MidiEditor::show()
	{
		Rml::Element* root = m_root.get();
		if (root == nullptr || m_view.empty() || !m_project.contains("tracks"))
			return;
		if (m_part == EditorPart::Editor)
		{
			if (Rml::Element* editor = root->QuerySelector(".midi-editor"))
				editor->SetClass("empty", m_view.at("parts").empty());
			if (Rml::Element* keep = root->QuerySelector("#editor-keep-contents"))
				keep->SetClass("checked", m_view.at("keepContents").get<bool>());
			std::string active;
			for (const Json& part : m_project.at("midiParts"))
				if (part.at("id") == m_view.at("activePart"))
					active = part.at("name").get<std::string>();
			setInner(root->QuerySelector("#editor-active-part"), encode(active.empty() ? "No Part" : active));
			setInner(root->QuerySelector(".editor-global-tracks"), globalTracksRml());
			setInner(root->QuerySelector(".editor-parts"), editorRml());
			fillMenus();
		}
		else if (m_part == EditorPart::Visibility)
		{
			setInner(root->QuerySelector(".editor-visibility"), visibilityRml());
		}
		else
		{
			setInner(root->QuerySelector(".editor-global-track-list"), globalTracksRml());
		}
		if (Rml::Context* context = root->GetContext())
			context->RequestNextUpdate(0);
	}

	// The stand-in for the note display: a row per part, in the editor's order, the active and the editable ones
	// marked. Global tracks above it, as their names.
	std::string MidiEditor::editorRml() const
	{
		std::string rml;
		const Json& editable = m_view.at("editable");
		for (const Json& id : m_view.at("parts"))
			for (const Json& part : m_project.at("midiParts"))
				if (part.at("id") == id)
				{
					const Json* owner = track(part.at("track").get<uint64_t>());
					rml += R"(<div class="editor-part)" + std::string(id == m_view.at("activePart") ? " active" : "") +
						(contains(editable, id.get<uint64_t>()) ? " editable" : "") + R"(" part=")" + id.dump() +
						R"("><span class="editor-part-track">)" +
						encode(owner != nullptr ? owner->at("name").get<std::string>() : "") +
						R"(</span><span class="editor-part-name">)" + encode(part.at("name").get<std::string>()) +
						"</span></div>";
				}
		return rml;
	}

	// In the editor window, the global tracks it shows, in the track list's order (p. 1145); in the inspector's
	// Global Tracks section, a toggle per global track of the project.
	std::string MidiEditor::globalTracksRml() const
	{
		std::string rml;
		const Json& shown = m_view.at("globalTracks");
		for (const Json& track : m_project.at("tracks"))
		{
			const std::string type = track.at("type").get<std::string>();
			if (type != "marker" && type != "ruler")
				continue;
			const auto trackId = track.at("id").get<uint64_t>();
			const std::string name = encode(track.at("name").get<std::string>());
			if (m_part == EditorPart::GlobalTracks)
				rml += R"(<button class="editor-global-toggle)" +
					std::string(contains(shown, trackId) ? " checked" : "") + R"(" track=")" + idText(trackId) +
					R"(">)" + name + "</button>";
			else if (contains(shown, trackId))
				rml += R"(<div class="editor-global-track" track=")" + idText(trackId) + R"(">)" + name + "</div>";
		}
		return rml;
	}

	// The tracks whose parts the editor can show (p. 1148): MIDI and instrument tracks, and folders with such tracks
	// in them. A check while all their parts show, a rectangle (.partial) while some do (p. 1151).
	std::string MidiEditor::visibilityRml() const
	{
		std::string rml;
		for (const Json& track : m_project.at("tracks"))
		{
			const auto trackId = track.at("id").get<uint64_t>();
			const bool folder = track.at("type") == "folder";
			if (!holdsParts(track) && !folder)
				continue;
			const auto [parts, shown] = partsShown(trackId);
			if (folder && parts == 0)
				continue;
			std::string state;
			if (parts > 0 && shown == parts)
				state = " checked";
			else if (shown > 0)
				state = " partial";
			rml += R"(<div class="editor-visibility-track)" +
				std::string(m_view.at("activeTrack") == trackId ? " active" : "") + R"(" track=")" + idText(trackId) +
				R"("><button class="editor-visibility-check)" + state + '"' + (parts == 0 ? R"( disabled="")" : "") +
				R"(/><span class="editor-visibility-name">)" + encode(track.at("name").get<std::string>()) +
				"</span></div>";
		}
		return rml;
	}

	void MidiEditor::fillMenus()
	{
		if (Rml::Element* menu = m_menus.at(0).get())
		{
			std::string rml;
			for (const auto& [mode, label] : g_editingModes)
				rml += std::string(R"(<menuitem type="toggle" editing-mode=")") + mode + '"' +
					(m_view.at("editingMode") == mode ? R"( checked="")" : "") + ">" + label + "</menuitem>";
			setInner(menu, rml);
		}
		if (Rml::Element* menu = m_menus.at(1).get())
		{
			// Under the name of each track, its parts by position (p. 1143).
			std::string rml;
			uint64_t last = 0;
			for (const Json& id : m_view.at("parts"))
				for (const Json& part : m_project.at("midiParts"))
					if (part.at("id") == id)
					{
						const auto owner = part.at("track").get<uint64_t>();
						if (owner != last)
						{
							const Json* found = track(owner);
							rml += R"(<menuitem class="editor-menu-track" disabled="">)" +
								encode(found != nullptr ? found->at("name").get<std::string>() : "") + "</menuitem>";
							last = owner;
						}
						rml += R"(<menuitem type="toggle" active-part=")" + id.dump() + '"' +
							(id == m_view.at("activePart") ? R"( checked="")" : "") + ">" +
							encode(part.at("name").get<std::string>()) + "</menuitem>";
					}
			setInner(menu, rml);
		}
	}

	void MidiEditor::set(Json parameters)
	{
		parameters["editor"] = m_name;
		m_binding.send("editor.set", std::move(parameters)); // editor.changed shows it
	}

	void MidiEditor::toggleTrack(uint64_t track, bool exclusive)
	{
		const auto [parts, shown] = partsShown(track);
		if (parts == 0)
			return;
		if (exclusive)
			set({{"exclusive", true}, {"showTracks", {track}}});
		else if (shown == parts)
			set({{"hideTracks", {track}}});
		else
			set({{"showTracks", {track}}}); // also where some show: the rectangle shows them all
	}

	void MidiEditor::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr || m_view.empty())
			return;
		const Rml::Element* root = m_root.get();
		const bool shift = event.GetParameter("shift_key", 0) != 0;
		if (event.GetId() == Rml::EventId::Dblclick)
		{
			// Double-clicking a track shows it exclusively (p. 1151).
			if (closest(target, "editor-visibility-name", root) != nullptr)
				toggleTrack(idOf(target, "track", root), true);
			return;
		}
		if (target->HasAttribute("editing-mode"))
		{
			set({{"editingMode", attribute(*target, "editing-mode")}});
		}
		else if (target->HasAttribute("active-part"))
		{
			set({{"activePart", idOf(target, "active-part", nullptr)}});
		}
		else if (target->GetId() == "editor-keep-contents")
		{
			set({{"keepContents", !m_view.at("keepContents").get<bool>()}});
		}
		else if (target->GetId() == "editor-move")
		{
			// Open in Separate Window and Open in Lower Zone (p. 1196): the other editor takes the parts.
			if (m_view.at("parts").empty())
				return;
			m_binding.send("editor.open",
				{{"editor", m_name == "window" ? "lower" : "window"}, {"parts", m_view.at("parts")},
					{"activePart", m_view.at("activePart")}});
			m_binding.send("editor.close", {{"editor", m_name}});
			if (m_moved)
				m_moved();
		}
		else if (closest(target, "editor-visibility-check", root) != nullptr)
		{
			toggleTrack(idOf(target, "track", root), shift);
		}
		else if (closest(target, "editor-visibility-name", root) != nullptr)
		{
			// Clicking a shown track's name activates it for editing (p. 1149).
			const uint64_t clicked = idOf(target, "track", root);
			if (partsShown(clicked).second > 0 && (track(clicked) == nullptr || holdsParts(*track(clicked))))
				set({{"activeTrack", clicked}});
		}
		else if (closest(target, "editor-global-toggle", root) != nullptr)
		{
			const uint64_t clicked = idOf(target, "track", root);
			Json tracks = Json::array();
			for (const Json& shown : m_view.at("globalTracks"))
				if (shown != clicked)
					tracks.push_back(shown);
			if (!contains(m_view.at("globalTracks"), clicked))
				tracks.push_back(clicked);
			set({{"globalTracks", tracks}});
		}
		else if (const uint64_t part = idOf(target, "part", root); part != 0)
		{
			set({{"activePart", part}}); // clicking an event of a part activates it (p. 1152)
		}
	}
}
