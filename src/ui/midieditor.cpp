// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/midieditor.h"

#include "engine/midi/chords.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <map>
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

		// Whether the track's parts can show: audio tracks' in the audio part editor, else MIDI and instrument tracks'.
		bool holdsParts(const Json& track, bool audio)
		{
			const std::string type = track.at("type").get<std::string>();
			return audio ? type == "audio" : type == "midi" || type == "instrument";
		}

		std::string idText(uint64_t value)
		{
			return std::to_string(value);
		}

		// How much a track in the Visibility tab indents per folder around it, in dp, as the track visibility's.
		constexpr int g_indent = 12;
	}

	void ProjectReads::read(std::function<void(const engine::Result&)> done)
	{
		if (!m_loading)
		{
			m_current.push_back(std::move(done));
			send();
		}
		else if (m_binding.handed() == m_sentAt)
			m_current.push_back(std::move(done));
		else
			m_next.push_back(std::move(done));
	}

	void ProjectReads::send()
	{
		m_loading = true;
		m_sentAt = m_binding.handed();
		m_binding.send("project.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_loading = false;
				std::vector<std::function<void(const engine::Result&)>> done = std::exchange(m_current, {});
				// Before the answer goes on, so that an editor that asks again in it joins the next read.
				if (!m_next.empty())
				{
					m_current = std::exchange(m_next, {});
					send();
				}
				for (const std::function<void(const engine::Result&)>& each : done)
					each(result);
			});
	}

	MidiEditor::MidiEditor(EngineBinding& binding, ProjectReads& reads, Rml::Element& root, std::string view,
		EditorPart part, std::function<void()> moved)
		: m_binding(binding), m_reads(reads), m_root(root.GetObserverPtr()), m_name(std::move(view)), m_part(part),
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
				R"(</div>)" + (window ? R"(<div class="editor-global-tracks"/>)" : "") + R"(</div>)");
			showContent();
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
			// Global Tracks, and Chord Editing (pp. 1215-1218): the selected notes' chord, the 3-note and 4-note
			// chord buttons, Inversions and Drop Notes.
			std::string chords;
			for (const engine::midi::ChordType& type : engine::midi::chordTypes())
				chords += std::string(R"(<button class="editor-chord" chord=")") + type.name + R"(" title=")" +
					type.name + R"(">)" + (type.suffix[0] == 0 ? "Maj" : type.suffix) + "</button>";
			std::string voicings;
			for (const auto& [voicing, label] : std::vector<std::pair<const char*, const char*>>{
					 {"invertUp", "Inversion Up"}, {"invertDown", "Inversion Down"}, {"drop2", "Drop 2"},
					 {"drop3", "Drop 3"}, {"drop24", "Drop 2 + 4"}})
				voicings += std::string(R"(<button class="editor-voicing" voicing=")") + voicing + R"(">)" + label +
					"</button>";
			root.SetInnerRML(R"(<div class="editor-global-tracks-section"><div class="section-header">Global )"
							 R"(Tracks</div><div class="editor-global-track-list"/></div>)"
							 R"(<div class="editor-chord-editing"><div class="section-header">Chord Editing</div>)"
							 R"(<div class="editor-chord-type display"/><div class="editor-chords">)" +
				chords + R"(</div><div class="editor-voicings">)" + voicings + "</div></div>");
		}
		for (const Rml::EventId event : g_events)
			root.AddEventListener(event, this);

		// quantize.set and snap.set make no undo step, so the grid comes with quantize.changed and whether the audio
		// part editor snaps with snap.changed.
		for (const char* event : {"history.changed", "project.reset", "quantize.changed", "snap.changed"})
			m_subscriptions.push_back(m_binding.on(event,
				[this, reset = std::string(event) == "project.reset",
					history = std::string(event) == "history.changed"](const engine::Event& changed)
				{
					// A step that changed only channels needs no project read, which the editor shows nothing of
					// (#1881). But the drum pads' samples decide which pitches Show Drum Sounds in Use by Instrument
					// lists (editor.get's soundsInUse): after a channel-only step that changed more than levels and
					// sends, such as Reset Pad, it asks for its view again.
					const bool levels = (history || reset) && m_levelSteps.levelsOnly(changed);
					if (changedOnly(changed, {"channel.changed"}))
					{
						if (!levels)
							requestView();
						return;
					}
					requestProject();
					if (reset)
						requestView(); // a new project closes the editors
				}));
		m_subscriptions.push_back(
			m_binding.on("channel.changed", [this](const engine::Event& event) { m_levelSteps.keep(event); }));
		// Tracks hidden in the project window can't show (p. 1151); hiding them is no undo step.
		if (m_part == EditorPart::Visibility)
			m_subscriptions.push_back(m_binding.on("trackVisibility.changed",
				[this](const engine::Event& event)
				{
					if (m_project.contains("trackVisibility"))
						m_project["trackVisibility"]["hidden"] = event.data.at("hidden");
					show();
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

	// ponytail: reads the whole project after every change, as the event display does, once for all the editors.
	void MidiEditor::requestProject()
	{
		if (m_loading)
		{
			m_stale = true;
			return;
		}
		m_loading = true;
		m_reads.read(
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_loading = false;
				if (result.ok())
				{
					m_project = result.value;
					m_read = m_binding.handed();
					m_levelSteps.read(m_project); // so that a fader's first step on a channel asks for no view
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

	std::vector<uint64_t> MidiEditor::inside(uint64_t track) const
	{
		std::vector<uint64_t> tracks = {track};
		for (size_t i = 0; i < tracks.size(); ++i) // the folder's tracks, and theirs
			for (const Json& child : m_project.value("tracks", Json::array()))
				if (child.at("parent") == tracks[i])
					tracks.push_back(child.at("id").get<uint64_t>());
		return tracks;
	}

	bool MidiEditor::hiddenInProject(uint64_t id) const
	{
		const Json hidden = m_project.value("trackVisibility", Json::object()).value("hidden", Json::array());
		for (const Json* found = track(id); found != nullptr; found = track(found->at("parent").get<uint64_t>()))
			if (contains(hidden, found->at("id").get<uint64_t>()))
				return true;
		return false;
	}

	// As editor.set's showTracks refuses them (p. 1151): a track hidden in the project window, by itself or in a
	// hidden folder, or one that holds locked events or parts, also in a track inside it.
	bool MidiEditor::showable(uint64_t id) const
	{
		if (hiddenInProject(id))
			return false;
		const std::vector<uint64_t> tracks = inside(id);
		for (const char* kind : {"midiParts", "audioParts", "audioEvents"})
			for (const Json& item : m_project.value(kind, Json::array()))
				if (item.value("lock", false) &&
					std::find(tracks.begin(), tracks.end(), item.at("track").get<uint64_t>()) != tracks.end())
					return false;
		return true;
	}

	std::pair<size_t, size_t> MidiEditor::partsShown(uint64_t track) const
	{
		const std::vector<uint64_t> tracks = inside(track);
		std::pair<size_t, size_t> counts;
		const Json parts = m_view.value("parts", Json::array());
		for (const Json& part : projectParts())
		{
			const auto owner = part.at("track").get<uint64_t>();
			const bool shown = contains(parts, part.at("id").get<uint64_t>());
			// showTracks leaves out the parts of tracks hidden in the project window, so they count while they show.
			if (std::find(tracks.begin(), tracks.end(), owner) != tracks.end() && (shown || !hiddenInProject(owner)))
			{
				++counts.first;
				counts.second += shown ? 1 : 0;
			}
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
			for (const Json& part : projectParts())
				if (part.at("id") == m_view.at("activePart"))
					active = part.at("name").get<std::string>();
			setInner(root->QuerySelector("#editor-active-part"), encode(active.empty() ? "No Part" : active));
			setInner(root->QuerySelector(".editor-global-tracks"), globalTracksRml());
			showContent();
			if (m_keys)
				m_keys->show(m_project, m_view, m_read);
			if (m_list)
				m_list->show(m_project, m_view);
			if (m_audio)
				m_audio->show(m_project, m_view);
			if (m_pattern)
				m_pattern->show(m_project, m_view, m_read);
			fillMenus();
		}
		else if (m_part == EditorPart::Visibility)
		{
			if (Rml::Element* list = root->QuerySelector(".editor-visibility"))
			{
				list->SetClass("closed", !m_view.at("open").get<bool>());
				showVisibility(*list, visibilityRows());
				fold(*list);
			}
		}
		else
		{
			setInner(root->QuerySelector(".editor-global-track-list"), globalTracksRml());
			setInner(root->QuerySelector(".editor-chord-type"), encode(chordName()));
		}
		if (Rml::Context* context = root->GetContext())
			context->RequestNextUpdate(0);
	}

	bool MidiEditor::showsAudio() const
	{
		return m_view.value("content", std::string("key")) == "audioPart";
	}

	Json MidiEditor::projectParts() const
	{
		return m_project.value(showsAudio() ? "audioParts" : "midiParts", Json::array());
	}

	void MidiEditor::showContent()
	{
		const std::string shown = m_view.value("content", std::string("key"));
		Rml::Element* editor = m_root.get() != nullptr ? m_root.get()->QuerySelector(".midi-editor") : nullptr;
		if (editor == nullptr || (shown == "list" && m_list) || (shown == "audioPart" && m_audio) ||
			(shown == "pattern" && m_pattern) ||
			((shown == "key" || shown == "drum") && m_keys && m_keys->drum() == (shown == "drum")))
			return;
		m_keys.reset();
		m_list.reset();
		m_audio.reset();
		m_pattern.reset();
		for (const char* name : {".key-editor", ".list-editor", ".audio-part-editor", ".pattern-editor"})
			if (Rml::Element* old = editor->QuerySelector(name))
				editor->RemoveChild(old);
		Rml::Element* added = editor->AppendChild(editor->GetOwnerDocument()->CreateElement("div"));
		if (shown == "list")
		{
			added->SetClass("list-editor", true);
			m_list = std::make_unique<ListEditor>(m_binding, *added, m_name);
		}
		else if (shown == "pattern")
		{
			added->SetClass("pattern-editor", true);
			m_pattern = std::make_unique<PatternEditor>(m_binding, *added, m_name);
		}
		else if (shown == "audioPart")
		{
			added->SetClass("audio-part-editor", true);
			m_audio = std::make_unique<AudioPartEditor>(m_binding, *added, m_name);
		}
		else
		{
			// The drum editor (#129) is the key editor with the drum map's sounds as its rows.
			added->SetClass("key-editor", true);
			m_keys = std::make_unique<KeyEditor>(m_binding, *added, m_name, shown == "drum");
			m_keys->setDrumMapSetup(m_drumMapSetup);
		}
	}

	void MidiEditor::setDrumMapSetup(std::function<void()> open)
	{
		m_drumMapSetup = std::move(open);
		if (m_keys)
			m_keys->setDrumMapSetup(m_drumMapSetup);
	}

	// The chord the selected notes make, as the Chord Editing section's Chord Type field shows it (p. 1216).
	std::string MidiEditor::chordName() const
	{
		std::vector<int> pitches;
		for (const Json& selected : m_view.value("selection", Json::array()))
			for (const Json& part : m_project.at("midiParts"))
				if (part.at("id") == selected.at("part"))
					for (const Json& note : part.at("notes"))
						if (note.at("id") == selected.at("note"))
							pitches.push_back(note.at("pitch").get<int>());
		if (pitches.empty())
			return "";
		const std::optional<engine::midi::Chord> chord = engine::midi::recognizeChord(pitches);
		return chord ? chord->name() : "No Chord";
	}

	// A chord type or voicing button: note.voicing on the selected notes of each part, in one undo step.
	void MidiEditor::voice(const std::string& key, const std::string& value)
	{
		std::map<uint64_t, Json> notes;
		for (const Json& selected : m_view.value("selection", Json::array()))
			notes[selected.at("part").get<uint64_t>()].push_back(selected.at("note"));
		Json commands = Json::array();
		for (auto& [part, ids] : notes)
			commands.push_back(
				{{"command", "note.voicing"}, {"parameters", {{"part", part}, {"notes", ids}, {key, value}}}});
		if (commands.size() == 1)
			m_binding.send("note.voicing", commands[0].at("parameters"));
		else if (!commands.empty())
			m_binding.send("history.transaction", {{"description", "Change chord"}, {"commands", std::move(commands)}});
	}

	// In the editor window, the global tracks it shows, in the track list's order (p. 1145); in the inspector's
	// Global Tracks section, a toggle per global track of the project.
	std::string MidiEditor::globalTracksRml() const
	{
		std::string rml;
		const Json& shown = m_view.at("globalTracks");
		for (const Json& track : m_project.at("tracks"))
		{
			// The engine's global tracks (editor.set globalTracks, reference manual p. 1144).
			const std::string type = track.at("type").get<std::string>();
			if (type != "tempo" && type != "signature" && type != "chord" && type != "ruler" && type != "arranger" &&
				type != "marker" && type != "transpose")
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

	// The tracks whose parts the editor can show (p. 1148): MIDI and instrument tracks, or in the audio part editor
	// audio tracks, and folders with such tracks in them, the tracks in a folder indented under it unless it's
	// collapsed. A check while all their parts show, a rectangle (.partial) while some do (p. 1151); disabled while a
	// click would change nothing or ask editor.set for what it refuses.
	std::vector<MidiEditor::VisibilityRow> MidiEditor::visibilityRows() const
	{
		std::vector<VisibilityRow> rows;
		std::map<uint64_t, int> levels;
		const bool open = m_view.at("open").get<bool>();
		for (const Json& track : m_project.at("tracks"))
		{
			const auto trackId = track.at("id").get<uint64_t>();
			const auto parent = track.at("parent").get<uint64_t>();
			const auto above = levels.find(parent);
			const int level = above != levels.end() ? above->second + 1 : 0;
			levels[trackId] = level;
			const bool folder = track.at("type") == "folder";
			if (!holdsParts(track, showsAudio()) && !folder)
				continue;
			const auto [parts, shown] = partsShown(trackId);
			if (folder && parts == 0)
				continue;
			VisibilityRow row;
			row.track = trackId;
			row.level = level;
			row.folder = folder;
			row.name = track.at("name").get<std::string>();
			if (parts > 0 && shown == parts)
				row.state = "checked";
			else if (shown > 0)
				row.state = "partial";
			const bool shows = showable(trackId);
			row.active = m_view.at("activeTrack") == trackId;
			row.unavailable = !shows;
			row.disabled = !open || parts == 0 || (!shows && shown == 0);
			rows.push_back(std::move(row));
		}
		return rows;
	}

	void MidiEditor::showVisibility(Rml::Element& list, const std::vector<VisibilityRow>& rows)
	{
		const auto same = [&list, &rows]
		{
			if (list.GetNumChildren() != static_cast<int>(rows.size()))
				return false;
			for (size_t i = 0; i < rows.size(); ++i)
			{
				Rml::Element* shown = list.GetChild(static_cast<int>(i));
				const Rml::Element* name = shown->QuerySelector(".editor-visibility-name");
				if (shown->GetAttribute<Rml::String>("track", "") != idText(rows[i].track) ||
					shown->GetAttribute<int>("level", -1) != rows[i].level ||
					shown->IsClassSet("parent") != rows[i].folder || name == nullptr ||
					name->GetInnerRML() != encode(rows[i].name) ||
					shown->QuerySelector(".editor-visibility-check") == nullptr)
					return false;
			}
			return true;
		};
		if (same())
		{
			for (size_t i = 0; i < rows.size(); ++i)
			{
				Rml::Element* shown = list.GetChild(static_cast<int>(i));
				shown->SetClass("active", rows[i].active);
				shown->SetClass("unavailable", rows[i].unavailable);
				Rml::Element* check = shown->QuerySelector(".editor-visibility-check");
				check->SetClass("checked", rows[i].state == "checked");
				check->SetClass("partial", rows[i].state == "partial");
				if (rows[i].disabled)
					check->SetAttribute("disabled", "");
				else
					check->RemoveAttribute("disabled");
			}
			return;
		}
		std::string rml;
		for (const VisibilityRow& row : rows)
		{
			std::string classes = row.active ? " active" : "";
			classes += row.unavailable ? " unavailable" : "";
			classes += row.folder ? " parent" : "";
			rml += R"(<div class="editor-visibility-track)" + classes + R"(" track=")" + idText(row.track) +
				R"(" level=")" + std::to_string(row.level) + R"(">)";
			rml += R"(<span class="tree-indent"/><span class="tree-toggle"/>)";
			rml += R"(<button class="editor-visibility-check)" + (row.state.empty() ? "" : " " + row.state) + '"' +
				(row.disabled ? R"( disabled="")" : "") + R"(/><span class="editor-visibility-name">)" +
				encode(row.name) + "</span></div>";
		}
		list.SetInnerRML(rml);
		// The indents, sized from the levels: folders nest without a limit, which no rule per level covers.
		for (int i = 0; i < list.GetNumChildren(); ++i)
		{
			Rml::Element* row = list.GetChild(i);
			const int width = row->GetAttribute<int>("level", 0) * g_indent;
			if (Rml::Element* indent = row->QuerySelector(".tree-indent"))
				indent->SetProperty("width", std::to_string(width) + "dp");
		}
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
			const Json parts = projectParts();
			for (const Json& id : m_view.at("parts"))
				for (const Json& part : parts)
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

	// Hides the rows inside collapsed folders (.hidden) and marks the expanded folders (.expanded).
	void MidiEditor::fold(Rml::Element& list) const
	{
		for (int i = 0; i < list.GetNumChildren(); ++i)
		{
			Rml::Element* row = list.GetChild(i);
			const uint64_t id = idOf(row, "track", nullptr);
			bool hidden = false;
			for (const Json* above = track(id); above != nullptr; above = track(above->at("parent").get<uint64_t>()))
				hidden = hidden || (above->at("id") != id && m_collapsed.count(above->at("id").get<uint64_t>()) != 0);
			row->SetClass("hidden", hidden);
			row->SetClass("expanded", row->IsClassSet("parent") && m_collapsed.count(id) == 0);
		}
	}

	void MidiEditor::toggleTrack(uint64_t track, bool exclusive)
	{
		const auto [parts, shown] = partsShown(track);
		// A closed view has no parts to change, and a track that can't show can only hide what it shows.
		const bool shows = showable(track);
		if (parts == 0 || !m_view.at("open").get<bool>() || (!shows && (exclusive || shown == 0)))
			return;
		if (exclusive)
			set({{"exclusive", true}, {"showTracks", {track}}});
		else if (shown == parts || !shows)
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
		// A click on a folder's name or its triangle collapses or expands it (p. 1151), in this tab only.
		if (Rml::Element* row = closest(target, "editor-visibility-track", root); row != nullptr &&
			row->IsClassSet("parent") &&
			(closest(target, "tree-toggle", root) != nullptr ||
				closest(target, "editor-visibility-name", root) != nullptr))
		{
			const uint64_t folder = idOf(row, "track", root);
			if (m_collapsed.erase(folder) == 0)
				m_collapsed.insert(folder);
			// In place, as rebuilding the rows would lose the double click that shows the folder exclusively.
			fold(*row->GetParentNode());
			return;
		}
		if (target->HasAttribute("chord"))
		{
			voice("chord", attribute(*target, "chord"));
		}
		else if (target->HasAttribute("voicing"))
		{
			voice("voicing", attribute(*target, "voicing"));
		}
		else if (target->HasAttribute("editing-mode"))
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
					{"activePart", m_view.at("activePart")}, {"content", m_view.value("content", "key")}});
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
			if (partsShown(clicked).second > 0 &&
				(track(clicked) == nullptr || holdsParts(*track(clicked), showsAudio())))
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
