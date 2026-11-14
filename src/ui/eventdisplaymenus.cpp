// SPDX-License-Identifier: GPL-3.0-or-later
// The event display's menus, buttons and dialogs in its document (ui/eventdisplay.h): the ruler's time format menu,
// the horizontal zoom presets menu with its dialogs, the auto-scroll button with its settings menu, and the Repeat
// Events dialog.
#include "ui/eventdisplay.h"

#include "engine/time/tempomap.h"
#include "platform/log.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// The checked attribute gives a widget the state class .checked.
		void check(Rml::Element& element, bool on)
		{
			if (on)
				element.SetAttribute("checked", "");
			else
				element.RemoveAttribute("checked");
		}

		// The element or its nearest ancestor with the tag, or null.
		Rml::Element* closest(Rml::Element* element, const std::string& tag)
		{
			while (element != nullptr && element->GetTagName() != tag)
				element = element->GetParentNode();
			return element;
		}

		// The result that a click on the element closes a dialog with: its own, or its nearest ancestor's.
		std::string resultOf(Rml::Element* element)
		{
			while (element != nullptr && !element->HasAttribute("result"))
				element = element->GetParentNode();
			return element != nullptr && !element->HasAttribute("disabled")
				? element->GetAttribute<Rml::String>("result", "")
				: std::string();
		}

		std::string valueOf(Rml::Element* field)
		{
			auto* control = dynamic_cast<Rml::ElementFormControl*>(field);
			return control != nullptr ? control->GetValue() : std::string();
		}

		// An element for a menu, with the text and the attribute that says what it stands for.
		Rml::ElementPtr menuElement(Rml::Element& menu, const char* tag, const char* attribute,
			const std::string& value, const std::string& text)
		{
			Rml::ElementPtr element = menu.GetOwnerDocument()->CreateElement(tag);
			element->SetAttribute(attribute, value);
			element->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
			return element;
		}

		struct ToolInfo
		{
			Tool tool;
			const char* name;
			const char* title;
			bool crossHair; // whether the tool uses the cross-hair cursor (reference manual p. 110)
		};

		constexpr std::array<ToolInfo, 12> g_tools{{
			{Tool::ObjectSelection, "objectSelection", "Object Selection", true},
			{Tool::RangeSelection, "rangeSelection", "Range Selection", true},
			{Tool::Comp, "comp", "Comp", false},
			{Tool::Draw, "draw", "Draw", true},
			{Tool::Erase, "erase", "Erase", false},
			{Tool::Split, "split", "Split", true},
			{Tool::Glue, "glue", "Glue", false},
			{Tool::Mute, "mute", "Mute", false},
			{Tool::Zoom, "zoom", "Zoom", false},
			{Tool::Line, "line", "Line", true},
			{Tool::Play, "play", "Play", false},
			{Tool::TimeWarp, "timeWarp", "Time Warp", false},
		}};

		constexpr std::array<const char*, 5> g_lineModes = {"line", "parabola", "sine", "triangle", "square"};
		constexpr std::array<const char*, 2> g_playModes = {"play", "scrub"};
		constexpr std::array<const char*, 3> g_warpModes = {"warpGrid", "warpGridFollow", "freeWarp"};
	}

	const char* toolName(Tool tool)
	{
		return g_tools.at(static_cast<size_t>(tool)).name;
	}

	const char* toolTitle(Tool tool)
	{
		return g_tools.at(static_cast<size_t>(tool)).title;
	}

	std::optional<Tool> toolFromName(const std::string& name)
	{
		for (const ToolInfo& info : g_tools)
			if (name == info.name)
				return info.tool;
		return std::nullopt;
	}

	void EventDisplay::findControls()
	{
		m_formatMenu = m_document->GetElementById("ruler-format-menu");
		m_presetsMenu = m_document->GetElementById("zoom-presets-menu");
		m_autoScrollButton = m_document->GetElementById("auto-scroll");
		m_autoScrollMenu = m_document->GetElementById("auto-scroll-menu");
		m_snapButton = m_document->GetElementById("snap");
		m_snapTypeButton = m_document->GetElementById("snap-type");
		m_snapTypeMenu = m_document->GetElementById("snap-type-menu");
		m_gridTypeButton = m_document->GetElementById("grid-type");
		m_gridTypeMenu = m_document->GetElementById("grid-type-menu");
		m_zeroCrossingButton = m_document->GetElementById("snap-zero-crossing");
		m_rootKeyButton = m_document->GetElementById("root-key");
		m_rootKeyMenu = m_document->GetElementById("root-key-menu");
		Rml::ElementList buttons;
		m_document->QuerySelectorAll(buttons, "button[tool], menuitem[tool]");
		m_toolButtons.assign(buttons.begin(), buttons.end());
		m_combineButton = m_document->GetElementById("combine-selection-tools");
		m_objectMenu = m_document->GetElementById("object-selection-menu");
		m_lineMenu = m_document->GetElementById("line-menu");
		m_playMenu = m_document->GetElementById("play-menu");
		m_timeWarpMenu = m_document->GetElementById("time-warp-menu");
		m_toolbox = m_document->GetElementById("toolbox");
		m_contextMenu = m_document->GetElementById("event-display-menu");
		m_notification = m_document->GetElementById("tool-notification");
		m_refusal = m_document->GetElementById("edit-refused");
		if (m_formatMenu != nullptr)
			m_ruler->SetAttribute("contextmenu", m_formatMenu->GetId()); // a right click in the ruler opens it
		showContextMenus();
	}

	// The cycle markers of the active marker track, for the zoom presets menu.
	void EventDisplay::requestMarkers()
	{
		m_binding.send("marker.list", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_cycleMarkers.clear();
				for (const Json& marker : result.value.at("markers"))
				{
					if (marker.at("type") != "cycle")
						continue;
					std::string name = marker.at("name").get<std::string>();
					if (name.empty())
						name = "Cycle Marker " + std::to_string(marker.at("number").get<int64_t>());
					m_cycleMarkers.push_back({std::move(name), marker.at("positionSamples").get<int64_t>(),
						marker.at("endSamples").get<int64_t>()});
				}
				showPresets();
			});
	}

	// What attracts with snap to events besides the events (reference manual p. 106: markers are events on the marker
	// tracks): every marker track's markers, the active one's or not, as in snap.position. Each marker's position, and
	// a cycle marker's end, in project samples as the engine converts them, from the same project.get answer as the
	// events, so that they never come from different revisions.
	void EventDisplay::readMarkerMagnets(const Json& project)
	{
		m_markers.clear();
		std::map<uint64_t, time::TimeBase> bases; // each track's
		for (const Json& track : project.at("tracks"))
			bases[track.at("id").get<uint64_t>()] =
				track.at("timeBase") == "linear" ? time::TimeBase::Linear : time::TimeBase::Musical;
		for (const Json& marker : project.at("markers"))
		{
			const time::TimeBase base = bases[marker.at("track").get<uint64_t>()];
			const auto magnet = [this, base](const Json& position)
			{ return time::toSamples({base, position.get<int64_t>()}, m_time.tempo, m_time.sampleRate); };
			m_markers.push_back(magnet(marker.at("position")));
			if (!marker.at("end").is_null()) // position markers have no end
				m_markers.push_back(magnet(marker.at("end")));
		}
	}

	void EventDisplay::click(Rml::Element* target, bool control)
	{
		// A tool button chooses its tool; once it has, a second click opens its menu (showTools). So do the tools in
		// the toolbox and the context menu.
		if (const Rml::Element* button = closest(target, "button"); button != nullptr && button->HasAttribute("tool"))
		{
			if (const std::optional<Tool> tool = toolFromName(button->GetAttribute<Rml::String>("tool", "")))
				setTool(*tool);
			return;
		}
		const Rml::Element* item = closest(target, "menuitem");
		if (item == nullptr || item->HasAttribute("disabled"))
			return;
		if (item->HasAttribute("tool"))
		{
			if (const std::optional<Tool> tool = toolFromName(item->GetAttribute<Rml::String>("tool", "")))
				setTool(*tool);
			return;
		}
		const Rml::Element* menu = item->GetParentNode();
		const std::string& id = item->GetId();
		if (menu == m_formatMenu)
		{
			// With Ctrl/Cmd, the format is the primary time format too, for all windows (reference manual p. 69).
			if (const auto format = timeFormatFromName(item->GetAttribute<Rml::String>("format", "")))
			{
				setRulerFormat(*format);
				if (control)
					m_binding.send("project.setTimeFormat", {{"timeFormat", timeFormatName(*format)}});
			}
		}
		else if (menu == m_presetsMenu)
		{
			const int marker = item->GetAttribute<int>("cycle-marker", -1);
			if (item->HasAttribute("preset"))
				applyZoomPreset(item->GetAttribute<Rml::String>("preset", ""));
			else if (marker >= 0 && marker < static_cast<int>(m_cycleMarkers.size()))
				zoomTo(static_cast<double>(m_cycleMarkers[static_cast<size_t>(marker)].start) / m_time.sampleRate,
					static_cast<double>(m_cycleMarkers[static_cast<size_t>(marker)].end) / m_time.sampleRate);
			else if (id == "zoom-full")
				zoomFull();
			else if (id == "zoom-preset-add")
				openDialog("zoom-preset.rml");
			else if (id == "zoom-presets-organize")
				openDialog("zoom-presets.rml");
		}
		else if (menu == m_rootKeyMenu)
		{
			m_binding.send("project.set", {{"rootKey", item->GetAttribute<Rml::String>("root-key", "")}});
		}
		else if (menu == m_snapTypeMenu)
		{
			setSnap({{"type", item->GetAttribute<Rml::String>("snap-type", "")}});
		}
		else if (menu == m_gridTypeMenu)
		{
			// In the format the menu shows, which may not be the engine's yet.
			setSnap({{"grid", item->GetAttribute<Rml::String>("grid-type", "")}, {"format", timeFormatName(m_format)}});
		}
		else if (menu == m_objectMenu)
		{
			if (item->HasAttribute("sizing"))
				setSizingMovesContents(item->GetAttribute<Rml::String>("sizing", "") == "moveContents");
		}
		else if (menu == m_lineMenu)
		{
			const auto mode = item->GetAttribute<Rml::String>("line-mode", "");
			for (size_t i = 0; i < g_lineModes.size(); ++i)
				if (mode == g_lineModes[i])
					setLineMode(static_cast<LineMode>(i));
		}
		else if (menu == m_playMenu)
		{
			setPlayMode(item->GetAttribute<Rml::String>("play-mode", "") == "scrub" ? PlayMode::Scrub : PlayMode::Play);
		}
		else if (menu == m_timeWarpMenu)
		{
			const auto mode = item->GetAttribute<Rml::String>("warp-mode", "");
			for (size_t i = 0; i < g_warpModes.size(); ++i)
				if (mode == g_warpModes[i])
				{
					setTool(Tool::TimeWarp);
					setWarpMode(static_cast<WarpMode>(i));
				}
		}
		else if (menu == m_autoScrollMenu)
		{
			if (id == "page-scroll")
				setAutoScrollMode(AutoScrollMode::Page);
			else if (id == "stationary-cursor")
				setAutoScrollMode(AutoScrollMode::Stationary);
			else if (id == "suspend-auto-scroll")
				setSuspendAutoScrollWhenEditing(!m_suspendWhenEditing);
		}
	}

	// Opens a dialog from the folder of the display's document, modal.
	void EventDisplay::openDialog(const char* file)
	{
		if (m_dialog)
			return;
		const std::string path = std::filesystem::u8path(m_root->GetOwnerDocument()->GetSourceURL())
									 .replace_filename(file)
									 .generic_u8string();
		Rml::ElementDocument* dialog = m_root->GetContext()->LoadDocument(path);
		if (dialog == nullptr)
		{
			logging::warning("The event display can't open the dialog {}", path);
			return;
		}
		if (Rml::Element* list = dialog->GetElementById("zoom-presets-list"))
		{
			// The organize dialog: a row per preset, with its name to edit and a button that deletes it.
			m_organized = m_presets;
			std::string rows;
			for (size_t i = 0; i < m_organized.size(); ++i)
				rows += R"(<div class="preset" index=")" + std::to_string(i) + R"("><input type="text" value=")" +
					Rml::StringUtilities::EncodeRml(m_organized[i].at("name").get<std::string>()) +
					R"("/><button>Delete</button></div>)";
			list->SetInnerRML(rows);
		}
		dialog->AddEventListener(Rml::EventId::Click, this);
		dialog->Show(Rml::ModalFlag::Modal);
		m_dialog = dialog->GetObserverPtr();
	}

	// A click in a dialog, before a button with a result closes it: OK, also when Enter clicks it, takes what the
	// dialog says.
	void EventDisplay::dialogEvent(Rml::Event& event)
	{
		Rml::Element* dialog = event.GetCurrentElement();
		Rml::Element* target = event.GetTargetElement();
		if (dialog->GetElementById("tempo-detection-dialog") != nullptr)
		{
			if (const Rml::Element* button = closest(target, "button"); button != nullptr && !button->GetId().empty())
				detectTempo(button->GetId());
			return;
		}
		if (resultOf(target) != "ok")
		{
			// A row's Delete button marks the row deleted, for the dialog's style sheet to hide it.
			if (const Rml::Element* button = closest(target, "button"))
				if (Rml::Element* row = button->GetParentNode(); row != nullptr && row->IsClassSet("preset"))
					row->SetClass("deleted", true);
			return;
		}
		if (const Rml::Element* count = dialog->GetElementById("repeat-count"))
		{
			const Rml::Element* shared = dialog->GetElementById("repeat-shared");
			repeat(static_cast<int>(std::lround(count->GetAttribute<float>("value", 1.0F))),
				shared != nullptr && shared->HasAttribute("checked"));
		}
		else if (Rml::Element* distance = dialog->GetElementById("calculate-tempo-distance"))
		{
			const Rml::Element* barStart = dialog->GetElementById("calculate-tempo-bar-start");
			const std::vector<uint64_t> parts = selectedMidi();
			if (!parts.empty())
				edit("tempo.fromMidi",
					{{"parts", parts}, {"distance", std::stoll(valueOf(distance))},
						{"beginAtBarStart", barStart != nullptr && barStart->HasAttribute("checked")}});
		}
		else if (dialog->GetElementById("import-copy") != nullptr)
		{
			importAudio(*dialog);
		}
		else if (Rml::Element* name = dialog->GetElementById("zoom-preset-name"))
		{
			if (const std::string text = valueOf(name); !text.empty())
				addZoomPreset(text);
		}
		else if (Rml::Element* list = dialog->GetElementById("zoom-presets-list"))
		{
			// The presets that weren't deleted, renamed as typed, from the presets the dialog listed.
			Json presets = Json::array();
			Rml::ElementList rows;
			list->QuerySelectorAll(rows, ".preset");
			for (Rml::Element* row : rows)
			{
				const int index = row->GetAttribute<int>("index", -1);
				if (row->IsClassSet("deleted") || index < 0 || index >= static_cast<int>(m_organized.size()))
					continue;
				Json preset = m_organized[static_cast<size_t>(index)];
				if (const std::string text = valueOf(row->QuerySelector("input")); !text.empty())
					preset["name"] = text;
				presets.push_back(std::move(preset));
			}
			m_presets = std::move(presets);
			savePresets();
		}
	}

	// The Tempo Detection dialog's buttons (reference manual pp. 1460-1461): Analyze detects the tempo afresh, the
	// factors, Offbeat Correction and Smooth Tempo detect it again with them, Reset removes what it made.
	void EventDisplay::detectTempo(const std::string& button)
	{
		TempoDetection& detection = m_detection;
		Json parameters = {{"event", detection.event}};
		if (button == "tempo-detection-reset")
		{
			detection = TempoDetection{detection.event};
			parameters["reset"] = true;
			edit("tempo.detect", std::move(parameters));
			return;
		}
		if (button == "tempo-detection-analyze")
			detection = TempoDetection{detection.event};
		else if (button == "tempo-detection-double")
			detection.factor *= 2.0;
		else if (button == "tempo-detection-halve")
			detection.factor /= 2.0;
		else if (button == "tempo-detection-four-thirds")
			detection.factor *= 4.0 / 3.0;
		else if (button == "tempo-detection-three-quarters")
			detection.factor *= 3.0 / 4.0;
		else if (button == "tempo-detection-offbeat")
			detection.offbeat = !detection.offbeat;
		else if (button == "tempo-detection-smooth")
			detection.smooth = true;
		else
			return;
		parameters["factor"] = detection.factor;
		parameters["offbeat"] = detection.offbeat;
		parameters["smooth"] = detection.smooth;
		edit("tempo.detect", std::move(parameters));
	}

	void EventDisplay::showFormat()
	{
		if (!alive() || m_formatMenu == nullptr)
			return;
		for (int i = 0; i < m_formatMenu->GetNumChildren(); ++i)
			if (Rml::Element* item = m_formatMenu->GetChild(i); item->HasAttribute("format"))
				check(*item, item->GetAttribute<Rml::String>("format", "") == timeFormatName(m_format));
	}

	// The presets before Zoom Full, the cycle markers after it.
	void EventDisplay::showPresets()
	{
		if (!alive() || m_presetsMenu == nullptr)
			return;
		for (int i = m_presetsMenu->GetNumChildren() - 1; i >= 0; --i)
			if (Rml::Element* child = m_presetsMenu->GetChild(i);
				child->HasAttribute("preset") || child->HasAttribute("cycle-marker"))
				m_presetsMenu->RemoveChild(child);
		Rml::Element* full = m_presetsMenu->QuerySelector("#zoom-full");
		if (full == nullptr)
			return;
		for (const Json& preset : m_presets)
		{
			const auto name = preset.at("name").get<std::string>();
			m_presetsMenu->InsertBefore(menuElement(*m_presetsMenu, "menuitem", "preset", name, name), full);
		}
		Rml::Element* next = full->GetNextSibling();
		if (!m_cycleMarkers.empty())
			m_presetsMenu->InsertBefore(menuElement(*m_presetsMenu, "menuseparator", "cycle-marker", "", ""), next);
		for (size_t i = 0; i < m_cycleMarkers.size(); ++i)
			m_presetsMenu->InsertBefore(
				menuElement(*m_presetsMenu, "menuitem", "cycle-marker", std::to_string(i), m_cycleMarkers[i].name),
				next);
	}

	namespace
	{
		// What the grid type menu and its button call a grid type (reference manual pp. 107-108).
		const char* gridText(engine::GridType grid)
		{
			using engine::GridType;
			switch (grid)
			{
			case GridType::Bar:
				return "Bar";
			case GridType::Beat:
				return "Beat";
			case GridType::Quantize:
				return "Use Quantize";
			case GridType::AdaptToZoom:
				return "Adapt to Zoom";
			case GridType::Ms1:
				return "1ms";
			case GridType::Ms10:
				return "10ms";
			case GridType::Ms100:
				return "100ms";
			case GridType::Ms1000:
				return "1000ms";
			case GridType::Subframe:
				return "Subframe";
			case GridType::QuarterFrame:
				return "1/4 Frame";
			case GridType::HalfFrame:
				return "1/2 Frame";
			case GridType::Frame:
				return "1 Frame";
			case GridType::TwoFrames:
				return "2 Frames";
			case GridType::Second:
				return "1 Second";
			case GridType::Samples1:
				return "1 Sample";
			case GridType::Samples10:
				return "10 Samples";
			case GridType::Samples100:
				return "100 Samples";
			case GridType::Samples44100:
				return "44100 Samples";
			}
			return engine::gridTypeName(grid);
		}

		// The grid types of the ruler's time format, from the engine's table.
		std::vector<engine::GridType> gridTypes(TimeFormat format)
		{
			const std::optional<engine::GridFormat> grid = engine::gridFormatFromName(timeFormatName(format));
			return grid ? engine::gridTypes(*grid) : std::vector<engine::GridType>();
		}
	}

	// A format sent along counts until the engine answers: settings it sent before keep the display's (takeSnap).
	void EventDisplay::setSnap(const Json& settings)
	{
		const bool format = settings.contains("format");
		if (format)
			++m_formatsSent;
		m_binding.send("snap.set", settings,
			[this, format, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				if (format)
					--m_formatsSent;
				if (!result.ok())
					logging::warning("The event display can't change the snap settings: {}", result.error);
			});
	}

	// The Project Root Key button shows the project's, its menu checks it.
	void EventDisplay::showRootKey(const Json& project)
	{
		if (m_rootKeyMenu == nullptr)
			return;
		const std::string key = project.value("rootKey", std::string());
		for (int i = 0; i < m_rootKeyMenu->GetNumChildren(); ++i)
		{
			Rml::Element* item = m_rootKeyMenu->GetChild(i);
			const bool chosen = item->GetAttribute<Rml::String>("root-key", "") == key;
			check(*item, chosen);
			if (chosen && m_rootKeyButton != nullptr && m_rootKeyButton->GetInnerRML() != item->GetInnerRML())
				m_rootKeyButton->SetInnerRML(item->GetInnerRML());
		}
	}

	// The buttons show the settings, the snap type menu checks the type, the grid type menu has the format's grid
	// types with the grid checked.
	void EventDisplay::showSnap()
	{
		if (!alive())
			return;
		if (m_snapButton != nullptr)
			check(*m_snapButton, m_snap.on);
		if (m_zeroCrossingButton != nullptr)
			check(*m_zeroCrossingButton, m_snap.zeroCrossing);
		if (m_snapTypeMenu != nullptr)
			for (int i = 0; i < m_snapTypeMenu->GetNumChildren(); ++i)
			{
				Rml::Element* item = m_snapTypeMenu->GetChild(i);
				const bool chosen = item->GetAttribute<Rml::String>("snap-type", "") == snapTypeName(m_snap.type);
				check(*item, chosen);
				if (chosen && m_snapTypeButton != nullptr)
					m_snapTypeButton->SetInnerRML(item->GetInnerRML());
			}
		if (m_gridTypeButton != nullptr)
			m_gridTypeButton->SetInnerRML(Rml::StringUtilities::EncodeRml(gridText(m_snap.grid())));
		if (m_gridTypeMenu == nullptr)
			return;
		while (m_gridTypeMenu->GetNumChildren() > 0)
			m_gridTypeMenu->RemoveChild(m_gridTypeMenu->GetChild(0));
		for (const engine::GridType grid : gridTypes(m_format))
		{
			Rml::ElementPtr item =
				menuElement(*m_gridTypeMenu, "menuitem", "grid-type", engine::gridTypeName(grid), gridText(grid));
			check(*item, grid == m_snap.grid());
			m_gridTypeMenu->AppendChild(std::move(item));
		}
	}

	// The root names the tool and shows the cross-hair cursor if the tool uses it; the tool's button is checked and
	// opens its menu (tool-menu) on the next click; the menus check the modes.
	void EventDisplay::showTools()
	{
		if (!alive())
			return;
		m_root->SetAttribute("tool", toolName(m_tool));
		m_root->SetAttribute("line-mode", g_lineModes.at(static_cast<size_t>(m_lineMode)));
		m_root->SetAttribute("play-mode", g_playModes.at(static_cast<size_t>(m_playMode)));
		m_root->SetAttribute("warp-mode", g_warpModes.at(static_cast<size_t>(m_warpMode)));
		m_root->SetClass("cross-hair", m_crossHair && g_tools.at(static_cast<size_t>(m_tool)).crossHair);
		for (Rml::Element* button : m_toolButtons)
		{
			const bool chosen = button->GetAttribute<Rml::String>("tool", "") == toolName(m_tool);
			check(*button, chosen);
			if (chosen && button->HasAttribute("tool-menu"))
				button->SetAttribute("menu", button->GetAttribute<Rml::String>("tool-menu", ""));
			else
				button->RemoveAttribute("menu");
		}
		if (m_combineButton != nullptr)
			check(*m_combineButton, m_combine);
		if (m_objectMenu != nullptr)
			for (int i = 0; i < m_objectMenu->GetNumChildren(); ++i)
				if (Rml::Element* item = m_objectMenu->GetChild(i); item->HasAttribute("sizing"))
					check(*item,
						item->GetAttribute<Rml::String>("sizing", "") ==
							(m_sizingMovesContents ? "moveContents" : "normal"));
		if (m_lineMenu != nullptr)
			for (int i = 0; i < m_lineMenu->GetNumChildren(); ++i)
				if (Rml::Element* item = m_lineMenu->GetChild(i); item->HasAttribute("line-mode"))
					check(*item,
						item->GetAttribute<Rml::String>("line-mode", "") ==
							g_lineModes.at(static_cast<size_t>(m_lineMode)));
		if (m_playMenu != nullptr)
			for (int i = 0; i < m_playMenu->GetNumChildren(); ++i)
				if (Rml::Element* item = m_playMenu->GetChild(i); item->HasAttribute("play-mode"))
					check(*item,
						item->GetAttribute<Rml::String>("play-mode", "") ==
							g_playModes.at(static_cast<size_t>(m_playMode)));
		if (m_timeWarpMenu != nullptr)
			for (int i = 0; i < m_timeWarpMenu->GetNumChildren(); ++i)
				if (Rml::Element* item = m_timeWarpMenu->GetChild(i); item->HasAttribute("warp-mode"))
					check(*item,
						item->GetAttribute<Rml::String>("warp-mode", "") ==
							g_warpModes.at(static_cast<size_t>(m_warpMode)));
	}

	void EventDisplay::showAutoScroll()
	{
		if (!alive())
			return;
		if (m_autoScrollButton != nullptr)
		{
			check(*m_autoScrollButton, m_autoScroll);
			m_autoScrollButton->SetClass("suspended", m_suspended);
		}
		if (m_autoScrollMenu == nullptr)
			return;
		const std::map<std::string, bool> items = {{"page-scroll", m_autoScrollMode == AutoScrollMode::Page},
			{"stationary-cursor", m_autoScrollMode == AutoScrollMode::Stationary},
			{"suspend-auto-scroll", m_suspendWhenEditing}};
		for (const auto& [id, on] : items)
			if (Rml::Element* item = m_autoScrollMenu->QuerySelector("#" + id))
				check(*item, on);
	}
}
