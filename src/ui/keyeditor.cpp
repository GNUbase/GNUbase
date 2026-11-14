// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/keyeditor.h"

#include "engine/midi/chords.h"
#include "engine/midi/quantizesettings.h"
#include "platform/log.h"
#include "ui/controllerlanes.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <map>
#include <sstream>
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
		// dp the drumstick goes from its press before its release deletes the sound's later notes too (p. 1242)
		constexpr float g_dragThreshold = 4.0F;
		// The zoom the keyboard's left part drags (p. 1261): its limits, and the dp a drag goes to the right to double
		// it.
		constexpr float g_minPitchZoom = 0.25F;
		constexpr float g_maxPitchZoom = 4.0F;
		constexpr float g_zoomDoubling = 100.0F;

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

		// The Length Quantize menu (p. 1196): Quantize Link, then note values; the drum editor's Insert Length also
		// has Drum-Map Link (p. 1240).
		constexpr std::array<const char*, 12> g_lengths = {
			"link", "drumMap", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/4T", "1/8T", "1/16T"};

		const char* lengthLabel(const std::string& length)
		{
			if (length == "link")
				return "Quantize Link";
			return length == "drumMap" ? "Drum-Map Link" : nullptr;
		}

		// The drum editor's tools (p. 1224): no trim, split or glue; the draw tool is the drumstick.
		bool drumTool(KeyTool tool)
		{
			return tool != KeyTool::Trim && tool != KeyTool::Split && tool != KeyTool::Glue;
		}

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

		Rml::Property dp(float value)
		{
			return {value, Rml::Unit::DP};
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

		// Names separated by commas, as the Articulations field shows a note's attributes.
		std::string joined(const std::vector<std::string>& names)
		{
			std::string text;
			for (const std::string& name : names)
				text += (text.empty() ? "" : ", ") + name;
			return text;
		}

		// A pitch typed: a number 0-127 or a name such as C1.
		std::optional<int> parsePitch(const std::string& text)
		{
			for (int pitch = 0; pitch < g_pitches; ++pitch)
				if (text == std::to_string(pitch) || text == engine::midi::pitchName(pitch))
					return pitch;
			return std::nullopt;
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

	std::string menuIdSuffix(const std::string& view)
	{
		if (view == "lower" || view == "window")
			return {};
		std::string suffix = "-" + view;
		std::replace(suffix.begin(), suffix.end(), ':', '-');
		return suffix;
	}

	KeyEditor::KeyEditor(EngineBinding& binding, Rml::Element& root, std::string view, bool drum)
		: m_binding(binding), m_root(root.GetObserverPtr()), m_name(std::move(view)),
		  m_menuSuffix(menuIdSuffix(m_name)), m_drum(drum)
	{
		// The tools' buttons and the toolbox's items.
		const auto toolsRml = [drum](const char* tag, const char* attributes)
		{
			std::string rml;
			for (size_t i = 0; i < g_tools.size(); ++i)
			{
				if (drum && !drumTool(g_tools[i].first))
					continue;
				const char* label = drum && g_tools[i].first == KeyTool::Draw ? "Drumstick" : g_toolLabels[i];
				const char* title = drum && g_tools[i].first == KeyTool::Draw ? "Drumstick" : g_toolTitles[i];
				rml += std::string("<") + tag + attributes + R"( key-tool=")" + g_tools[i].second + R"(" title=")" +
					title + R"(">)" + label + "</" + tag + ">";
			}
			return rml;
		};
		const std::string tools = toolsRml("button", R"( class="key-tool")");
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
				 {"end", "End"}, {"length", "Length"}, {"pitch", "Pitch"}, {"articulations", "Articulations"},
				 {"velocity", "Velocity"}, {"releaseVelocity", "Off Velocity"}, {"channel", "Channel"}})
			info += std::string(R"(<div class="info-field" info-field=")") + field + R"("><span class="info-label">)" +
				label + R"(</span><textdisplay class="display" field=")" + field + R"("/></div>)";
		// The rows of the grid from the highest pitch down, and the piano keyboard beside them: the white keys, then
		// the black keys over them. The C keys are labelled (p. 1204). The drum editor's rows are its drum sounds
		// (render).
		std::string whiteKeys;
		std::string blackKeys;
		std::string rows;
		for (int pitch = g_pitches - 1; pitch >= 0; --pitch)
		{
			const std::string shade = black(pitch) && !drum ? " black" : "";
			if (!drum)
				(black(pitch) ? blackKeys : whiteKeys) += R"(<div class="key-editor-key)" + shade + R"(" pitch=")" +
					std::to_string(pitch) + R"(">)" + (pitch % 12 == 0 ? engine::midi::pitchName(pitch) : "") +
					"</div>";
			rows += R"(<div class="key-editor-row)" + shade + R"("/>)";
		}
		const std::string keys = whiteKeys + blackKeys;
		root.SetClass("drum-editor", drum);
		updateOrder();
		root.SetInnerRML(R"(<div class="toolbar key-editor-toolbar">)" + tools + R"(<span class="separator"/>)" +
			transpose + R"(<span class="separator"/>)" + inputs +
			R"(<button id="key-length-quantize" menu="key-length-menu)" + m_menuSuffix +
			R"(" title="Length Quantize"/>)"
			R"(<valuefield id="key-insert-velocity" min="1" max="127" default="100" step="1" decimals="0")"
			R"( title="Insert Velocity"/>)" +
			(drum ? R"(<span class="separator"/><button id="drum-map" menu="drum-map-menu)" + m_menuSuffix +
						R"(" title="Drum Map"/>)"
						R"(<button class="key-input" setting="pitchVisibility" title="Pitch Visibility On/Off">)"
						R"(Pitch Visibility</button><button id="drum-pitch-options" menu="drum-pitch-menu)" +
						m_menuSuffix +
						R"(" title="Select Pitch Visibility Options">Options</button>)"
						R"(<button class="key-input" setting="showNoteLength" title="Show Note Length On/Off">)"
						R"(Note Length</button><button id="drum-grid-type" menu="drum-grid-menu)" +
						m_menuSuffix + R"(" title="Grid Type"/>)"
				  : "") +
			R"(</div>)"
			R"(<div class="info-line key-editor-info empty"><span class="info-empty">No Object Selected</span>)" +
			info + R"(</div><div class="editor-parts"/><div class="key-editor-body"><div class="key-editor-keys">)" +
			keys + (drum ? std::string() : std::string(R"(<div class="key-editor-keys-zoom"/>)")) +
			R"(</div><div class="key-editor-grid"><div class="key-editor-rows">)" + rows +
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
			if (drum || std::string(length) != "drumMap")
				items += std::string(R"(<menuitem type="toggle" length=")") + length + R"(">)" +
					(lengthLabel(length) != nullptr ? lengthLabel(length) : length) + "</menuitem>";
		std::string modes;
		for (const auto& [mode, label] : g_lineModes)
			modes += std::string(R"(<menuitem type="toggle" line-mode=")") + mode + R"(">)" + label + "</menuitem>";
		// The toolbox: the toolbar's tools, as its buttons show them.
		const std::string toolbox = toolsRml("menuitem", "");
		holder->SetInnerRML(R"(<menu id="key-length-menu)" + m_menuSuffix + R"(">)" + items +
			R"(</menu><menu id="key-line-menu)" + m_menuSuffix + R"(">)" + modes +
			R"(</menu><menu id="key-editor-toolbox)" + m_menuSuffix + R"(" class="toolbox">)" + toolbox + "</menu>" +
			(drum ? R"(<menu id="drum-map-menu)" + m_menuSuffix + R"("/><menu id="drum-pitch-menu)" + m_menuSuffix +
						R"("><menuitem type="toggle" setting="reverseSounds">Reverse Drum Sound List</menuitem></menu>)"
						R"(<menu id="drum-grid-menu)" +
						m_menuSuffix +
						R"("><menuitem type="toggle" grid-type="quantize">Use Quantize</menuitem>)"
						R"(<menuitem type="toggle" grid-type="drumMap">Use Snap from Drum Map</menuitem></menu>)"
				  : std::string()));
		while (holder->HasChildNodes())
		{
			Rml::Element* menu = document->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
			menu->AddEventListener(Rml::EventId::Click, this);
			m_menus.push_back(menu->GetObserverPtr());
		}
		for (const Rml::EventId event : {Rml::EventId::Mousedown, Rml::EventId::Mousemove, Rml::EventId::Mouseup,
				 Rml::EventId::Click, Rml::EventId::Dblclick, Rml::EventId::Change})
			root.AddEventListener(event, this);
		document->AddEventListener(Rml::EventId::Mouseup, this);
		document->AddEventListener(Rml::EventId::Mousemove, this);
		document->AddEventListener(Rml::EventId::Mouseout, this);
		m_document = document->GetObserverPtr();
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
		const std::string toolbox = "key-editor-toolbox" + m_menuSuffix;
		grid->SetAttribute("contextmenu", m_toolboxOnRightClick ? toolbox : "");
		grid->SetAttribute("modifiercontextmenu", m_toolboxOnRightClick ? "" : toolbox);
	}

	KeyEditor::~KeyEditor()
	{
		// A key held while the editor goes ends its pitch, whichever way the editor goes: closing, its window closed,
		// another editor shown instead, the lower zone's panel or an in-place editor going, or the switch between the
		// key and drum editor, which builds a key editor of its own. Sending may throw, which a destructor must not
		// pass on.
		try
		{
			sound(std::nullopt);
		}
		catch (const std::exception& error)
		{
			logging::warning("The key editor cannot end the pitch of its pressed key: {}", error.what());
		}
		if (Rml::Element* document = m_document.get())
		{
			document->RemoveEventListener(Rml::EventId::Mouseup, this);
			document->RemoveEventListener(Rml::EventId::Mousemove, this);
			document->RemoveEventListener(Rml::EventId::Mouseout, this);
		}
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
				button->SetAttribute("menu", "key-line-menu" + m_menuSuffix);
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
						note.at("releaseVelocity").get<double>(), note.at("channel").get<int>(), item.editable,
						note.value("articulations", std::vector<std::string>())});
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
		if (m_range)
		{
			m_from = m_range->first;
			m_to = std::max(m_range->second, m_from + 1);
		}
		updateOrder();
		render();
		m_lanes->show(m_project, m_view, m_from, m_to);
	}

	void KeyEditor::setTimeRange(std::optional<std::pair<int64_t, int64_t>> range)
	{
		// The in-place editor's first show() comes before its layout: its first scroll to the notes waits for one.
		scrollToNotes();
		if (range == m_range)
			return;
		m_range = range;
		if (!m_range)
		{
			show(Json(m_project), Json(m_view)); // the parts' range again
			return;
		}
		// The in-place editor's range changes while the project window scrolls or zooms: the notes move, nothing is
		// read or built again.
		m_from = m_range->first;
		m_to = std::max(m_range->second, m_from + 1);
		placeItems();
		m_lanes->setRange(m_from, m_to);
		if (const Rml::Element* root = m_root.get(); root != nullptr && root->GetContext() != nullptr)
			root->GetContext()->RequestNextUpdate(0);
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
		const float row =
			(static_cast<float>(m_rowOf.at(static_cast<size_t>(std::clamp(pitch, 0, g_pitches - 1)))) + 0.5F) /
			g_pitches * height;
		body->SetScrollTop(std::clamp(row - (client / 2.0F), 0.0F, std::max(height - client, 0.0F)));
		m_scrolled = true; // a pitch scrolled to is where it stays
		return true;
	}

	// The drag on the keyboard's left part (p. 1261): to the right it zooms the pitches vertically in, to the left
	// out, and up or down it scrolls the note display. The pitch row at the top of the view when the drag started
	// stays where it was, moved by as much as the mouse went.
	void KeyEditor::zoomScroll(Rml::Event& event)
	{
		Rml::Element* root = m_root.get();
		Rml::Element* body = root != nullptr ? root->QuerySelector(".key-editor-body") : nullptr;
		if (body == nullptr)
			return;
		Rml::Context* context = root->GetContext();
		const float ratio = context != nullptr ? context->GetDensityIndependentPixelRatio() : 1.0F;
		const float dx = event.GetParameter("mouse_x", 0.0F) - m_pressedX;
		const float dy = event.GetParameter("mouse_y", 0.0F) - m_pressedY;
		if (!m_zoomDrag && std::abs(dx) < g_dragThreshold * ratio && std::abs(dy) < g_dragThreshold * ratio)
			return; // still a click on a key
		m_zoomDrag = true;
		setPitchZoom(m_zoomStart * std::exp2(dx / ratio / g_zoomDoubling));
		body->SetScrollTop((m_scrollStart * m_pitchZoom / m_zoomStart) - dy);
		if (context != nullptr)
			context->RequestNextUpdate(0);
	}

	void KeyEditor::setPitchZoom(float zoom)
	{
		zoom = std::clamp(zoom, g_minPitchZoom, g_maxPitchZoom);
		if (zoom == m_pitchZoom)
			return;
		m_pitchZoom = zoom;
		Rml::Element* root = m_root.get();
		if (root == nullptr || m_drum)
			return;
		// The keyboard and the rows follow the zoom; the keys, the notes and the cursor lie in percent of them, so
		// they scale with them.
		if (Rml::Element* keys = root->QuerySelector(".key-editor-keys"))
			keys->SetProperty(Rml::PropertyId::Height, dp(static_cast<float>(g_pitches) * m_rowDp * zoom));
		Rml::ElementList rows;
		root->QuerySelectorAll(rows, ".key-editor-row");
		for (Rml::Element* row : rows)
			row->SetProperty(Rml::PropertyId::Height, dp(m_rowDp * zoom));
	}

	void KeyEditor::place(Rml::Element& element, int64_t start, int64_t length, int pitch) const
	{
		const auto range = static_cast<double>(m_to - m_from);
		element.SetProperty(Rml::PropertyId::Left, percent(static_cast<double>(start - m_from) / range * 100.0));
		// The drum editor's notes are diamonds at their start (p. 1233): the style sheet sizes them.
		if (!element.IsClassSet("drum-note"))
			element.SetProperty(Rml::PropertyId::Width, percent(static_cast<double>(length) / range * 100.0));
		if (pitch >= 0)
		{
			const double row = m_rowOf.at(static_cast<size_t>(pitch));
			element.SetProperty(Rml::PropertyId::Top, percent(row * 100.0 / g_pitches));
			if (!element.IsClassSet("drum-note"))
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

		// The selection only changes classes, so that a press that selects keeps the element under the mouse.
		std::string rml;
		for (const Note& note : m_notes)
			rml += R"(<div class="key-note)" + std::string(diamonds() ? " drum-note" : "") +
				(note.editable ? "" : " inactive") + R"(" part=")" + std::to_string(note.part) + R"(" note=")" +
				std::to_string(note.id) + R"("/>)";
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
					setClass(notes->GetChild(i), "selected", selected(*found));
		placeItems();

		if (m_drum)
			renderDrumMap();
		// The keys of selected notes are lit (p. 1204), and the drum sounds of selected notes.
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
		setInner(
			root->QuerySelector("#key-length-quantize"), lengthLabel(length) != nullptr ? lengthLabel(length) : length);
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
		if (Rml::Element* menu = m_menus.size() > 4 ? m_menus[4].get() : nullptr)
		{
			Rml::ElementList items;
			menu->QuerySelectorAll(items, "menuitem[setting]");
			for (Rml::Element* item : items)
				if (m_view.value(attribute(*item, "setting"), false) != item->HasAttribute("checked"))
				{
					if (item->HasAttribute("checked"))
						item->RemoveAttribute("checked");
					else
						item->SetAttribute("checked", "");
				}
		}
		// The drum editor's grid type (p. 1227).
		const std::string gridType = m_view.value("gridType", std::string("quantize"));
		setInner(root->QuerySelector("#drum-grid-type"), gridType == "drumMap" ? "Drum Map Snap" : "Use Quantize");
		if (Rml::Element* menu = m_menus.size() > 5 ? m_menus[5].get() : nullptr)
		{
			Rml::ElementList items;
			menu->QuerySelectorAll(items, "menuitem");
			for (Rml::Element* item : items)
				if ((attribute(*item, "grid-type") == gridType) != item->HasAttribute("checked"))
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
			setClass(cursor, "off", !m_view.value("stepInput", false));
		renderInfo();
		scrollToNotes();
		if (Rml::Context* context = root->GetContext())
			context->RequestNextUpdate(0);
	}

	void KeyEditor::scrollToNotes()
	{
		if (m_scrolled)
			return;
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

	void KeyEditor::placeItems()
	{
		Rml::Element* root = m_root.get();
		if (root == nullptr)
			return;
		Rml::ElementList shown;
		root->QuerySelectorAll(shown, ".editor-part");
		for (Rml::Element* element : shown)
			for (const Part& part : m_parts)
				if (part.id == idAttribute(*element, "part"))
					place(*element, part.start, part.end - part.start, -1);
		// The notes' elements are in m_notes' order (render), and a note being drawn after them (press).
		if (const Rml::Element* notes = root->QuerySelector(".key-editor-notes"))
		{
			const Rml::Element* drawn = m_drawn.get();
			size_t next = 0;
			for (int i = 0; i < notes->GetNumChildren(); ++i)
			{
				Rml::Element& child = *notes->GetChild(i);
				if (&child == drawn)
				{
					place(child, std::llround(m_pressedAt.first), m_drawnLength, m_pressedAt.second);
				}
				else if (next < m_notes.size())
				{
					place(child, m_notes[next].start, m_notes[next].length, m_notes[next].pitch);
					++next;
				}
			}
		}
		if (Rml::Element* cursor = root->QuerySelector(".key-editor-step-cursor");
			cursor != nullptr && m_view.value("stepInput", false))
			place(*cursor, m_view.value("stepPosition", int64_t{0}), 0, -1);
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
			{"channel", std::to_string(first.channel + 1)}, {"articulations", joined(first.articulations)}};
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
		const int row = std::clamp(static_cast<int>(std::floor(y * g_pitches)), 0, g_pitches - 1);
		return {static_cast<double>(m_from) + (x * static_cast<double>(m_to - m_from)),
			m_order.at(static_cast<size_t>(row))};
	}

	// The quantize grid (quantize.set, p. 1196) from the project start; Ctrl (Cmd) turns snap off (p. 1213).
	int64_t KeyEditor::grid(int pitch) const
	{
		// The drum editor's Use Snap from Drum Map (p. 1227): the sound's snap.
		if (const Json* map = m_drum && pitch >= 0 && m_view.value("gridType", "") == "drumMap" ? activeMap() : nullptr)
			return soundSnap(*map, pitch);
		const std::string grid = m_project.value("quantize", Json::object()).value("grid", std::string("1/16"));
		return engine::midi::noteValueFromString(grid).value_or(engine::midi::NoteValue{}).ticks();
	}

	int64_t KeyEditor::soundSnap(const Json& map, int pitch)
	{
		const std::string snap = map.at("sounds").at(static_cast<size_t>(pitch)).at("snap").get<std::string>();
		return engine::midi::noteValueFromString(snap).value_or(engine::midi::NoteValue{}).ticks();
	}

	int64_t KeyEditor::snap(double ticks, bool off, int pitch) const
	{
		if (off)
			return std::llround(ticks);
		const auto step = static_cast<double>(grid(pitch));
		return std::llround(std::round(ticks / step) * step);
	}

	int64_t KeyEditor::lengthQuantize(int pitch) const
	{
		const std::string length = m_view.value("lengthQuantize", std::string("link"));
		if (length == "drumMap")
		{
			const Json* map = activeMap();
			return map != nullptr ? soundSnap(*map, pitch) : grid();
		}
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

	void KeyEditor::addNotes(
		const Part& part, const std::vector<int64_t>& starts, int64_t length, int pitch, std::optional<double> velocity)
	{
		const double inserted = velocity.value_or(m_view.value("insertVelocity", 100.0 / g_maxVelocity));
		Json notes = Json::array();
		for (const int64_t start : starts)
			notes.push_back({{"position", start - part.start}, {"length", std::max<int64_t>(length, 1)},
				{"pitch", pitch}, {"velocity", inserted}});
		m_binding.send("note.add", {{"part", part.id}, {"notes", notes}});
	}

	int KeyEditor::shifted(int pitch, int rows) const
	{
		const int row = m_rowOf.at(static_cast<size_t>(pitch)) + rows;
		return row >= 0 && row < g_pitches ? m_order.at(static_cast<size_t>(row)) : -1;
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
		if (field == "articulations")
		{
			// The Articulations field (#1112, p. 1290): the attributes typed, names separated by commas, for every
			// selected note; empty clears them.
			std::vector<std::string> names;
			std::string name;
			for (std::istringstream stream(text); std::getline(stream, name, ',');)
			{
				name.erase(0, name.find_first_not_of(' '));
				name.erase(name.find_last_not_of(' ') + 1);
				if (!name.empty())
					names.push_back(name);
			}
			std::map<uint64_t, Json> changes;
			for (const Note* note : notes)
			{
				Json change = noteChange(note->id);
				change["articulations"] = names;
				changes[note->part].push_back(change);
			}
			Json commands = Json::array();
			for (auto& [part, items] : changes)
				commands.push_back({{"command", "note.set"}, {"parameters", {{"part", part}, {"notes", items}}}});
			edit("Change Articulations", std::move(commands));
			return;
		}
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
		else if (target.HasAttribute("grid-type"))
		{
			setView({{"gridType", attribute(target, "grid-type")}});
		}
		else if (target.HasAttribute("line-mode"))
		{
			m_lineMode = attribute(target, "line-mode");
			setTool(KeyTool::Line);
		}
		else if (target.HasAttribute("drum-map") || target.HasAttribute("drum-map-action"))
		{
			drumMapMenu(target);
		}
		else if (target.IsClassSet("drum-sound-mute"))
		{
			const Json* map = activeMap();
			if (const Rml::Element* row = withAttribute(&target, "pitch", m_root.get());
				map != nullptr && row != nullptr)
			{
				const int pitch = std::stoi(attribute(*row, "pitch"));
				const bool mute = map->at("sounds").at(static_cast<size_t>(pitch)).at("mute").get<bool>();
				m_binding.send(
					"drumMap.set", {{"map", map->at("id")}, {"sounds", {{{"pitch", pitch}, {"mute", !mute}}}}});
			}
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
			// A keyboard drag hears the mouse in the whole document: zooming out to the left takes the mouse out of
			// the editor. The root's pass and the document's compute the same absolute state, so both are harmless.
			if (event.GetCurrentElement() == m_root.get() || m_gesture == Gesture::ZoomScroll)
				drag(event);
			break;
		case Rml::EventId::Mouseup:
		{
			// A key's pitch ends wherever the mouse is released; the document hears that, the root only within.
			sound(std::nullopt);
			// A drumstick's deletion and a keyboard's drag end wherever the mouse is released; the other gestures
			// only within.
			if (event.GetCurrentElement() == m_root.get() || m_gesture == Gesture::Erase ||
				m_gesture == Gesture::ZoomScroll)
			{
				release(event);
			}
			else if (const auto [grid, track] = foreignInPlaceGrid(event);
				grid != nullptr && m_gesture != Gesture::None)
			{
				// A gesture released over another in-place editor's grid: a move drops the selected notes there
				// (p. 1261); the others end without their effect.
				if (std::exchange(m_gesture, Gesture::None) == Gesture::Move)
				{
					moveToInPlace(event, *grid, track);
				}
				else
				{
					if (Rml::Element* drawn = m_drawn.get(); drawn != nullptr && drawn->GetParentNode() != nullptr)
						drawn->GetParentNode()->RemoveChild(drawn);
					m_pressed.reset();
					placeItems();
				}
			}
			break;
		}
		case Rml::EventId::Mouseout:
			// The mouse leaving the window ends the pitch too: the button going up out there sends no mouseup.
			if (target == m_document.get())
			{
				sound(std::nullopt);
				// The keyboard's drag ends too, for the same reason.
				if (m_gesture == Gesture::ZoomScroll)
					m_gesture = Gesture::None;
			}
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
			else if (const Rml::Element* row = withAttribute(target, "pitch", m_root.get());
				row != nullptr && target->HasAttribute("sound-field"))
				editSound(std::stoi(attribute(*row, "pitch")), attribute(*target, "sound-field"),
					event.GetParameter<Rml::String>("value", ""));
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
		// The keyboard's left part (p. 1261): a press plays the pitch under it as a key does, and a drag zooms the
		// pitches and scrolls the note display.
		if (!m_drum && target->IsClassSet("key-editor-keys-zoom"))
		{
			const Rml::Vector2f origin = target->GetAbsoluteOffset(Rml::BoxArea::Border);
			const float high = target->GetBox().GetSize(Rml::BoxArea::Border).y;
			const float y = event.GetParameter("mouse_y", 0.0F) - origin.y;
			const int row =
				std::clamp(static_cast<int>(std::floor(y / std::max(high, 1.0F) * g_pitches)), 0, g_pitches - 1);
			sound(m_order.at(static_cast<size_t>(row)));
			Rml::Element* body = root->QuerySelector(".key-editor-body");
			m_gesture = Gesture::ZoomScroll;
			m_zoomDrag = false;
			m_pressed.reset(); // a note an earlier press remembered must not resize on this gesture's release
			m_pressedX = event.GetParameter("mouse_x", 0.0F);
			m_pressedY = event.GetParameter("mouse_y", 0.0F);
			m_zoomStart = m_pitchZoom;
			m_scrollStart = body != nullptr ? body->GetScrollTop() : 0.0F;
			// The rows' height unzoomed, as the style sheet gives it (#1369): the zoom scales it.
			if (const Rml::Element* rowElement = root->QuerySelector(".key-editor-row"))
				if (const Rml::Style::LengthPercentageAuto height = rowElement->GetComputedValues().height();
					height.type == Rml::Style::LengthPercentageAuto::Length && height.value > 0.0F)
				{
					const Rml::Context* context = root->GetContext();
					const float ratio = context != nullptr ? context->GetDensityIndependentPixelRatio() : 1.0F;
					m_rowDp = height.value / ratio / m_pitchZoom;
				}
			return;
		}
		if (const Rml::Element* key = withAttribute(target, "pitch", root);
			!m_drum && key != nullptr && key->IsClassSet("key-editor-key"))
		{
			sound(std::stoi(attribute(*key, "pitch")));
			return;
		}
		// A click on a drum sound's pitch auditions it as the track's map plays the sound (#882, p. 1236).
		if (const Rml::Element* row = withAttribute(target, "pitch", root);
			m_drum && row != nullptr && target->IsClassSet("drum-sound-pitch"))
		{
			sound(std::stoi(attribute(*row, "pitch")));
			return;
		}
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
		m_pressedX = event.GetParameter("mouse_x", 0.0F);
		m_pressedY = event.GetParameter("mouse_y", 0.0F);
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
			// Near an end the press resizes, elsewhere it moves (pp. 1213-1214).
			m_gesture = edgeGesture(target, event);
			if (!selected(*pressed))
				select({{pressed->part, pressed->id}});
			return;
		case KeyTool::Draw:
		{
			// A click draws a note of the length quantize's length, a drag a multiple of it (p. 1212). The drumstick
			// deletes the note clicked, and with Show Note Length resizes it at its ends instead (p. 1242).
			if (pressed != nullptr && m_drum)
			{
				const Gesture edge = diamonds() ? Gesture::Move : edgeGesture(target, event);
				m_gesture = edge == Gesture::Move ? Gesture::Erase : edge;
				if (m_gesture != Gesture::Erase && !selected(*pressed))
					select({{pressed->part, pressed->id}});
			}
			if (pressed == nullptr)
				startDraw(ticks);
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
			// note.split per part, which gives the rests the notes' values and expression from the cut on.
			Json commands = Json::array();
			for (const Part& part : m_parts)
			{
				Json ids = Json::array();
				for (const Note* each : notes)
					if (each->part == part.id && cut > each->start && cut < each->start + each->length)
						ids.push_back(each->id);
				if (!ids.empty())
					commands.push_back({{"command", "note.split"},
						{"parameters", {{"part", part.id}, {"notes", ids}, {"position", cut - part.start}}}});
			}
			edit("Split", std::move(commands));
			return;
		}
		case KeyTool::Line: // only on the controller lanes
			return;
		case KeyTool::Glue:
		{
			// Glues the note to the next one of its pitch in its part; the first one's values stay (p. 1215), and the
			// next one's expression joins its own.
			if (pressed == nullptr)
				return;
			const Note* next = nullptr;
			for (const Note& each : m_notes)
				if (each.part == pressed->part && each.pitch == pressed->pitch && each.start > pressed->start &&
					(next == nullptr || each.start < next->start))
					next = &each;
			if (next == nullptr)
				return;
			edit("Glue",
				Json::array({{{"command", "note.glue"},
					{"parameters", {{"part", pressed->part}, {"note", pressed->id}, {"next", next->id}}}}}));
			return;
		}
		}
	}

	KeyEditor::Gesture KeyEditor::edgeGesture(Rml::Element* target, const Rml::Event& event) const
	{
		const Rml::Element* root = m_root.get();
		Rml::Element* box = withAttribute(target, "note", root);
		if (box == nullptr)
			return Gesture::Move;
		const float ratio =
			root->GetContext() != nullptr ? root->GetContext()->GetDensityIndependentPixelRatio() : 1.0F;
		const Rml::Vector2f origin = box->GetAbsoluteOffset(Rml::BoxArea::Border);
		const float width = box->GetBox().GetSize(Rml::BoxArea::Border).x;
		const float x = event.GetParameter("mouse_x", 0.0F) - origin.x;
		const float edge = g_edge * ratio;
		if (width > 3 * edge && x >= width - edge)
			return Gesture::ResizeEnd;
		if (width > 3 * edge && x <= edge)
			return Gesture::ResizeStart;
		return Gesture::Move;
	}

	void KeyEditor::startDraw(double ticks)
	{
		Rml::Element* root = m_root.get();
		const int pitch = m_pressedAt.second;
		const int64_t start =
			snap(ticks - (m_snapOff ? 0.0 : static_cast<double>(grid(pitch)) / 2.0), m_snapOff, pitch);
		if (partAt(start) == nullptr)
			return;
		m_pressedAt.first = static_cast<double>(start);
		m_drawnFrom = ticks;
		m_gesture = Gesture::Draw;
		Rml::Element* notes = root->QuerySelector(".key-editor-notes");
		Rml::ElementPtr drawn = root->GetOwnerDocument()->CreateElement("div");
		drawn->SetClass("key-note", true);
		drawn->SetClass("drum-note", diamonds());
		drawn->SetClass("drawing", true);
		Rml::Element* added = notes->AppendChild(std::move(drawn));
		m_drawnLength = lengthQuantize(m_pressedAt.second);
		place(*added, start, m_drawnLength, m_pressedAt.second);
		m_drawn = added->GetObserverPtr();
	}

	KeyEditor::DrumDraw KeyEditor::drumDraw(const Rml::Event& event) const
	{
		const auto [ticks, pitch] = at(event);
		const Rml::Context* context = m_root.get()->GetContext();
		const float ratio = context != nullptr ? context->GetDensityIndependentPixelRatio() : 1.0F;
		DrumDraw draw{std::llround(m_pressedAt.first), lengthQuantize(m_pressedAt.second), m_pressedAt.second,
			m_view.value("insertVelocity", 100.0 / g_maxVelocity)};
		// Up or down changes the velocity, a step of 127 per dp; with Alt the pitch instead.
		if (event.GetParameter("alt_key", 0) != 0)
			draw.pitch = pitch;
		else
			draw.velocity = static_cast<double>(std::clamp<int64_t>(std::lround(draw.velocity * g_maxVelocity) +
									std::lround((m_pressedY - event.GetParameter("mouse_y", 0.0F)) / ratio),
								1, static_cast<int64_t>(g_maxVelocity))) /
				g_maxVelocity;
		// With Shift the note moves along; with Snap off and Show Note Length a drag changes its length; else it
		// repeats at every step of the grid it was dragged over.
		const double moved = ticks - m_drawnFrom;
		if (event.GetParameter("shift_key", 0) != 0)
			draw.start += snap(moved, m_snapOff, m_pressedAt.second);
		else if (m_snapOff && !diamonds())
			draw.length = std::max<int64_t>(draw.length + std::llround(moved), 1);
		else
			draw.repeats = std::max<int64_t>(1,
				static_cast<int64_t>(
					std::ceil((ticks - m_pressedAt.first) / static_cast<double>(grid(m_pressedAt.second)))));
		return draw;
	}

	// How far a move or resize goes: the pressed note's start, or its end, snapped where the mouse took it.
	int64_t KeyEditor::delta(Gesture gesture, double ticks) const
	{
		if (!m_pressed)
			return 0;
		const int64_t edge = gesture == Gesture::ResizeEnd ? m_pressed->start + m_pressed->length : m_pressed->start;
		return snap(static_cast<double>(edge) + ticks - m_pressedAt.first, m_snapOff, m_pressed->pitch) - edge;
	}

	void KeyEditor::drag(Rml::Event& event)
	{
		if (m_gesture == Gesture::None)
			return;
		if (m_gesture == Gesture::ZoomScroll)
		{
			zoomScroll(event);
			return;
		}
		const auto [ticks, pitch] = at(event);
		const Rml::Element* notes = m_root.get()->QuerySelector(".key-editor-notes");
		if (notes == nullptr)
			return;
		if (m_gesture == Gesture::Draw)
		{
			if (Rml::Element* drawn = m_drawn.get(); drawn != nullptr && m_drum)
			{
				const DrumDraw draw = drumDraw(event);
				place(*drawn, draw.start, draw.length, draw.pitch);
			}
			else if (drawn != nullptr)
			{
				const int64_t step = lengthQuantize(m_pressedAt.second);
				const auto steps = std::max<int64_t>(
					1, static_cast<int64_t>(std::ceil((ticks - m_pressedAt.first) / static_cast<double>(step))));
				m_drawnLength = steps * step;
				place(*drawn, std::llround(m_pressedAt.first), m_drawnLength, m_pressedAt.second);
			}
			return;
		}
		if (!m_pressed || m_gesture == Gesture::Erase)
			return;
		const int64_t dt = delta(m_gesture, ticks);
		const int rows = m_gesture == Gesture::Move
			? m_rowOf.at(static_cast<size_t>(pitch)) - m_rowOf.at(static_cast<size_t>(m_pressedAt.second))
			: 0;
		for (int i = 0; i < notes->GetNumChildren(); ++i)
		{
			Rml::Element* element = notes->GetChild(i);
			const Note* shown = noteOf(element);
			if (shown == nullptr || !selected(*shown))
				continue;
			if (m_gesture == Gesture::Move)
				place(*element, shown->start + dt, shown->length,
					shifted(shown->pitch, rows) >= 0 ? shifted(shown->pitch, rows) : shown->pitch);
			else if (m_gesture == Gesture::ResizeEnd)
				place(*element, shown->start, std::max<int64_t>(shown->length + dt, 1), shown->pitch);
			else
				place(*element, shown->start + std::min(dt, shown->length - 1),
					shown->length - std::min(dt, shown->length - 1), shown->pitch);
		}
	}

	void KeyEditor::sound(std::optional<int> pitch)
	{
		// Into the track's channel as if from its MIDI input (midi.inject), on MIDI channel 1 as chord.audition plays;
		// without an audio device or an instrument nothing sounds, which is no error to report. The drum editor plays
		// the sound of the pitch itself, not the first with its I-note (drumSounds).
		const auto inject = [this](uint64_t channel, int note, bool on)
		{
			m_binding.send("midi.inject",
				{{"channel", channel}, {"drumSounds", m_drum},
					{"messages", Json::array({{{"position", 0}, {"bytes", {on ? 0x90 : 0x80, note, on ? 100 : 0}}}})}},
				[](const engine::Result&) {});
		};
		if (const auto sounding = std::exchange(m_sounding, std::nullopt))
			inject(sounding->first, sounding->second, false);
		if (!pitch)
			return;
		const Json activeTrack = m_view.value("activeTrack", Json(0));
		for (const Json& track : m_project.value("tracks", Json::array()))
			if (track.at("id") == activeTrack && track.value("channel", Json()).is_number_unsigned())
			{
				m_sounding = std::make_pair(track.at("channel").get<uint64_t>(), *pitch);
				inject(m_sounding->first, *pitch, true);
			}
	}

	void KeyEditor::release(Rml::Event& event)
	{
		const Gesture gesture = std::exchange(m_gesture, Gesture::None);
		if (gesture == Gesture::None)
			return;
		// The keyboard's drag edits nothing; the note an earlier press remembered must not resize either.
		if (gesture == Gesture::ZoomScroll)
		{
			m_pressed.reset();
			return;
		}
		const auto [ticks, pitch] = at(event);
		if (gesture == Gesture::Erase && m_pressed)
		{
			// The drumstick deletes the note clicked, and with a drag to the right the notes of its sound up to
			// where it is released (p. 1242). A click that hardly moves deletes only that note, not the doubles and
			// close flams under the mouse.
			const Rml::Context* context = m_root.get()->GetContext();
			const float ratio = context != nullptr ? context->GetDensityIndependentPixelRatio() : 1.0F;
			const bool dragged = event.GetParameter("mouse_x", 0.0F) - m_pressedX >= g_dragThreshold * ratio;
			std::map<uint64_t, Json> removed;
			const int64_t until = std::max(static_cast<int64_t>(std::llround(ticks)), m_pressed->start);
			for (const Note& each : m_notes)
				if (each.editable && each.pitch == m_pressed->pitch &&
					(each.id == m_pressed->id || (dragged && each.start >= m_pressed->start && each.start <= until)))
					removed[each.part].push_back(each.id);
			Json commands = Json::array();
			for (const auto& [part, notes] : removed)
				commands.push_back({{"command", "note.remove"}, {"parameters", {{"part", part}, {"notes", notes}}}});
			edit("Erase", std::move(commands));
			return;
		}
		if (gesture == Gesture::Draw)
		{
			Rml::Element* drawn = m_drawn.get();
			if (drawn != nullptr && drawn->GetParentNode() != nullptr)
				drawn->GetParentNode()->RemoveChild(drawn);
			if (m_drum)
			{
				// The drumstick repeats notes at every step of the grid it was dragged over (p. 1241).
				const DrumDraw draw = drumDraw(event);
				const int64_t step = grid(m_pressedAt.second);
				const Part* part = partAt(draw.start);
				std::vector<int64_t> starts;
				for (int64_t i = 0; part != nullptr && i < draw.repeats && draw.start + (i * step) < part->end; ++i)
					starts.push_back(draw.start + (i * step));
				if (part != nullptr)
					addNotes(*part, starts, draw.length, draw.pitch, draw.velocity);
				return;
			}
			const auto start = std::llround(m_pressedAt.first);
			const int64_t length = lengthQuantize(m_pressedAt.second);
			const auto steps = std::max<int64_t>(
				1, static_cast<int64_t>(std::ceil((ticks - m_pressedAt.first) / static_cast<double>(length))));
			if (const Part* part = partAt(start))
				addNotes(*part, {start}, steps * length, m_pressedAt.second);
			return;
		}
		if (!m_pressed)
			return;
		// A move or resize of the selected notes, the pressed one snapped (pp. 1213-1214).
		const bool end = gesture == Gesture::ResizeEnd;
		const int64_t dt = delta(gesture, ticks);
		const int rows = gesture == Gesture::Move
			? m_rowOf.at(static_cast<size_t>(pitch)) - m_rowOf.at(static_cast<size_t>(m_pressedAt.second))
			: 0;
		if (dt == 0 && rows == 0)
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
				if (note->start + dt < partStart || shifted(note->pitch, rows) < 0)
					return; // nothing moves before its part or out of the pitches
				change["position"] = note->start + dt - partStart;
				change["pitch"] = shifted(note->pitch, rows);
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

	std::pair<Rml::Element*, uint64_t> KeyEditor::foreignInPlaceGrid(const Rml::Event& event) const
	{
		// Only an in-place editor drops its notes into another one (p. 1261): the other editors' grids lie on time
		// axes of their own, so a drop there wouldn't land at the time under the mouse.
		if (m_name.rfind("inPlace:", 0) != 0)
			return {nullptr, 0};
		Rml::Element* grid = nullptr;
		for (Rml::Element* up = event.GetTargetElement(); up != nullptr; up = up->GetParentNode())
		{
			if (up->IsClassSet("key-editor-grid"))
			{
				grid = up;
			}
			else if (up->IsClassSet("in-place-editor"))
			{
				const auto track = up->GetAttribute<Rml::String>("track", "");
				if (grid != nullptr && !track.empty() && "inPlace:" + track != m_name)
					return {grid, std::stoull(track)};
				return {nullptr, 0};
			}
		}
		return {nullptr, 0};
	}

	// A move released over another in-place editor's grid (p. 1261): the selected notes go to the target track's
	// part at the drop's time, as note.move, which keeps their distances to the pressed one and everything they
	// have; one note.move per source part, in one history.transaction when there are several, so one undo step
	// brings them all back.
	void KeyEditor::moveToInPlace(const Rml::Event& event, Rml::Element& grid, uint64_t track)
	{
		const std::vector<const Note*> notes = selectedNotes();
		if (notes.empty() || !m_pressed)
		{
			m_pressed.reset();
			return;
		}
		// The drop in the target's grid: its x lies on the same timeline as this one's, its row gives the pitch.
		const Rml::Vector2f origin = grid.GetAbsoluteOffset(Rml::BoxArea::Border);
		const Rml::Vector2f size = grid.GetBox().GetSize(Rml::BoxArea::Border);
		if (size.x <= 0.0F || size.y <= 0.0F)
		{
			placeItems(); // the dragged notes back where they are
			return;
		}
		const double x = (event.GetParameter("mouse_x", 0.0F) - origin.x) / size.x;
		const double y = (event.GetParameter("mouse_y", 0.0F) - origin.y) / size.y;
		const double ticks = static_cast<double>(m_from) + (x * static_cast<double>(m_to - m_from));
		const int row = std::clamp(static_cast<int>(std::floor(y * g_pitches)), 0, g_pitches - 1);
		const int64_t dt = delta(Gesture::Move, ticks);
		const int rows = row - m_rowOf.at(static_cast<size_t>(m_pressedAt.second));
		// The target track's part where the pressed note lands, on the project's timeline.
		const TimeContext time = projectTime(m_project);
		std::map<uint64_t, const Json*> tracks;
		const auto trackList = m_project.find("tracks");
		if (trackList != m_project.end())
			for (const Json& each : *trackList)
				tracks[each.at("id").get<uint64_t>()] = &each;
		const int64_t pressedStart = m_pressed->start + dt;
		const Json* target = nullptr;
		int64_t targetStart = 0;
		const auto parts = m_project.find("midiParts");
		const auto trackJson = tracks.find(track);
		if (parts != m_project.end() && trackJson != tracks.end())
			for (const Json& part : *parts)
			{
				if (part.at("track").get<uint64_t>() != track)
					continue;
				const auto position = part.at("position").get<int64_t>();
				const int64_t start = ticksOf(*trackJson->second, position, time);
				const int64_t end = ticksOf(*trackJson->second, position + part.at("length").get<int64_t>(), time);
				if (pressedStart >= start && pressedStart < end)
				{
					target = &part;
					targetStart = start;
					break;
				}
			}
		if (target == nullptr)
		{
			placeItems(); // no part to take the notes: they stay where they are
			return;
		}
		// Every note has to fit the pitches and start in the target part or later, as for a move within one.
		for (const Note* note : notes)
			if (note->start + dt < targetStart || shifted(note->pitch, rows) < 0)
			{
				placeItems();
				return;
			}
		// The selected notes by their part: each part's notes go as one note.move, which keeps everything the notes
		// have; several parts go in one history.transaction, so the move is one undo step either way.
		const int semitones = m_order.at(static_cast<size_t>(row)) - m_pressedAt.second;
		std::map<uint64_t, Json> moving;
		for (const Note* note : notes)
			moving[note->part].push_back(note->id);
		Json commands = Json::array();
		for (const auto& [part, ids] : moving)
			commands.push_back({{"command", "note.move"},
				{"parameters",
					{{"part", part}, {"notes", ids}, {"target", target->at("id")}, {"ticks", dt},
						{"semitones", semitones}}}});
		m_pressed.reset();
		edit("Move", std::move(commands));
	}

	// With the object selection tool a double click on a note deletes it, in the empty grid it inserts a note of the
	// length quantize's length (p. 1211). In the drum editor a drag to the right after it, as with the drumstick,
	// inserts a note at every step of the grid, or with Snap on deletes the sound's notes up to the release
	// (pp. 1241-1242).
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
		const bool ctrl = event.GetParameter("ctrl_key", 0) != 0 || event.GetParameter("meta_key", 0) != 0;
		if (const Note* pressed = noteOf(event.GetTargetElement()); pressed != nullptr)
		{
			if (pressed->editable && m_drum && !ctrl)
			{
				m_pressed = *pressed; // the release deletes it, as the drumstick's does
				m_gesture = Gesture::Erase;
			}
			else if (pressed->editable)
			{
				edit("Delete",
					Json::array({{{"command", "note.remove"},
						{"parameters", {{"part", pressed->part}, {"notes", Json::array({pressed->id})}}}}}));
			}
			return;
		}
		const auto [ticks, pitch] = at(event);
		if (m_drum)
		{
			m_pressedAt.second = pitch;
			m_pressedY = event.GetParameter("mouse_y", 0.0F);
			m_snapOff = ctrl;
			startDraw(ticks); // the release inserts, as the drumstick's does
			return;
		}
		const int64_t start = snap(ticks - (ctrl ? 0.0 : static_cast<double>(grid()) / 2.0), ctrl);
		if (const Part* part = partAt(start))
			addNotes(*part, {start}, lengthQuantize(pitch), pitch);
	}
	const Json* KeyEditor::activeMap() const
	{
		const Json activeTrack = m_view.value("activeTrack", Json(0));
		uint64_t id = 0;
		for (const Json& track : m_project.value("tracks", Json::array()))
			if (track.at("id") == activeTrack)
				id = track.value("drumMap", uint64_t{0});
		if (id != 0 && m_project.contains("drumMaps"))
			for (const Json& map : m_project.at("drumMaps"))
				if (map.at("id") == id)
					return &map;
		return nullptr;
	}

	// The key editor's rows go from the highest pitch down; the drum editor's follow the drum map's order, or without
	// one go up from pitch 0.
	void KeyEditor::updateOrder()
	{
		m_order.clear();
		if (const Json* map = m_drum ? activeMap() : nullptr)
			m_order = map->at("order").get<std::vector<int>>();
		if (m_order.size() != static_cast<size_t>(g_pitches))
		{
			m_order.clear();
			for (int pitch = 0; pitch < g_pitches; ++pitch)
				m_order.push_back(m_drum ? pitch : g_pitches - 1 - pitch);
		}
		if (m_drum && m_view.value("pitchVisibility", false) && m_view.value("reverseSounds", false))
			std::reverse(m_order.begin(), m_order.end());
		m_rowOf.assign(static_cast<size_t>(g_pitches), 0);
		for (size_t row = 0; row < m_order.size(); ++row)
			m_rowOf.at(static_cast<size_t>(m_order[row])) = static_cast<int>(row);
	}

	// The drum sound list (pp. 1233-1234): a row per sound in the map's order with its pitch, and with a map its name,
	// mute button, snap, I-note, O-note, channel and output, which a double click edits; without a map only the
	// pitches.
	std::string KeyEditor::soundsRml() const
	{
		const Json* map = activeMap();
		std::string rml;
		const auto field = [](const char* name, const char* title, const std::string& value)
		{
			return std::string(R"(<textdisplay class="drum-sound-field drum-sound-)") + name + R"(" sound-field=")" +
				name + R"(" title=")" + title + R"(" value=")" + Rml::StringUtilities::EncodeRml(value) + R"("/>)";
		};
		for (const int pitch : m_order)
		{
			rml += R"(<div class="key-editor-key drum-sound" pitch=")" + std::to_string(pitch) +
				R"("><span class="drum-sound-pitch">)" + engine::midi::pitchName(pitch) + "</span>";
			if (map != nullptr)
			{
				const Json& sound = map->at("sounds").at(static_cast<size_t>(pitch));
				const Json& channel = sound.at("channel");
				const std::string output = sound.at("output").get<std::string>();
				rml += std::string(R"(<button class="drum-sound-mute)") +
					(sound.at("mute").get<bool>() ? " checked" : "") + R"(" title="Mute">M</button>)" +
					R"(<textdisplay class="drum-sound-name" sound-field="name" title="Drum Sound" value=")" +
					Rml::StringUtilities::EncodeRml(sound.at("name").get<std::string>()) + R"("/>)" +
					field("snap", "Snap", sound.at("snap").get<std::string>()) +
					field("iNote", "I-Note", engine::midi::pitchName(sound.at("iNote").get<int>())) +
					field("oNote", "O-Note", engine::midi::pitchName(sound.at("oNote").get<int>())) +
					field("channel", "Channel",
						channel.is_null() ? std::string("Any") : std::to_string(channel.get<int>() + 1)) +
					field("output", "Output", output.empty() ? std::string("Default") : output);
			}
			rml += "</div>";
		}
		return rml;
	}

	void KeyEditor::renderDrumMap()
	{
		Rml::Element* root = m_root.get();
		const Json* map = activeMap();
		if (Rml::Element* keys = root->QuerySelector(".key-editor-keys"))
			if (std::string rml = soundsRml(); rml != m_soundsRml)
			{
				keys->SetInnerRML(rml);
				m_soundsRml = std::move(rml);
			}
		setInner(root->QuerySelector("#drum-map"),
			map != nullptr ? Rml::StringUtilities::EncodeRml(map->at("name").get<std::string>()) : "No Drum Map");
		// The Map menu (p. 1234): No Drum Map and the project's maps, the track's checked, then the Drum Map Setup's
		// New Map, New Copy and Remove (p. 1236) for the track's map.
		const Json current = map != nullptr ? map->at("id") : Json(0);
		std::string rml = std::string(R"(<menuitem type="toggle" drum-map="0")") + (current == 0 ? " checked" : "") +
			">No Drum Map</menuitem>";
		for (const Json& each : m_project.value("drumMaps", Json::array()))
			rml += R"(<menuitem type="toggle" drum-map=")" + std::to_string(each.at("id").get<uint64_t>()) + "\"" +
				(each.at("id") == current ? " checked" : "") + ">" +
				Rml::StringUtilities::EncodeRml(each.at("name").get<std::string>()) + "</menuitem>";
		rml += R"(<menuseparator/><menuitem drum-map-action="new">New Map</menuitem>)";
		if (map != nullptr)
			rml += R"(<menuitem drum-map-action="copy">New Copy</menuitem>)"
				   R"(<menuitem drum-map-action="remove">Remove</menuitem>)";
		rml += R"(<menuseparator/><menuitem drum-map-action="setup">Drum Map Setup...</menuitem>)";
		if (Rml::Element* menu = m_menus.size() > 3 ? m_menus[3].get() : nullptr;
			menu != nullptr && rml != m_mapMenuRml)
		{
			menu->SetInnerRML(rml);
			m_mapMenuRml = rml;
		}
	}

	// A map chosen in the Map menu becomes the active track's; New Map and New Copy add one that does, in the same undo
	// step, Remove removes the track's; Drum Map Setup opens the setup.
	void KeyEditor::drumMapMenu(Rml::Element& target)
	{
		const Json track = m_view.value("activeTrack", Json(0));
		const std::string action = attribute(target, "drum-map-action");
		if (action == "setup")
		{
			if (m_drumMapSetup)
				m_drumMapSetup();
			return;
		}
		if (track == 0)
			return;
		if (target.HasAttribute("drum-map"))
		{
			m_binding.send("track.set", {{"track", track}, {"drumMap", idAttribute(target, "drum-map")}});
			return;
		}
		const Json* map = activeMap();
		if (action == "remove" && map != nullptr)
		{
			m_binding.send("drumMap.remove", {{"map", map->at("id")}});
			return;
		}
		Json parameters = {{"track", track}};
		if (action == "copy" && map != nullptr)
			parameters["copy"] = map->at("id");
		m_binding.send("drumMap.add", parameters);
	}

	// A drum sound's setting typed into the list: its name, snap (a note value), I-note and O-note (a pitch name or
	// number) or channel (1-16, or Any).
	void KeyEditor::editSound(int pitch, const std::string& field, const std::string& text)
	{
		const Json* map = activeMap();
		if (map == nullptr)
			return;
		Json sound = {{"pitch", pitch}};
		if (field == "name")
		{
			sound["name"] = text;
		}
		else if (field == "snap")
		{
			if (!engine::midi::noteValueFromString(text))
				return;
			sound["snap"] = text;
		}
		else if (field == "iNote" || field == "oNote")
		{
			const std::optional<int> note = parsePitch(text);
			if (!note)
				return;
			sound[field] = *note;
		}
		else if (field == "output")
		{
			sound["output"] = text == "Default" ? std::string() : text;
		}
		else if (field == "channel")
		{
			int channel = 0; // Any
			try
			{
				channel = std::stoi(text);
			}
			catch (const std::exception&)
			{
				channel = 0;
			}
			if (channel < 0 || channel > 16)
				return;
			sound["channel"] = channel == 0 ? Json(nullptr) : Json(channel - 1);
		}
		else
		{
			return;
		}
		m_binding.send("drumMap.set", {{"map", map->at("id")}, {"sounds", Json::array({sound})}});
	}
}
