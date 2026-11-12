// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/infoline.h"

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
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// An audio event's volume, as audioEvent.set takes it.
		constexpr double g_minVolume = -144.0;
		constexpr double g_maxVolume = 6.0;

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
			// A MIDI part's transpose and velocity (p. 1107), added to the track's MIDI modifiers.
			R"(<div class="info-field" info-field="transpose" section="transpose" label="Transpose">)"
			R"(<span class="info-label">Transpose</span>)"
			R"(<valuefield field="transpose" min="-127" max="127" default="0" step="1" decimals="0"/>)"
			// What the project root key adds (p. 420), in brackets: the engine's rootKeyTranspose.
			R"(<span class="info-root-transpose" field="rootKeyTranspose"/></div>)"
			R"(<div class="info-field" info-field="velocity" section="velocity" label="Velocity">)"
			R"(<span class="info-label">Velocity</span>)"
			R"(<valuefield field="velocity" min="-127" max="127" default="0" step="1" decimals="0"/></div>)"
			// A MIDI part's root key and Global Transpose (pp. 417, 421, #168).
			R"(<div class="info-field" info-field="rootKey" section="rootKey" label="Root Key">)"
			R"(<span class="info-label">Root Key</span>)"
			R"(<select field="rootKey" title="Root Key"/></div>)"
			R"(<div class="info-field" info-field="globalTranspose" section="globalTranspose" label="Global Transpose">)"
			R"(<button type="toggle" field="globalTranspose")"
			R"( title="Global Transpose: Independent">Independent</button></div>)";

		// The root keys, as midiPart.set takes them.
		const widgets::Options& rootKeys()
		{
			static const widgets::Options keys = {{"", "-"}, {"C", "C"}, {"C#", "C#"}, {"D", "D"}, {"D#", "D#"},
				{"E", "E"}, {"F", "F"}, {"F#", "F#"}, {"G", "G"}, {"G#", "G#"}, {"A", "A"}, {"A#", "A#"}, {"B", "B"}};
			return keys;
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

		// What a field's gesture changes, as EngineBinding::sendStep names it: the info line's step owner, then the
		// field.
		std::string stepKey(const std::string& owner, const std::string& field)
		{
			return owner + field;
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
			  }))
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
				if (m_showsMarkers)
					editMarkers(field, text);
				else
					edit(field, text, number.value_or(0.0F));
				m_absolute = false;
			}
		}
		const std::vector<uint64_t>& selection = m_display.selection();
		const TimeFormat format = m_display.rulerFormat();
		std::vector<uint64_t> markers = m_markerSource ? m_markerSource() : std::vector<uint64_t>();
		const bool markersChanged = markers != m_markerSelection;
		if ((selection == m_selection && m_display.loads() == m_loads && format == m_format && !markersChanged) ||
			!m_root->IsVisible(true)) // hidden: it asks once it shows
			return;
		// Whichever selection changed last shows; a marker clicked on the marker track wins.
		if (selection != m_selection && !selection.empty())
			m_showsMarkers = false;
		if (markersChanged)
			m_showsMarkers = !markers.empty();
		m_markerSelection = std::move(markers);
		m_markers.clear();
		if (m_showsMarkers)
			for (const MarkerItem& marker : readMarkers(m_display.project()).markers)
				if (std::find(m_markerSelection.begin(), m_markerSelection.end(), marker.id) != m_markerSelection.end())
					m_markers.push_back(marker);
		m_selection = selection;
		m_loads = m_display.loads();
		m_format = format;
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

	int InfoLine::partValue(uint64_t part, const char* key) const
	{
		const Json& project = m_display.project();
		if (!project.is_object())
			return 0;
		for (const Json& midi : project.at("midiParts"))
			if (midi.at("id") == part)
				return midi.value(key, 0);
		return 0;
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

	void InfoLine::show()
	{
		if (!m_root)
			return;
		Rml::Element& root = *m_root.get();
		setClass(root, "markers", m_showsMarkers);
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
		for (const char* field : {"end", "length", "mute", "lock"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", false);
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
		// Not while the project is older than the field's own steps (#716).
		if (Rml::Element* volume = root.QuerySelector("valuefield[field=volume]");
			volume != nullptr && audio && !m_binding.ahead(stepKey(m_stepOwner, "volume"), m_display.read()))
			setValue(*volume, widgets::format(static_cast<float>(volumeOf(first.at("id").get<uint64_t>()))));
		for (const char* field : {"mute", "lock"})
			if (Rml::Element* button = root.QuerySelector("button[field=" + std::string(field) + "]"))
				setFlag(*button, "checked", first.at(field).get<bool>());
		// A MIDI part's transpose and velocity.
		const bool midi = first.at("kind") == "midi";
		for (const char* field : {"transpose", "velocity"})
		{
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", !midi);
			if (Rml::Element* value = root.QuerySelector("valuefield[field=" + std::string(field) + "]");
				value != nullptr && midi && !m_binding.ahead(stepKey(m_stepOwner, field), m_display.read()))
				setValue(*value, widgets::format(static_cast<float>(partValue(first.at("id").get<uint64_t>(), field))));
		}
		const Json* part = midi ? partOf(first.at("id").get<uint64_t>()) : nullptr;
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
		if (Rml::Element* key = root.QuerySelector("select[field=rootKey]"))
			widgets::fill(*key, rootKeys(), part->value("rootKey", std::string()));
		if (Rml::Element* button = root.QuerySelector("button[field=globalTranspose]"))
			setFlag(*button, "checked", part->value("globalTranspose", std::string()) == "independent");
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
		if (target == nullptr || !target->HasAttribute("field") || !shows() || m_dropping)
			return;
		const auto field = target->GetAttribute<Rml::String>("field", "");
		m_absolute = m_absoluteTake && m_absoluteTake->first == field;
		m_absoluteTake.reset();
		if (m_showsMarkers)
			editMarkers(field, event.GetParameter<Rml::String>("value", ""));
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
		setLabel(root, "name", "Description");
		setLabel(root, "start", "Position");
		for (const char* field : {"end", "length"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", !first.cycle);
		for (const char* field : {"volume", "mute", "lock", "transpose", "velocity", "rootKey", "globalTranspose"})
			if (Rml::Element* shown = root.QuerySelector("[info-field=" + std::string(field) + "]"))
				setClass(*shown, "unavailable", true);
	}

	// As for events: relative to the first marker's value, or with Ctrl+Enter the value typed for every one. A new
	// position keeps a cycle marker's length; end and length change the cycle markers.
	void InfoLine::editMarkers(const std::string& field, const std::string& text)
	{
		const ProjectMarkers project = readMarkers(m_display.project());
		const TimeContext& context = project.time;
		const MarkerItem& first = m_markers.front();
		const auto cycle =
			std::find_if(m_markers.begin(), m_markers.end(), [](const MarkerItem& m) { return m.cycle; });
		Json commands = Json::array();
		const auto set = [&commands, &project](const MarkerItem& marker, double start, double end)
		{
			const auto track = std::find_if(project.tracks.begin(), project.tracks.end(),
				[&marker](const ProjectMarkers::Track& each) { return each.id == marker.track; });
			if (track == project.tracks.end() || start < 0.0 || (marker.cycle && end <= start))
				return;
			Json parameters = {{"marker", marker.id}};
			if (start != marker.start)
				parameters["position"] = project.position(*track, start);
			if (marker.cycle && (end != marker.end || start != marker.start))
				parameters["end"] = project.position(*track, end);
			if (parameters.size() > 1)
				commands.push_back({{"command", "marker.set"}, {"parameters", std::move(parameters)}});
		};
		if (field == "name")
		{
			for (const MarkerItem& marker : m_markers)
				if (marker.name != text)
					commands.push_back(
						{{"command", "marker.set"}, {"parameters", {{"marker", marker.id}, {"name", text}}}});
		}
		else if (field == "start")
		{
			if (const std::optional<double> typed = parseTime(m_format, text, context))
				for (const MarkerItem& marker : m_markers)
				{
					const double start = m_absolute ? *typed : marker.start + (*typed - first.start);
					set(marker, start, start + (marker.end - marker.start));
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
					if (marker.cycle)
					{
						const double change = *length - (cycle->end - cycle->start);
						const double to = m_absolute ? *length : marker.end - marker.start + change;
						set(marker, marker.start,
							field == "end" && m_absolute ? cycle->start + *length : marker.start + to);
					}
		}
		if (commands.empty())
		{
			show(); // the value that holds
			return;
		}
		const std::map<std::string, const char*> labels = {
			{"name", "Description"}, {"start", "Position"}, {"end", "End"}, {"length", "Length"}};
		const std::string description = std::string("Change Marker ") + labels.at(field);
		m_binding.send("history.transaction", {{"description", description}, {"commands", std::move(commands)}});
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
			for (const Json& event : m_events)
				if (event.at("kind") == "midi")
					commands.push_back(
						{{"command", "midiPart.set"}, {"parameters", {{"part", event.at("id")}, {"name", text}}}});
		}
		else if (field == "volume")
		{
			const double change = number - volumeOf(first.at("id").get<uint64_t>());
			for (const Json& event : m_events)
				if (event.at("kind") == "audio")
				{
					const uint64_t id = event.at("id").get<uint64_t>();
					const double volume = m_absolute ? number : volumeOf(id) + change;
					commands.push_back({{"command", "audioEvent.set"},
						{"parameters", {{"event", id}, {"volume", std::clamp(volume, g_minVolume, g_maxVolume)}}}});
				}
		}
		else if (field == "transpose" || field == "velocity")
		{
			// Relative to the first part's value, or with Ctrl+Enter the same for all; each from -127 to 127.
			const auto typed = static_cast<int>(std::lround(number));
			const int change = typed - partValue(first.at("id").get<uint64_t>(), field.c_str());
			for (const Json& event : m_events)
				if (event.at("kind") == "midi")
				{
					const uint64_t id = event.at("id").get<uint64_t>();
					const int own = partValue(id, field.c_str());
					const int value = std::clamp(m_absolute ? typed : own + change, -127, 127);
					// During a gesture the project may not have its earlier steps yet: a drag back to where it
					// started still sends that value.
					if (value != own || gesture != 0)
						commands.push_back(
							{{"command", "midiPart.set"}, {"parameters", {{"part", id}, {field, value}}}});
				}
		}
		else if (field == "rootKey" || field == "globalTranspose")
		{
			const std::string value =
				field == "rootKey" ? text : std::string(number != 0.0F ? "independent" : "follow");
			for (const Json& event : m_events)
				if (const Json* part = event.at("kind") == "midi" ? partOf(event.at("id").get<uint64_t>()) : nullptr;
					part != nullptr && part->value(field, std::string()) != value)
					commands.push_back(
						{{"command", "midiPart.set"}, {"parameters", {{"part", event.at("id")}, {field, value}}}});
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
		// A drag, wheel turns or a held arrow key on the volume: one undo step.
		m_binding.sendStep(gesture, stepKey(m_stepOwner, field), "Change " + label, std::move(commands),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && !result.ok())
					m_loads = 0; // refused: the next update shows the events as they are again
			});
	}
}
