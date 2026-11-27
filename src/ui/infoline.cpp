// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/infoline.h"

#include "engine/midi/chords.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// An audio event's volume, as audioEvent.set takes it.
		constexpr double g_minVolume = -144.0;
		constexpr double g_maxVolume = 6.0;
		// A note's velocities, as the key editor's info line shows and takes them (pp. 1140, 1199, 1259).
		constexpr double g_maxVelocity = 127.0;
		constexpr int g_pitches = 128;

		// Each field is a section of the set "infoLine", which the info line's setup pane #info-line-setup shows, hides
		// and orders (p. 1575); a field the selection doesn't have is unavailable.
		const char* const g_rml =
			R"(<span class="info-empty">No Object Selected</span>)"
			R"(<div class="info-field" info-field="name" section="name" label="Name">)"
			R"(<span class="info-label">Name</span>)"
			R"(<textdisplay class="display" field="name"/></div>)"
			R"(<div class="info-field" info-field="start" section="start" label="Start">)"
			R"(<span class="info-label">Start</span>)"
			R"(<textdisplay class="display" field="start"/></div>)"
			R"(<div class="info-field" info-field="end" section="end" label="End">)"
			R"(<span class="info-label">End</span>)"
			R"(<textdisplay class="display" field="end"/></div>)"
			R"(<div class="info-field" info-field="length" section="length" label="Length">)"
			R"(<span class="info-label">Length</span>)"
			R"(<textdisplay class="display" field="length"/></div>)"
			// A selection range's top and bottom track (p. 288).
			R"(<div class="info-field" info-field="topTrack" section="topTrack" label="Top Track">)"
			R"(<span class="info-label">Top Track</span>)"
			R"(<select field="topTrack" title="Top Track"/></div>)"
			R"(<div class="info-field" info-field="bottomTrack" section="bottomTrack" label="Bottom Track">)"
			R"(<span class="info-label">Bottom Track</span>)"
			R"(<select field="bottomTrack" title="Bottom Track"/></div>)"
			// A note's pitch, off velocity and channel, which the info line shows while an in-place editor's
			// selection changed last (p. 1259), as that editor's own info line does (p. 1199).
			R"(<div class="info-field" info-field="pitch" section="pitch" label="Pitch">)"
			R"(<span class="info-label">Pitch</span>)"
			R"(<textdisplay class="display" field="pitch"/></div>)"
			R"(<div class="info-field" info-field="volume" section="volume" label="Volume">)"
			R"(<span class="info-label">Volume</span>)"
			R"(<valuefield field="volume" min="-144" max="6" default="0" step="0.5" decimals="2")"
			R"( unit="dB"/></div>)"
			R"(<div class="info-field" info-field="mute" section="mute" label="Mute">)"
			R"(<button type="toggle" field="mute")"
			R"( title="Mute">Mute</button></div>)"
			R"(<div class="info-field" info-field="lock" section="lock" label="Lock">)"
			R"(<button type="toggle" field="lock")"
			R"( title="Lock">Lock</button></div>)"
			// A MIDI part's transpose and velocity (p. 1107), added to the track's MIDI modifiers; an audio event's
			// transpose (p. 420, #1555), within the time stretcher's real-time range.
			R"(<div class="info-field" info-field="transpose" section="transpose" label="Transpose">)"
			R"(<span class="info-label">Transpose</span>)"
			R"(<valuefield field="transpose" min="-127" max="127" default="0" step="1" decimals="0"/>)"
			// What the project root key adds (p. 420), in brackets: the engine's rootKeyTranspose.
			R"(<span class="info-root-transpose" field="rootKeyTranspose"/></div>)"
			R"(<div class="info-field" info-field="velocity" section="velocity" label="Velocity">)"
			R"(<span class="info-label">Velocity</span>)"
			R"(<valuefield field="velocity" min="-127" max="127" default="0" step="1" decimals="0"/></div>)"
			R"(<div class="info-field" info-field="releaseVelocity" section="releaseVelocity" label="Off Velocity">)"
			R"(<span class="info-label">Off Velocity</span>)"
			R"(<textdisplay class="display" field="releaseVelocity"/></div>)"
			R"(<div class="info-field" info-field="channel" section="channel" label="Channel">)"
			R"(<span class="info-label">Channel</span>)"
			R"(<textdisplay class="display" field="channel"/></div>)"
			// A MIDI part's or an audio event's root key and Global Transpose (pp. 417, 421, #168, #1368).
			R"(<div class="info-field" info-field="rootKey" section="rootKey" label="Root Key">)"
			R"(<span class="info-label">Root Key</span>)"
			R"(<select field="rootKey" title="Root Key"/></div>)"
			R"(<div class="info-field" info-field="globalTranspose" section="globalTranspose" label="Global Transpose">)"
			R"(<button type="toggle" field="globalTranspose")"
			R"( title="Global Transpose: Follow">Follow</button></div>)"
			// A selected automation point's value, in its parameter's unit, and Terminator (p. 881).
			R"(<div class="info-field" info-field="value" section="value" label="Value">)"
			R"(<span class="info-label">Value</span>)"
			R"(<textdisplay class="display" field="value"/></div>)"
			R"(<div class="info-field" info-field="terminator" section="terminator" label="Terminator">)"
			R"(<span class="info-label">Terminator</span>)"
			R"(<button type="toggle" field="terminator" title="Terminator">No</button></div>)";

		// The transpose a selected event or part can have either way: a MIDI part's -127 to 127, an audio event's
		// -24 to 24, the time stretcher's real-time range (#1555).
		int limitOf(const engine::Json& event)
		{
			return event.at("kind") == "audio" ? 24 : 127;
		}

		void setClass(Rml::Element& element, const char* name, bool set)
		{
			if (element.IsClassSet(name) != set)
				element.SetClass(name, set);
		}

		void setValue(Rml::Element& element, const std::string& value)
		{
			if (element.GetAttribute<Rml::String>("value", "") != value)
				element.SetAttribute("value", value);
		}

		void setText(Rml::Element* element, const char* text)
		{
			if (element != nullptr && element->GetInnerRML() != text)
				element->SetInnerRML(text);
		}

		// A field's label, and its button in the info line's setup pane (p. 430: Description and Position for markers).
		void setLabel(Rml::Element& root, const std::string& field, const char* label)
		{
			setText(root.QuerySelector("[info-field=" + field + "] .info-label"), label);
			if (Rml::ElementDocument* document = root.GetOwnerDocument())
				setText(document->QuerySelector("[setup=infoLine] [setup-section=" + field + "]"), label);
		}

		void setFlag(Rml::Element& element, const char* name, bool set)
		{
			if (set && !element.HasAttribute(name))
				element.SetAttribute(name, "");
			else if (!set && element.HasAttribute(name))
				element.RemoveAttribute(name);
		}

		// The selected arranger events, as cycle markers on the arranger track in timeline order (as project.get lists
		// them), which the info line shows and edits as it does markers (p. 405).
		std::vector<MarkerItem> arrangerEvents(const Json& project, const std::vector<uint64_t>& selection)
		{
			std::vector<MarkerItem> items;
			if (!project.is_object() || !project.contains("arranger"))
				return items;
			const Json& arranger = project.at("arranger");
			const double rate = projectTime(project).sampleRate;
			for (const Json& event : arranger.at("events"))
			{
				const auto id = event.at("id").get<uint64_t>();
				if (std::find(selection.begin(), selection.end(), id) != selection.end())
					items.push_back(
						{id, arranger.at("track").get<uint64_t>(), true, 0, event.at("name").get<std::string>(),
							event.at("start").get<double>() / rate, event.at("endSamples").get<double>() / rate});
			}
			return items;
		}

		// What a field's gesture changes, as EngineBinding::sendStep names it: the info line's step owner, what the
		// field edits (the selected "events" or an in-place editor's "notes") and the field. The steps of the events'
		// fields keep the value that each event they change shows, by its id, with their record (#1471), and so do the
		// steps of the notes' velocity (#1550).
		std::string stepKey(const std::string& owner, const char* edits, const std::string& field)
		{
			return owner + edits + " " + field;
		}

		// The merge key of a field's undo steps: the step key and the selection, by its size and a hash of its ids,
		// so that a gesture that goes on over the field while another selection shows is an undo step for each.
		std::string mergeName(const std::string& key, const std::vector<uint64_t>& ids)
		{
			std::string selection;
			for (const uint64_t id : ids)
				selection += std::to_string(id) + " ";
			return key + " " + std::to_string(ids.size()) + " " + std::to_string(std::hash<std::string>{}(selection));
		}
	}

	InfoLine::InfoLine(EventDisplay& display, EngineBinding& binding, Rml::Element& root)
		: m_display(display), m_binding(binding), m_stepOwner(binding.stepOwner("info line")),
		  m_root(root.GetObserverPtr()),
		  // Another project ends the edits in progress without taking them: the ids may be the new project's too.
		  m_reset(binding.on("project.reset",
			  [this](const engine::Event&)
			  {
				  Rml::Context* context = m_root ? m_root->GetContext() : nullptr;
				  Rml::Element* focus = context != nullptr ? context->GetFocusElement() : nullptr;
				  if (focus == nullptr || !m_root->Contains(focus))
					  return;
				  m_dropping = true;
				  focus->Blur();
				  m_dropping = false;
			  })),
		  // The in-place editors' selections (p. 1259): whichever changed last shows its notes.
		  m_editors(binding.on("editor.changed", [this](const engine::Event& event) { followEditor(event.data); }))
	{
		root.SetInnerRML(g_rml);
		root.AddEventListener(Rml::EventId::Change, this);
		root.AddEventListener(Rml::EventId::Keydown, this, true); // whether Enter comes with Ctrl, before the field
		show();
	}

	InfoLine::~InfoLine()
	{
		if (!m_root)
			return;
		m_root->RemoveEventListener(Rml::EventId::Change, this);
		m_root->RemoveEventListener(Rml::EventId::Keydown, this, true);
	}

	// An in-place editor's changed view (p. 1259): the one whose selection changed last shows its notes. An empty
	// selection or a closing editor gives the info line back to what was selected before.
	void InfoLine::followEditor(const Json& view)
	{
		const std::string name = view.value("editor", std::string());
		if (name.rfind("inPlace:", 0) != 0)
			return;
		const bool leaves = m_showsNotes && m_noteView == name;
		if (!view.value("open", false))
		{
			m_viewSelections.erase(name);
			if (leaves)
			{
				m_showsNotes = false;
				m_notesDirty = true;
			}
			return;
		}
		const Json selection = view.value("selection", Json::array());
		const auto last = m_viewSelections.find(name);
		if (last != m_viewSelections.end() && last->second == selection)
			return;
		m_viewSelections[name] = selection;
		if (selection.empty())
		{
			if (leaves)
			{
				m_showsNotes = false;
				m_notesDirty = true;
			}
			return;
		}
		m_showsNotes = true;
		m_showsMarkers = false;
		m_showsPoints = false;
		m_showsRange = false;
		m_noteView = name;
		m_noteSelection = selection;
		m_notesDirty = true;
	}

	void InfoLine::update()
	{
		if (!m_root)
			return;
		// Ctrl+Enter took the value the field had, nothing typed, so it sent no change: that value goes to all selected
		// the same.
		if (m_absoluteTake)
		{
			const auto [field, text] = *m_absoluteTake;
			m_absoluteTake.reset();
			const std::optional<float> number = widgets::parse(text);
			const bool numeric = field == "volume" || field == "transpose" || field == "velocity";
			if (shows() && (!numeric || number))
			{
				m_absolute = true;
				if (m_showsRange)
					editRange(field, text);
				else if (m_showsNotes)
					editNotes(field, text, number.value_or(0.0F), 0);
				else if (m_showsMarkers)
					editMarkers(field, text);
				else if (m_showsPoints)
					editPoints(field, text);
				else
					edit(field, text, number.value_or(0.0F));
				m_absolute = false;
			}
		}
		const std::vector<uint64_t>& selection = m_display.selection();
		const TimeFormat format = m_display.rulerFormat();
		std::vector<uint64_t> markers = m_markerSource ? m_markerSource() : std::vector<uint64_t>();
		const bool markersChanged = markers != m_markerSelection;
		std::vector<uint64_t> arranger = m_arrangerSource ? m_arrangerSource() : std::vector<uint64_t>();
		const bool arrangerChanged = arranger != m_arrangerSelection;
		const std::map<uint64_t, std::set<uint64_t>>& points = m_display.selectedPoints();
		const bool pointsChanged = points != m_pointSelection;
		const std::optional<RangeSelection>& range = m_display.range();
		const bool rangeChanged = range.has_value() != m_range.has_value() ||
			(range && m_range &&
				(range->start != m_range->start || range->end != m_range->end || range->tracks != m_range->tracks));
		if ((selection == m_selection && m_display.answers() == m_loads && format == m_format && !markersChanged &&
				!arrangerChanged && !pointsChanged && !rangeChanged && !m_notesDirty) ||
			!m_root->IsVisible(true)) // hidden: it asks once it shows
			return;
		// Whichever selection changed last shows; a marker clicked on the marker track wins.
		if (selection != m_selection && !selection.empty())
		{
			m_showsMarkers = false;
			m_showsNotes = false;
			m_showsPoints = false;
			m_showsRange = false;
		}
		// Markers and arranger events show in the same fields: the one of them deselected doesn't end the other.
		for (const bool ofArranger : {false, true})
		{
			const std::vector<uint64_t>& items = ofArranger ? arranger : markers;
			if (!(ofArranger ? arrangerChanged : markersChanged) || (items.empty() && m_arranger != ofArranger))
				continue;
			// Markers that only went from the selection, such as removed ones, don't bring the info line back to the
			// markers from the event the user selected last.
			const std::vector<uint64_t>& before = ofArranger ? m_arrangerSelection : m_markerSelection;
			if (!m_showsMarkers && !items.empty() &&
				std::all_of(items.begin(), items.end(),
					[&before](uint64_t id) { return std::find(before.begin(), before.end(), id) != before.end(); }))
				continue;
			m_showsMarkers = !items.empty();
			m_arranger = ofArranger;
			if (m_showsMarkers)
			{
				m_showsNotes = false;
				m_showsPoints = false;
				m_showsRange = false;
			}
		}
		if (pointsChanged)
		{
			m_showsPoints = !points.empty();
			if (m_showsPoints)
			{
				m_showsMarkers = false;
				m_showsNotes = false;
				m_showsRange = false;
			}
		}
		if (rangeChanged)
		{
			m_showsRange = range.has_value();
			if (m_showsRange)
			{
				m_showsMarkers = false;
				m_showsNotes = false;
				m_showsPoints = false;
			}
		}
		m_range = range;
		m_pointSelection = points;
		m_markerSelection = std::move(markers);
		m_arrangerSelection = std::move(arranger);
		m_markers.clear();
		m_selection = selection;
		m_loads = m_display.answers();
		m_format = format;
		m_notesDirty = false;
		if (m_showsRange)
		{
			m_events = Json::array(); // asked for again when the events show
			show();
			return;
		}
		if (m_showsNotes)
		{
			m_events = Json::array(); // asked for again when the events show
			m_notes = notesOf(m_noteSelection);
			show();
			return;
		}
		m_notes.clear();
		m_points = m_showsPoints ? pointsOf() : std::vector<Point>();
		if (m_showsPoints)
		{
			m_events = Json::array(); // asked for again when the events show
			show();
			return;
		}
		if (m_showsMarkers && m_arranger)
			m_markers = arrangerEvents(m_display.project(), m_arrangerSelection);
		else if (m_showsMarkers)
			for (const MarkerItem& marker : readMarkers(m_display.project()).markers)
				if (std::find(m_markerSelection.begin(), m_markerSelection.end(), marker.id) != m_markerSelection.end())
					m_markers.push_back(marker);
		if (m_showsMarkers)
		{
			m_events = Json::array(); // asked for again when the events show
			show();
			return;
		}
		if (selection.empty())
		{
			m_events = Json::array();
			show();
			return;
		}
		m_binding.send("event.get", {{"events", selection}},
			[this, asked = selection, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || asked != m_selection)
					return;
				m_events = result.ok() ? result.value.at("events") : Json::array();
				// The first one is the one that starts first.
				std::stable_sort(m_events.begin(), m_events.end(),
					[](const Json& a, const Json& b) { return a.at("start") < b.at("start"); });
				show();
			});
	}

	const widgets::Options& rootKeyOptions(bool differ)
	{
		static const widgets::Options keys = {{"", "-"}, {"C", "C"}, {"C#", "C#"}, {"D", "D"}, {"D#", "D#"}, {"E", "E"},
			{"F", "F"}, {"F#", "F#"}, {"G", "G"}, {"G#", "G#"}, {"A", "A"}, {"A#", "A#"}, {"B", "B"}};
		static const widgets::Options differing = [&]
		{
			widgets::Options all = {{g_differentRootKeys, ""}};
			all.insert(all.end(), keys.begin(), keys.end());
			return all;
		}();
		return differ ? differing : keys;
	}

	double InfoLine::volumeOf(uint64_t event) const
	{
		const Json& project = m_display.project();
		if (!project.is_object())
			return 0.0;
		for (const Json& audio : project.at("audioEvents"))
			if (audio.at("id") == event)
				return audio.at("volume").get<double>();
		return 0.0;
	}

	int InfoLine::partValue(const Json& event, const char* key) const
	{
		const Json* item = transposableOf(event);
		return item != nullptr ? item->value(key, 0) : 0;
	}

	std::vector<const engine::Json*> InfoLine::eventsOf(uint64_t part) const
	{
		std::vector<const Json*> events;
		const Json& project = m_display.project();
		if (project.is_object())
			for (const Json& audio : project.at("audioEvents"))
				if (audio.value("part", uint64_t{0}) == part)
					events.push_back(&audio);
		// The first one is the one the user sees first.
		std::stable_sort(events.begin(), events.end(),
			[](const Json* a, const Json* b) { return a->at("position") < b->at("position"); });
		return events;
	}

	const engine::Json* InfoLine::transposableOf(const Json& event) const
	{
		const Json& project = m_display.project();
		const auto kind = event.at("kind").get<std::string>();
		if (kind == "midi")
			return partOf(event.at("id").get<uint64_t>());
		if (kind == "audioPart")
		{
			const std::vector<const Json*> events = eventsOf(event.at("id").get<uint64_t>());
			return events.empty() ? nullptr : events.front();
		}
		if (kind != "audio" || !project.is_object())
			return nullptr;
		for (const Json& audio : project.at("audioEvents"))
			if (audio.at("id") == event.at("id"))
				return &audio;
		return nullptr;
	}

	const engine::Json* InfoLine::partOf(uint64_t part) const
	{
		const Json& project = m_display.project();
		if (!project.is_object())
			return nullptr;
		for (const Json& midi : project.at("midiParts"))
			if (midi.at("id") == part)
				return &midi;
		return nullptr;
	}

	std::string InfoLine::fieldValue(
		const char* edits, const std::string& field, uint64_t item, std::string value) const
	{
		const Json* shown = m_binding.shown(stepKey(m_stepOwner, edits, field), m_display.read());
		const std::string id = std::to_string(item);
		if (shown != nullptr && shown->contains(id))
			return shown->at(id).get<std::string>();
		return value;
	}

	double InfoLine::fieldNumber(const char* edits, const std::string& field, uint64_t item, double value) const
	{
		const std::optional<float> kept = widgets::parse(fieldValue(edits, field, item, std::string()));
		return kept ? static_cast<double>(*kept) : value;
	}

	// The changes that setting the fields sends aren't the user's: with several items selected, the first one's value
	// would reach the others (#1719).
	void InfoLine::show()
	{
		const bool showing = m_showing;
		m_showing = true;
		showFields();
		m_showing = showing;
	}

	void InfoLine::showFields()
	{
		if (!m_root)
			return;
		Rml::Element& root = *m_root.get();
		setClass(root, "markers", m_showsMarkers && !m_arranger);
		setClass(root, "arranger", m_showsMarkers && m_arranger);
		setClass(root, "notes", m_showsNotes);
		setClass(root, "points", m_showsPoints);
		setClass(root, "range", m_showsRange);
		if (m_showsRange)
		{
			showRange();
			return;
		}
		// The range's fields and labels (showRange) only for a selection range.
		for (const char* field : {"topTrack", "bottomTrack"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", true);
		setLabel(root, "start", "Start");
		setLabel(root, "end", "End");
		setLabel(root, "length", "Length");
		if (m_showsPoints)
		{
			showPoints();
			return;
		}
		if (m_showsNotes)
		{
			showNotes();
			return;
		}
		if (m_showsMarkers)
		{
			showMarkers();
			return;
		}
		setClass(root, "empty", m_events.empty());
		setClass(root, "multiple", m_events.size() > 1);
		if (m_events.empty())
			return;
		setLabel(root, "name", "Name");
		setLabel(root, "start", "Start");
		for (const char* field : {"name", "end", "length", "mute", "lock"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", false);
		// The notes' fields show only for an in-place editor's selection (p. 1259), the automation points' only for
		// selected points (p. 881).
		for (const char* field : {"pitch", "releaseVelocity", "channel", "value", "terminator"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", true);
		const Json& first = m_events.front();
		const TimeContext context = projectTime(m_display.project());
		const double start = first.at("start").get<double>() / context.sampleRate;
		const double end = first.at("end").get<double>() / context.sampleRate;
		const bool audio = first.at("kind") == "audio";
		const std::map<std::string, std::string> texts = {{"name", first.at("name").get<std::string>()},
			{"start", formatTime(m_format, start, context)}, {"end", formatTime(m_format, end, context)},
			{"length", formatLength(m_format, start, end, context)}};
		for (const auto& [field, text] : texts)
			if (Rml::Element* display = root.QuerySelector("textdisplay[field=" + field + "]"))
				setValue(*display, text);
		if (Rml::Element* name = root.QuerySelector("textdisplay[field=name]"))
			setFlag(*name, "disabled", audio); // an audio event's name is its clip's
		if (Rml::Element* volume = root.QuerySelector("[info-field=volume]"))
			setClass(*volume, "unavailable", !audio);
		if (Rml::Element* volume = root.QuerySelector("valuefield[field=volume]"); volume != nullptr && audio)
			setValue(*volume,
				fieldValue("events", "volume", first.at("id").get<uint64_t>(),
					widgets::format(static_cast<float>(volumeOf(first.at("id").get<uint64_t>())))));
		for (const char* field : {"mute", "lock"})
			if (Rml::Element* button = root.QuerySelector("button[field=" + std::string(field) + "]"))
				setFlag(*button, "checked", first.at(field).get<bool>());
		// A MIDI part's transpose and velocity, an audio event's transpose (#1555).
		const bool midi = first.at("kind") == "midi";
		for (const char* field : {"transpose", "velocity"})
		{
			const bool has = midi || (audio && std::string(field) == "transpose");
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", !has);
			Rml::Element* value = root.QuerySelector("valuefield[field=" + std::string(field) + "]");
			if (value == nullptr || !has)
				continue;
			const std::string limit = std::to_string(limitOf(first));
			for (const auto& [attribute, bound] : {std::pair{"min", "-" + limit}, std::pair{"max", limit}})
				if (value->GetAttribute<Rml::String>(attribute, "") != bound)
					value->SetAttribute(attribute, bound);
			setValue(*value,
				fieldValue("events", field, first.at("id").get<uint64_t>(),
					widgets::format(static_cast<float>(partValue(first, field)))));
		}
		// A MIDI part's or an audio event's root key and Global Transpose (#1368), an audio part's events' (#1555).
		const Json* part = transposableOf(first);
		for (const char* field : {"rootKey", "globalTranspose"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", part == nullptr);
		if (part == nullptr)
			return;
		if (Rml::Element* step = root.QuerySelector("[field=rootKeyTranspose]"))
		{
			const int semitones = part->value("rootKeyTranspose", 0);
			const std::string text = semitones == 0
				? std::string()
				: "(" + std::string(semitones > 0 ? "+" : "") + std::to_string(semitones) + ")";
			if (step->GetInnerRML() != text)
				step->SetInnerRML(text);
		}
		// An audio part's events whose root keys or Global Transpose differ show an empty value (#1644).
		const std::vector<const Json*> events =
			first.at("kind") == "audioPart" ? eventsOf(first.at("id").get<uint64_t>()) : std::vector<const Json*>();
		const auto differ = [&events, part](const char* field)
		{
			return std::any_of(events.begin(), events.end(), [&](const Json* event)
				{ return event->value(field, std::string()) != part->value(field, std::string()); });
		};
		if (Rml::Element* key = root.QuerySelector("select[field=rootKey]"))
		{
			const bool different = differ("rootKey");
			widgets::fill(*key, rootKeyOptions(different),
				different ? std::string(g_differentRootKeys) : part->value("rootKey", std::string()));
		}
		if (Rml::Element* button = root.QuerySelector("button[field=globalTranspose]"))
		{
			const bool different = differ("globalTranspose");
			const bool independent = !different && part->value("globalTranspose", std::string()) == "independent";
			setFlag(*button, "checked", independent);
			// The button reads the value it holds (p. 421): Independent while checked, else Follow.
			std::string text = independent ? "Independent" : "Follow";
			if (different)
				text.clear();
			setText(button, text.c_str());
			Rml::String title = independent ? "Global Transpose: Independent" : "Global Transpose: Follow";
			if (different)
				title = "Global Transpose";
			if (button->GetAttribute<Rml::String>("title", "") != title)
				button->SetAttribute("title", title);
		}
	}

	void InfoLine::ProcessEvent(Rml::Event& event)
	{
		const Rml::Element* target = event.GetTargetElement();
		if (event == Rml::EventId::Keydown)
		{
			// Ctrl (Cmd) with Enter in the text field of a display or value field: an absolute take, which the field's
			// change applies, or update when nothing was typed and no change comes.
			const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
			const auto* input = rmlui_dynamic_cast<const Rml::ElementFormControlInput*>(target);
			const Rml::Element* field = input != nullptr ? input->GetParentNode() : nullptr;
			if ((key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER) &&
				(event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0) &&
				field != nullptr && field->HasAttribute("field"))
				m_absoluteTake.emplace(field->GetAttribute<Rml::String>("field", ""), input->GetValue());
			return;
		}
		// A display's, a value field's or a button's change, not that of the text field inside a display.
		if (target == nullptr || !target->HasAttribute("field") || !shows() || m_dropping || m_showing)
			return;
		const auto field = target->GetAttribute<Rml::String>("field", "");
		m_absolute = m_absoluteTake && m_absoluteTake->first == field;
		m_absoluteTake.reset();
		if (m_showsRange)
			editRange(field, event.GetParameter<Rml::String>("value", ""));
		else if (m_showsNotes)
			editNotes(field, event.GetParameter<Rml::String>("value", ""), event.GetParameter<float>("value", 0.0F),
				event.GetParameter<int>("gesture", 0));
		else if (m_showsMarkers)
			editMarkers(field, event.GetParameter<Rml::String>("value", ""));
		else if (m_showsPoints)
			editPoints(field, event.GetParameter<Rml::String>("value", ""));
		else if (target->GetTagName() == "button")
			edit(field, {}, event.GetParameter<bool>("value", false) ? 1.0F : 0.0F);
		else
			edit(field, event.GetParameter<Rml::String>("value", ""), event.GetParameter<float>("value", 0.0F),
				event.GetParameter<int>("gesture", 0));
		m_absolute = false;
	}

	// The first selected marker: its description and position, and a cycle marker's end and length.
	void InfoLine::showMarkers()
	{
		Rml::Element& root = *m_root.get();
		// None of them in the display's project: removed, before the marker lines drop them from their selection.
		setClass(root, "empty", m_markers.empty());
		if (m_markers.empty())
		{
			setClass(root, "multiple", false);
			return;
		}
		setClass(root, "multiple", m_markers.size() > 1);
		const MarkerItem& first = m_markers.front();
		const TimeContext context = projectTime(m_display.project());
		const std::map<std::string, std::string> texts = {{"name", first.name},
			{"start", formatTime(m_format, first.start, context)}, {"end", formatTime(m_format, first.end, context)},
			{"length", formatLength(m_format, first.start, first.end, context)}};
		for (const auto& [field, text] : texts)
			if (Rml::Element* display = root.QuerySelector("textdisplay[field=" + field + "]"))
				setValue(*display, text);
		if (Rml::Element* name = root.QuerySelector("textdisplay[field=name]"))
			setFlag(*name, "disabled", false);
		setLabel(root, "name", m_arranger ? "Name" : "Description");
		setLabel(root, "start", m_arranger ? "Start" : "Position");
		if (Rml::Element* shown = root.QuerySelector("[info-field=name]"))
			setClass(*shown, "unavailable", false);
		for (const char* field : {"end", "length"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", !first.cycle);
		for (const char* field : {"volume", "mute", "lock", "transpose", "velocity", "pitch", "releaseVelocity",
				 "channel", "rootKey", "globalTranspose", "value", "terminator"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", true);
	}

	// As for events: relative to the first marker's value, or with Ctrl+Enter the value typed for every one. A new
	// position keeps a cycle marker's length; end and length change the cycle markers. Arranger events are edited so
	// too, with arranger.set.
	void InfoLine::editMarkers(const std::string& field, const std::string& text)
	{
		const ProjectMarkers project = readMarkers(m_display.project());
		const char* command = m_arranger ? "arranger.set" : "marker.set";
		const char* key = m_arranger ? "event" : "marker";
		const TimeContext& context = project.time;
		const MarkerItem& first = m_markers.front();
		const auto cycle =
			std::find_if(m_markers.begin(), m_markers.end(), [](const MarkerItem& m) { return m.cycle; });
		Json commands = Json::array();
		const auto trackOf = [&project](const MarkerItem& marker)
		{
			return std::find_if(project.tracks.begin(), project.tracks.end(),
				[&marker](const ProjectMarkers::Track& each) { return each.id == marker.track; });
		};
		// A cycle marker's new end, which keeps its start.
		const auto resize = [&commands, &project, &trackOf, command, key](const MarkerItem& marker, double end)
		{
			const auto track = trackOf(marker);
			if (track != project.tracks.end() && marker.cycle && end > marker.start && end != marker.end)
				commands.push_back(
					{{"command", command}, {"parameters", {{key, marker.id}, {"end", project.position(*track, end)}}}});
		};
		if (field == "name")
		{
			// An arranger event keeps its name when the field is left empty, as on the lane and in the chain lists.
			for (const MarkerItem& marker : m_markers)
				if (marker.name != text && !(m_arranger && text.empty()))
					commands.push_back({{"command", command}, {"parameters", {{key, marker.id}, {"name", text}}}});
		}
		else if (field == "start")
		{
			if (const std::optional<double> typed = parseTime(m_format, text, context))
			{
				// Only the position is sent, so that a cycle marker keeps its length in its track's time base. The
				// markers are in timeline order, so that none of them goes before the start when the first one doesn't.
				for (const MarkerItem& marker : m_markers)
				{
					const double start = m_absolute ? *typed : marker.start + (*typed - first.start);
					if (const auto track = trackOf(marker);
						track != project.tracks.end() && start >= 0.0 && start != marker.start)
						commands.push_back({{"command", command},
							{"parameters", {{key, marker.id}, {"position", project.position(*track, start)}}}});
				}
			}
		}
		else if ((field == "end" || field == "length") && cycle != m_markers.end())
		{
			// How long each cycle marker gets.
			std::optional<double> length;
			if (field == "end")
			{
				if (const std::optional<double> typed = parseTime(m_format, text, context))
					length = *typed - cycle->start;
			}
			else
			{
				length = parseLength(m_format, text, cycle->start, context);
			}
			if (length)
				for (const MarkerItem& marker : m_markers)
				{
					if (!marker.cycle)
						continue;
					if (!m_absolute)
					{
						resize(marker, marker.end + (*length - (cycle->end - cycle->start)));
					}
					else if (field == "end")
					{
						resize(marker, cycle->start + *length);
					}
					else if (const std::optional<double> own = parseLength(m_format, text, marker.start, context))
					{
						// A length in bars and beats isn't the same stretch of seconds at every marker.
						resize(marker, marker.start + *own);
					}
				}
		}
		if (commands.empty())
		{
			show(); // the value that holds
			return;
		}
		const std::map<std::string, const char*> labels = {{"name", m_arranger ? "Name" : "Description"},
			{"start", m_arranger ? "Start" : "Position"}, {"end", "End"}, {"length", "Length"}};
		const std::string description =
			std::string(m_arranger ? "Change Arranger Event " : "Change Marker ") + labels.at(field);
		m_binding.send("history.transaction", {{"description", description}, {"commands", std::move(commands)}});
	}

	// The selection range: its start, end and length, and its top and bottom track among the display's tracks.
	void InfoLine::showRange()
	{
		Rml::Element& root = *m_root.get();
		setClass(root, "empty", !m_range);
		setClass(root, "multiple", false);
		if (!m_range)
			return;
		const RangeSelection& range = *m_range;
		const TimeContext context = projectTime(m_display.project());
		const double start = static_cast<double>(range.start) / context.sampleRate;
		const double end = static_cast<double>(range.end) / context.sampleRate;
		const std::map<std::string, std::string> texts = {{"start", formatTime(m_format, start, context)},
			{"end", formatTime(m_format, end, context)}, {"length", formatLength(m_format, start, end, context)}};
		for (const auto& [field, text] : texts)
			if (Rml::Element* display = root.QuerySelector("textdisplay[field=" + field + "]"))
				setValue(*display, text);
		setLabel(root, "start", "Range Start");
		setLabel(root, "end", "Range End");
		setLabel(root, "length", "Range Length");
		Rml::ElementList fields;
		root.QuerySelectorAll(fields, "[info-field]");
		for (Rml::Element* field : fields)
		{
			const auto name = field->GetAttribute<Rml::String>("info-field", "");
			setClass(*field, "unavailable",
				name != "start" && name != "end" && name != "length" && name != "topTrack" && name != "bottomTrack");
		}
		// The display's tracks by name; the range's top and bottom are the first and last of its tracks among them,
		// as a folded folder's lane stands for the tracks inside it.
		const std::vector<uint64_t> order = m_display.shownTracks();
		const Json& projectTracks = m_display.project().value("tracks", Json::array());
		widgets::Options tracks;
		for (const uint64_t id : order)
			for (const Json& track : projectTracks)
				if (track.value("id", uint64_t{0}) == id)
					tracks.emplace_back(std::to_string(id), track.value("name", std::string()));
		const auto shown = [&order](uint64_t track)
		{ return std::find(order.begin(), order.end(), track) != order.end(); };
		const auto top = std::find_if(range.tracks.begin(), range.tracks.end(), shown);
		const auto bottom = std::find_if(range.tracks.rbegin(), range.tracks.rend(), shown);
		if (Rml::Element* list = root.QuerySelector("select[field=topTrack]"))
			widgets::fill(*list, tracks, top != range.tracks.end() ? std::to_string(*top) : std::string());
		if (Rml::Element* list = root.QuerySelector("select[field=bottomTrack]"))
			widgets::fill(*list, tracks, bottom != range.tracks.rend() ? std::to_string(*bottom) : std::string());
	}

	void InfoLine::editRange(const std::string& field, const std::string& text)
	{
		if (!m_range)
			return;
		const RangeSelection shown = *m_range;
		RangeSelection range = shown;
		const TimeContext context = projectTime(m_display.project());
		const double rate = context.sampleRate;
		bool changed = false;
		if (field == "start" || field == "end")
		{
			if (const std::optional<double> typed = parseTime(m_format, text, context))
			{
				const int64_t to = positionSamples(*typed, rate);
				const bool start = field == "start";
				if (start ? to <= range.end : to >= range.start)
				{
					(start ? range.start : range.end) = to;
					changed = true;
				}
			}
		}
		else if (field == "length")
		{
			const double start = static_cast<double>(range.start) / rate;
			if (const std::optional<double> length = parseLength(m_format, text, start, context))
			{
				range.end = std::max(range.start, positionSamples(start + *length, rate));
				changed = true;
			}
		}
		else if ((field == "topTrack" || field == "bottomTrack") && !range.tracks.empty())
		{
			// The chosen track becomes the edge: the tracks between it and the edge are taken in or out, as the top
			// and bottom nudges take them one by one (p. 289), never past the other edge.
			const bool top = field == "topTrack";
			const uint64_t chosen = std::strtoull(text.c_str(), nullptr, 10); // 0, no track, for no number
			const std::vector<uint64_t> order = m_display.shownTracks();
			const auto index = [&order](uint64_t track)
			{ return std::find(order.begin(), order.end(), track) - order.begin(); };
			const auto size = static_cast<std::ptrdiff_t>(order.size());
			const auto at = index(chosen);
			const auto edge = index(top ? range.tracks.front() : range.tracks.back());
			const auto other = index(top ? range.tracks.back() : range.tracks.front());
			if (at < size && edge < size && other < size && (top ? at <= other : at >= other))
			{
				const auto inside = std::find(range.tracks.begin(), range.tracks.end(), chosen);
				if (inside != range.tracks.end())
				{
					if (top)
						range.tracks.erase(range.tracks.begin(), inside);
					else
						range.tracks.erase(std::next(inside), range.tracks.end());
				}
				else if (top && at < edge)
				{
					range.tracks.insert(range.tracks.begin(), order.begin() + at, order.begin() + edge);
				}
				else if (!top && at > edge)
				{
					range.tracks.insert(range.tracks.end(), order.begin() + edge + 1, order.begin() + at + 1);
				}
				changed = range.tracks != shown.tracks;
			}
		}
		if (!changed)
		{
			show(); // the range as it is
			return;
		}
		m_display.setRange(std::move(range));
	}

	std::vector<InfoLine::Point> InfoLine::pointsOf() const
	{
		std::vector<Point> points;
		const Json& project = m_display.project();
		if (!project.is_object())
			return points;
		const TimeContext context = projectTime(project);
		for (const Json& lane : project.at("automation"))
		{
			const auto selected = m_pointSelection.find(lane.at("id").get<uint64_t>());
			if (selected == m_pointSelection.end())
				continue;
			Point base;
			base.lane = selected->first;
			for (const Json& track : project.at("tracks"))
				if (track.at("id") == lane.at("track"))
					base.linear = track.value("timeBase", std::string("musical")) == "linear";
			if (const Json* targets = m_display.automationTargets(lane.at("track").get<uint64_t>()))
				for (const Json& info : *targets)
					if (info.at("target") == lane.at("target"))
					{
						base.minimum = info.at("minimum").get<double>();
						base.maximum = info.at("maximum").get<double>();
					}
			for (const Json& json : lane.at("points"))
				if (selected->second.count(json.at("id").get<uint64_t>()) != 0)
				{
					Point point = base;
					point.id = json.at("id").get<uint64_t>();
					point.position = json.at("position").get<int64_t>();
					point.seconds = point.linear ? static_cast<double>(point.position) / context.sampleRate
												 : context.tempo.toSeconds(point.position);
					point.value = json.at("value").get<double>();
					point.terminator = json.value("terminator", false);
					points.push_back(point);
				}
		}
		std::stable_sort(
			points.begin(), points.end(), [](const Point& a, const Point& b) { return a.seconds < b.seconds; });
		return points;
	}

	// The first selected automation point: its position, value and Terminator (p. 881).
	void InfoLine::showPoints()
	{
		Rml::Element& root = *m_root.get();
		// None of them in the display's project: removed, before the display drops them from its selection.
		setClass(root, "empty", m_points.empty());
		setClass(root, "multiple", m_points.size() > 1);
		if (m_points.empty())
			return;
		setLabel(root, "start", "Position");
		const Point& first = m_points.front();
		if (Rml::Element* start = root.QuerySelector("textdisplay[field=start]"))
			setValue(*start, formatTime(m_format, first.seconds, projectTime(m_display.project())));
		if (Rml::Element* value = root.QuerySelector("textdisplay[field=value]"))
			setValue(*value, widgets::format(static_cast<float>(first.value)));
		if (Rml::Element* button = root.QuerySelector("button[field=terminator]"))
		{
			setFlag(*button, "checked", first.terminator);
			setText(button, first.terminator ? "Yes" : "No");
		}
		Rml::ElementList fields;
		root.QuerySelectorAll(fields, "[info-field]");
		for (Rml::Element* field : fields)
		{
			const auto name = field->GetAttribute<Rml::String>("info-field", "");
			setClass(*field, "unavailable", name != "start" && name != "value" && name != "terminator");
		}
	}

	// As for events: relative to the first point's position or value, or with Ctrl+Enter the value typed for every one,
	// kept in each parameter's range; Terminator goes to all, which makes the gap after each (p. 881).
	void InfoLine::editPoints(const std::string& field, const std::string& text)
	{
		const TimeContext context = projectTime(m_display.project());
		const Point& first = m_points.front();
		std::map<uint64_t, Json> changes; // automationPoint.set's points, by lane
		if (field == "start")
		{
			if (const std::optional<double> typed = parseTime(m_format, text, context))
				for (const Point& point : m_points)
				{
					const double to = clampPositionSeconds(
						m_absolute ? *typed : point.seconds + (*typed - first.seconds), context.sampleRate);
					const int64_t position =
						point.linear ? positionSamples(to, context.sampleRate) : context.tempo.toTicks(to);
					if (position != point.position)
						changes[point.lane].push_back({{"id", point.id}, {"position", position}});
				}
		}
		else if (field == "value")
		{
			if (const std::optional<float> typed = widgets::parse(text))
				for (const Point& point : m_points)
				{
					const double value = std::clamp(
						m_absolute ? *typed : point.value + (*typed - first.value), point.minimum, point.maximum);
					if (value != point.value)
						changes[point.lane].push_back({{"id", point.id}, {"value", value}});
				}
		}
		else if (field == "terminator")
		{
			for (const Point& point : m_points)
				if (point.terminator == first.terminator)
					changes[point.lane].push_back({{"id", point.id}, {"terminator", !first.terminator}});
		}
		Json commands = Json::array();
		for (auto& [lane, points] : changes)
			commands.push_back(
				{{"command", "automationPoint.set"}, {"parameters", {{"lane", lane}, {"points", std::move(points)}}}});
		if (commands.empty())
		{
			show(); // the value that holds
			return;
		}
		const std::map<std::string, const char*> labels = {
			{"start", "Position"}, {"value", "Value"}, {"terminator", "Terminator"}};
		m_binding.send("history.transaction",
			{{"description", std::string("Change Automation Point ") + labels.at(field)},
				{"commands", std::move(commands)}});
	}

	// The first selected note of an in-place editor, as its own info line shows it (pp. 1199, 1259): positions in
	// bars and beats, the pitch as a name, the velocities 0-127 and the channel 1-16.
	void InfoLine::showNotes()
	{
		Rml::Element& root = *m_root.get();
		// None of them in the project shown: removed, before the editor's selection drops them.
		setClass(root, "empty", m_notes.empty());
		setClass(root, "multiple", m_notes.size() > 1);
		if (m_notes.empty())
			return;
		setLabel(root, "name", "Name"); // the markers may have renamed it
		setLabel(root, "start", "Start");
		const Note& first = m_notes.front();
		const TimeContext context = projectTime(m_display.project());
		const double start = context.tempo.toSeconds(first.start);
		const double end = context.tempo.toSeconds(first.start + first.length);
		const std::map<std::string, std::string> texts = {{"start", formatTime(TimeFormat::BarsBeats, start, context)},
			{"end", formatTime(TimeFormat::BarsBeats, end, context)},
			{"length", formatLength(TimeFormat::BarsBeats, start, end, context)},
			{"pitch", engine::midi::pitchName(first.pitch)},
			{"releaseVelocity", std::to_string(std::lround(first.releaseVelocity * g_maxVelocity))},
			{"channel", std::to_string(first.channel + 1)}};
		for (const auto& [field, text] : texts)
			if (Rml::Element* display = root.QuerySelector("textdisplay[field=" + field + "]"))
				setValue(*display, text);
		for (const char* field : {"start", "end", "length", "pitch", "velocity", "releaseVelocity", "channel"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", false);
		for (const char* field :
			{"name", "volume", "mute", "lock", "transpose", "rootKey", "globalTranspose", "value", "terminator"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", true);
		if (Rml::Element* velocity = root.QuerySelector("valuefield[field=velocity]"))
			setValue(*velocity,
				fieldValue("notes", "velocity", first.id,
					widgets::format(static_cast<float>(std::lround(first.velocity * g_maxVelocity)))));
	}

	// The notes an in-place editor's selection names, from the display's project, by start, then pitch, as the key
	// editor orders them.
	std::vector<InfoLine::Note> InfoLine::notesOf(const Json& selection) const
	{
		std::vector<Note> notes;
		const Json& project = m_display.project();
		if (!project.is_object())
			return notes;
		std::map<uint64_t, const Json*> parts;
		for (const Json& part : project.at("midiParts"))
			parts[part.at("id").get<uint64_t>()] = &part;
		for (const Json& selected : selection)
		{
			const auto part = parts.find(selected.at("part").get<uint64_t>());
			if (part == parts.end())
				continue; // removed, before the editor's selection drops it
			const int64_t start = partStart(*part->second);
			for (const Json& note : part->second->at("notes"))
				if (note.at("id") == selected.at("note"))
					notes.push_back({part->first, note.at("id").get<uint64_t>(),
						start + note.at("position").get<int64_t>(), note.at("length").get<int64_t>(),
						note.at("pitch").get<int>(), note.at("velocity").get<double>(),
						note.at("releaseVelocity").get<double>(), note.at("channel").get<int>()});
		}
		std::stable_sort(notes.begin(), notes.end(),
			[](const Note& a, const Note& b) { return std::tie(a.start, a.pitch) < std::tie(b.start, b.pitch); });
		return notes;
	}

	int64_t InfoLine::partStart(const Json& part) const
	{
		const Json& project = m_display.project();
		const auto position = part.at("position").get<int64_t>();
		if (!project.is_object())
			return position;
		for (const Json& track : project.at("tracks"))
			if (track.at("id") == part.at("track") && track.value("timeBase", std::string("musical")) == "linear")
			{
				// A linear track's part position is in seconds, as the key editor converts it.
				const TimeContext context = projectTime(project);
				return context.tempo.toTicks(position, context.sampleRate);
			}
		return position;
	}

	// The in-place editor's selected notes, as its own info line edits them (pp. 1140, 1259): relative to the first
	// note's value, or with Ctrl+Enter the value typed for every one; the channel is set for all.
	void InfoLine::editNotes(const std::string& field, const std::string& text, float number, int gesture)
	{
		if (m_notes.empty())
			return;
		const Note& first = m_notes.front();
		const TimeContext context = projectTime(m_display.project());
		std::optional<int64_t> delta; // what the first note's value changes by
		std::optional<int64_t> typed; // the value typed, which an absolute take gives every note
		if (field == "start" || field == "end")
		{
			const std::optional<double> seconds = parseTime(TimeFormat::BarsBeats, text, context);
			if (!seconds)
			{
				show(); // the value that holds
				return;
			}
			typed = context.tempo.toTicks(*seconds);
			delta = *typed - (field == "start" ? first.start : first.start + first.length);
		}
		else if (field == "length")
		{
			const double start = context.tempo.toSeconds(first.start);
			const std::optional<double> seconds = parseLength(TimeFormat::BarsBeats, text, start, context);
			if (!seconds)
			{
				show();
				return;
			}
			typed = context.tempo.toTicks(start + *seconds) - first.start;
			delta = *typed - first.length;
		}
		else if (field == "pitch")
		{
			// A number 0-127 or a name such as C3.
			std::optional<int> value;
			try
			{
				value = std::stoi(text);
			}
			catch (const std::exception&)
			{
				for (int pitch = 0; pitch < g_pitches && !value; ++pitch)
					if (engine::midi::pitchName(pitch) == text)
						value = pitch;
			}
			if (!value)
			{
				show();
				return;
			}
			typed = *value;
			delta = *value - first.pitch;
		}
		else if (field == "velocity")
		{
			typed = std::lround(number);
			delta = *typed - std::lround(fieldNumber("notes", field, first.id, first.velocity * g_maxVelocity));
		}
		else if (field == "releaseVelocity" || field == "channel")
		{
			try
			{
				typed = std::stoi(text);
			}
			catch (const std::exception&)
			{
				show();
				return;
			}
			if (field == "releaseVelocity")
			{
				delta = *typed - std::lround(first.releaseVelocity * g_maxVelocity);
			}
			else
			{
				delta = 0; // the channel is set for all, not moved
			}
		}
		else
		{
			return;
		}
		if (!typed || !delta)
		{
			return; // a field the notes don't have
		}
		// Each note's part's start, for the positions that note.set takes relative to it.
		std::map<uint64_t, int64_t> starts;
		for (const Note& note : m_notes)
			if (starts.count(note.part) == 0)
				if (const Json* part = partOf(note.part))
					starts[note.part] = partStart(*part);
		std::map<uint64_t, Json> changes;
		Json shown = Json::object(); // what the velocity field shows for each note changed
		for (const Note& note : m_notes)
		{
			const auto start = starts.find(note.part);
			if (start == starts.end())
				continue;
			Json change = Json::object();
			change["id"] = note.id;
			if (field == "start")
			{
				const int64_t to = m_absolute ? *typed : note.start + *delta;
				change["position"] = std::max<int64_t>(to, 0) - start->second;
			}
			else if (field == "end")
			{
				const int64_t to = m_absolute ? *typed : note.start + note.length + *delta;
				change["length"] = std::max<int64_t>(to - note.start, 1);
			}
			else if (field == "length")
			{
				change["length"] = std::max<int64_t>(m_absolute ? *typed : note.length + *delta, 1);
			}
			else if (field == "pitch")
			{
				change["pitch"] = std::clamp<int64_t>(m_absolute ? *typed : note.pitch + *delta, 0, g_pitches - 1);
			}
			else if (field == "velocity" || field == "releaseVelocity")
			{
				// Each note's velocity from what the field would show for it while the project is behind (#1632).
				const double base = field == "velocity"
					? fieldNumber("notes", field, note.id, note.velocity * g_maxVelocity) / g_maxVelocity
					: note.releaseVelocity;
				const double value = m_absolute ? (static_cast<double>(*typed) / g_maxVelocity)
												: base + (static_cast<double>(*delta) / g_maxVelocity);
				change[field] = std::clamp(value, 0.0, 1.0);
				if (field == "velocity")
					shown[std::to_string(note.id)] =
						widgets::format(static_cast<float>(std::lround(change[field].get<double>() * g_maxVelocity)));
			}
			else if (field == "channel")
			{
				change["channel"] = std::clamp<int64_t>(*typed - 1, 0, 15);
			}
			changes[note.part].push_back(std::move(change));
		}
		Json commands = Json::array();
		for (auto& [part, notes] : changes)
			commands.push_back({{"command", "note.set"}, {"parameters", {{"part", part}, {"notes", notes}}}});
		if (commands.empty())
		{
			show(); // the value that holds
			return;
		}
		const std::map<std::string, std::string> labels = {{"releaseVelocity", "Off Velocity"}};
		std::string label = labels.count(field) != 0 ? labels.at(field) : field;
		label.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(label.front())));
		// A drag, wheel turns or a held arrow key on the velocity: one undo step for each selection, as for the events.
		std::vector<uint64_t> ids;
		ids.reserve(m_notes.size());
		for (const Note& note : m_notes)
			ids.push_back(note.id);
		const std::string key = stepKey(m_stepOwner, "notes", field);
		m_binding.sendStep(
			gesture, key, "Change " + label, std::move(commands),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && !result.ok())
					m_loads = 0; // refused: the next update shows the notes as they are again
			},
			shown, mergeName(key, ids));
	}

	void InfoLine::edit(const std::string& field, const std::string& text, float number, int gesture)
	{
		const TimeContext context = projectTime(m_display.project());
		const double rate = context.sampleRate;
		const Json& first = m_events.front();
		std::vector<uint64_t> ids;
		for (const Json& event : m_events)
			ids.push_back(event.at("id").get<uint64_t>());
		const auto seconds = [rate](const Json& event, const char* key) { return event.at(key).get<double>() / rate; };
		Json commands = Json::array();
		Json shown = Json::object(); // what the volume, transpose and velocity fields show for each event changed

		// Start, end and length: where each event's start or end goes, as event.move or event.resize takes it; ticks
		// in bars and beats, exact on musical tracks.
		const auto shift = [this, &context, rate](const Json& event, const char* key, double to)
		{
			if (m_format == TimeFormat::BarsBeats)
				return Json{{"ticks",
					context.tempo.toTicks(clampPositionSeconds(to, rate)) -
						event.at(std::string(key) + "Ticks").get<int64_t>()}};
			return Json{{"delta", positionSamples(to, rate) - event.at(key).get<int64_t>()}};
		};
		std::function<std::optional<double>(const Json&)> target; // where the edge goes, in seconds
		const char* edge = "end";
		if (field == "start" || field == "end")
		{
			const std::optional<double> typed = parseTime(m_format, text, context);
			target = [typed](const Json&) { return typed; };
			edge = field == "start" ? "start" : "end";
		}
		else if (field == "length")
		{
			target = [this, &text, &context, &seconds](const Json& event) -> std::optional<double>
			{
				const double start = seconds(event, "start");
				const std::optional<double> length = parseLength(m_format, text, start, context);
				return length ? std::optional<double>(start + *length) : std::nullopt;
			};
		}
		if (target)
		{
			const auto command = [edge](Json parameters, Json events)
			{
				parameters["events"] = std::move(events);
				if (std::string(edge) == "start")
					return Json{{"command", "event.move"}, {"parameters", std::move(parameters)}};
				parameters["edge"] = "end";
				return Json{{"command", "event.resize"}, {"parameters", std::move(parameters)}};
			};
			if (m_absolute)
			{
				for (const Json& event : m_events)
					if (const std::optional<double> to = target(event))
						commands.push_back(command(shift(event, edge, *to), Json::array({event.at("id")})));
			}
			else if (const std::optional<double> to = target(first))
			{
				commands.push_back(command(shift(first, edge, *to), ids));
			}
		}
		else if (field == "name")
		{
			// MIDI and audio parts have names of their own; audio events show their clip's.
			Json parts = Json::array();
			for (const Json& event : m_events)
				if (event.at("kind") != "audio")
					parts.push_back(event.at("id"));
			if (!parts.empty())
				commands.push_back({{"command", "event.set"}, {"parameters", {{"events", parts}, {"name", text}}}});
		}
		else if (field == "volume")
		{
			// Each event's volume from what the field would show for it while the project is behind (#1632).
			const auto base = [this](uint64_t id) { return fieldNumber("events", "volume", id, volumeOf(id)); };
			const double change = number - base(first.at("id").get<uint64_t>());
			for (const Json& event : m_events)
				if (event.at("kind") == "audio")
				{
					const uint64_t id = event.at("id").get<uint64_t>();
					const double volume = std::clamp(m_absolute ? number : base(id) + change, g_minVolume, g_maxVolume);
					commands.push_back(
						{{"command", "audioEvent.set"}, {"parameters", {{"event", id}, {"volume", volume}}}});
					shown[std::to_string(id)] = widgets::format(static_cast<float>(volume));
				}
		}
		else if (field == "transpose" || field == "velocity")
		{
			// Relative to the first one's value, or with Ctrl+Enter the same for all: a MIDI part's from -127 to 127,
			// an audio event's transpose from -24 to 24 (#1555).
			const auto typed = static_cast<int>(std::lround(number));
			// Each one's value from what the field would show for it while the project is behind (#1632).
			const auto shownOf = [this, &field](const Json& event)
			{
				return static_cast<int>(std::lround(fieldNumber("events", field, event.at("id").get<uint64_t>(),
					static_cast<double>(partValue(event, field.c_str())))));
			};
			const int change = typed - shownOf(first);
			for (const Json& event : m_events)
			{
				const bool midi = event.at("kind") == "midi";
				if (!midi && (event.at("kind") != "audio" || field != "transpose"))
					continue;
				const uint64_t id = event.at("id").get<uint64_t>();
				const int own = shownOf(event);
				const int limit = limitOf(event);
				const int value = std::clamp(m_absolute ? typed : own + change, -limit, limit);
				// During a gesture the project may not have its earlier steps yet: a drag back to where it
				// started still sends that value.
				if (value != own || gesture != 0)
					commands.push_back({{"command", midi ? "midiPart.set" : "audioEvent.set"},
						{"parameters", {{midi ? "part" : "event", id}, {field, value}}}});
				shown[std::to_string(id)] = widgets::format(static_cast<float>(value));
			}
		}
		else if (field == "rootKey" && text == g_differentRootKeys)
		{
			// The events' root keys differ, as shown: nothing chosen.
		}
		else if (field == "rootKey" || field == "globalTranspose")
		{
			const std::string value =
				field == "rootKey" ? text : std::string(number != 0.0F ? "independent" : "follow");
			for (const Json& event : m_events)
			{
				const bool midi = event.at("kind") == "midi";
				const auto id = event.at("id").get<uint64_t>();
				if (event.at("kind") == "audioPart")
				{
					// An audio part has no root key of its own: its events get it (#1555).
					for (const Json* audio : eventsOf(id))
						if (audio->value(field, std::string()) != value)
							commands.push_back({{"command", "audioEvent.set"},
								{"parameters", {{"event", audio->at("id")}, {field, value}}}});
				}
				else if (const Json* item = transposableOf(event);
					item != nullptr && item->value(field, std::string()) != value)
				{
					commands.push_back({{"command", midi ? "midiPart.set" : "audioEvent.set"},
						{"parameters", {{midi ? "part" : "event", id}, {field, value}}}});
				}
			}
		}
		else if (field == "mute" || field == "lock")
		{
			commands.push_back({{"command", "event.set"}, {"parameters", {{"events", ids}, {field, number != 0.0F}}}});
		}
		if (commands.empty())
		{
			show(); // the value that holds
			return;
		}
		const std::map<std::string, std::string> labels = {
			{"rootKey", "Root Key"}, {"globalTranspose", "Global Transpose"}};
		std::string label = labels.count(field) != 0 ? labels.at(field) : field;
		label.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(label.front())));
		// A drag, wheel turns or a held arrow key on the volume: one undo step, for each selection of its own, also
		// when another one shows during the gesture (mergeName).
		const std::string key = stepKey(m_stepOwner, "events", field);
		m_binding.sendStep(
			gesture, key, "Change " + label, std::move(commands),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && !result.ok())
					m_loads = 0; // refused: the next update shows the events as they are again
			},
			shown, mergeName(key, ids));
	}
}
