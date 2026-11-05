// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/keyeditor.h"

#include "engine/midi/chords.h"
#include "engine/midi/quantizesettings.h"
#include "ui/controllerlanes.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr int g_pitches = 128;
		constexpr double g_maxVelocity = 127.0;
		constexpr float g_edge = 6.0F; // dp at a note's ends where the object selection tool resizes it

		constexpr std::array<std::pair<KeyTool, const char*>, 7> g_tools = {{
			{KeyTool::ObjectSelection, "objectSelection"},
			{KeyTool::Draw, "draw"},
			{KeyTool::Line, "line"},
			{KeyTool::Erase, "erase"},
			{KeyTool::Trim, "trim"},
			{KeyTool::Split, "split"},
			{KeyTool::Glue, "glue"},
		}};
		constexpr std::array<const char*, 7> g_toolTitles = {
			"Object Selection", "Draw", "Line", "Erase", "Trim", "Split", "Glue"};
		constexpr std::array<const char*, 7> g_toolLabels = {
			"Select", "Draw", "Line", "Erase", "Trim", "Split", "Glue"};
		// The line tool's modes (p. 1186), its menu's.
		constexpr std::array<std::pair<const char*, const char*>, 6> g_lineModes = {{
			{"line", "Line"},
			{"parabola", "Parabola"},
			{"sine", "Sine"},
			{"triangle", "Triangle"},
			{"square", "Square"},
			{"paint", "Paint"},
		}};

		// The Length Quantize menu (p. 1196): Quantize Link, then note values.
		constexpr std::array<const char*, 11> g_lengths = {
			"link", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/4T", "1/8T", "1/16T"};

		// The step and MIDI input buttons (p. 1197): editor.set's settings they toggle.
		struct Input
		{
			const char* setting;
			const char* title;
			const char* label;
		};
		constexpr std::array<Input, 8> g_inputs = {{
			{"stepInput", "Step Input", "Step"},
			{"midiInput", "MIDI Input", "MIDI"},
			{"moveInsert", "Move Insert Mode", "Insert"},
			{"recordPitch", "Record Pitch", "Pitch"},
			{"recordVelocity", "Record Note On Velocity", "On"},
			{"recordReleaseVelocity", "Record Note Off Velocity", "Off"},
			{"keepNotes", "Keep Existing Notes", "Keep"},
			{"autoSelectControllers", "Auto Select Controllers", "Auto"},
		}};

		// The transpose palette (p. 1195): semitones.
		struct Transpose
		{
			int semitones;
			const char* title;
			const char* label;
		};
		constexpr std::array<Transpose, 4> g_transpose = {{
			{1, "Move Up", "+1"},
			{-1, "Move Down", "-1"},
			{12, "Move Up More", "+12"},
			{-12, "Move Down More", "-12"},
		}};

		bool black(int pitch)
		{
			const int key = pitch % 12;
			return key == 1 || key == 3 || key == 6 || key == 8 || key == 10;
		}

		// A key of the keyboard beside the grid of pitch rows, top and height in rows from the keyboard's top. A black
		// key is its pitch's row. The 7 white keys of an octave share its 12 rows equally, as on a piano, from the
		// bottom of the C's row up; the keyboard's top cuts the last octave's highest white key.
		std::pair<double, double> keyRows(int pitch)
		{
			if (black(pitch))
				return {static_cast<double>(g_pitches - 1 - pitch), 1.0};
			constexpr double share = 12.0 / 7.0;
			const int octave = pitch - (pitch % 12);
			int white = 0;
			for (int each = octave; each < pitch; ++each)
				white += black(each) ? 0 : 1;
			const double bottom = octave + (white * share);
			const double top = std::min(bottom + share, static_cast<double>(g_pitches));
			return {g_pitches - top, top - bottom};
		}

		std::string attribute(const Rml::Element& element, const char* name)
		{
			return element.GetAttribute<Rml::String>(name, "");
		}

		uint64_t idAttribute(const Rml::Element& element, const char* name)
		{
			const std::string value = attribute(element, name);
			return value.empty() ? 0 : std::stoull(value);
		}

		Rml::Element* withAttribute(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->HasAttribute(name))
					return element;
			return nullptr;
		}

		void setClass(Rml::Element* element, const char* name, bool set)
		{
			if (element != nullptr && element->IsClassSet(name) != set)
				element->SetClass(name, set);
		}

		void setInner(Rml::Element* element, const std::string& rml)
		{
			if (element != nullptr && element->GetInnerRML() != rml)
				element->SetInnerRML(rml);
		}

		Rml::Property percent(double value)
		{
			return {static_cast<float>(value), Rml::Unit::PERCENT};
		}

		int64_t ticksOf(const Json& track, int64_t value, const TimeContext& time)
		{
			if (track.value("timeBase", std::string("musical")) == "linear")
				return time.tempo.toTicks(value, time.sampleRate);
			return value;
		}

		Json noteChange(uint64_t id)
		{
			return {{"id", id}};
		}
	}

	const char* keyToolName(KeyTool tool)
	{
		for (const auto& [each, name] : g_tools)
			if (each == tool)
				return name;
		return "objectSelection";
	}

	std::optional<KeyTool> keyToolFromName(const std::string& name)
	{
		for (const auto& [tool, each] : g_tools)
			if (name == each)
				return tool;
		return std::nullopt;
	}

	KeyEditor::KeyEditor(EngineBinding& binding, Rml::Element& root, std::string view)
		: m_binding(binding), m_root(root.GetObserverPtr()), m_name(std::move(view))
	{
		std::string tools;
		for (size_t i = 0; i < g_tools.size(); ++i)
			tools += std::string(R"(<button class="key-tool" key-tool=")") + g_tools[i].second + R"(" title=")" +
				g_toolTitles[i] + R"(">)" + g_toolLabels[i] + "</button>";
		std::string transpose;
		for (const Transpose& each : g_transpose)
			transpose += R"(<button class="key-transpose" semitones=")" + std::to_string(each.semitones) +
				R"(" title=")" + each.title + R"(">)" + each.label + "</button>";
		std::string inputs;
		for (const Input& each : g_inputs)
			inputs += std::string(R"(<button class="key-input" setting=")") + each.setting + R"(" title=")" +
				each.title + R"(">)" + each.label + "</button>";
		std::string info;
		for (const auto& [field, label] : std::vector<std::pair<const char*, const char*>>{{"start", "Start"},
				 {"end", "End"}, {"length", "Length"}, {"pitch", "Pitch"}, {"velocity", "Velocity"},
				 {"releaseVelocity", "Off Velocity"}, {"channel", "Channel"}})
			info += std::string(R"(<div class="info-field" info-field=")") + field + R"("><span class="info-label">)" +
				label + R"(</span><textdisplay class="display" field=")" + field + R"("/></div>)";
		// The rows of the grid from the highest pitch down, and the piano keyboard beside them: the white keys, then
		// the black keys over them. The C keys are labelled (p. 1204).
		std::string whiteKeys;
		std::string blackKeys;
		std::string rows;
		for (int pitch = g_pitches - 1; pitch >= 0; --pitch)
		{
			const std::string shade = black(pitch) ? " black" : "";
			(black(pitch) ? blackKeys : whiteKeys) += R"(<div class="key-editor-key)" + shade + R"(" pitch=")" +
				std::to_string(pitch) + R"(">)" + (pitch % 12 == 0 ? engine::midi::pitchName(pitch) : "") + "</div>";
			rows += R"(<div class="key-editor-row)" + shade + R"("/>)";
		}
		const std::string keys = whiteKeys + blackKeys;
		root.SetInnerRML(R"(<div class="toolbar key-editor-toolbar">)" + tools + R"(<span class="separator"/>)" +
			transpose + R"(<span class="separator"/>)" + inputs +
			R"(<button id="key-length-quantize" menu="key-length-menu" title="Length Quantize"/>)"
			R"(<valuefield id="key-insert-velocity" min="1" max="127" default="100" step="1" decimals="0")"
			R"( title="Insert Velocity"/></div>)"
			R"(<div class="info-line key-editor-info empty"><span class="info-empty">No Object Selected</span>)" +
			info + R"(</div><div class="editor-parts"/><div class="key-editor-body"><div class="key-editor-keys">)" +
			keys + R"(</div><div class="key-editor-grid"><div class="key-editor-rows">)" + rows +
			R"(</div><div class="key-editor-notes"/><div class="key-editor-step-cursor off"/></div></div>)"
			R"(<div class="controller-lanes"/>)");
		Rml::ElementList placed;
		root.QuerySelectorAll(placed, ".key-editor-key");
		for (Rml::Element* key : placed)
		{
			const auto [top, height] = keyRows(std::stoi(attribute(*key, "pitch")));
			key->SetProperty(Rml::PropertyId::Top, percent(top * 100.0 / g_pitches));
			key->SetProperty(Rml::PropertyId::Height, percent(height * 100.0 / g_pitches));
		}
		m_lanes = std::make_unique<ControllerLanes>(binding, *root.QuerySelector(".controller-lanes"), m_name);
		Rml::ElementDocument* document = root.GetOwnerDocument();
		const Rml::ElementPtr holder = document->CreateElement("div");
		std::string items;
		for (const char* length : g_lengths)
			items += std::string(R"(<menuitem type="toggle" length=")") + length + R"(">)" +
				(std::string(length) == "link" ? "Quantize Link" : length) + "</menuitem>";
		std::string modes;
		for (const auto& [mode, label] : g_lineModes)
			modes += std::string(R"(<menuitem type="toggle" line-mode=")") + mode + R"(">)" + label + "</menuitem>";
		// The toolbox: the toolbar's tools, as its buttons show them.
		std::string toolbox;
		for (size_t i = 0; i < g_tools.size(); ++i)
			toolbox += std::string(R"(<menuitem key-tool=")") + g_tools[i].second + R"(" title=")" + g_toolTitles[i] +
				R"(">)" + g_toolLabels[i] + "</menuitem>";
		holder->SetInnerRML(R"(<menu id="key-length-menu">)" + items + R"(</menu><menu id="key-line-menu">)" + modes +
			R"(</menu><menu id="key-editor-toolbox" class="toolbox">)" + toolbox + "</menu>");
		while (holder->HasChildNodes())
		{
			Rml::Element* menu = document->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
			menu->AddEventListener(Rml::EventId::Click, this);
			m_menus.push_back(menu->GetObserverPtr());
		}
		for (const Rml::EventId event : {Rml::EventId::Mousedown, Rml::EventId::Mousemove, Rml::EventId::Mouseup,
				 Rml::EventId::Click, Rml::EventId::Dblclick, Rml::EventId::Change})
			root.AddEventListener(event, this);
		setTool(KeyTool::ObjectSelection);

		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto toolbox = values.find("ui.toolboxOnRightClick"); toolbox != values.end())
				{
					m_toolboxOnRightClick = toolbox->get<bool>();
					m_toolboxKnown = true;
					showToolbox();
				}
			}));
		m_binding.send("settings.get", {{"key", "ui.toolboxOnRightClick"}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok() || m_toolboxKnown) // newer changes win
					return;
				m_toolboxOnRightClick = result.value.at("value").get<bool>();
				showToolbox();
			});
		showToolbox();
	}

	void KeyEditor::showToolbox()
	{
		Rml::Element* root = m_root.get();
		Rml::Element* grid = root != nullptr ? root->QuerySelector(".key-editor-grid") : nullptr;
		if (grid == nullptr)
			return;
		// Where the context menu would be, an empty attribute: no menu, nor one of the elements around the grid
		// (docs/features/widgets.md).
		grid->SetAttribute("contextmenu", m_toolboxOnRightClick ? "key-editor-toolbox" : "");
		grid->SetAttribute("modifiercontextmenu", m_toolboxOnRightClick ? "" : "key-editor-toolbox");
	}

	KeyEditor::~KeyEditor()
	{
		if (Rml::Element* root = m_root.get())
			for (const Rml::EventId event : {Rml::EventId::Mousedown, Rml::EventId::Mousemove, Rml::EventId::Mouseup,
					 Rml::EventId::Click, Rml::EventId::Dblclick, Rml::EventId::Change})
				root->RemoveEventListener(event, this);
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (Rml::Element* element = menu.get())
			{
				element->RemoveEventListener(Rml::EventId::Click, this);
				if (Rml::Element* parent = element->GetParentNode())
					parent->RemoveChild(element);
			}
	}

	void KeyEditor::setTool(KeyTool tool)
	{
		m_tool = tool;
		Rml::Element* root = m_root.get();
		if (root == nullptr)
			return;
		Rml::ElementList buttons;
		root->QuerySelectorAll(buttons, ".key-tool");
		for (Rml::Element* button : buttons)
		{
			const bool chosen = attribute(*button, "key-tool") == keyToolName(tool);
			setClass(button, "checked", chosen);
			// The chosen line tool's button opens the menu of its modes (p. 1186).
			if (chosen && tool == KeyTool::Line)
				button->SetAttribute("menu", "key-line-menu");
			else if (button->HasAttribute("menu"))
				button->RemoveAttribute("menu");
		}
		// The toolbox's items, by the attribute that gives them the state class .checked.
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (const Rml::Element* element = menu.get())
				for (int i = 0; i < element->GetNumChildren(); ++i)
					if (Rml::Element* item = element->GetChild(i); item->HasAttribute("key-tool"))
					{
						if (attribute(*item, "key-tool") == keyToolName(tool))
							item->SetAttribute("checked", "");
						else
							item->RemoveAttribute("checked");
					}
		root->SetAttribute("tool", keyToolName(tool));
		if (const Rml::Element* menu = m_menus.size() > 1 ? m_menus[1].get() : nullptr)
			for (int i = 0; i < menu->GetNumChildren(); ++i)
				if (Rml::Element* item = menu->GetChild(i);
					(attribute(*item, "line-mode") == m_lineMode) != item->HasAttribute("checked"))
				{
					if (item->HasAttribute("checked"))
						item->RemoveAttribute("checked");
					else
						item->SetAttribute("checked", "");
				}
		if (m_lanes)
			m_lanes->setTool(keyToolName(tool), m_lineMode);
	}

	void KeyEditor::show(const Json& project, const Json& view)
	{
		m_project = project;
		m_view = view;
		m_parts.clear();
		m_notes.clear();
		m_selection.clear();
		if (!m_project.contains("tracks") || !m_view.contains("parts"))
			return;
		const TimeContext time = projectTime(m_project);
		std::map<uint64_t, const Json*> tracks;
		for (const Json& track : m_project.at("tracks"))
			tracks[track.at("id").get<uint64_t>()] = &track;
		const Json& editable = m_view.at("editable");
		for (const Json& id : m_view.at("parts"))
			for (const Json& part : m_project.at("midiParts"))
			{
				if (part.at("id") != id)
					continue;
				const auto trackId = part.at("track").get<uint64_t>();
				const Json& track = *tracks.at(trackId);
				const auto position = part.at("position").get<int64_t>();
				const Part item{id.get<uint64_t>(), trackId, ticksOf(track, position, time),
					ticksOf(track, position + part.at("length").get<int64_t>(), time),
					std::find(editable.begin(), editable.end(), id) != editable.end()};
				m_parts.push_back(item);
				for (const Json& note : part.at("notes"))
					m_notes.push_back({item.id, note.at("id").get<uint64_t>(),
						item.start + note.at("position").get<int64_t>(), note.at("length").get<int64_t>(),
						note.at("pitch").get<int>(), note.at("velocity").get<double>(),
						note.at("releaseVelocity").get<double>(), note.at("channel").get<int>(), item.editable});
			}
		for (const Json& selected : m_view.value("selection", Json::array()))
			m_selection.emplace_back(selected.at("part").get<uint64_t>(), selected.at("note").get<uint64_t>());
		m_from = 0;
		m_to = 1;
		if (!m_parts.empty())
		{
			m_from = m_parts.front().start;
			m_to = m_parts.front().end;
			for (const Part& part : m_parts)
			{
				m_from = std::min(m_from, part.start);
				m_to = std::max(m_to, part.end);
			}
			m_to = std::max(m_to, m_from + 1);
		}
		render();
		m_lanes->show(m_project, m_view, m_from, m_to);
	}

	bool KeyEditor::scrollToPitch(int pitch)
	{
		Rml::Element* body = m_root.get() != nullptr ? m_root.get()->QuerySelector(".key-editor-body") : nullptr;
		if (body == nullptr)
			return false;
		const float height = body->GetScrollHeight();
		const float client = body->GetClientHeight();
		if (height <= 0.0F || client <= 0.0F)
			return false;
		const float row = (static_cast<float>(g_pitches - 1 - pitch) + 0.5F) / g_pitches * height;
		body->SetScrollTop(std::clamp(row - (client / 2.0F), 0.0F, std::max(height - client, 0.0F)));
		return true;
	}

	void KeyEditor::place(Rml::Element& element, int64_t start, int64_t length, int pitch) const
	{
		const auto range = static_cast<double>(m_to - m_from);
		element.SetProperty(Rml::PropertyId::Left, percent(static_cast<double>(start - m_from) / range * 100.0));
		element.SetProperty(Rml::PropertyId::Width, percent(static_cast<double>(length) / range * 100.0));
		if (pitch >= 0)
		{
			element.SetProperty(Rml::PropertyId::Top, percent((g_pitches - 1 - pitch) * 100.0 / g_pitches));
			element.SetProperty(Rml::PropertyId::Height, percent(100.0 / g_pitches));
		}
	}

	// The parts by track, a row per track in the editor's order (the reference's track display, p. 1154): each part
	// at its time, .active and .editable as MidiEditor's view says.
	std::string KeyEditor::partsRml() const
	{
		std::string rml;
		std::vector<uint64_t> tracks;
		for (const Part& part : m_parts)
			if (std::find(tracks.begin(), tracks.end(), part.track) == tracks.end())
				tracks.push_back(part.track);
		std::map<uint64_t, std::string> names;
		for (const Json& part : m_project.at("midiParts"))
			names[part.at("id").get<uint64_t>()] = part.at("name").get<std::string>();
		for (const uint64_t track : tracks)
		{
			rml += R"(<div class="editor-part-row" track=")" + std::to_string(track) + R"(">)";
			for (const Part& part : m_parts)
				if (part.track == track)
					rml += R"(<div class="editor-part)" +
						std::string(Json(part.id) == m_view.at("activePart") ? " active" : "") +
						(part.editable ? " editable" : "") + R"(" part=")" + std::to_string(part.id) + R"(">)" +
						Rml::StringUtilities::EncodeRml(names[part.id]) + "</div>";
			rml += "</div>";
		}
		return rml;
	}

	void KeyEditor::render()
	{
		Rml::Element* root = m_root.get();
		if (root == nullptr)
			return;
		setInner(root->QuerySelector(".editor-parts"), partsRml());
		Rml::ElementList shown;
		root->QuerySelectorAll(shown, ".editor-part");
		for (Rml::Element* element : shown)
			for (const Part& part : m_parts)
				if (part.id == idAttribute(*element, "part"))
					place(*element, part.start, part.end - part.start, -1);

		// The selection only changes classes, so that a press that selects keeps the element under the mouse.
		std::string rml;
		for (const Note& note : m_notes)
			rml += R"(<div class="key-note)" + std::string(note.editable ? "" : " inactive") + R"(" part=")" +
				std::to_string(note.part) + R"(" note=")" + std::to_string(note.id) + R"("/>)";
		// Compared with what was set last: the element's own RML has the classes set since.
		Rml::Element* notes = root->QuerySelector(".key-editor-notes");
		if (notes != nullptr && rml != m_notesRml)
		{
			notes->SetInnerRML(rml);
			m_notesRml = rml;
		}
		if (notes != nullptr)
			for (int i = 0; i < notes->GetNumChildren(); ++i)
				if (const Note* found = noteOf(notes->GetChild(i)))
				{
					place(*notes->GetChild(i), found->start, found->length, found->pitch);
					setClass(notes->GetChild(i), "selected", selected(*found));
				}

		// The keys of selected notes are lit (p. 1204).
		Rml::ElementList keys;
		root->QuerySelectorAll(keys, ".key-editor-key");
		std::vector<bool> lit(g_pitches, false);
		for (const Note* note : selectedNotes())
			lit[static_cast<size_t>(note->pitch)] = true;
		for (Rml::Element* key : keys)
			setClass(key, "selected", lit[static_cast<size_t>(std::stoi(attribute(*key, "pitch")))]);

		Rml::ElementList inputs;
		root->QuerySelectorAll(inputs, ".key-input");
		for (Rml::Element* button : inputs)
			setClass(button, "checked", m_view.value(attribute(*button, "setting"), false));
		const std::string length = m_view.value("lengthQuantize", std::string("link"));
		setInner(root->QuerySelector("#key-length-quantize"), length == "link" ? "Quantize Link" : length);
		if (Rml::Element* menu = m_menus.front().get())
		{
			Rml::ElementList items;
			menu->QuerySelectorAll(items, "menuitem");
			for (Rml::Element* item : items)
				if ((attribute(*item, "length") == length) != item->HasAttribute("checked"))
				{
					if (item->HasAttribute("checked"))
						item->RemoveAttribute("checked");
					else
						item->SetAttribute("checked", "");
				}
		}
		if (Rml::Element* velocity = root->QuerySelector("#key-insert-velocity"))
		{
			const std::string value =
				std::to_string(std::lround(m_view.value("insertVelocity", 100.0 / g_maxVelocity) * g_maxVelocity));
			if (attribute(*velocity, "value") != value)
				velocity->SetAttribute("value", value);
		}
		if (Rml::Element* cursor = root->QuerySelector(".key-editor-step-cursor"))
		{
			const bool on = m_view.value("stepInput", false);
			setClass(cursor, "off", !on);
			if (on)
				place(*cursor, m_view.value("stepPosition", int64_t{0}), 0, -1);
		}
		renderInfo();
		if (!m_scrolled)
		{
			int low = 60;
			int high = 60;
			if (!m_notes.empty())
			{
				low = high = m_notes.front().pitch;
				for (const Note& note : m_notes)
				{
					low = std::min(low, note.pitch);
					high = std::max(high, note.pitch);
				}
			}
			m_scrolled = scrollToPitch((low + high) / 2);
		}
		if (Rml::Context* context = root->GetContext())
			context->RequestNextUpdate(0);
	}

	// The info line (p. 1199): the first selected note's start, end and length in bars and beats, pitch, velocities
	// 0-127 and channel 1-16; .multiple with several notes selected.
	void KeyEditor::renderInfo()
	{
		Rml::Element* line = m_root.get()->QuerySelector(".key-editor-info");
		if (line == nullptr)
			return;
		const std::vector<const Note*> notes = selectedNotes();
		setClass(line, "empty", notes.empty());
		setClass(line, "multiple", notes.size() > 1);
		if (notes.empty())
			return;
		const Note& first = *notes.front();
		const TimeContext time = projectTime(m_project);
		const double start = time.tempo.toSeconds(first.start);
		const double end = time.tempo.toSeconds(first.start + first.length);
		const std::map<std::string, std::string> texts = {{"start", formatTime(TimeFormat::BarsBeats, start, time)},
			{"end", formatTime(TimeFormat::BarsBeats, end, time)},
			{"length", formatLength(TimeFormat::BarsBeats, start, end, time)},
			{"pitch", engine::midi::pitchName(first.pitch)},
			{"velocity", std::to_string(std::lround(first.velocity * g_maxVelocity))},
			{"releaseVelocity", std::to_string(std::lround(first.releaseVelocity * g_maxVelocity))},
			{"channel", std::to_string(first.channel + 1)}};
		for (const auto& [field, text] : texts)
			if (Rml::Element* display = line->QuerySelector("textdisplay[field=" + field + "]");
				display != nullptr && attribute(*display, "value") != text)
				display->SetAttribute("value", text);
	}

	const KeyEditor::Note* KeyEditor::note(uint64_t part, uint64_t id) const
	{
		for (const Note& each : m_notes)
			if (each.part == part && each.id == id)
				return &each;
		return nullptr;
	}

	const KeyEditor::Note* KeyEditor::noteOf(Rml::Element* element) const
	{
		const Rml::Element* found = withAttribute(element, "note", m_root.get());
		return found == nullptr ? nullptr : note(idAttribute(*found, "part"), idAttribute(*found, "note"));
	}

	bool KeyEditor::selected(const Note& note) const
	{
		return std::find(m_selection.begin(), m_selection.end(), std::make_pair(note.part, note.id)) !=
			m_selection.end();
	}

	// By start, then pitch.
	std::vector<const KeyEditor::Note*> KeyEditor::selectedNotes() const
	{
		std::vector<const Note*> notes;
		for (const Note& each : m_notes)
			if (selected(each))
				notes.push_back(&each);
		std::stable_sort(notes.begin(), notes.end(),
			[](const Note* a, const Note* b) { return std::tie(a->start, a->pitch) < std::tie(b->start, b->pitch); });
		return notes;
	}

	const KeyEditor::Part* KeyEditor::partAt(int64_t ticks) const
	{
		const Part* found = nullptr;
		for (const Part& part : m_parts)
			if (part.editable && ticks >= part.start && ticks < part.end &&
				(found == nullptr || Json(part.id) == m_view.at("activePart")))
				found = &part;
		return found;
	}

	std::pair<double, int> KeyEditor::at(const Rml::Event& event) const
	{
		Rml::Element* grid = m_root.get()->QuerySelector(".key-editor-grid");
		if (grid == nullptr)
			return {0.0, 0};
		const Rml::Vector2f origin = grid->GetAbsoluteOffset(Rml::BoxArea::Border);
		const Rml::Vector2f size = grid->GetBox().GetSize(Rml::BoxArea::Border);
		const float x = (event.GetParameter("mouse_x", 0.0F) - origin.x) / std::max(size.x, 1.0F);
		const float y = (event.GetParameter("mouse_y", 0.0F) - origin.y) / std::max(size.y, 1.0F);
		const int pitch = std::clamp(g_pitches - 1 - static_cast<int>(std::floor(y * g_pitches)), 0, g_pitches - 1);
		return {static_cast<double>(m_from) + (x * static_cast<double>(m_to - m_from)), pitch};
	}

	// The quantize grid (quantize.set, p. 1196) from the project start; Ctrl (Cmd) turns snap off (p. 1213).
	int64_t KeyEditor::grid() const
	{
		const std::string grid = m_project.value("quantize", Json::object()).value("grid", std::string("1/16"));
		return engine::midi::noteValueFromString(grid).value_or(engine::midi::NoteValue{}).ticks();
	}

	int64_t KeyEditor::snap(double ticks, bool off) const
	{
		if (off)
			return std::llround(ticks);
		const auto step = static_cast<double>(grid());
		return std::llround(std::round(ticks / step) * step);
	}

	int64_t KeyEditor::lengthQuantize() const
	{
		const std::string length = m_view.value("lengthQuantize", std::string("link"));
		if (length == "link")
			return grid();
		return engine::midi::noteValueFromString(length).value_or(engine::midi::NoteValue{}).ticks();
	}

	void KeyEditor::select(std::vector<std::pair<uint64_t, uint64_t>> notes)
	{
		m_selection = std::move(notes);
		Json selection = Json::array();
		for (const auto& [part, note] : m_selection)
			selection.push_back({{"part", part}, {"note", note}});
		render(); // at once; editor.changed confirms it
		setView({{"selection", selection}});
	}

	void KeyEditor::setView(Json parameters)
	{
		parameters["editor"] = m_name;
		m_binding.send("editor.set", std::move(parameters));
	}

	void KeyEditor::edit(const std::string& description, Json commands)
	{
		if (commands.empty())
			return;
		if (commands.size() == 1)
			m_binding.send(commands[0].at("command").get<std::string>(), commands[0].at("parameters"));
		else
			m_binding.send("history.transaction", {{"description", description}, {"commands", std::move(commands)}});
	}

	void KeyEditor::addNote(const Part& part, int64_t start, int64_t length, int pitch)
	{
		const double velocity = m_view.value("insertVelocity", 100.0 / g_maxVelocity);
		m_binding.send("note.add",
			{{"part", part.id},
				{"notes",
					Json::array({{{"position", start - part.start}, {"length", std::max<int64_t>(length, 1)},
						{"pitch", pitch}, {"velocity", velocity}}})}});
	}

	void KeyEditor::transpose(int semitones)
	{
		std::map<uint64_t, Json> changes;
		for (const Note* note : selectedNotes())
		{
			const int pitch = note->pitch + semitones;
			if (pitch < 0 || pitch >= g_pitches)
				return; // the palette moves all or none
			Json change = noteChange(note->id);
			change["pitch"] = pitch;
			changes[note->part].push_back(change);
		}
		Json commands = Json::array();
		for (auto& [part, notes] : changes)
			commands.push_back({{"command", "note.set"}, {"parameters", {{"part", part}, {"notes", notes}}}});
		edit("Transpose", std::move(commands));
	}

	// Edits on the info line go to every selected note, relative to the first one's value; the channel is the same
	// for all.
	void KeyEditor::editInfo(const std::string& field, const std::string& text)
	{
		const std::vector<const Note*> notes = selectedNotes();
		if (notes.empty())
			return;
		const Note& first = *notes.front();
		const TimeContext time = projectTime(m_project);
		const double start = time.tempo.toSeconds(first.start);
		std::optional<int64_t> delta;
		const char* key = nullptr;
		if (field == "start" || field == "end")
		{
			if (const std::optional<double> seconds = parseTime(TimeFormat::BarsBeats, text, time))
			{
				const int64_t typed = time.tempo.toTicks(*seconds);
				delta = typed - (field == "start" ? first.start : first.start + first.length);
				key = field == "start" ? "position" : "length";
			}
		}
		else if (field == "length")
		{
			if (const std::optional<double> seconds = parseLength(TimeFormat::BarsBeats, text, start, time))
			{
				delta = time.tempo.toTicks(start + *seconds) - first.start - first.length;
				key = "length";
			}
		}
		else
		{
			// A number, or for the pitch a name such as C3.
			int value = 0;
			try
			{
				value = std::stoi(text);
			}
			catch (const std::exception&)
			{
				bool named = false;
				for (int pitch = 0; field == "pitch" && pitch < g_pitches && !named; ++pitch)
					if (engine::midi::pitchName(pitch) == text)
					{
						value = pitch;
						named = true;
					}
				if (!named)
					return;
			}
			if (field == "pitch")
			{
				delta = value - first.pitch;
				key = "pitch";
			}
			else if (field == "velocity" || field == "releaseVelocity")
			{
				delta = value -
					std::lround(
						field == "velocity" ? first.velocity * g_maxVelocity : first.releaseVelocity * g_maxVelocity);
				key = field == "velocity" ? "velocity" : "releaseVelocity";
			}
			else if (field == "channel")
			{
				delta = value - 1;
				key = "channel";
			}
		}
		if (!delta || key == nullptr)
			return;
		std::map<uint64_t, Json> changes;
		for (const Note* note : notes)
		{
			Json change = noteChange(note->id);
			const std::string name = key;
			if (name == "position")
				change["position"] = std::max<int64_t>(note->start + *delta, 0) - [&]
				{
					for (const Part& part : m_parts)
						if (part.id == note->part)
							return part.start;
					return int64_t{0};
				}();
			else if (name == "length")
				change["length"] = std::max<int64_t>(note->length + *delta, 1);
			else if (name == "pitch")
				change["pitch"] = std::clamp<int64_t>(note->pitch + *delta, 0, g_pitches - 1);
			else if (name == "channel")
				change["channel"] = std::clamp<int64_t>(*delta, 0, 15);
			else
				change[name] = std::clamp((name == "velocity" ? note->velocity : note->releaseVelocity) +
						(static_cast<double>(*delta) / g_maxVelocity),
					0.0, 1.0);
			changes[note->part].push_back(change);
		}
		Json commands = Json::array();
		for (auto& [part, items] : changes)
			commands.push_back({{"command", "note.set"}, {"parameters", {{"part", part}, {"notes", items}}}});
		edit("Change notes", std::move(commands));
	}

	void KeyEditor::toolbar(Rml::Element& target)
	{
		if (target.HasAttribute("key-tool"))
		{
			if (const std::optional<KeyTool> tool = keyToolFromName(attribute(target, "key-tool")))
				setTool(*tool);
		}
		else if (target.HasAttribute("semitones"))
		{
			transpose(std::stoi(attribute(target, "semitones")));
		}
		else if (target.HasAttribute("setting"))
		{
			const std::string setting = attribute(target, "setting");
			setView({{setting, !m_view.value(setting, false)}});
		}
		else if (target.HasAttribute("length"))
		{
			setView({{"lengthQuantize", attribute(target, "length")}});
		}
		else if (target.HasAttribute("line-mode"))
		{
			m_lineMode = attribute(target, "line-mode");
			setTool(KeyTool::Line);
		}
	}

	void KeyEditor::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr || m_view.empty())
			return;
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
			if (event.GetParameter("button", 0) == 0)
				press(event);
			break;
		case Rml::EventId::Mousemove:
			drag(event);
			break;
		case Rml::EventId::Mouseup:
			release(event);
			break;
		case Rml::EventId::Dblclick:
			doubleClick(event);
			break;
		case Rml::EventId::Click:
			toolbar(*target);
			break;
		case Rml::EventId::Change:
			if (target->HasAttribute("field"))
				editInfo(attribute(*target, "field"), event.GetParameter<Rml::String>("value", ""));
			else if (target->GetId() == "key-insert-velocity")
				setView({{"insertVelocity",
					std::clamp(event.GetParameter<float>("value", 100.0F) / g_maxVelocity, 0.0, 1.0)}});
			break;
		default:
			break;
		}
	}

	void KeyEditor::press(Rml::Event& event)
	{
		Rml::Element* root = m_root.get();
		Rml::Element* target = event.GetTargetElement();
		const Rml::Element* area = root->QuerySelector(".key-editor-grid");
		bool inGrid = false;
		for (const Rml::Element* each = target; each != nullptr && each != root; each = each->GetParentNode())
			inGrid = inGrid || each == area;
		if (!inGrid)
			return;
		const bool shift = event.GetParameter("shift_key", 0) != 0;
		const bool ctrl = event.GetParameter("ctrl_key", 0) != 0 || event.GetParameter("meta_key", 0) != 0;
		const bool alt = event.GetParameter("alt_key", 0) != 0;
		m_pressedAt = at(event);
		m_snapOff = ctrl;
		m_gesture = Gesture::None;
		const Note* pressed = noteOf(target);
		if (pressed != nullptr && !pressed->editable)
			pressed = nullptr;
		m_pressed.reset();
		if (pressed != nullptr)
			m_pressed = *pressed;
		const auto ticks = m_pressedAt.first;
		switch (m_tool)
		{
		case KeyTool::ObjectSelection:
			if (pressed == nullptr)
			{
				// A click in the empty grid deselects; with step input on it moves the step input cursor (p. 1220).
				if (!shift && !ctrl)
					select({});
				if (m_view.value("stepInput", false))
					setView({{"stepPosition", std::max<int64_t>(snap(ticks, false), 0)}});
				return;
			}
			if (shift || ctrl)
			{
				std::vector<std::pair<uint64_t, uint64_t>> notes = m_selection;
				const auto found = std::find(notes.begin(), notes.end(), std::make_pair(pressed->part, pressed->id));
				if (found != notes.end())
					notes.erase(found);
				else
					notes.emplace_back(pressed->part, pressed->id);
				select(std::move(notes));
				return;
			}
			{
				// Near an end the press resizes, elsewhere it moves (pp. 1213-1214).
				Rml::Element* box = withAttribute(target, "note", root);
				const float ratio =
					root->GetContext() != nullptr ? root->GetContext()->GetDensityIndependentPixelRatio() : 1.0F;
				const Rml::Vector2f origin = box->GetAbsoluteOffset(Rml::BoxArea::Border);
				const float width = box->GetBox().GetSize(Rml::BoxArea::Border).x;
				const float x = event.GetParameter("mouse_x", 0.0F) - origin.x;
				const float edge = g_edge * ratio;
				if (width > 3 * edge && x >= width - edge)
					m_gesture = Gesture::ResizeEnd;
				else if (width > 3 * edge && x <= edge)
					m_gesture = Gesture::ResizeStart;
				else
					m_gesture = Gesture::Move;
			}
			if (!selected(*pressed))
				select({{pressed->part, pressed->id}});
			return;
		case KeyTool::Draw:
		{
			// A click draws a note of the length quantize's length, a drag a multiple of it (p. 1212).
			if (pressed != nullptr)
				return;
			const int64_t start = snap(ticks - (m_snapOff ? 0.0 : static_cast<double>(grid()) / 2.0), m_snapOff);
			if (partAt(start) == nullptr)
				return;
			m_pressedAt.first = static_cast<double>(start);
			m_gesture = Gesture::Draw;
			Rml::Element* notes = root->QuerySelector(".key-editor-notes");
			Rml::ElementPtr drawn = root->GetOwnerDocument()->CreateElement("div");
			drawn->SetClass("key-note", true);
			drawn->SetClass("drawing", true);
			Rml::Element* added = notes->AppendChild(std::move(drawn));
			place(*added, start, lengthQuantize(), m_pressedAt.second);
			m_drawn = added->GetObserverPtr();
			return;
		}
		case KeyTool::Erase:
			if (pressed != nullptr)
				edit("Erase",
					Json::array({{{"command", "note.remove"},
						{"parameters", {{"part", pressed->part}, {"notes", Json::array({pressed->id})}}}}}));
			return;
		case KeyTool::Trim:
		{
			// Cuts off the end from the mouse on, with Alt the start up to it (p. 1215).
			if (pressed == nullptr)
				return;
			const int64_t cut = std::llround(ticks);
			if (cut <= pressed->start || cut >= pressed->start + pressed->length)
				return;
			Json change = noteChange(pressed->id);
			if (alt)
			{
				change["position"] = pressed->start + (cut - pressed->start) - [&]
				{
					for (const Part& part : m_parts)
						if (part.id == pressed->part)
							return part.start;
					return int64_t{0};
				}();
				change["length"] = pressed->start + pressed->length - cut;
			}
			else
			{
				change["length"] = cut - pressed->start;
			}
			edit("Trim",
				Json::array({{{"command", "note.set"},
					{"parameters", {{"part", pressed->part}, {"notes", Json::array({change})}}}}}));
			return;
		}
		case KeyTool::Split:
		{
			// Splits the note at the snapped position; a selected one splits every selected note there (p. 1215).
			if (pressed == nullptr)
				return;
			const int64_t cut = snap(ticks, m_snapOff);
			std::vector<const Note*> notes = {pressed};
			if (selected(*pressed))
				notes = selectedNotes();
			Json commands = Json::array();
			for (const Note* each : notes)
			{
				if (cut <= each->start || cut >= each->start + each->length)
					continue;
				int64_t partStart = 0;
				for (const Part& part : m_parts)
					if (part.id == each->part)
						partStart = part.start;
				Json change = noteChange(each->id);
				change["length"] = cut - each->start;
				commands.push_back({{"command", "note.set"},
					{"parameters", {{"part", each->part}, {"notes", Json::array({change})}}}});
				commands.push_back({{"command", "note.add"},
					{"parameters",
						{{"part", each->part},
							{"notes",
								{{{"position", cut - partStart}, {"length", each->start + each->length - cut},
									{"pitch", each->pitch}, {"velocity", each->velocity},
									{"releaseVelocity", each->releaseVelocity}, {"channel", each->channel}}}}}}});
			}
			edit("Split", std::move(commands));
			return;
		}
		case KeyTool::Line: // only on the controller lanes
			return;
		case KeyTool::Glue:
		{
			// Glues the note to the next one of its pitch in its part; the first one's values stay (p. 1215).
			if (pressed == nullptr)
				return;
			const Note* next = nullptr;
			for (const Note& each : m_notes)
				if (each.part == pressed->part && each.pitch == pressed->pitch && each.start > pressed->start &&
					(next == nullptr || each.start < next->start))
					next = &each;
			if (next == nullptr)
				return;
			Json change = noteChange(pressed->id);
			change["length"] = std::max(next->start + next->length, pressed->start + pressed->length) - pressed->start;
			edit("Glue",
				Json::array({{{"command", "note.set"},
								 {"parameters", {{"part", pressed->part}, {"notes", Json::array({change})}}}},
					{{"command", "note.remove"},
						{"parameters", {{"part", pressed->part}, {"notes", Json::array({next->id})}}}}}));
			return;
		}
		}
	}

	// How far a move or resize goes: the pressed note's start, or its end, snapped where the mouse took it.
	int64_t KeyEditor::delta(Gesture gesture, double ticks) const
	{
		if (!m_pressed)
			return 0;
		const int64_t edge = gesture == Gesture::ResizeEnd ? m_pressed->start + m_pressed->length : m_pressed->start;
		return snap(static_cast<double>(edge) + ticks - m_pressedAt.first, m_snapOff) - edge;
	}

	void KeyEditor::drag(Rml::Event& event)
	{
		if (m_gesture == Gesture::None)
			return;
		const auto [ticks, pitch] = at(event);
		const Rml::Element* notes = m_root.get()->QuerySelector(".key-editor-notes");
		if (notes == nullptr)
			return;
		if (m_gesture == Gesture::Draw)
		{
			if (Rml::Element* drawn = m_drawn.get())
			{
				const int64_t step = lengthQuantize();
				const auto steps = std::max<int64_t>(
					1, static_cast<int64_t>(std::ceil((ticks - m_pressedAt.first) / static_cast<double>(step))));
				place(*drawn, std::llround(m_pressedAt.first), steps * step, m_pressedAt.second);
			}
			return;
		}
		if (!m_pressed)
			return;
		const int64_t dt = delta(m_gesture, ticks);
		const int dp = m_gesture == Gesture::Move ? pitch - m_pressedAt.second : 0;
		for (int i = 0; i < notes->GetNumChildren(); ++i)
		{
			Rml::Element* element = notes->GetChild(i);
			const Note* shown = noteOf(element);
			if (shown == nullptr || !selected(*shown))
				continue;
			if (m_gesture == Gesture::Move)
				place(*element, shown->start + dt, shown->length, std::clamp(shown->pitch + dp, 0, g_pitches - 1));
			else if (m_gesture == Gesture::ResizeEnd)
				place(*element, shown->start, std::max<int64_t>(shown->length + dt, 1), shown->pitch);
			else
				place(*element, shown->start + std::min(dt, shown->length - 1),
					shown->length - std::min(dt, shown->length - 1), shown->pitch);
		}
	}

	void KeyEditor::release(Rml::Event& event)
	{
		const Gesture gesture = std::exchange(m_gesture, Gesture::None);
		if (gesture == Gesture::None)
			return;
		const auto [ticks, pitch] = at(event);
		if (gesture == Gesture::Draw)
		{
			Rml::Element* drawn = m_drawn.get();
			if (drawn != nullptr && drawn->GetParentNode() != nullptr)
				drawn->GetParentNode()->RemoveChild(drawn);
			const auto start = std::llround(m_pressedAt.first);
			const int64_t step = lengthQuantize();
			const auto steps = std::max<int64_t>(
				1, static_cast<int64_t>(std::ceil((ticks - m_pressedAt.first) / static_cast<double>(step))));
			if (const Part* part = partAt(start))
				addNote(*part, start, steps * step, m_pressedAt.second);
			return;
		}
		if (!m_pressed)
			return;
		// A move or resize of the selected notes, the pressed one snapped (pp. 1213-1214).
		const bool end = gesture == Gesture::ResizeEnd;
		const int64_t dt = delta(gesture, ticks);
		const int dp = gesture == Gesture::Move ? pitch - m_pressedAt.second : 0;
		if (dt == 0 && dp == 0)
			return;
		std::map<uint64_t, Json> changes;
		for (const Note* note : selectedNotes())
		{
			int64_t partStart = 0;
			for (const Part& part : m_parts)
				if (part.id == note->part)
					partStart = part.start;
			Json change = noteChange(note->id);
			if (gesture == Gesture::Move)
			{
				if (note->start + dt < partStart || note->pitch + dp < 0 || note->pitch + dp >= g_pitches)
					return; // nothing moves before its part or out of the pitches
				change["position"] = note->start + dt - partStart;
				change["pitch"] = note->pitch + dp;
			}
			else if (end)
			{
				change["length"] = std::max<int64_t>(note->length + dt, 1);
			}
			else
			{
				const int64_t moved = std::max(std::min(dt, note->length - 1), partStart - note->start);
				change["position"] = note->start + moved - partStart;
				change["length"] = note->length - moved;
			}
			changes[note->part].push_back(change);
		}
		Json commands = Json::array();
		for (auto& [part, items] : changes)
			commands.push_back({{"command", "note.set"}, {"parameters", {{"part", part}, {"notes", items}}}});
		// Selected controller events move with the notes (p. 1188), such as those Auto Select Controllers selects.
		std::map<uint64_t, Json> controllers;
		for (const Json& selected : m_view.value("controllerSelection", Json::array()))
			for (const Json& part : gesture == Gesture::Move ? m_project.at("midiParts") : Json::array())
				if (part.at("id") == selected.at("part"))
					for (const Json& controller : part.at("controllers"))
						if (controller.at("id") == selected.at("controller"))
							controllers[part.at("id").get<uint64_t>()].push_back({{"id", controller.at("id")},
								{"position", std::max<int64_t>(controller.at("position").get<int64_t>() + dt, 0)}});
		for (auto& [part, items] : controllers)
			commands.push_back(
				{{"command", "controller.set"}, {"parameters", {{"part", part}, {"controllers", items}}}});
		edit(gesture == Gesture::Move ? "Move" : "Resize", std::move(commands));
	}

	// With the object selection tool a double click on a note deletes it, in the empty grid it inserts a note of the
	// length quantize's length (p. 1211).
	void KeyEditor::doubleClick(Rml::Event& event)
	{
		if (m_tool != KeyTool::ObjectSelection)
			return;
		Rml::Element* root = m_root.get();
		const Rml::Element* area = root->QuerySelector(".key-editor-grid");
		bool inGrid = false;
		for (const Rml::Element* each = event.GetTargetElement(); each != nullptr && each != root;
			each = each->GetParentNode())
			inGrid = inGrid || each == area;
		if (!inGrid)
			return;
		m_gesture = Gesture::None;
		if (const Note* pressed = noteOf(event.GetTargetElement()); pressed != nullptr)
		{
			if (pressed->editable)
				edit("Delete",
					Json::array({{{"command", "note.remove"},
						{"parameters", {{"part", pressed->part}, {"notes", Json::array({pressed->id})}}}}}));
			return;
		}
		const auto [ticks, pitch] = at(event);
		const bool ctrl = event.GetParameter("ctrl_key", 0) != 0 || event.GetParameter("meta_key", 0) != 0;
		const int64_t start = snap(ticks - (ctrl ? 0.0 : static_cast<double>(grid()) / 2.0), ctrl);
		if (const Part* part = partAt(start))
			addNote(*part, start, lengthQuantize(), pitch);
	}
}
