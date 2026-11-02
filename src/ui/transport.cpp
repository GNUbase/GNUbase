// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/transport.h"

#include "platform/log.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
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
		constexpr const char* g_timeDisplayKey = "ui.timeDisplay";
		constexpr std::array<Rml::EventId, 7> g_events{Rml::EventId::Click, Rml::EventId::Change,
			Rml::EventId::Mousedown, Rml::EventId::Mouseup, Rml::EventId::Mouseout, Rml::EventId::Dragstart,
			Rml::EventId::Dragend};
		constexpr int g_largestSize = 4;
		// Wheel turns on the tempo field that follow each other within this are one gesture.
		constexpr std::chrono::milliseconds g_wheelPause{400};
		// Rewind and Forward held: how long they wait before they wind, and how many steps a second they wind.
		constexpr std::chrono::milliseconds g_windDelay{300};
		constexpr double g_windSpeed = 4.0;

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
		engine::Engine& engine, EngineBinding& binding, Rml::Element& root, RunCommand run, ReleaseCommand release)
		: m_engine(engine), m_binding(binding), m_run(std::move(run)), m_release(std::move(release)),
		  m_root(root.GetObserverPtr()),
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
				if (const auto options = values.find(g_timeDisplayKey); options != values.end())
					followTimeDisplay(*options);
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
		m_binding.send("settings.get", {{"key", g_timeDisplayKey}},
			[this, alive](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					followTimeDisplay(result.value.at("value"));
			});
		show();
	}

	TransportControls::~TransportControls()
	{
		release();
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
		// Before the field or display under the mouse: the tempo field turns with the wheel itself.
		if (on)
			document->AddEventListener(Rml::EventId::Mousescroll, this, true);
		else
			document->RemoveEventListener(Rml::EventId::Mousescroll, this, true);
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
		m_binding.send("settings.set", {{"key", g_formatsKey}, {"value", formats}});
		show();
	}

	void TransportControls::exchangeFormats()
	{
		const TimeFormat primary = format("primary");
		m_formats["primary"] = format("secondary");
		setFormat("secondary", primary);
	}

	void TransportControls::toggleBeatCountOnly()
	{
		m_beatCountOnly = !m_beatCountOnly;
		saveTimeDisplay();
	}

	void TransportControls::resize(int steps)
	{
		m_size = std::clamp(m_size + steps, 0, g_largestSize);
		saveTimeDisplay();
		if (m_sized)
			m_sized(m_size);
	}

	void TransportControls::saveTimeDisplay()
	{
		m_binding.send("settings.set",
			{{"key", g_timeDisplayKey}, {"value", {{"beatCountOnly", m_beatCountOnly}, {"size", m_size}}}});
		show();
	}

	void TransportControls::followTimeDisplay(const Json& options)
	{
		if (!options.is_object())
			return;
		if (const auto beats = options.find("beatCountOnly"); beats != options.end() && beats->is_boolean())
			m_beatCountOnly = beats->get<bool>();
		if (const auto size = options.find("size"); size != options.end() && size->is_number_integer())
			m_size = std::clamp(size->get<int>(), 0, g_largestSize);
		show();
		if (m_sized)
			m_sized(m_size);
	}

	void TransportControls::followFormats(const Json& formats)
	{
		if (!formats.is_object())
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
				else
					m_sent.reset(); // read after the tempo the gesture set
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
		const Rml::Element* root = m_root.get();
		Rml::Element* target = event.GetTargetElement();
		if (root == nullptr || target == nullptr)
			return;
		if (event == Rml::EventId::Mousescroll)
		{
			Rml::Element* control = withAttribute(target, "field");
			if (control == nullptr)
				control = withAttribute(target, "time");
			if (control == nullptr || !inside(control, root) || control->IsClassSet("editing"))
				return;
			if (control->HasAttribute("time"))
			{
				wheel(*control, event.GetParameter("wheel_delta_y", 0.0F));
				event.StopPropagation(); // nothing around it scrolls
			}
			else if (attribute(*control, "field") == "tempo")
			{
				wheelTempo();
			}
			return;
		}
		if (event == Rml::EventId::Mousedown)
		{
			const Rml::Element* button = withAttribute(target, "hold");
			if (event.GetParameter("button", 0) == 0 && button != nullptr && inside(button, root) &&
				!button->HasAttribute("disabled") && button->HasAttribute("command"))
			{
				release();
				m_holding = attribute(*button, "command");
				m_run(m_holding, Json::object());
			}
			return;
		}
		if (event == Rml::EventId::Mouseup)
		{
			release();
			return;
		}
		if (event == Rml::EventId::Mouseout)
		{
			// The mouse leaving the button held, also out of the window, ends it as the mouse button going up would.
			if (m_holding.empty())
				return;
			Rml::Context* context = root->GetContext();
			Rml::Element* hover = context != nullptr ? context->GetHoverElement() : nullptr;
			const Rml::Element* button = hover != nullptr ? withAttribute(hover, "hold") : nullptr;
			if (button == nullptr || attribute(*button, "command") != m_holding)
				release();
			return;
		}
		if (event == Rml::EventId::Dragstart || event == Rml::EventId::Dragend)
		{
			const Rml::Element* field = withAttribute(target, "field");
			if (field == nullptr || !inside(field, root) || attribute(*field, "field") != "tempo")
				return;
			if (event == Rml::EventId::Dragstart)
			{
				m_gesture = Gesture::Drag; // wheel turns just before go into the drag's undo step
				m_wheelPause.reset();
			}
			else
			{
				endTempoGesture();
			}
			return;
		}
		if (event == Rml::EventId::Click)
		{
			if (const Rml::Element* item = withAttribute(target, "slot");
				item != nullptr && item->HasAttribute("format"))
			{
				if (const std::optional<TimeFormat> format = timeFormatFromName(attribute(*item, "format")))
					setFormat(attribute(*item, "slot"), *format);
				return;
			}
			const Rml::Element* button = withAttribute(target, "command");
			if (button == nullptr || !inside(button, root) || button->HasAttribute("disabled") ||
				button->HasAttribute("hold"))
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
			const std::optional<float> tempo = widgets::parse(text);
			if (tempo && m_gesture != Gesture::None)
				m_tempo = tempo; // set at the gesture's end
			else if (tempo)
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

	void TransportControls::wheel(Rml::Element& element, float delta)
	{
		// Up is later, by as many steps as the wheel turned, at least one. A scroll sideways, as touchpads and tilt
		// wheels send, has no vertical delta and moves nothing.
		if (delta == 0.0F)
			return;
		const long turned = std::lround(-delta);
		long steps = turned;
		if (steps == 0)
			steps = delta < 0.0F ? 1 : -1;
		const std::string time = attribute(element, "time");
		const double rate = m_time.sampleRate;
		const double left = static_cast<double>(m_status.value("left", int64_t{0})) / rate;
		const double right = static_cast<double>(m_status.value("right", int64_t{0})) / rate;
		double from = right; // the range's length moves the right locator
		if (time == "position")
			from = static_cast<double>(m_position) / rate;
		else if (time == "left")
			from = left;
		double to = from + static_cast<double>(steps); // by seconds in seconds and samples
		switch (format(attribute(element, "format", "primary")))
		{
		case TimeFormat::BarsBeats:
		{
			const time::Ticks at = m_time.tempo.toTicks(from);
			const time::Ticks beat =
				time::g_ticksPerQuarter * 4 / std::max(m_time.signatures.signatureAt(at).denominator, 1);
			to = m_time.tempo.toSeconds(std::max<time::Ticks>(0, at + (steps * beat)));
			break;
		}
		case TimeFormat::Timecode:
			to = from +
				(static_cast<double>(steps) * (m_time.frameRate.pullDown ? 1.001 : 1.0) /
					std::max(m_time.frameRate.framesPerSecond, 1));
			break;
		case TimeFormat::UserFrames:
			to = from + (static_cast<double>(steps) / std::max(m_time.userFramesPerSecond, 1));
			break;
		case TimeFormat::Seconds:
		case TimeFormat::Samples:
			break;
		}
		// The next turn goes on from here, even before transport.changed says so.
		if (time == "position")
		{
			m_position = samples(to);
			m_binding.send("transport.locate", {{"position", m_position}});
		}
		else
		{
			const std::string locator = time == "left" ? "left" : "right";
			m_status[locator] = samples(time == "length" ? std::max(to, left) : to);
			m_binding.send("transport.set", {{locator, m_status[locator]}});
		}
	}

	void TransportControls::wheelTempo()
	{
		if (m_gesture == Gesture::Drag)
			return;
		m_gesture = Gesture::Wheel;
		m_wheeled = std::chrono::steady_clock::now();
		if (!m_wheelPause)
			m_wheelPause =
				m_binding.watch<bool>([this] { return std::chrono::steady_clock::now() - m_wheeled >= g_wheelPause; },
					[this](const bool& paused)
					{
						if (paused)
							endTempoGesture();
					});
	}

	void TransportControls::endTempoGesture()
	{
		m_gesture = Gesture::None;
		m_wheelPause.reset();
		if (const std::optional<float> tempo = std::exchange(m_tempo, std::nullopt))
		{
			// Until the project's tempo is read again, the field keeps what it set rather than the tempo before.
			m_sent = tempo;
			m_binding.send("project.set", {{"tempo", *tempo}});
			requestProject();
		}
		show();
	}

	void TransportControls::release()
	{
		if (!m_holding.empty() && m_release)
			m_release(m_holding);
		m_holding.clear();
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
		const std::string tempo =
			widgets::format(m_sent.value_or(static_cast<float>(m_time.tempo.points().front().bpm)));
		for (Rml::Element* element : all("[field=tempo]"))
			if (m_gesture == Gesture::None && // during one, the field shows where the gesture took it
				attribute(*element, "value") != tempo)
				element->SetAttribute("value", tempo);
		const time::TimeSignature signature = m_time.signatures.signatureAt(0);
		for (Rml::Element* element : all("[field=signature]"))
			setText(*element, std::to_string(signature.numerator) + "/" + std::to_string(signature.denominator));
		if (Rml::Element* document = m_document.get())
		{
			Rml::ElementList items;
			document->QuerySelectorAll(items, "menuitem[slot]");
			for (Rml::Element* item : items)
				setChecked(*item, attribute(*item, "format") == timeFormatName(format(attribute(*item, "slot"))));
			items.clear();
			document->QuerySelectorAll(items, "[option=beatCountOnly]");
			for (Rml::Element* item : items)
				setChecked(*item, m_beatCountOnly);
		}
		for (Rml::Element* element : all("[format=timeDisplay]"))
			for (int size = 0; size <= g_largestSize; ++size)
				element->SetClass("size-" + std::to_string(size), size == m_size);
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
			if (time == "position" && m_beatCountOnly && attribute(*element, "format") == "timeDisplay")
			{
				const time::Ticks at = m_time.tempo.toTicks(static_cast<double>(m_position) / rate);
				setText(*element, std::to_string(m_time.signatures.toBarsBeats(at).beat + 1));
			}
			else if (time == "position")
			{
				setText(*element, formatTime(format, static_cast<double>(m_position) / rate, m_time));
			}
			else if (time == "left")
			{
				setText(*element, formatTime(format, left, m_time));
			}
			else if (time == "right")
			{
				setText(*element, formatTime(format, right, m_time));
			}
			else if (time == "length")
			{
				setText(*element, formatLength(format, left, std::max(left, right), m_time));
			}
		}
	}

	void Winding::start(double seconds)
	{
		m_step = seconds;
		m_last = Clock::now() + g_windDelay;
		m_binding.send("transport.locate", {{"by", seconds}});
	}

	bool Winding::step()
	{
		if (m_step == 0.0)
			return false;
		const Clock::time_point now = Clock::now();
		if (now > m_last)
		{
			const double wound = std::chrono::duration<double>(now - m_last).count() * g_windSpeed * m_step;
			m_last = now;
			m_binding.send("transport.locate", {{"by", wound}});
		}
		return true;
	}
}
