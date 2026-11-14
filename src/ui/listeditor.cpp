// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/listeditor.h"

#include "engine/midi/articulationmaps.h"
#include "engine/midi/chords.h"
#include "engine/midi/midivalues.h"
#include "engine/midi/quantizesettings.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <tuple>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr double g_maxVelocity = 127.0;
		constexpr double g_maxController = 4294967295.0;
		constexpr int64_t g_bendCenter = 8192;

		// What the root hears in the bubble phase.
		constexpr std::array<Rml::EventId, 5> g_events = {Rml::EventId::Mousedown, Rml::EventId::Mousemove,
			Rml::EventId::Click, Rml::EventId::Change, Rml::EventId::Dragend};

		// The event types, as controller.add names them, with the list's names (p. 1253), in the filters bar's order.
		constexpr std::array<std::pair<const char*, const char*>, 8> g_types = {{
			{"note", "Note"},
			{"controller", "Controller"},
			{"program", "Program Change"},
			{"channelPressure", "Aftertouch"},
			{"polyPressure", "Poly Pressure"},
			{"pitchBend", "Pitchbend"},
			{"sysEx", "SysEx"},
			{"articulation", "Articulation"}, // directions (#1112, p. 1291), which have no channel
		}};

		constexpr std::array<std::pair<const char*, const char*>, 3> g_tools = {{
			{"objectSelection", "Object Selection"},
			{"draw", "Draw"},
			{"erase", "Erase"},
		}};

		// The event list's columns (p. 1251-1252) after Type.
		constexpr std::array<std::pair<const char*, const char*>, 8> g_fields = {{
			{"start", "Start"},
			{"end", "End"},
			{"length", "Length"},
			{"subtype", "Subtype"},
			{"value", "Main Value"},
			{"secondary", "Secondary Value"},
			{"channel", "Channel"},
			{"attributes", "Attributes"},
		}};

		// The Focus menu (p. 1250).
		constexpr std::array<std::pair<const char*, const char*>, 4> g_focuses = {{
			{"none", "No Focus"},
			{"type", "Event Types"},
			{"subtype", "Event Types and Subtype"},
			{"channel", "Event Channels"},
		}};

		constexpr std::array<const char*, 11> g_lengths = {
			"link", "1/1", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/4T", "1/8T", "1/16T"};

		// Whether events of the type have a MIDI channel: SysEx and directions don't.
		bool hasChannel(const std::string& type)
		{
			return type != "sysEx" && type != "articulation";
		}

		const char* typeLabel(const std::string& type)
		{
			for (const auto& [name, label] : g_types)
				if (type == name)
					return label;
			return "";
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

		Rml::Element* closest(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->IsClassSet(name))
					return element;
			return nullptr;
		}

		void setClass(Rml::Element* element, const char* name, bool set)
		{
			if (element != nullptr && element->IsClassSet(name) != set)
				element->SetClass(name, set);
		}

		void setAttribute(Rml::Element* element, const char* name, const std::string& value)
		{
			if (element != nullptr && attribute(*element, name) != value)
				element->SetAttribute(name, value);
		}

		void setChecked(Rml::Element& item, bool checked)
		{
			if (checked && !item.HasAttribute("checked"))
				item.SetAttribute("checked", "");
			else if (!checked && item.HasAttribute("checked"))
				item.RemoveAttribute("checked");
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

		// Controller values and pressure in MIDI 1.0's 0-127, pitch bend in -8192 to 8191, and back.
		int64_t toMidi1(double value)
		{
			return engine::midi::controllerToMidi1(static_cast<uint32_t>(std::llround(value * g_maxController)));
		}

		double fromMidi1(int64_t value)
		{
			return engine::midi::controllerFromMidi1(static_cast<uint8_t>(std::clamp<int64_t>(value, 0, 127))) /
				g_maxController;
		}

		int64_t toBend(double value)
		{
			return engine::midi::pitchBendToMidi1(static_cast<uint32_t>(std::llround(value * g_maxController))) -
				g_bendCenter;
		}

		double fromBend(int64_t value)
		{
			return engine::midi::pitchBendFromMidi1(
					   static_cast<uint16_t>(std::clamp<int64_t>(value + g_bendCenter, 0, (2 * g_bendCenter) - 1))) /
				g_maxController;
		}

		std::string hex(const std::vector<int>& data)
		{
			std::string text;
			for (const int byte : data)
			{
				std::array<char, 4> digits{};
				std::snprintf(digits.data(), digits.size(), "%02X", byte);
				text += (text.empty() ? "" : " ") + std::string(digits.data());
			}
			return text;
		}

		// Bytes typed in hexadecimal, with or without the F0 and F7 around them; nothing if they aren't.
		std::optional<std::vector<int>> parseHex(const std::string& text)
		{
			std::istringstream stream(text);
			std::vector<int> data;
			std::string word;
			while (stream >> word)
			{
				size_t used = 0;
				int byte = 0;
				try
				{
					byte = std::stoi(word, &used, 16);
				}
				catch (const std::exception&)
				{
					return std::nullopt;
				}
				if (used != word.size() || byte < 0 || byte > 0xFF)
					return std::nullopt;
				data.push_back(byte);
			}
			if (data.empty() || data.front() != 0xF0)
				data.insert(data.begin(), 0xF0);
			if (data.size() < 2 || data.back() != 0xF7)
				data.push_back(0xF7);
			if (!std::all_of(data.begin() + 1, data.end() - 1, [](int byte) { return byte < 0x80; }))
				return std::nullopt;
			return data;
		}
	}

	ListEditor::ListEditor(EngineBinding& binding, Rml::Element& root, std::string view)
		: m_binding(binding), m_root(root.GetObserverPtr()), m_name(std::move(view))
	{
		std::string tools;
		for (const auto& [tool, title] : g_tools)
			tools += std::string(R"(<button class="list-tool" list-tool=")") + tool + R"(" title=")" + title + R"(">)" +
				title + "</button>";
		std::string filters;
		for (const auto& [type, label] : g_types)
			filters += std::string(R"(<button class="list-filter" event-type=")") + type + R"(" title="Hide )" + label +
				R"(">)" + label + "</button>";
		// The header, and the empty row under the events that drawing can start in, have the rows' columns.
		std::string header = R"(<span class="list-type">Type</span>)";
		std::string spacers = R"(<span class="list-type"/>)";
		for (const auto& [field, label] : g_fields)
		{
			header += std::string(R"(<span class="list-cell" field=")") + field + R"(">)" + label + "</span>";
			spacers += std::string(R"(<span class="list-cell" field=")") + field + R"("/>)";
		}
		root.SetInnerRML(R"(<div class="toolbar list-editor-toolbar">)" + tools +
			R"(<span class="separator"/><button id="list-insert-type" menu="list-insert-type-menu" )"
			R"(title="Insert Event Type"/><button id="list-length-quantize" menu="list-length-menu" )"
			R"(title="Length Quantize"/><valuefield id="list-insert-velocity" min="1" max="127" default="100" )"
			R"(step="1" decimals="0" title="Note Insert Velocity"/></div>)"
			R"(<div class="toolbar list-editor-filters"><button id="list-focus" menu="list-focus-menu" title="Focus"/>)" +
			filters + R"(</div><div class="list-editor-header list-row">)" + header +
			R"(<div class="list-event-lane"/><div class="list-value-lane"/></div>)"
			R"(<div class="list-editor-body"><div class="list-editor-rows"/>)"
			R"(<div class="list-row list-filler">)" +
			spacers + R"(<div class="list-event-lane"/><div class="list-value-lane"/></div></div>)");
		// The menus go into the body, so that no zone clips them.
		Rml::ElementDocument* document = root.GetOwnerDocument();
		std::string types;
		for (const auto& [type, label] : g_types)
			if (std::string(type) != "articulation") // a direction needs a name: the Articulations lane adds them
				types +=
					std::string(R"(<menuitem type="toggle" insert-type=")") + type + R"(">)" + label + "</menuitem>";
		std::string lengths;
		for (const char* length : g_lengths)
			lengths += std::string(R"(<menuitem type="toggle" length=")") + length + R"(">)" +
				(std::string(length) == "link" ? "Quantize Link" : length) + "</menuitem>";
		std::string focuses;
		for (const auto& [focus, label] : g_focuses)
			focuses += std::string(R"(<menuitem type="toggle" focus=")") + focus + R"(">)" + label + "</menuitem>";
		for (const std::string& rml : {R"(<menu id="list-insert-type-menu">)" + types + "</menu>",
				 R"(<menu id="list-length-menu">)" + lengths + "</menu>",
				 R"(<menu id="list-focus-menu">)" + focuses + "</menu>"})
		{
			const Rml::ElementPtr holder = document->CreateElement("div");
			holder->SetInnerRML(rml);
			Rml::Element* menu = document->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
			menu->AddEventListener(Rml::EventId::Click, this);
			m_menus.push_back(menu->GetObserverPtr());
		}
		// The mouseup that ends a gesture goes to the document (listen), so that it ends wherever the mouse goes up in
		// it. Past the window's edge RmlUi has no element to send the mouseup to; during a gesture the body is a drag
		// element (in-gesture), whose dragend comes wherever the button goes up.
		for (const Rml::EventId event : g_events)
			root.AddEventListener(event, this);
		// Before a text display takes Enter: Ctrl (Cmd) with it sets every selected event to the value (p. 1255).
		root.AddEventListener(Rml::EventId::Keydown, this, true);
	}

	ListEditor::~ListEditor()
	{
		listen(false);
		if (Rml::Element* root = m_root.get())
		{
			for (const Rml::EventId event : g_events)
				root->RemoveEventListener(event, this);
			root->RemoveEventListener(Rml::EventId::Keydown, this, true);
		}
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (Rml::Element* element = menu.get())
			{
				element->RemoveEventListener(Rml::EventId::Click, this);
				if (Rml::Element* parent = element->GetParentNode())
					parent->RemoveChild(element);
			}
	}

	void ListEditor::show(const Json& project, const Json& view)
	{
		m_project = project;
		m_view = view;
		m_parts.clear();
		m_rows.clear();
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
				const Json& track = *tracks.at(part.at("track").get<uint64_t>());
				const auto position = part.at("position").get<int64_t>();
				const Part item{id.get<uint64_t>(), ticksOf(track, position, time),
					ticksOf(track, position + part.at("length").get<int64_t>(), time),
					std::find(editable.begin(), editable.end(), id) != editable.end()};
				m_parts.push_back(item);
				for (const Json& note : part.at("notes"))
				{
					Row row;
					row.part = item.id;
					row.id = note.at("id").get<uint64_t>();
					row.type = "note";
					row.partStart = item.start;
					row.start = item.start + note.at("position").get<int64_t>();
					row.length = note.at("length").get<int64_t>();
					row.number = note.at("pitch").get<int>();
					row.value = note.at("velocity").get<double>();
					row.release = note.at("releaseVelocity").get<double>();
					row.channel = note.at("channel").get<int>();
					row.articulations = note.value("articulations", std::vector<std::string>());
					row.editable = item.editable;
					m_rows.push_back(row);
				}
				for (const Json& controller : part.at("controllers"))
				{
					Row row;
					row.part = item.id;
					row.id = controller.at("id").get<uint64_t>();
					row.type = controller.at("type").get<std::string>();
					row.partStart = item.start;
					row.start = item.start + controller.at("position").get<int64_t>();
					row.number = controller.value("number", 0);
					row.value = controller.value("value", 0.0);
					row.channel = controller.value("channel", 0);
					row.data = controller.value("data", std::vector<int>());
					row.articulation = controller.value("articulation", std::string());
					row.reset = controller.value("reset", false);
					row.editable = item.editable;
					m_rows.push_back(row);
				}
			}
		// In playback order (p. 1251): by time, notes first.
		std::stable_sort(m_rows.begin(), m_rows.end(),
			[](const Row& a, const Row& b)
			{
				return std::make_tuple(a.start, !a.isNote(), a.part, a.id) <
					std::make_tuple(b.start, !b.isNote(), b.part, b.id);
			});
		for (const Json& selected : m_view.value("selection", Json::array()))
			m_selection.emplace(selected.at("part").get<uint64_t>(), selected.at("note").get<uint64_t>());
		for (const Json& selected : m_view.value("controllerSelection", Json::array()))
			m_selection.emplace(selected.at("part").get<uint64_t>(), selected.at("controller").get<uint64_t>());
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
	}

	// The filters bar's types and the Focus menu's filter, from the first selected event (p. 1250).
	bool ListEditor::shown(const Row& row, const Row* first) const
	{
		if (m_hidden.count(row.type) != 0)
			return false;
		if (m_focus == "none" || first == nullptr)
			return true;
		const Row& focus = *first;
		if (m_focus == "channel")
			return hasChannel(row.type) && hasChannel(focus.type) && row.channel == focus.channel;
		if (row.type != focus.type)
			return false;
		if (row.type == "articulation")
			return m_focus == "type" || row.articulation == focus.articulation;
		return m_focus == "type" || row.type == "sysEx" || row.type == "channelPressure" || row.type == "pitchBend" ||
			row.number == focus.number;
	}

	std::optional<int64_t> ListEditor::number(const Row& row, const std::string& field)
	{
		const std::string& type = row.type;
		if (field == "start")
			return row.start;
		if (field == "end" && row.isNote())
			return row.start + row.length;
		if (field == "length" && row.isNote())
			return row.length;
		if (field == "channel" && hasChannel(type))
			return row.channel + 1;
		if (field == "subtype")
		{
			if (type == "note" || type == "polyPressure" || type == "controller" || type == "program")
				return row.number;
			if (type == "channelPressure")
				return toMidi1(row.value);
			if (type == "pitchBend")
				return toBend(row.value);
		}
		if (field == "value")
		{
			if (type == "note")
				return std::lround(row.value * g_maxVelocity);
			if (type == "controller" || type == "polyPressure")
				return toMidi1(row.value);
		}
		if (field == "secondary" && type == "note")
			return std::lround(row.release * g_maxVelocity);
		return std::nullopt;
	}

	std::string ListEditor::text(const Row& row, const std::string& field) const
	{
		if (field == "attributes")
		{
			// SysEx's bytes, a direction's name, a note's attributes (#1112, p. 1291).
			if (row.type == "sysEx")
				return hex(row.data);
			if (row.type == "articulation")
				return row.articulation + (row.reset ? " (Reset)" : "");
			return engine::midi::joinNames(row.articulations);
		}
		const std::optional<int64_t> value = number(row, field);
		if (!value)
			return "";
		const TimeContext time = projectTime(m_project);
		if (field == "start" || field == "end")
			return formatTime(TimeFormat::BarsBeats, time.tempo.toSeconds(*value), time);
		if (field == "length")
			return formatLength(TimeFormat::BarsBeats, time.tempo.toSeconds(row.start),
				time.tempo.toSeconds(row.start + row.length), time);
		if (field == "subtype" && (row.type == "note" || row.type == "polyPressure"))
			return engine::midi::pitchName(static_cast<int>(*value)); // a pitch (p. 1252)
		return std::to_string(*value);
	}

	std::optional<int64_t> ListEditor::parse(const std::string& field, const std::string& typed, const Row& row) const
	{
		const TimeContext time = projectTime(m_project);
		if (field == "start" || field == "end")
		{
			const std::optional<double> seconds = parseTime(TimeFormat::BarsBeats, typed, time);
			return seconds ? std::optional<int64_t>(time.tempo.toTicks(*seconds)) : std::nullopt;
		}
		if (field == "length")
		{
			const double start = time.tempo.toSeconds(row.start);
			const std::optional<double> seconds = parseLength(TimeFormat::BarsBeats, typed, start, time);
			return seconds ? std::optional<int64_t>(time.tempo.toTicks(start + *seconds) - row.start) : std::nullopt;
		}
		char* end = nullptr;
		const int64_t value = std::strtoll(typed.c_str(), &end, 10);
		if (!typed.empty() && end != nullptr && *end == 0)
			return value;
		// A pitch can be a name, such as C3.
		for (int pitch = 0; pitch < 128; ++pitch)
			if (engine::midi::pitchName(pitch) == typed)
				return pitch;
		return std::nullopt;
	}

	std::optional<double> ListEditor::bar(const Row& row)
	{
		if (row.type == "sysEx" || row.type == "articulation")
			return std::nullopt;
		if (row.type == "program")
			return row.number / g_maxVelocity;
		return row.value;
	}

	void ListEditor::render()
	{
		Rml::Element* root = m_root.get();
		if (root == nullptr)
			return;
		root->SetAttribute("tool", m_tool);
		Rml::ElementList buttons;
		root->QuerySelectorAll(buttons, ".list-tool");
		for (Rml::Element* button : buttons)
			setClass(button, "checked", attribute(*button, "list-tool") == m_tool);
		buttons.clear();
		root->QuerySelectorAll(buttons, ".list-filter");
		for (Rml::Element* button : buttons)
			setClass(button, "checked", m_hidden.count(attribute(*button, "event-type")) != 0);
		const std::string length = m_view.value("lengthQuantize", std::string("link"));
		const auto label = [](Rml::Element* element, const std::string& text)
		{
			if (element != nullptr && element->GetInnerRML() != text)
				element->SetInnerRML(text);
		};
		label(root->QuerySelector("#list-insert-type"), typeLabel(m_insertType));
		label(root->QuerySelector("#list-length-quantize"), length == "link" ? "Quantize Link" : length);
		for (const auto& [focus, name] : g_focuses)
			if (m_focus == focus)
				label(root->QuerySelector("#list-focus"), name);
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (Rml::Element* element = menu.get())
			{
				Rml::ElementList items;
				element->QuerySelectorAll(items, "menuitem");
				for (Rml::Element* item : items)
					setChecked(*item,
						attribute(*item, "insert-type") == m_insertType || attribute(*item, "length") == length ||
							attribute(*item, "focus") == m_focus);
			}
		setAttribute(root->QuerySelector("#list-insert-velocity"), "value",
			std::to_string(std::lround(m_view.value("insertVelocity", 100.0 / g_maxVelocity) * g_maxVelocity)));

		// The rows only change when events come, go or move; the values go into the elements there. The selection
		// only changes classes, so that the second click of a double click that edits a value finds its element.
		const std::vector<const Row*> selection = selectedRows();
		const Row* first = selection.empty() ? nullptr : selection.front();
		std::string rml;
		for (const Row& row : m_rows)
		{
			if (!shown(row, first))
				continue;
			rml += R"(<div class="list-row)" + std::string(row.editable ? "" : " inactive") + R"(" part=")" +
				std::to_string(row.part) + (row.isNote() ? R"(" note=")" : R"(" controller=")") +
				std::to_string(row.id) + R"(" type=")" + row.type + R"("><span class="list-type">)" +
				typeLabel(row.type) + "</span>";
			for (const auto& [field, name] : g_fields)
			{
				const bool editable = row.editable &&
					(number(row, field) ||
						(field == std::string("attributes") &&
							(row.type == "sysEx" || row.type == "articulation" || row.isNote())));
				rml += std::string(R"(<textdisplay class="list-cell" field=")") + field + '"' +
					(editable ? "" : R"( disabled="")") + "/>";
			}
			rml += R"(<div class="list-event-lane"><div class="list-event"/></div><div class="list-value-lane">)" +
				std::string(bar(row) ? R"(<div class="list-value"/>)" : "") + "</div></div>";
		}
		Rml::Element* rows = root->QuerySelector(".list-editor-rows");
		if (rows == nullptr)
			return;
		if (rml != m_rowsRml)
		{
			rows->SetInnerRML(rml);
			m_rowsRml = rml;
		}
		const auto range = static_cast<double>(m_to - m_from);
		for (int i = 0; i < rows->GetNumChildren(); ++i)
		{
			Rml::Element* element = rows->GetChild(i);
			const Row* row = rowOf(element);
			if (row == nullptr)
				continue;
			setClass(element, "selected", selected(*row));
			for (const auto& [field, name] : g_fields)
				setAttribute(element->QuerySelector(std::string("textdisplay[field=") + field + "]"), "value",
					text(*row, field));
			if (Rml::Element* event = element->QuerySelector(".list-event"))
			{
				const double left = static_cast<double>(row->start - m_from) / range * 100.0;
				const double width = static_cast<double>(row->length) / range * 100.0;
				event->SetProperty(Rml::PropertyId::Left, percent(left));
				if (row->isNote())
					event->SetProperty(Rml::PropertyId::Width, percent(width));
			}
			if (Rml::Element* value = element->QuerySelector(".list-value"))
			{
				const auto changed = m_values.find(row->key());
				const double width = changed != m_values.end() ? changed->second : bar(*row).value_or(0.0);
				value->SetProperty(Rml::PropertyId::Width, percent(width * 100.0));
			}
		}
		if (Rml::Context* context = root->GetContext())
			context->RequestNextUpdate(0);
	}

	const ListEditor::Row* ListEditor::rowOf(Rml::Element* element) const
	{
		const Rml::Element* found = closest(element, "list-row", m_root.get());
		if (found == nullptr)
			return nullptr;
		const bool note = found->HasAttribute("note");
		const uint64_t part = idAttribute(*found, "part");
		const uint64_t id = idAttribute(*found, note ? "note" : "controller");
		for (const Row& row : m_rows)
			if (row.part == part && row.id == id && row.isNote() == note)
				return &row;
		return nullptr;
	}

	bool ListEditor::selected(const Row& row) const
	{
		return m_selection.count(row.key()) != 0;
	}

	std::vector<const ListEditor::Row*> ListEditor::selectedRows() const
	{
		std::vector<const Row*> rows;
		for (const Row& row : m_rows)
			if (selected(row))
				rows.push_back(&row);
		return rows;
	}

	double ListEditor::ticksAt(const Rml::Event& event, Rml::Element& lane) const
	{
		return ticksAt(event, lane.GetAbsoluteOffset(Rml::BoxArea::Border).x,
			std::max(lane.GetBox().GetSize(Rml::BoxArea::Border).x, 1.0F));
	}

	double ListEditor::ticksAt(const Rml::Event& event, float left, float width) const
	{
		const float x = (event.GetParameter("mouse_x", 0.0F) - left) / width;
		return static_cast<double>(m_from) + (x * static_cast<double>(m_to - m_from));
	}

	int64_t ListEditor::grid() const
	{
		const std::string grid = m_project.value("quantize", Json::object()).value("grid", std::string("1/16"));
		return engine::midi::noteValueFromString(grid).value_or(engine::midi::NoteValue{}).ticks();
	}

	int64_t ListEditor::snap(double ticks) const
	{
		const auto step = static_cast<double>(grid());
		return std::llround(std::round(ticks / step) * step);
	}

	int64_t ListEditor::lengthQuantize() const
	{
		const std::string length = m_view.value("lengthQuantize", std::string("link"));
		if (length == "link")
			return grid();
		return engine::midi::noteValueFromString(length).value_or(engine::midi::NoteValue{}).ticks();
	}

	const ListEditor::Part* ListEditor::partAt(int64_t ticks) const
	{
		const Part* found = nullptr;
		for (const Part& part : m_parts)
			if (part.editable && ticks >= part.start && ticks < part.end &&
				(found == nullptr || Json(part.id) == m_view.at("activePart")))
				found = &part;
		return found;
	}

	void ListEditor::select(std::set<std::pair<uint64_t, uint64_t>> rows, bool show)
	{
		m_selection = std::move(rows);
		Json notes = Json::array();
		Json controllers = Json::array();
		for (const Row& row : m_rows)
			if (selected(row))
			{
				if (row.isNote())
					notes.push_back({{"part", row.part}, {"note", row.id}});
				else
					controllers.push_back({{"part", row.part}, {"controller", row.id}});
			}
		if (show)
			render(); // at once; editor.changed confirms it
		setView({{"selection", notes}, {"controllerSelection", controllers}});
	}

	void ListEditor::setView(Json parameters)
	{
		parameters["editor"] = m_name;
		m_binding.send("editor.set", std::move(parameters));
	}

	// Sends the changes, each part's note.set or controller.set: one alone, several in a history.transaction.
	void ListEditor::edit(const std::string& description, std::map<std::pair<uint64_t, std::string>, Json> changes)
	{
		Json commands = Json::array();
		for (auto& [key, items] : changes)
			commands.push_back({{"command", key.second},
				{"parameters", {{"part", key.first}, {key.second == "note.set" ? "notes" : "controllers", items}}}});
		if (commands.size() == 1)
			m_binding.send(commands[0].at("command").get<std::string>(), commands[0].at("parameters"));
		else if (!commands.empty())
			m_binding.send("history.transaction", {{"description", description}, {"commands", std::move(commands)}});
	}

	// A value typed into the event list (p. 1254): for a selected event, every selected event that has the field
	// changes by as much (the subtype and the values only those of its type), with Ctrl (Cmd) to the same value.
	void ListEditor::editField(const Row& edited, const std::string& field, const std::string& typed)
	{
		if (field == "attributes")
		{
			std::map<std::pair<uint64_t, std::string>, Json> changes;
			if (const std::optional<std::vector<int>> data = parseHex(typed); data && edited.type == "sysEx")
			{
				changes[{edited.part, "controller.set"}].push_back({{"id", edited.id}, {"data", *data}});
				edit("Change SysEx", std::move(changes));
			}
			else if (edited.isNote())
			{
				// The note's attributes, names separated by commas; empty clears them (p. 1291).
				changes[{edited.part, "note.set"}].push_back(
					{{"id", edited.id}, {"articulations", engine::midi::splitNames(typed)}});
				edit("Change Articulations", std::move(changes));
			}
			else if (edited.type == "articulation")
			{
				// A direction's name, as the attributes column shows it.
				std::string name = typed;
				if (const std::string reset = " (Reset)";
					name.size() > reset.size() && name.compare(name.size() - reset.size(), reset.size(), reset) == 0)
					name.resize(name.size() - reset.size());
				if (!name.empty())
				{
					changes[{edited.part, "controller.set"}].push_back({{"id", edited.id}, {"articulation", name}});
					edit("Change Articulation", std::move(changes));
				}
			}
			return;
		}
		const std::optional<int64_t> typedValue = parse(field, typed, edited);
		const std::optional<int64_t> was = number(edited, field);
		if (!typedValue || !was)
			return;
		std::vector<const Row*> rows = {&edited};
		if (selected(edited))
		{
			rows.clear();
			const bool sameType = field == "subtype" || field == "value" || field == "secondary";
			for (const Row* row : selectedRows())
				if (row->editable && number(*row, field) && (!sameType || row->type == edited.type))
					rows.push_back(row);
		}
		std::map<std::pair<uint64_t, std::string>, Json> changes;
		for (const Row* row : rows)
		{
			const std::optional<int64_t> current = number(*row, field);
			if (!current)
				continue;
			const int64_t value = m_ctrl ? *typedValue : *current + (*typedValue - *was);
			Json change = {{"id", row->id}};
			const std::string& type = row->type;
			if (field == "start")
				change["position"] = std::max<int64_t>(value - row->partStart, 0);
			else if (field == "end")
				change["length"] = std::max<int64_t>(value - row->start, 1);
			else if (field == "length")
				change["length"] = std::max<int64_t>(value, 1);
			else if (field == "channel")
				change["channel"] = std::clamp<int64_t>(value - 1, 0, 15);
			else if (field == "secondary")
				change["releaseVelocity"] = static_cast<double>(std::clamp<int64_t>(value, 0, 127)) / g_maxVelocity;
			else if (field == "value" && type == "note")
				change["velocity"] = static_cast<double>(std::clamp<int64_t>(value, 0, 127)) / g_maxVelocity;
			else if (field == "value" || type == "channelPressure")
				change["value"] = fromMidi1(value);
			else if (type == "pitchBend")
				change["value"] = fromBend(value);
			else if (type == "note")
				change["pitch"] = std::clamp<int64_t>(value, 0, 127);
			else
				change["number"] = std::clamp<int64_t>(value, 0, 127);
			changes[{row->part, row->isNote() ? "note.set" : "controller.set"}].push_back(change);
		}
		edit("Change events", std::move(changes));
	}

	// Draws an event of the Insert Event Type (p. 1254): a note of the length quantize's length and the insert
	// velocity on C3, the others with a middle value.
	void ListEditor::add(int64_t start, int64_t length)
	{
		const Part* part = partAt(start);
		if (part == nullptr)
			return;
		const int64_t position = start - part->start;
		if (m_insertType == "note")
		{
			m_binding.send("note.add",
				{{"part", part->id},
					{"notes",
						Json::array({{{"position", position}, {"length", std::max<int64_t>(length, 1)}, {"pitch", 60},
							{"velocity", m_view.value("insertVelocity", 100.0 / g_maxVelocity)}}})}});
			return;
		}
		Json controller = {{"position", position}, {"type", m_insertType}};
		if (m_insertType == "sysEx")
			controller["data"] = {0xF0, 0xF7};
		else if (m_insertType == "program")
			controller["number"] = 0;
		else if (m_insertType == "pitchBend")
			controller["value"] = 0.5;
		else
			controller["value"] = fromMidi1(64);
		if (m_insertType == "controller")
			controller["number"] = 1;
		else if (m_insertType == "polyPressure")
			controller["number"] = 60;
		m_binding.send("controller.add", {{"part", part->id}, {"controllers", Json::array({controller})}});
	}

	void ListEditor::erase(const Row& row)
	{
		if (row.isNote())
			m_binding.send("note.remove", {{"part", row.part}, {"notes", Json::array({row.id})}});
		else
			m_binding.send("controller.remove", {{"part", row.part}, {"controllers", Json::array({row.id})}});
	}

	void ListEditor::click(Rml::Element& target, bool ctrl)
	{
		if (target.HasAttribute("list-tool"))
		{
			m_tool = attribute(target, "list-tool");
		}
		else if (target.HasAttribute("insert-type"))
		{
			m_insertType = attribute(target, "insert-type");
		}
		else if (target.HasAttribute("focus"))
		{
			m_focus = attribute(target, "focus");
		}
		else if (target.HasAttribute("length"))
		{
			setView({{"lengthQuantize", attribute(target, "length")}});
			return;
		}
		else if (target.HasAttribute("event-type"))
		{
			// A click hides or shows the type; with Ctrl (Cmd) it shows that type alone, and again all (p. 1254).
			const std::string type = attribute(target, "event-type");
			if (ctrl)
			{
				std::set<std::string> others;
				for (const auto& [each, label] : g_types)
					if (type != each)
						others.insert(each);
				m_hidden = m_hidden == others ? std::set<std::string>() : others;
			}
			else if (m_hidden.erase(type) == 0)
			{
				m_hidden.insert(type);
			}
		}
		else
		{
			return;
		}
		render();
	}

	void ListEditor::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr || m_view.empty())
			return;
		const bool ctrl = event.GetParameter("ctrl_key", 0) != 0 || event.GetParameter("meta_key", 0) != 0;
		switch (event.GetId())
		{
		case Rml::EventId::Keydown:
			m_ctrl = ctrl;
			break;
		case Rml::EventId::Mousedown:
			if (event.GetParameter("button", 0) != 0)
				break;
			if (event.GetCurrentElement() == m_document.get())
			{
				// A left press anywhere in the document while a gesture goes on: the gesture's mouseup never came, as
				// when the mouse went up outside the window. It ends without its effect before anything hears the
				// press. Only the bars change, so render() keeps the rows' elements, the press's target among them.
				if (cancel())
					render();
				break;
			}
			press(event);
			break;
		case Rml::EventId::Mousemove:
			drag(event);
			break;
		case Rml::EventId::Mouseup:
			if (event.GetParameter("button", 0) == 0)
				release(event);
			break;
		case Rml::EventId::Dragend:
			// After the mouseup, if there was one: a gesture that it ended is over.
			if (m_gesture != Gesture::None)
				release(event);
			break;
		case Rml::EventId::Click:
			click(*target, ctrl);
			break;
		case Rml::EventId::Change:
			if (target->GetId() == "list-insert-velocity")
				setView({{"insertVelocity",
					std::clamp(event.GetParameter<float>("value", 100.0F) / g_maxVelocity, 0.0, 1.0)}});
			else if (const Row* row = rowOf(target); row != nullptr && target->HasAttribute("field"))
				editField(*row, attribute(*target, "field"), event.GetParameter<Rml::String>("value", ""));
			break;
		default:
			break;
		}
	}

	void ListEditor::press(Rml::Event& event)
	{
		// The document's mousedown in the capture phase (ProcessEvent) has ended a gesture whose end never came.
		begin(event);
		if (m_gesture == Gesture::None)
			return;
		listen(true);
		dragBody(true);
	}

	bool ListEditor::cancel()
	{
		listen(false);
		dragBody(false);
		m_gesture = Gesture::None;
		const bool shown = !m_values.empty();
		m_values.clear();
		return shown;
	}

	void ListEditor::listen(bool document)
	{
		if (Rml::Element* listened = m_document.get())
		{
			listened->RemoveEventListener(Rml::EventId::Mouseup, this);
			listened->RemoveEventListener(Rml::EventId::Mousedown, this, true);
		}
		m_document.reset();
		if (const Rml::Element* root = m_root.get(); document && root != nullptr)
			if (Rml::ElementDocument* owner = root->GetOwnerDocument())
			{
				owner->AddEventListener(Rml::EventId::Mouseup, this);
				// In the capture phase: before the press reaches the root or anything else in the document.
				owner->AddEventListener(Rml::EventId::Mousedown, this, true);
				m_document = owner->GetObserverPtr();
			}
	}

	void ListEditor::dragBody(bool on)
	{
		Rml::Element* root = m_root.get();
		Rml::Element* body = root == nullptr ? nullptr : root->QuerySelector(".list-editor-body");
		if (body == nullptr)
			return;
		body->SetClass("in-gesture", on);
		// RmlUi looks for the drag element right after a press's mousedown, from the computed values: the press that
		// starts a gesture needs them with the class, the press that ends a lost one, or comes in the same frame as a
		// mouseup, without it.
		if (Rml::ElementDocument* owner = root->GetOwnerDocument())
			owner->UpdateDocument();
	}

	void ListEditor::begin(Rml::Event& event)
	{
		m_gesture = Gesture::None;
		const Rml::Element* root = m_root.get();
		Rml::Element* target = event.GetTargetElement();
		if (closest(target, "list-editor-body", root) == nullptr)
			return;
		const bool modified = event.GetParameter("shift_key", 0) != 0 || event.GetParameter("ctrl_key", 0) != 0 ||
			event.GetParameter("meta_key", 0) != 0;
		const Row* row = rowOf(target);
		if (row != nullptr && !row->editable)
			row = nullptr;
		m_pressed.reset();
		if (row != nullptr)
			m_pressed = *row;
		Rml::Element* lane = closest(target, "list-event-lane", root);
		if (m_tool == "draw")
		{
			// Draws in the event display: a click a note of the length quantize's length, a drag a multiple of it.
			if (lane != nullptr)
			{
				m_pressedAt = static_cast<double>(snap(ticksAt(event, *lane) - (static_cast<double>(grid()) / 2.0)));
				m_gesture = Gesture::Draw;
			}
			return;
		}
		if (row == nullptr)
		{
			if (closest(target, "list-filler", root) != nullptr && !modified)
				select({});
			return;
		}
		if (m_tool == "erase")
		{
			erase(*row);
			return;
		}
		if (const Rml::Element* values = closest(target, "list-value-lane", root); values != nullptr && bar(*row))
		{
			// A drag in the value display sets each event's value it crosses (p. 1255).
			m_gesture = Gesture::Values;
			m_values.clear();
			drag(event);
			return;
		}
		if (modified)
		{
			std::set<std::pair<uint64_t, uint64_t>> rows = m_selection;
			if (rows.erase(row->key()) == 0)
				rows.insert(row->key());
			select(std::move(rows));
			return;
		}
		// Decides the move before selecting: select() renders, and a Focus filter can then rebuild the rows, which
		// destroys target and lane.
		if (lane != nullptr && closest(target, "list-event", root) != nullptr)
		{
			m_gesture = Gesture::Move;
			m_laneLeft = lane->GetAbsoluteOffset(Rml::BoxArea::Border).x;
			m_laneWidth = std::max(lane->GetBox().GetSize(Rml::BoxArea::Border).x, 1.0F);
			m_pressedAt = ticksAt(event, m_laneLeft, m_laneWidth);
		}
		// With a Focus filter a move's rows wait for editor.changed: rebuilt under the press, they would take the
		// pressed element from under the mouse, and RmlUi would start no drag, so no dragend past the window's edge.
		if (!selected(*row))
			select({row->key()}, m_gesture != Gesture::Move || m_focus == "none");
	}

	void ListEditor::drag(Rml::Event& event)
	{
		if (m_gesture != Gesture::Values)
			return;
		Rml::Element* target = event.GetTargetElement();
		const Row* row = rowOf(target);
		Rml::Element* lane = closest(target, "list-value-lane", m_root.get());
		if (row == nullptr || lane == nullptr || !row->editable || !bar(*row))
			return;
		const Rml::Vector2f origin = lane->GetAbsoluteOffset(Rml::BoxArea::Border);
		const float width = std::max(lane->GetBox().GetSize(Rml::BoxArea::Border).x, 1.0F);
		m_values[row->key()] = std::clamp((event.GetParameter("mouse_x", 0.0F) - origin.x) / width, 0.0F, 1.0F);
		render();
	}

	void ListEditor::release(Rml::Event& event)
	{
		listen(false);
		dragBody(false);
		const Gesture gesture = std::exchange(m_gesture, Gesture::None);
		const Rml::Element* root = m_root.get();
		if (gesture == Gesture::Draw)
		{
			int64_t length = lengthQuantize();
			if (Rml::Element* lane = closest(event.GetTargetElement(), "list-event-lane", root))
				length *= std::max<int64_t>(1,
					static_cast<int64_t>(
						std::ceil((ticksAt(event, *lane) - m_pressedAt) / static_cast<double>(length))));
			add(std::llround(m_pressedAt), length);
		}
		else if (gesture == Gesture::Values)
		{
			std::map<std::pair<uint64_t, std::string>, Json> changes;
			for (const Row& row : m_rows)
			{
				const auto found = m_values.find(row.key());
				if (found == m_values.end())
					continue;
				Json change = {{"id", row.id}};
				if (row.isNote())
					change["velocity"] = std::max(std::round(found->second * g_maxVelocity), 1.0) / g_maxVelocity;
				else if (row.type == "program")
					change["number"] = std::lround(found->second * g_maxVelocity);
				else
					change["value"] = found->second;
				changes[{row.part, row.isNote() ? "note.set" : "controller.set"}].push_back(change);
			}
			m_values.clear();
			edit("Change values", std::move(changes));
		}
		else if (gesture == Gesture::Move && m_pressed)
		{
			// Moves the selected events by as far as the pressed one went, snapped (p. 1255).
			const int64_t delta =
				snap(static_cast<double>(m_pressed->start) + ticksAt(event, m_laneLeft, m_laneWidth) - m_pressedAt) -
				m_pressed->start;
			if (delta == 0)
				return;
			std::map<std::pair<uint64_t, std::string>, Json> changes;
			for (const Row* row : selectedRows())
			{
				if (!row->editable)
					continue;
				if (row->start + delta < row->partStart)
					return; // nothing moves before its part
				changes[{row->part, row->isNote() ? "note.set" : "controller.set"}].push_back(
					{{"id", row->id}, {"position", row->start + delta - row->partStart}});
			}
			edit("Move", std::move(changes));
		}
	}
}
