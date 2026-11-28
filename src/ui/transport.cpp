// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/transport.h"

#include "engine/time/tempomap.h"
#include "platform/log.h"
#include "ui/signaturetrack.h"
#include "ui/tempotrack.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr const char* g_formatsKey = "ui.timeFormats";
		constexpr const char* g_timeDisplayKey = "ui.timeDisplay";
		constexpr const char* g_tapModeKey = "transport.tapTempoMode";
		constexpr std::array<Rml::EventId, 7> g_events{Rml::EventId::Click, Rml::EventId::Change,
			Rml::EventId::Mousedown, Rml::EventId::Mouseup, Rml::EventId::Mouseout, Rml::EventId::Dragstart,
			Rml::EventId::Dragend};
		constexpr int g_largestSize = 4;
		// Wheel turns on the tempo field that follow each other within this are one gesture.
		constexpr std::chrono::milliseconds g_wheelPause{400};
		// Fractions of a wheel step on a time display that stay together: a pause this long forgets them.
		constexpr std::chrono::milliseconds g_fractionPause{1000};
		// Taps on Tap Tempo that follow each other within this are one run; the run's tempo is set when it ends.
		constexpr double g_tapPause = 2.0;
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

		// A <textdisplay> shows its value attribute; other elements show their text. Returns whether that changed.
		bool setText(Rml::Element& element, const std::string& text)
		{
			if (element.GetTagName() == "textdisplay")
			{
				if (attribute(element, "value") == text)
					return false;
				element.SetAttribute("value", text);
				return true;
			}
			const std::string encoded = Rml::StringUtilities::EncodeRml(text);
			if (element.GetInnerRML() == encoded)
				return false;
			element.SetInnerRML(encoded);
			return true;
		}

		// Throws std::invalid_argument for a control whose parameters aren't a JSON object, or whose command and
		// parameters check refuses.
		void checkControls(Rml::Element& root, const CheckCommand& check)
		{
			Rml::ElementList found;
			root.QuerySelectorAll(found, "[command]");
			if (root.HasAttribute("command"))
				found.insert(found.begin(), &root);
			for (const Rml::Element* control : found)
			{
				const std::string command = attribute(*control, "command");
				const std::string text = attribute(*control, "parameters", "{}");
				const Json parameters = Json::parse(text, nullptr, false);
				if (!parameters.is_object())
				{
					std::string message = "the parameters of the transport control for ";
					message.append(command).append(" aren't a JSON object: ").append(text);
					throw std::invalid_argument(message);
				}
				if (check)
					check(command, parameters);
			}
		}
	}

	TransportControls::TransportControls(engine::Engine& engine, EngineBinding& binding, Rml::Element& root,
		RunCommand run, ReleaseCommand release, const CheckCommand& check)
		: m_engine(engine), m_binding(binding), m_run(std::move(run)), m_release(std::move(release)),
		  m_root(root.GetObserverPtr()),
		  m_formats{{"primary", TimeFormat::BarsBeats}, {"secondary", TimeFormat::Timecode},
			  {"timeDisplay", TimeFormat::BarsBeats}}
	{
		// Before it listens to anything, so that a refused control leaves nothing behind.
		checkControls(root, check);
		if (Rml::ElementDocument* document = root.GetOwnerDocument())
			m_document = document->GetObserverPtr();
		listen(true);
		m_subscriptions.push_back(m_binding.on(
			"transport.changed", [this](const engine::Event& event) { followTransport(event.data, event.request); }));
		// Not after a step that changed only channels, which it doesn't show (#1881).
		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event,
				[this](const engine::Event& changed)
				{
					if (!changedOnly(changed, {"channel.changed"}))
						requestProject();
				}));
		m_subscriptions.push_back(m_binding.on("quantize.changed",
			[this](const engine::Event& event)
			{
				m_autoQuantize = event.data.value("autoQuantize", false);
				show();
			}));
		m_subscriptions.push_back(m_binding.on("project.reset", [this](const engine::Event&) { requestQuantize(); }));
		m_subscriptions.push_back(m_binding.on("project.changed",
			[this](const engine::Event& event)
			{
				const auto picked = timeFormatFromName(event.data.value("timeFormat", std::string()));
				if (picked && *picked != format("primary"))
				{
					m_formats["primary"] = *picked;
					show();
				}
			}));
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto formats = values.find(g_formatsKey); formats != values.end())
					followFormats(*formats);
				if (const auto options = values.find(g_timeDisplayKey); options != values.end())
					followTimeDisplay(*options, event.request);
				if (const auto mode = values.find(g_tapModeKey); mode != values.end())
					followTapMode(*mode);
			}));
		const std::weak_ptr<bool> alive = m_alive;
		requestProject();
		requestQuantize();
		m_binding.send("transport.get", Json::object(),
			[this, alive](const engine::Result& result)
			{
				if (alive.expired())
					return;
				if (result.ok())
					followTransport(result.value, result.request);
				answered();
			});
		m_binding.send("settings.get", {{"key", g_formatsKey}},
			[this, alive](const engine::Result& result)
			{
				if (alive.expired())
					return;
				if (result.ok())
					followFormats(result.value.at("value"));
				answered();
			});
		m_binding.send("settings.get", {{"key", g_timeDisplayKey}},
			[this, alive](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					followTimeDisplay(result.value.at("value"), result.request);
			});
		m_binding.send("settings.get", {{"key", g_tapModeKey}},
			[this, alive](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					followTapMode(result.value.at("value"));
			});
		show();
	}

	std::optional<double> tappedTempo(const std::vector<double>& taps)
	{
		if (taps.size() < 2 || taps.back() <= taps.front())
			return std::nullopt;
		const double beat = (taps.back() - taps.front()) / static_cast<double>(taps.size() - 1);
		return std::clamp(60.0 / beat, time::g_minBpm, time::g_maxBpm);
	}

	std::string tapTempoMessage(const std::string& mode, bool tempoTrackActive, bool selection, bool playing)
	{
		std::string where = "inserts a tempo event at the cursor position";
		if (mode == "display")
			where = "displays the tempo only";
		else if (mode == "project" || (mode == "auto" && !tempoTrackActive))
			where = tempoTrackActive ? "changes the first tempo event" : "sets the project tempo";
		else if (mode == "selection" && !selection)
			where = "inserts no tempo event without a selection";
		else if (mode == "selection" || (mode == "auto" && selection))
			where = "inserts a tempo event at the selection start";
		else if (mode == "playbackStart" || (mode == "auto" && playing))
			where = "inserts a tempo event at the last playback start position";
		return "Tap Tempo: " + where;
	}

	TransportControls::~TransportControls()
	{
		release();
		try
		{
			endTempoGesture(false); // the tempo it reached is set, although its fields go (#566, #569)
		}
		catch (const std::exception& error)
		{
			logging::warning("The transport controls couldn't set the tempo of their gesture: {}", error.what());
		}
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

	// The primary format is the project's time format (reference manual p. 317), the others the user setting.
	void TransportControls::setFormat(const std::string& slot, TimeFormat format)
	{
		const bool changed = std::exchange(m_formats[slot], format) != format;
		if (slot == "primary")
		{
			if (changed)
				m_binding.send("project.setTimeFormat", {{"timeFormat", timeFormatName(format)}});
		}
		else
		{
			Json formats = Json::object();
			for (const auto& [name, value] : m_formats)
				if (name != "primary")
					formats[name] = timeFormatName(value);
			m_binding.send("settings.set", {{"key", g_formatsKey}, {"value", formats}});
		}
		show();
	}

	void TransportControls::exchangeFormats()
	{
		const TimeFormat primary = format("primary");
		setFormat("primary", format("secondary"));
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
		m_sentDisplay = m_binding.send("settings.set",
			{{"key", g_timeDisplayKey}, {"value", {{"beatCountOnly", m_beatCountOnly}, {"size", m_size}}}});
		show();
	}

	// An answer or an echo of something older than the newest change sent leaves what the next change goes on from
	// as it is, until the engine's answer to that change comes (#569).
	bool TransportControls::older(engine::RequestId request, engine::RequestId sent)
	{
		return request != 0 && request < sent;
	}

	void TransportControls::followTimeDisplay(const Json& options, engine::RequestId request)
	{
		if (!options.is_object() || older(request, m_sentDisplay))
			return;
		const int before = m_size;
		if (const auto beats = options.find("beatCountOnly"); beats != options.end() && beats->is_boolean())
			m_beatCountOnly = beats->get<bool>();
		if (const auto size = options.find("size"); size != options.end() && size->is_number_integer())
			m_size = std::clamp(size->get<int>(), 0, g_largestSize);
		show();
		// The window takes its size's preset when the size changed, not for another option, and not for the size the
		// window was opened at: it may have been sized by the user since (#569).
		if (m_sized && m_size != before && std::exchange(m_displayRead, true))
			m_sized(m_size);
		m_displayRead = true;
	}

	void TransportControls::followFormats(const Json& formats)
	{
		if (!formats.is_object())
			return;
		for (const auto& [slot, name] : formats.items())
			if (slot != "primary" && name.is_string()) // the project's, before #537 a user setting
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
				if (result.ok())
				{
					m_tempoTrackActive = result.value.value("tempoTrackActive", false);
					if (const std::optional<TimeFormat> format =
							timeFormatFromName(result.value.value("timeFormat", std::string("barsBeats"))))
						m_formats["primary"] = *format;
				}
				if (std::exchange(m_stale, false))
					requestProject();
				else
					m_sent.reset(); // read after the tempo the gesture set
				show();
				if (!std::exchange(m_projectRead, true))
					answered();
			});
	}

	void TransportControls::requestQuantize()
	{
		m_binding.send("quantize.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_autoQuantize = result.value.value("autoQuantize", false);
				show();
			});
	}

	void TransportControls::onShown(std::function<void()> shown)
	{
		if (m_unanswered == 0)
			shown();
		else
			m_shown = std::move(shown);
	}

	void TransportControls::answered()
	{
		if (m_unanswered == 0)
			return;
		--m_unanswered;
		if (m_unanswered == 0 && m_shown)
			std::exchange(m_shown, {})();
	}

	void TransportControls::followTransport(const Json& status, engine::RequestId request)
	{
		if (older(request, m_sentTransport))
			return;
		m_status = status;
		m_position = status.at("position").get<int64_t>();
		if (status.at("playing").get<bool>() && !m_playback)
			m_playback = m_binding.watch<int64_t>(
				m_root.get(), [this] { return m_engine.playbackPosition(); },
				[this](const int64_t& sample)
				{
					m_position = sample;
					const bool timeChanged = showTime(); // the window draws only when a display's text changed
					const bool tempoChanged = showTempo();
					return timeChanged || tempoChanged;
				});
		else if (!status.at("playing").get<bool>())
			m_playback.reset();
		show();
	}

	int64_t TransportControls::samples(double seconds) const
	{
		return positionSamples(seconds, m_time.sampleRate);
	}

	double TransportControls::amountStart(const std::string& time) const
	{
		const char* locator = time == "preRollAmount" ? "left" : "right";
		return static_cast<double>(m_status.value(locator, int64_t{0})) / m_time.sampleRate;
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
				(m_start ? m_start : m_run)(m_holding, Json::object());
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
			if (const Rml::Element* item = withAttribute(target, "tap-mode"); item != nullptr)
			{
				m_tapMode = attribute(*item, "tap-mode"); // until the setting says so
				m_binding.send("settings.set", {{"key", g_tapModeKey}, {"value", m_tapMode}});
				show();
				return;
			}
			if (const Rml::Element* field = withAttribute(target, "field");
				field != nullptr && inside(field, root) && !field->HasAttribute("disabled"))
			{
				if (attribute(*field, "field") == "tempoTrack")
				{
					m_tempoTrackActive = !m_tempoTrackActive; // until the project is read again
					m_binding.send("project.set", {{"tempoTrackActive", m_tempoTrackActive}});
					show();
					return;
				}
				if (attribute(*field, "field") == "autoQuantize")
				{
					m_autoQuantize = !m_autoQuantize; // until quantize.changed says so
					m_binding.send("quantize.set", {{"autoQuantize", m_autoQuantize}});
					show();
					return;
				}
				if (attribute(*field, "field") == "tap")
				{
					tap();
					return;
				}
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
			{
				m_tempo = tempo; // set at the gesture's end
			}
			else if (tempo)
			{
				const auto [command, parameters] = currentTempoCommand(m_time, m_tempoTrackActive, m_position, *tempo);
				m_binding.send(command, parameters);
			}
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
		const bool amount = isAmount(time);
		const std::optional<double> seconds = time == "length" || amount
			? parseLength(format, text, amount ? amountStart(time) : left, m_time)
			: parseTime(format, text, m_time);
		if (!seconds)
			show(); // the text typed goes
		else if (time == "position")
			m_binding.send("transport.locate", {{"position", samples(*seconds)}});
		else if (time == "left" || time == "right" || amount)
			m_binding.send("transport.set", {{time, samples(*seconds)}});
		else if (time == "length")
			m_binding.send("transport.set", {{"right", samples(left + *seconds)}});
	}

	void TransportControls::wheel(Rml::Element& element, float delta)
	{
		// Up is later, by as many whole steps as the wheel turned: the fractions that a high-resolution wheel or a
		// touchpad sends add up, per display and direction, until they make one (#566, #569). A scroll sideways, as
		// touchpads and tilt wheels send, has no vertical delta and moves nothing. More than a billion steps, which
		// only injected input sends, are a billion: that many fit a long, and that many beats in ticks fit 64 bits
		// (#457).
		if (!(std::abs(delta) > 0.0F)) // also NaN
			return;
		const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
		const double turned = std::clamp(-delta, -1e9F, 1e9F);
		if (m_wheelOn != &element || now - m_wheelAt >= g_fractionPause || (m_wheelRest * turned) < 0.0)
			m_wheelRest = 0.0;
		m_wheelOn = &element;
		m_wheelAt = now;
		m_wheelRest += turned;
		const double whole = std::trunc(m_wheelRest);
		m_wheelRest -= whole;
		const long steps = std::lround(whole);
		if (steps == 0)
			return;
		const std::string time = attribute(element, "time");
		const double rate = m_time.sampleRate;
		const double left = static_cast<double>(m_status.value("left", int64_t{0})) / rate;
		const double right = static_cast<double>(m_status.value("right", int64_t{0})) / rate;
		double from = right; // the range's length moves the right locator
		if (time == "position")
			from = static_cast<double>(m_position) / rate;
		else if (time == "left")
			from = left;
		else if (isAmount(time)) // where the amount ends
			from = amountStart(time) + (static_cast<double>(m_status.value(time, int64_t{0})) / rate);
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
		// The next turn goes on from here, even before transport.changed says so, and not from where an older turn's
		// transport.changed says (#569).
		if (time == "position")
		{
			m_position = samples(to);
			sendTransport("transport.locate", {{"position", m_position}});
		}
		else if (isAmount(time))
		{
			m_status[time] = samples(std::max(to - amountStart(time), 0.0));
			sendTransport("transport.set", {{time, m_status[time]}});
		}
		else
		{
			const std::string locator = time == "left" ? "left" : "right";
			m_status[locator] = samples(time == "length" ? std::max(to, left) : to);
			sendTransport("transport.set", {{locator, m_status[locator]}});
		}
	}

	void TransportControls::sendTransport(const std::string& command, const Json& parameters)
	{
		m_sentTransport = m_binding.send(command, parameters,
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || result.ok())
					return;
				// A refused change has no transport.changed: show what the transport has instead of the turn.
				if (m_sentTransport == result.request)
					m_sentTransport = 0;
				m_binding.send("transport.get", Json::object(),
					[this, alive](const engine::Result& status)
					{
						if (!alive.expired() && status.ok())
							followTransport(status.value, status.request);
					});
			});
	}

	void TransportControls::wheelTempo()
	{
		if (m_gesture == Gesture::Drag)
			return;
		m_gesture = Gesture::Wheel;
		m_wheeled = std::chrono::steady_clock::now();
		if (!m_wheelPause)
			m_wheelPause = m_binding.watch<bool>(
				m_root.get(), [this] { return std::chrono::steady_clock::now() - m_wheeled >= g_wheelPause; },
				[this](const bool& paused)
				{
					if (paused)
						endTempoGesture();
				});
	}

	void TransportControls::endTempoGesture(bool read)
	{
		m_gesture = Gesture::None;
		m_wheelPause.reset();
		if (const std::optional<float> tempo = std::exchange(m_tempo, std::nullopt))
		{
			// Until the project's tempo is read again, the field keeps what it set rather than the tempo before.
			m_sent = tempo;
			const auto [command, parameters] = currentTempoCommand(m_time, m_tempoTrackActive, m_position, *tempo);
			m_binding.send(command, parameters);
			if (read)
				requestProject();
		}
		if (read)
			show();
	}

	void TransportControls::tap()
	{
		const double now = Rml::GetSystemInterface()->GetElapsedTime();
		if (!m_taps.empty() && now - m_taps.back() >= g_tapPause)
			m_taps.clear();
		m_taps.push_back(now);
		if (!m_tapPause)
			m_tapPause = m_binding.watch<bool>(
				m_root.get(),
				[this] { return Rml::GetSystemInterface()->GetElapsedTime() - m_taps.back() >= g_tapPause; },
				[this](const bool& paused)
				{
					if (paused)
						endTap();
				});
		// Where the tempo goes, shown again with every tap.
		const bool selection = m_selectionStart && m_selectionStart().has_value();
		const std::string message =
			tapTempoMessage(m_tapMode, m_tempoTrackActive, selection, m_status.value("playing", false));
		if (Rml::Element* document = m_document.get())
		{
			Rml::ElementList messages;
			document->QuerySelectorAll(messages, "notification.tap-tempo-message");
			for (Rml::Element* element : messages)
				element->SetAttribute("text", message);
		}
		show();
	}

	void TransportControls::followTapMode(const Json& mode)
	{
		if (mode.is_string())
			m_tapMode = mode.get<std::string>();
		show();
	}

	void TransportControls::endTap()
	{
		const std::optional<double> tempo = tappedTempo(m_taps);
		m_taps.clear();
		m_tapPause.reset();
		if (tempo)
		{
			engine::Json parameters = {{"bpm", *tempo}};
			if (const std::optional<int64_t> start = m_selectionStart ? m_selectionStart() : std::nullopt)
				parameters["selection"] = *start;
			m_binding.send("tempo.tap", parameters);
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
		const auto on = [this](const std::string& key)
		{
			const auto value = m_status.find(key);
			return value != m_status.end() && value->is_boolean() && value->get<bool>();
		};
		for (Rml::Element* element : all("[state]"))
			setChecked(*element, on(attribute(*element, "state")));
		for (Rml::Element* element : all("[waiting]"))
			element->SetClass("waiting", on(attribute(*element, "waiting")));
		showTempo();
		for (Rml::Element* element : all("[field=tempoTrack]"))
			setChecked(*element, m_tempoTrackActive);
		for (Rml::Element* element : all("[field=autoQuantize]"))
			setChecked(*element, m_autoQuantize);
		for (Rml::Element* element : all("[field=tap]"))
			element->SetClass("tapping", !m_taps.empty());
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
			items.clear();
			document->QuerySelectorAll(items, "menuitem[tap-mode]");
			for (Rml::Element* item : items)
				setChecked(*item, attribute(*item, "tap-mode") == m_tapMode);
		}
		for (Rml::Element* element : all("[format=timeDisplay]"))
			for (int size = 0; size <= g_largestSize; ++size)
				element->SetClass("size-" + std::to_string(size), size == m_size);
		// Each time display's format as a state class, so that the stylesheets give it the width of the format's widest
		// text and running the transport doesn't change it (#1335).
		for (Rml::Element* element : all("[time]"))
		{
			const std::string name = timeFormatName(format(attribute(*element, "format", "primary")));
			for (const char* each : {"barsBeats", "seconds", "timecode", "samples", "userFrames"})
				element->SetClass(std::string("format-") + each, name == each);
		}
		showTime();
	}

	bool TransportControls::showTempo()
	{
		if (!alive() || m_gesture != Gesture::None) // during one, the fields show where the gesture took it
			return false;
		const std::optional<double> tapped = tappedTempo(m_taps); // while tapping, the tapped tempo
		const double tempo = tapped ? *tapped : m_sent.value_or(static_cast<float>(currentTempo(m_time, m_position)));
		const std::string text = widgets::format(static_cast<float>(tempo));
		bool changed = false;
		for (Rml::Element* element : all("[field=tempo]"))
			if (attribute(*element, "value") != text)
			{
				element->SetAttribute("value", text);
				changed = true;
			}
		return changed;
	}

	bool TransportControls::showTime()
	{
		if (!alive())
			return false;
		bool changed = false;
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
				changed = setText(*element, std::to_string(m_time.signatures.toBarsBeats(at).beat + 1)) || changed;
			}
			else if (time == "position")
			{
				changed =
					setText(*element, formatTime(format, static_cast<double>(m_position) / rate, m_time)) || changed;
			}
			else if (time == "left")
			{
				changed = setText(*element, formatTime(format, left, m_time)) || changed;
			}
			else if (time == "right")
			{
				changed = setText(*element, formatTime(format, right, m_time)) || changed;
			}
			else if (time == "length")
			{
				changed = setText(*element, formatLength(format, left, std::max(left, right), m_time)) || changed;
			}
			else if (isAmount(time))
			{
				const double start = amountStart(time);
				const double amount = static_cast<double>(m_status.value(time, int64_t{0})) / rate;
				changed = setText(*element, formatLength(format, start, start + amount, m_time)) || changed;
			}
		}
		return changed;
	}

	void Winding::start(const std::string& id, double seconds)
	{
		m_winds.emplace_back(id, seconds); // also one of the same command that is held by another key or button
		m_last = Clock::now() + g_windDelay;
		m_binding.send("transport.locate", {{"by", seconds}});
	}

	void Winding::stop(const std::string& id)
	{
		// The newest of that command: each key or button that held it ends one.
		const auto ends = std::find_if(m_winds.rbegin(), m_winds.rend(),
			[&id](const std::pair<std::string, double>& wind) { return wind.first == id; });
		if (ends != m_winds.rend())
			m_winds.erase(std::next(ends).base());
	}

	bool Winding::step()
	{
		if (m_winds.empty())
			return false;
		const Clock::time_point now = Clock::now();
		if (now > m_last)
		{
			const double wound =
				std::chrono::duration<double>(now - m_last).count() * g_windSpeed * m_winds.back().second;
			m_last = now;
			m_binding.send("transport.locate", {{"by", wound}});
		}
		return true;
	}
}
