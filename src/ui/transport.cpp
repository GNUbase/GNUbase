// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/transport.h"

#include "platform/log.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr const char* g_formatsKey = "ui.timeFormats";
		constexpr std::array<Rml::EventId, 2> g_events{Rml::EventId::Click, Rml::EventId::Change};

		bool inside(const Rml::Element* element, const Rml::Element* ancestor)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element == ancestor)
					return true;
			return false;
		}

		// The element or the nearest one around it with the attribute, up to the document.
		Rml::Element* withAttribute(Rml::Element* element, const std::string& attribute)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->HasAttribute(attribute))
					return element;
			return nullptr;
		}

		std::string attribute(const Rml::Element& element, const std::string& name, const std::string& fallback = {})
		{
			return element.GetAttribute<Rml::String>(name, fallback);
		}

		void setChecked(Rml::Element& element, bool checked)
		{
			if (checked && !element.HasAttribute("checked"))
				element.SetAttribute("checked", "");
			else if (!checked && element.HasAttribute("checked"))
				element.RemoveAttribute("checked");
		}

		// A <textdisplay> shows its value attribute; other elements show their text.
		void setText(Rml::Element& element, const std::string& text)
		{
			if (element.GetTagName() == "textdisplay")
			{
				if (attribute(element, "value") != text)
					element.SetAttribute("value", text);
			}
			else if (element.GetInnerRML() != Rml::StringUtilities::EncodeRml(text))
			{
				element.SetInnerRML(Rml::StringUtilities::EncodeRml(text));
			}
		}

		// "3/4"; nothing for text that isn't a signature.
		std::optional<std::pair<int, int>> parseSignature(const std::string& text)
		{
			const size_t slash = text.find('/');
			if (slash == std::string::npos)
				return std::nullopt;
			const std::optional<float> numerator = widgets::parse(text.substr(0, slash));
			const std::optional<float> denominator = widgets::parse(text.substr(slash + 1));
			if (!numerator || !denominator || *numerator != std::floor(*numerator) ||
				*denominator != std::floor(*denominator))
				return std::nullopt;
			return std::make_pair(static_cast<int>(*numerator), static_cast<int>(*denominator));
		}
	}

	TransportControls::TransportControls(
		engine::Engine& engine, EngineBinding& binding, Rml::Element& root, RunCommand run)
		: m_engine(engine), m_binding(binding), m_run(std::move(run)), m_root(root.GetObserverPtr()),
		  m_formats{{"primary", TimeFormat::BarsBeats}, {"secondary", TimeFormat::Timecode},
			  {"timeDisplay", TimeFormat::BarsBeats}}
	{
		if (Rml::ElementDocument* document = root.GetOwnerDocument())
			m_document = document->GetObserverPtr();
		listen(true);
		m_subscriptions.push_back(
			m_binding.on("transport.changed", [this](const engine::Event& event) { followTransport(event.data); }));
		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event, [this](const engine::Event&) { requestProject(); }));
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto formats = values.find(g_formatsKey); formats != values.end())
					followFormats(*formats);
			}));
		const std::weak_ptr<bool> alive = m_alive;
		requestProject();
		m_binding.send("transport.get", Json::object(),
			[this, alive](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					followTransport(result.value);
			});
		m_binding.send("settings.get", {{"key", g_formatsKey}},
			[this, alive](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					followFormats(result.value.at("value"));
			});
		show();
	}

	TransportControls::~TransportControls()
	{
		listen(false);
	}

	void TransportControls::listen(bool on)
	{
		Rml::Element* document = m_document.get();
		if (document == nullptr)
			return;
		for (const Rml::EventId id : g_events)
			if (on)
				document->AddEventListener(id, this);
			else
				document->RemoveEventListener(id, this);
	}

	std::vector<Rml::Element*> TransportControls::all(const std::string& selector) const
	{
		Rml::ElementList found;
		if (Rml::Element* root = m_root.get())
			root->QuerySelectorAll(found, selector);
		return found;
	}

	bool TransportControls::edit(const std::string& field)
	{
		const bool time = field != "tempo" && field != "signature";
		for (Rml::Element* element : all(time ? "[time=" + field + "]" : "[field=" + field + "]"))
			if (element->IsVisible(true) && (!time || attribute(*element, "format", "primary") == "primary"))
			{
				element->DispatchEvent(Rml::EventId::Dblclick, Rml::Dictionary());
				return true;
			}
		return false;
	}

	TimeFormat TransportControls::format(const std::string& slot) const
	{
		const auto found = m_formats.find(slot);
		return found == m_formats.end() ? TimeFormat::BarsBeats : found->second;
	}

	void TransportControls::setFormat(const std::string& slot, TimeFormat format)
	{
		m_formats[slot] = format;
		Json formats = Json::object();
		for (const auto& [name, value] : m_formats)
			formats[name] = timeFormatName(value);
		m_formatsSaved = formats;
		m_binding.send("settings.set", {{"key", g_formatsKey}, {"value", formats}});
		show();
	}

	void TransportControls::exchangeFormats()
	{
		const TimeFormat primary = format("primary");
		m_formats["primary"] = format("secondary");
		setFormat("secondary", primary);
	}

	void TransportControls::followFormats(const Json& formats)
	{
		if (!formats.is_object() || formats == m_formatsSaved)
			return;
		for (const auto& [slot, name] : formats.items())
			if (name.is_string())
				if (const std::optional<TimeFormat> format = timeFormatFromName(name.get<std::string>()))
					m_formats[slot] = *format;
		show();
	}

	void TransportControls::requestProject()
	{
		if (m_loading)
		{
			m_stale = true; // asks again when this answer is in
			return;
		}
		m_loading = true;
		m_binding.send("project.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_loading = false;
				if (result.ok() && !readTimeContext(result.value, m_time))
					logging::warning("The transport controls can't read the project's tempo or signatures");
				if (std::exchange(m_stale, false))
					requestProject();
				show();
			});
	}

	void TransportControls::followTransport(const Json& status)
	{
		m_status = status;
		m_position = status.at("position").get<int64_t>();
		if (status.at("playing").get<bool>() && !m_playback)
			m_playback = m_binding.watch<int64_t>([this] { return m_engine.playbackPosition(); },
				[this](const int64_t& sample)
				{
					m_position = sample;
					showTime();
				});
		else if (!status.at("playing").get<bool>())
			m_playback.reset();
		show();
	}

	int64_t TransportControls::samples(double seconds) const
	{
		return std::max<int64_t>(0, std::llround(seconds * m_time.sampleRate));
	}

	void TransportControls::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* root = m_root.get();
		Rml::Element* target = event.GetTargetElement();
		if (root == nullptr || target == nullptr)
			return;
		if (event == Rml::EventId::Click)
		{
			if (Rml::Element* item = withAttribute(target, "slot"); item != nullptr && item->HasAttribute("format"))
			{
				if (const std::optional<TimeFormat> format = timeFormatFromName(attribute(*item, "format")))
					setFormat(attribute(*item, "slot"), *format);
				return;
			}
			Rml::Element* button = withAttribute(target, "command");
			if (button == nullptr || !inside(button, root) || button->HasAttribute("disabled"))
				return;
			const Json parameters = Json::parse(attribute(*button, "parameters", "{}"), nullptr, false);
			if (parameters.is_discarded())
				logging::warning(
					"The transport control {} has parameters that aren't JSON", attribute(*button, "command"));
			else
				m_run(attribute(*button, "command"), parameters);
			return;
		}
		// A display or field that the user changed, rather than the text field inside it.
		if (inside(target, root) && (target->HasAttribute("time") || target->HasAttribute("field")))
			changed(*target, event.GetParameter<Rml::String>("value", ""));
	}

	void TransportControls::changed(Rml::Element& element, const std::string& text)
	{
		const std::string field = attribute(element, "field");
		if (field == "tempo")
		{
			if (const std::optional<float> tempo = widgets::parse(text))
				m_binding.send("project.set", {{"tempo", *tempo}});
			return;
		}
		if (field == "signature")
		{
			if (const auto signature = parseSignature(text))
				m_binding.send("project.set",
					{{"signature", {{"numerator", signature->first}, {"denominator", signature->second}}}});
			show(); // a refused signature shows the project's again
			return;
		}
		const std::string time = attribute(element, "time");
		const TimeFormat format = this->format(attribute(element, "format", "primary"));
		const double left = static_cast<double>(m_status.value("left", int64_t{0})) / m_time.sampleRate;
		const std::optional<double> seconds =
			time == "length" ? parseLength(format, text, left, m_time) : parseTime(format, text, m_time);
		if (!seconds)
			show(); // the text typed goes
		else if (time == "position")
			m_binding.send("transport.locate", {{"position", samples(*seconds)}});
		else if (time == "left" || time == "right")
			m_binding.send("transport.set", {{time, samples(*seconds)}});
		else if (time == "length")
			m_binding.send("transport.set", {{"right", samples(left + *seconds)}});
	}

	void TransportControls::show()
	{
		if (!alive())
			return;
		for (Rml::Element* element : all("[state]"))
		{
			const auto value = m_status.find(attribute(*element, "state"));
			setChecked(*element, value != m_status.end() && value->is_boolean() && value->get<bool>());
		}
		const time::TempoPoint& tempo = m_time.tempo.points().front();
		for (Rml::Element* element : all("[field=tempo]"))
			if (attribute(*element, "value") != widgets::format(static_cast<float>(tempo.bpm)))
				element->SetAttribute("value", widgets::format(static_cast<float>(tempo.bpm)));
		const time::TimeSignature signature = m_time.signatures.signatureAt(0);
		for (Rml::Element* element : all("[field=signature]"))
			setText(*element, std::to_string(signature.numerator) + "/" + std::to_string(signature.denominator));
		if (Rml::Element* document = m_document.get())
		{
			Rml::ElementList items;
			document->QuerySelectorAll(items, "menuitem[slot]");
			for (Rml::Element* item : items)
				setChecked(*item, attribute(*item, "format") == timeFormatName(format(attribute(*item, "slot"))));
		}
		showTime();
	}

	void TransportControls::showTime()
	{
		if (!alive())
			return;
		const double rate = m_time.sampleRate;
		const double left = static_cast<double>(m_status.value("left", int64_t{0})) / rate;
		const double right = static_cast<double>(m_status.value("right", int64_t{0})) / rate;
		for (Rml::Element* element : all("[time]"))
		{
			const std::string time = attribute(*element, "time");
			const TimeFormat format = this->format(attribute(*element, "format", "primary"));
			if (time == "position")
				setText(*element, formatTime(format, static_cast<double>(m_position) / rate, m_time));
			else if (time == "left")
				setText(*element, formatTime(format, left, m_time));
			else if (time == "right")
				setText(*element, formatTime(format, right, m_time));
			else if (time == "length")
				setText(*element, formatLength(format, left, std::max(left, right), m_time));
		}
	}
}
