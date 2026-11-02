// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/infoline.h"

#include "ui/widgetutils.h"

#include <RmlUi/Core/Element.h>
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

		const char* const g_rml =
			R"(<span class="info-empty">No Object Selected</span>)"
			R"(<div class="info-field" info-field="name"><span class="info-label">Name</span>)"
			R"(<textdisplay class="display" field="name"/></div>)"
			R"(<div class="info-field" info-field="start"><span class="info-label">Start</span>)"
			R"(<textdisplay class="display" field="start"/></div>)"
			R"(<div class="info-field" info-field="end"><span class="info-label">End</span>)"
			R"(<textdisplay class="display" field="end"/></div>)"
			R"(<div class="info-field" info-field="length"><span class="info-label">Length</span>)"
			R"(<textdisplay class="display" field="length"/></div>)"
			R"(<div class="info-field" info-field="volume"><span class="info-label">Volume</span>)"
			R"(<valuefield field="volume" min="-144" max="6" default="0" step="0.5" decimals="2")"
			R"( unit="dB"/></div>)"
			R"(<div class="info-field" info-field="mute"><button type="toggle" field="mute")"
			R"( title="Mute">Mute</button></div>)"
			R"(<div class="info-field" info-field="lock"><button type="toggle" field="lock")"
			R"( title="Lock">Lock</button></div>)";

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

		void setFlag(Rml::Element& element, const char* name, bool set)
		{
			if (set && !element.HasAttribute(name))
				element.SetAttribute(name, "");
			else if (!set && element.HasAttribute(name))
				element.RemoveAttribute(name);
		}
	}

	InfoLine::InfoLine(EventDisplay& display, EngineBinding& binding, Rml::Element& root)
		: m_display(display), m_binding(binding), m_root(root.GetObserverPtr())
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
		// Ctrl+Enter took the value the field had, so it sent no change: that value goes to all selected the same.
		if (m_absoluteTake)
		{
			const auto [field, text] = *std::exchange(m_absoluteTake, std::nullopt);
			const std::optional<float> number = widgets::parse(text);
			if (!m_events.empty() && (field != "volume" || number))
			{
				m_absolute = true;
				edit(field, text, number.value_or(0.0F));
				m_absolute = false;
			}
		}
		const std::vector<uint64_t>& selection = m_display.selection();
		const TimeFormat format = m_display.rulerFormat();
		if (selection == m_selection && m_display.loads() == m_loads && format == m_format)
			return;
		m_selection = selection;
		m_loads = m_display.loads();
		m_format = format;
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

	void InfoLine::show()
	{
		if (!m_root)
			return;
		Rml::Element& root = *m_root.get();
		setClass(root, "empty", m_events.empty());
		setClass(root, "multiple", m_events.size() > 1);
		if (m_events.empty())
			return;
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
			setClass(*volume, "hidden", !audio);
		if (Rml::Element* volume = root.QuerySelector("valuefield[field=volume]"); volume != nullptr && audio)
			setValue(*volume, std::to_string(volumeOf(first.at("id").get<uint64_t>())));
		for (const char* field : {"mute", "lock"})
			if (Rml::Element* button = root.QuerySelector("button[field=" + std::string(field) + "]"))
				setFlag(*button, "checked", first.at(field).get<bool>());
	}

	void InfoLine::ProcessEvent(Rml::Event& event)
	{
		const Rml::Element* target = event.GetTargetElement();
		if (event == Rml::EventId::Keydown)
		{
			// Ctrl (Cmd) with Enter in the text field of a display or value field: an absolute take, which the field's
			// change applies, or update when the value didn't change and no change comes.
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
		if (target == nullptr || !target->HasAttribute("field") || m_events.empty())
			return;
		const auto field = target->GetAttribute<Rml::String>("field", "");
		m_absolute = m_absoluteTake && m_absoluteTake->first == field;
		m_absoluteTake.reset();
		if (target->GetTagName() == "button")
			edit(field, {}, event.GetParameter<bool>("value", false) ? 1.0F : 0.0F);
		else
			edit(field, event.GetParameter<Rml::String>("value", ""), event.GetParameter<float>("value", 0.0F));
		m_absolute = false;
	}

	void InfoLine::edit(const std::string& field, const std::string& text, float number)
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
				return Json{{"ticks", context.tempo.toTicks(to) - event.at(std::string(key) + "Ticks").get<int64_t>()}};
			return Json{{"delta", std::llround(to * rate) - event.at(key).get<int64_t>()}};
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
		else if (field == "mute" || field == "lock")
		{
			commands.push_back({{"command", "event.set"}, {"parameters", {{"events", ids}, {field, number != 0.0F}}}});
		}
		if (commands.empty())
		{
			show(); // the value that holds
			return;
		}
		std::string label = field;
		label.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(label.front())));
		m_binding.send("history.transaction", {{"description", "Change " + label}, {"commands", std::move(commands)}});
	}
}
