// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/samplerpanel.h"

#include "platform/log.h"
#include "ui/canvas.h"
#include "ui/rmlui.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <sstream>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		// The markers and the handles are drag elements: their drag events come wherever the mouse is, and the
		// dragend wherever the button goes up, outside the window too.
		constexpr std::array<Rml::EventId, 5> g_events = {Rml::EventId::Dragstart, Rml::EventId::Drag,
			Rml::EventId::Dragend, Rml::EventId::Click, Rml::EventId::Change};

		constexpr int64_t g_maxPixels = 65536; // media.peaks' limits
		constexpr int64_t g_maxColumnValues = 262144;

		// The sections and the parameters they show, by processor.list's names (reference manual pp. 718-722, 730).
		struct Section
		{
			const char* id;
			const char* title;
			std::vector<const char*> parameters;
		};

		const std::vector<Section>& sections()
		{
			static const std::vector<Section> list = {
				{"playback", "Playback",
					{"Playback mode", "Loop mode", "Start", "End", "Loop start", "Loop end", "Loop crossfade",
						"Fade in", "Fade out"}},
				{"voices", "", {"Reverse", "One shot", "Monophonic", "Legato", "Slices", "Sample gain"}},
				{"pitch", "Pitch", {"Fixed pitch", "Octave", "Coarse", "Fine", "Glide", "Fingered"}},
				{"filter", "Filter", {"Filter", "Filter type", "Cutoff", "Resonance", "Drive", "Cutoff key follow"}},
				{"amp", "Amp", {"Volume", "Pan", "Attack", "Decay", "Sustain", "Release"}},
				{"keyboard", "Keyboard", {"Pitchbend", "Root key", "Key low", "Key high"}},
			};
			return list;
		}

		// The markers of the waveform display and the parameters they set (p. 717): the fades' and the loop
		// crossfade's mark the lengths' ends, from Start, End and the loop's end.
		constexpr std::array<const char*, 7> g_markers = {
			"Start", "End", "Loop start", "Loop end", "Fade in", "Fade out", "Loop crossfade"};

		bool isLength(const std::string& marker)
		{
			return marker == "Fade in" || marker == "Fade out" || marker == "Loop crossfade";
		}

		// The bars between two markers that a drag moves together, keeping their distance (p. 717).
		struct Bar
		{
			const char* id;
			const char* from;
			const char* to;
		};
		constexpr std::array<Bar, 2> g_bars = {Bar{"range", "Start", "End"}, Bar{"loop", "Loop start", "Loop end"}};

		const Bar* barOf(const std::string& id)
		{
			for (const Bar& bar : g_bars)
				if (id == bar.id)
					return &bar;
			return nullptr;
		}

		// The keyboard's handles and the parameters they set (p. 730).
		struct Handle
		{
			const char* id;
			const char* parameter;
		};
		constexpr std::array<Handle, 3> g_handles = {
			Handle{"low", "Key low"}, Handle{"high", "Key high"}, Handle{"root", "Root key"}};

		const Handle* handleOf(const std::string& id)
		{
			for (const Handle& handle : g_handles)
				if (id == handle.id)
					return &handle;
			return nullptr;
		}

		constexpr int g_keys = 128;

		// How long the event received indicator stays lit after the last note-on or controller (p. 713).
		constexpr double g_receivedSeconds = 0.25;

		// The markers whose positions Snap to Zero Crossing moves to where the sample crosses zero (p. 713).
		bool snapsToZero(const std::string& marker)
		{
			return marker == "Start" || marker == "End" || marker == "Loop start" || marker == "Loop end";
		}

		double now()
		{
			Rml::SystemInterface* system = Rml::GetSystemInterface();
			return system != nullptr ? system->GetElapsedTime() : 0.0;
		}

		std::string encode(const std::string& text)
		{
			return Rml::StringUtilities::EncodeRml(text);
		}

		void setClass(Rml::Element* element, const char* name, bool set)
		{
			if (element != nullptr && element->IsClassSet(name) != set)
				element->SetClass(name, set);
		}

		void setText(Rml::Element* element, const std::string& text)
		{
			const std::string rml = encode(text);
			if (element != nullptr && element->GetInnerRML() != rml)
				element->SetInnerRML(rml);
		}

		void setChecked(Rml::Element* element, bool set)
		{
			if (element == nullptr || element->HasAttribute("checked") == set)
				return;
			if (set)
				element->SetAttribute("checked", "");
			else
				element->RemoveAttribute("checked");
		}

		Rml::Property percent(double value)
		{
			return {static_cast<float>(value * 100.0), Rml::Unit::PERCENT};
		}

		// The element or its nearest ancestor with the attribute.
		Rml::Element* withAttribute(Rml::Element* element, const char* name)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->HasAttribute(name))
					return element;
			return nullptr;
		}

		const Json* findIn(const Json& project, const char* list, const Json& id)
		{
			const auto items = project.find(list);
			if (items == project.end() || !id.is_number_unsigned() || id.get<uint64_t>() == 0)
				return nullptr;
			for (const Json& item : *items)
				if (item.at("id") == id)
					return &item;
			return nullptr;
		}

		bool isBlack(int key)
		{
			const int note = key % 12;
			return note == 1 || note == 3 || note == 6 || note == 8 || note == 10;
		}
	}

	SamplerPanel::SamplerPanel(EngineBinding& binding, Rml::Element& element,
		std::function<void(uint64_t track, bool keepSettings)> importSample)
		: m_binding(binding), m_importSample(std::move(importSample)), m_stepOwner(binding.stepOwner("sampler")),
		  m_root(element.GetObserverPtr())
	{
		std::string keys;
		for (int key = 0; key < g_keys; ++key)
			keys += R"(<div class="sampler-key)" + std::string(isBlack(key) ? " black" : "") + R"(" key=")" +
				std::to_string(key) + R"("/>)";
		std::string handles;
		for (const Handle& handle : g_handles)
			handles += std::string(R"(<div class="sampler-handle)") +
				(std::string(handle.id) == "root" ? " root" : "") + R"(" handle=")" + handle.id + R"(" title=")" +
				handle.parameter + R"("/>)";
		std::string markers;
		for (const Bar& bar : g_bars)
			markers += std::string(R"(<div class="sampler-bar)") + (std::string(bar.id) == "loop" ? " loop" : "") +
				R"(" bar=")" + bar.id + R"(" title="Move )" + bar.from + " and " + bar.to + R"("/>)";
		for (const char* marker : g_markers)
			markers += std::string(R"(<div class="sampler-marker)") +
				(std::string(marker).rfind("Loop", 0) == 0 ? " loop" : "") + (isLength(marker) ? " length" : "") +
				R"(" marker=")" + marker + R"(" title=")" + marker +
				R"("><div class="sampler-marker-line"/><div class="sampler-marker-flag"/></div>)";
		element.SetInnerRML(
			R"(<div class="sampler-panel empty" id="sampler-panel">)"
			R"(<div class="sampler-hint">Select a sampler track</div>)"
			R"(<div class="sampler-toolbar">)"
			R"(<button id="sampler-read" type="toggle" title="Read Automation" disabled="">Read</button>)"
			R"(<span id="sampler-received" class="sampler-received" title="Event Received Indicator"/>)"
			R"(<button id="sampler-zero" type="toggle" title="Snap to Zero Crossing">Zero</button>)"
			R"(<button id="sampler-import" title="Import Audio File">Import</button>)"
			R"(<span id="sampler-file" class="sampler-file"/>)"
			R"(<span id="sampler-message" class="sampler-message"/>)"
			R"(<valuefield id="sampler-level" class="sampler-level" title="Normalization Level" min="-96" max="0")"
			R"( default="0" value="0" decimals="1" unit="dB"/>)"
			R"(<button id="sampler-normalize" title="Normalize Sample">Normalize</button>)"
			R"(<button id="sampler-lock" type="toggle" title="Lock Parameter Settings">Lock</button>)"
			R"(<button id="sampler-midi-reset" title="MIDI Reset">Reset</button>)"
			R"(</div><div class="sampler-timeline"><ruler class="sampler-ruler"/>)"
			R"(<select id="sampler-ruler-format" class="sampler-ruler-format" title="Ruler Format"/></div>)"
			R"(<div class="sampler-view"><div class="sampler-display" id="sampler-display"><waveform class="sampler-wave"/>)"
			R"(<div class="sampler-outside sampler-before"/><div class="sampler-outside sampler-after"/>)"
			R"(<div class="sampler-loop"/>)" +
			markers +
			R"(</div><fader id="sampler-vertical-zoom" min="-6" max="0" default="0" step="0.1" title="Vertical Zoom"/>)"
			R"(</div><div class="sampler-zoom"><slider id="sampler-scroll" min="0" max="1" default="0" step="0.01"/>)"
			R"(<slider id="sampler-horizontal-zoom" min="0" max="1" default="0" step="0.01" title="Horizontal Zoom"/>)"
			R"(</div><div class="sampler-handles" id="sampler-handles">)" +
			handles + R"(</div><div class="sampler-keyboard" id="sampler-keyboard">)" + keys +
			R"(</div><div class="sampler-sections" id="sampler-sections"/></div>)");
		for (const Rml::EventId id : g_events)
			element.AddEventListener(id, this);
		element.AddEventListener(m_dropEvent, this);
		for (const char* name : {"media.peaksProgress", "media.peaksReady"})
			m_subscriptions.push_back(m_binding.on(name,
				[this](const engine::Event& event)
				{
					if (m_media != 0 && event.data.value("media", Json()) == m_media)
					{
						m_asked = {};
						ask();
					}
				}));
		// Lock Parameter Settings and the Normalization Level are a user setting, so they outlast the panel.
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				// The panel's own changes on their way, e.g. a drag of the level, aren't set back by older ones
				// (#1180).
				if (const auto settings = values.find("ui.samplerPanel"); settings != values.end() && m_savesSent == 0)
				{
					m_settingsKnown = true;
					applySettings(*settings);
				}
			}));
		// The event received indicator lights up at note-ons and controllers from the track's MIDI input (p. 713).
		m_subscriptions.push_back(
			m_binding.on("midi.activity", [this](const engine::Event& event) { received(event.data); }));
		m_subscriptions.push_back(
			m_binding.on("midi.changed", [this](const engine::Event& event) { readPorts(event.data); }));
		m_binding.send("settings.get", {{"key", "ui.samplerPanel"}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok() && !m_settingsKnown)
					applySettings(result.value.at("value"));
			});
		m_binding.send("processor.list", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					build(result.value);
			});
	}

	SamplerPanel::~SamplerPanel()
	{
		// The indicator stops listening with the panel. Sending may throw, which a destructor must not pass on.
		try
		{
			listen(false);
		}
		catch (const std::exception& error)
		{
			logging::warning("The sampler panel cannot stop listening for MIDI input: {}", error.what());
		}
		if (Rml::Element* root = m_root.get())
		{
			for (const Rml::EventId id : g_events)
				root->RemoveEventListener(id, this);
			root->RemoveEventListener(m_dropEvent, this);
		}
	}

	Rml::Element* SamplerPanel::find(const std::string& id) const
	{
		Rml::Element* root = m_root.get();
		return root == nullptr ? nullptr : root->QuerySelector("#" + id);
	}

	// The sections' controls, from the sampler's parameters: a toggle for those that are on or off, a drop-down list
	// for those with options, a value field for the others.
	void SamplerPanel::build(const Json& processors)
	{
		for (const Json& processor : processors.at("processors"))
		{
			if (processor.at("id") != "sampler")
				continue;
			for (const Json& item : processor.at("parameters"))
				m_parameters.push_back({item.at("id").get<uint32_t>(), item.at("name").get<std::string>(),
					item.at("minimum").get<double>(), item.at("maximum").get<double>(),
					item.at("default").get<double>(), item.value("unit", std::string()), item.value("stepped", false),
					item.value("options", std::vector<std::string>())});
		}
		Rml::Element* list = find("sampler-sections");
		if (list == nullptr)
			return;
		std::string rml;
		for (const Section& section : sections())
		{
			rml += std::string(R"(<div class="sampler-section" section=")") + section.id +
				R"("><div class="sampler-section-title">)" + section.title + "</div>";
			for (const char* name : section.parameters)
			{
				const Parameter* found = parameter(name);
				if (found == nullptr)
					continue;
				const std::string id = std::to_string(found->id);
				std::string attributes = R"( id="sampler-parameter-)";
				attributes.append(id).append(R"(" parameter=")").append(id);
				attributes.append(R"(" title=")").append(encode(found->name)).append("\"");
				const std::string label = R"(<span class="sampler-label">)" + encode(found->name) + "</span>";
				rml += R"(<div class="sampler-control">)";
				if (found->options == std::vector<std::string>{"Off", "On"})
				{
					rml.append(R"(<button type="toggle")").append(attributes).append(">");
					rml.append(encode(found->name)).append("</button>");
				}
				else if (!found->options.empty())
				{
					rml.append(label).append("<select").append(attributes).append("/>");
				}
				else
				{
					// Whole numbers step by one; shares of the sample show three decimals.
					std::string decimals = found->maximum <= 1.0 ? "3" : "1";
					if (found->stepped)
						decimals = R"(0" step="1)";
					rml.append(label).append("<valuefield").append(attributes);
					rml.append(R"( min=")").append(widgets::format(static_cast<float>(found->minimum)));
					rml.append(R"(" max=")").append(widgets::format(static_cast<float>(found->maximum)));
					rml.append(R"(" default=")").append(widgets::format(static_cast<float>(found->defaultValue)));
					rml.append(R"(" decimals=")").append(decimals);
					rml.append(R"(" unit=")").append(encode(found->unit)).append(R"("/>)");
				}
				rml += "</div>";
			}
			rml += "</div>";
		}
		list->SetInnerRML(rml);
		show();
	}

	const SamplerPanel::Parameter* SamplerPanel::parameter(const std::string& name) const
	{
		const auto found = std::find_if(
			m_parameters.begin(), m_parameters.end(), [&](const Parameter& each) { return each.name == name; });
		return found == m_parameters.end() ? nullptr : &*found;
	}

	double SamplerPanel::value(const std::string& name) const
	{
		const Parameter* found = parameter(name);
		if (found == nullptr)
			return 0.0;
		// A parameter whose change is on its way keeps what the user gave it, not the engine's older value, also after
		// another sampler track showed meanwhile (#1471).
		if (const Json* shown = m_channel.is_object() ? m_binding.shown(stepKey(found->id), m_read) : nullptr)
			return shown->get<double>();
		// Likewise for a bar's drag, which changes two parameters in one step.
		for (const Bar& bar : g_bars)
		{
			const Json* moved = m_channel.is_object() && (name == bar.from || name == bar.to)
				? m_binding.shown(barKey(bar.id), m_read)
				: nullptr;
			if (moved != nullptr && moved->is_object() && moved->contains(name))
				return moved->at(name).get<double>();
		}
		const auto set = m_values.find(found->id);
		return set != m_values.end() ? set->second : found->defaultValue;
	}

	std::string SamplerPanel::stepKey(uint32_t id) const
	{
		return m_stepOwner + "channel " + m_channel.value("id", Json()).dump() + " instrument " + std::to_string(id);
	}

	std::string SamplerPanel::barKey(const std::string& bar) const
	{
		return m_stepOwner + "channel " + m_channel.value("id", Json()).dump() + " bar " + bar;
	}

	void SamplerPanel::update(uint64_t track, const Json& project, uint64_t loads, uint64_t read)
	{
		m_read = read;
		if (m_received && now() - m_receivedAt >= g_receivedSeconds)
		{
			m_received = false;
			setClass(find("sampler-received"), "lit", false);
		}
		// The waveform's columns and the ruler follow their widths once they are laid out, and whenever they change.
		Rml::Element* wave = m_root ? m_root->QuerySelector(".sampler-wave") : nullptr;
		Rml::Element* ruler = m_root ? m_root->QuerySelector(".sampler-ruler") : nullptr;
		if (wave != nullptr && ruler != nullptr && m_media != 0 &&
			(wave->GetClientWidth() != m_askedWidth || ruler->GetClientWidth() != m_rulerWidth))
			showView();
		if (track == m_selected && loads == m_loads)
			return;
		m_selected = track;
		if (loads != m_loads)
			readTimeContext(project, m_time);
		m_loads = loads;
		const Json* found = findIn(project, "tracks", track);
		const Json* channel = found == nullptr ? nullptr : findIn(project, "channels", found->value("channel", Json()));
		if (channel != nullptr && channel->value("instrument", std::string()) != "sampler")
			channel = nullptr;
		if ((channel != nullptr ? track : 0) != m_track)
			showMessage(""); // what failed concerned the track shown before
		m_track = channel != nullptr ? track : 0;
		m_channel = channel != nullptr ? *channel : Json();
		listen(m_track != 0);
		std::map<uint32_t, double> values;
		if (channel != nullptr)
			for (const Json& item : channel->value("instrumentParameters", Json::array()))
				values[item.at("parameter").get<uint32_t>()] = item.at("value").get<double>();
		m_values = std::move(values);
		m_lanes.clear();
		m_reading = true;
		if (const auto lanes = project.find("automation"); channel != nullptr && lanes != project.end())
			for (const Json& lane : *lanes)
				if (lane.at("track") == track)
				{
					m_lanes.push_back(lane.at("id").get<uint64_t>());
					m_reading = m_reading && lane.at("read").get<bool>();
				}
		m_reading = m_reading && !m_lanes.empty();
		uint64_t media = 0;
		int64_t length = 0;
		std::string file = "No sample";
		const Json* clip = channel == nullptr ? nullptr : findIn(project, "clips", channel->value("sample", Json()));
		if (const Json* medium = clip == nullptr ? nullptr : findIn(project, "media", clip->at("media")))
		{
			media = medium->at("id").get<uint64_t>();
			length = medium->at("length").get<int64_t>();
			m_mediaChannels = std::max(medium->value("channels", 1), 1);
			m_sampleRate = std::max(medium->value("sampleRate", 48000.0), 1.0);
			file = clip->value("name", std::string());
		}
		setText(find("sampler-file"), file);
		if (media != m_media)
		{
			m_viewStart = 0.0; // another sample shows whole
			m_viewWidth = 1.0;
			m_gain = 1.0;
		}
		m_media = media;
		m_length = length;
		show();
	}

	void SamplerPanel::show()
	{
		Rml::Element* root = find("sampler-panel");
		if (root == nullptr)
			return;
		setClass(root, "empty", m_track == 0);
		m_showing = true;
		for (const Parameter& each : m_parameters)
		{
			Rml::Element* control = find("sampler-parameter-" + std::to_string(each.id));
			if (control == nullptr)
				continue;
			const double shown = value(each.name);
			if (each.options == std::vector<std::string>{"Off", "On"})
			{
				setChecked(control, shown >= 0.5);
			}
			else if (!each.options.empty())
			{
				widgets::Options options;
				for (size_t i = 0; i < each.options.size(); ++i)
					options.emplace_back(std::to_string(i), each.options[i]);
				widgets::fill(*control, options, std::to_string(std::lround(shown)));
			}
			else if (std::abs(widgets::number(*control, "value", static_cast<float>(shown) + 1.0F) - shown) > 1e-6)
			{
				control->SetAttribute("value", widgets::format(static_cast<float>(shown)));
			}
		}
		setChecked(find("sampler-lock"), m_lock);
		setChecked(find("sampler-read"), m_reading);
		if (Rml::Element* read = find("sampler-read");
			read != nullptr && read->HasAttribute("disabled") != m_lanes.empty())
		{
			if (m_lanes.empty())
				read->SetAttribute("disabled", "");
			else
				read->RemoveAttribute("disabled");
		}
		setChecked(find("sampler-zero"), m_snapToZero);
		if (Rml::Element* format = find("sampler-ruler-format"))
			widgets::fill(
				*format, {{"barsBeats", "Bars+Beats"}, {"seconds", "Seconds"}, {"samples", "Samples"}}, m_rulerFormat);
		if (Rml::Element* level = find("sampler-level"); level != nullptr &&
			std::abs(widgets::number(*level, "value", static_cast<float>(m_level) + 1.0F) - m_level) > 1e-6)
			level->SetAttribute("value", widgets::format(static_cast<float>(m_level)));
		m_showing = false;
		if (const Rml::Element* handles = find("sampler-handles"))
			for (int i = 0; i < handles->GetNumChildren(); ++i)
			{
				Rml::Element* handle = handles->GetChild(i);
				const auto id = handle->GetAttribute<Rml::String>("handle", "");
				const Handle* found = handleOf(id);
				if (found == nullptr)
					continue;
				const double key = std::clamp(std::round(value(found->parameter)), 0.0, g_keys - 1.0);
				handle->SetProperty(Rml::PropertyId::Left, percent(key / g_keys));
				setClass(handle, "dragged", id == m_handle);
			}
		if (const Rml::Element* keyboard = find("sampler-keyboard"))
		{
			const long rootKey = std::lround(value("Root key"));
			const long low = std::lround(value("Key low"));
			const long high = std::lround(value("Key high"));
			for (int key = 0; key < keyboard->GetNumChildren(); ++key)
			{
				Rml::Element* element = keyboard->GetChild(key);
				setClass(element, "root", key == rootKey);
				setClass(element, "outside", key < low || key > high);
			}
		}
		showView();
	}

	// Where a position on the display, a share of the sample from its start, lies in the part shown: 0 at its left
	// edge, 1 at its right one.
	double SamplerPanel::viewed(double position) const
	{
		return (position - m_viewStart) / m_viewWidth;
	}

	// The ruler, the waveform and the zoom sliders for the part of the sample shown (p. 717), the markers in it, and
	// the peaks for it.
	void SamplerPanel::showView()
	{
		Rml::Element* root = m_root.get();
		if (root == nullptr)
			return;
		const Rml::Context* context = root->GetContext();
		const double ratio = context == nullptr ? 1.0 : context->GetDensityIndependentPixelRatio();
		const double seconds = static_cast<double>(m_length) / m_sampleRate;
		if (auto* ruler = dynamic_cast<Ruler*>(root->QuerySelector(".sampler-ruler")))
		{
			// The sample's time from its start, at its own rate, with the project's tempo for bars and beats.
			TimeContext time = m_time;
			time.sampleRate = m_sampleRate;
			time.startTime = 0.0;
			time.sampleStart = 0.0;
			time.barOffset = 0;
			time.barShift = 0;
			ruler->setScale(rulerScale(timeFormatFromName(m_rulerFormat).value_or(TimeFormat::Seconds), time));
			ruler->SetAttribute("format", m_rulerFormat);
			ruler->SetAttribute("start", Rml::CreateString("%.17g", m_viewStart * seconds));
			m_rulerWidth = ruler->GetClientWidth();
			if (m_rulerWidth > 0.0F)
				ruler->SetAttribute("scale", Rml::CreateString("%.17g", m_viewWidth * seconds * ratio / m_rulerWidth));
		}
		if (auto* wave = dynamic_cast<Waveform*>(root->QuerySelector(".sampler-wave")))
		{
			m_askedWidth = wave->GetClientWidth();
			wave->SetAttribute("start", m_viewStart * static_cast<double>(m_length));
			if (m_askedWidth > 0.0F)
				wave->SetAttribute("scale", m_viewWidth * static_cast<double>(m_length) * ratio / m_askedWidth);
			wave->SetAttribute("gain", m_gain);
		}
		const double minimum = m_length > 64 ? 64.0 / static_cast<double>(m_length) : 1.0;
		m_showing = true;
		const auto setValue = [](Rml::Element* element, double value)
		{
			if (element != nullptr && !element->IsPseudoClassSet("active") &&
				std::abs(element->GetAttribute<double>("value", 0.0) - value) > 1e-6)
				element->SetAttribute("value", value);
		};
		setValue(find("sampler-scroll"), m_viewWidth < 1.0 ? m_viewStart / (1.0 - m_viewWidth) : 0.0);
		setValue(find("sampler-horizontal-zoom"), minimum < 1.0 ? std::log(m_viewWidth) / std::log(minimum) : 0.0);
		setValue(find("sampler-vertical-zoom"), -std::log2(m_gain));
		m_showing = false;
		showMarkers();
		ask();
	}

	// The horizontal zoom shows from the whole sample down to 64 frames around the middle of what it showed, the
	// scroll slider moves that along the sample, and the vertical zoom raises the waveform up to 64 times.
	void SamplerPanel::zoom(const std::string& slider, double value)
	{
		if (slider == "sampler-vertical-zoom")
		{
			m_gain = std::exp2(-std::clamp(value, -6.0, 0.0));
		}
		else if (slider == "sampler-horizontal-zoom")
		{
			const double minimum = m_length > 64 ? 64.0 / static_cast<double>(m_length) : 1.0;
			const double middle = m_viewStart + (m_viewWidth / 2.0);
			m_viewWidth = std::pow(minimum, std::clamp(value, 0.0, 1.0));
			m_viewStart = middle - (m_viewWidth / 2.0);
		}
		else
		{
			m_viewStart = std::clamp(value, 0.0, 1.0) * (1.0 - m_viewWidth);
		}
		m_viewStart = std::clamp(m_viewStart, 0.0, 1.0 - m_viewWidth);
		showView();
	}

	// Places the markers, the loop and the shades outside the start and end, in percent of the part of the sample
	// shown: geometry from the parameters. Reversed, the sampler counts them from the sample's end
	// (docs/features/sampler-track.md).
	void SamplerPanel::showMarkers()
	{
		Rml::Element* display = find("sampler-display");
		if (display == nullptr)
			return;
		const bool reverse = value("Reverse") >= 0.5;
		const auto at = [&](const std::string& name)
		{
			const double share = std::clamp(markerShare(name), 0.0, 1.0);
			return reverse ? 1.0 - share : share;
		};
		// A range between two positions, within the part shown: its left edge and width there.
		const auto range = [&](double first, double second)
		{
			const double left = std::clamp(viewed(std::min(first, second)), 0.0, 1.0);
			return std::make_pair(left, std::clamp(viewed(std::max(first, second)), 0.0, 1.0) - left);
		};
		Rml::ElementList markers;
		display->QuerySelectorAll(markers, ".sampler-marker");
		const bool looping = std::lround(value("Loop mode")) != 0;
		for (Rml::Element* marker : markers)
		{
			const auto name = marker->GetAttribute<Rml::String>("marker", "");
			const double shown = viewed(at(name));
			marker->SetProperty(Rml::PropertyId::Left, percent(shown));
			setClass(marker, "out-of-view", shown < 0.0 || shown > 1.0);
			setClass(marker, "dragged", name == m_dragged);
			setClass(marker, "off", !looping && name.rfind("Loop", 0) == 0);
		}
		for (const Bar& bar : g_bars)
		{
			Rml::Element* element = display->QuerySelector(std::string(".sampler-bar[bar=") + bar.id + "]");
			if (element == nullptr)
				continue;
			const auto [left, width] = range(at(bar.from), at(bar.to));
			element->SetProperty(Rml::PropertyId::Left, percent(left));
			element->SetProperty(Rml::PropertyId::Width, percent(width));
			setClass(element, "dragged", bar.id == m_bar);
			setClass(element, "off", !looping && std::string(bar.id) == "loop");
		}
		const double start = std::clamp(viewed(std::min(at("Start"), at("End"))), 0.0, 1.0);
		const double end = std::clamp(viewed(std::max(at("Start"), at("End"))), 0.0, 1.0);
		if (Rml::Element* before = display->QuerySelector(".sampler-before"))
			before->SetProperty(Rml::PropertyId::Width, percent(start));
		if (Rml::Element* after = display->QuerySelector(".sampler-after"))
		{
			after->SetProperty(Rml::PropertyId::Left, percent(end));
			after->SetProperty(Rml::PropertyId::Width, percent(1.0 - end));
		}
		if (Rml::Element* loop = display->QuerySelector(".sampler-loop"))
		{
			const auto [left, width] = range(at("Loop start"), at("Loop end"));
			loop->SetProperty(Rml::PropertyId::Left, percent(left));
			loop->SetProperty(Rml::PropertyId::Width, percent(width));
			setClass(loop, "off", !looping);
		}
	}

	// Asks media.peaks for the part of the sample shown, a column per pixel of the display, unless it asked for that
	// last; one question in flight, and what changed meanwhile is asked once it is answered.
	void SamplerPanel::ask()
	{
		Rml::Element* root = m_root.get();
		auto* wave = dynamic_cast<Waveform*>(root == nullptr ? nullptr : root->QuerySelector(".sampler-wave"));
		if (wave == nullptr)
			return;
		if (m_media == 0 || m_length < 1)
		{
			wave->setPeaks({}, {});
			m_asked = {};
			return;
		}
		if (m_asking)
		{
			m_again = true;
			return;
		}
		const float width = wave->GetClientWidth();
		if (!(width > 0.0F))
			return;
		const auto length = static_cast<double>(m_length);
		const int64_t from = std::clamp<int64_t>(std::llround(std::floor(m_viewStart * length)), 0, m_length - 1);
		const int64_t to =
			std::clamp<int64_t>(std::llround(std::ceil((m_viewStart + m_viewWidth) * length)), from + 1, m_length);
		const int64_t pixels =
			std::clamp<int64_t>(std::lround(width), 1, std::min(g_maxPixels, g_maxColumnValues / m_mediaChannels));
		const std::array<int64_t, 4> asked = {static_cast<int64_t>(m_media), from, to, pixels};
		if (asked == m_asked)
			return;
		m_asked = asked;
		m_asking = true;
		m_binding.send("media.peaks", {{"media", m_media}, {"start", from}, {"frames", to - from}, {"pixels", pixels}},
			[this, alive = std::weak_ptr<bool>(m_alive), media = m_media, from, to](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_asking = false;
				Rml::Element* shown = m_root ? m_root->QuerySelector(".sampler-wave") : nullptr;
				if (auto* waveform = dynamic_cast<Waveform*>(shown);
					waveform != nullptr && result.ok() && media == m_media)
				{
					std::vector<PeakColumns> columns;
					for (const Json& channel : result.value.at("channels"))
						columns.push_back({channel.at("min").get<std::vector<int16_t>>(),
							channel.at("max").get<std::vector<int16_t>>()});
					waveform->setPeaks({static_cast<double>(from), static_cast<double>(to - from)}, std::move(columns));
				}
				if (std::exchange(m_again, false) || media != m_media)
					ask();
			});
	}

	void SamplerPanel::set(uint32_t id, double value, int gesture)
	{
		if (!m_channel.is_object())
			return;
		const auto found = std::find_if(
			m_parameters.begin(), m_parameters.end(), [id](const Parameter& each) { return each.id == id; });
		const Json step = {{"command", "parameter.set"},
			{"parameters",
				{{"channel", m_channel.at("id")}, {"target", {{"type", "instrument"}, {"parameter", id}}},
					{"value", value}}}};
		m_binding.sendStep(gesture, stepKey(id), "Change " + (found != m_parameters.end() ? found->name : "Sampler"),
			Json::array({step}), {}, value);
	}

	// The longest crossfade the sampler plays: no longer than the loop and than what lies between Start and the loop's
	// start, the loop within Start and End (Sampler::loopOf).
	double SamplerPanel::crossfadeLimit() const
	{
		const double start = value("Start");
		const double end = std::max(value("End"), start);
		const double loopStart = std::clamp(value("Loop start"), start, end);
		const double loopEnd = std::clamp(value("Loop end"), start, end);
		return std::max(std::min(loopEnd - loopStart, loopStart - start), 0.0);
	}

	// Where a marker lies, a share of the sample: a fade's or the crossfade's at the end of its length, which is no
	// longer than the range or than the crossfade the sampler plays.
	double SamplerPanel::markerShare(const std::string& marker) const
	{
		const double range = std::max(value("End") - value("Start"), 0.0);
		if (marker == "Fade in")
			return value("Start") + std::clamp(value("Fade in"), 0.0, range);
		if (marker == "Fade out")
			return value("End") - std::clamp(value("Fade out"), 0.0, range);
		if (marker == "Loop crossfade")
			return value("Loop end") - std::clamp(value("Loop crossfade"), 0.0, crossfadeLimit());
		return value(marker);
	}

	// Moves the marker dragged to the mouse: Start stays before End, and the loop's start before its end; the fades
	// and the crossfade take the length from their marker's own end, no longer than the range or than the crossfade
	// the sampler plays.
	void SamplerPanel::drag(const Rml::Event& event)
	{
		Rml::Element* display = find("sampler-display");
		const Parameter* dragged = parameter(m_dragged);
		const float width = display == nullptr ? 0.0F : display->GetClientWidth();
		if (dragged == nullptr || !(width > 0.0F))
			return;
		const double x = m_viewStart +
			(std::clamp(
				 (event.GetParameter<float>("mouse_x", 0.0F) - display->GetAbsoluteLeft()) / static_cast<double>(width),
				 0.0, 1.0) *
				m_viewWidth);
		double share = value("Reverse") >= 0.5 ? 1.0 - x : x;
		const double range = std::max(value("End") - value("Start"), 0.0);
		if (m_dragged == "Fade in")
			share = std::clamp(share - value("Start"), 0.0, range);
		else if (m_dragged == "Fade out")
			share = std::clamp(value("End") - share, 0.0, range);
		else if (m_dragged == "Loop crossfade")
			share = std::clamp(value("Loop end") - share, 0.0, crossfadeLimit());
		else
			share = ordered(m_dragged, share);
		set(dragged->id, share, m_gesture);
		showMarkers();
	}

	// A position's share kept in order: Start before End, and the loop's start before its end.
	double SamplerPanel::ordered(const std::string& marker, double share) const
	{
		if (marker == "Start")
			return std::min(share, value("End"));
		if (marker == "End")
			return std::max(share, value("Start"));
		if (marker == "Loop start")
			return std::min(share, value("Loop end"));
		return std::max(share, value("Loop start"));
	}

	// With Snap to Zero Crossing, a marker's drag ends where the sample crosses zero nearest to where it was let go
	// (sampler.zeroCrossings), in the drag's undo step. Key commands, such as an undo right after the drag, wait for it
	// (sendAwaited); the panel's own mouse input doesn't.
	void SamplerPanel::snapToZero(const std::string& marker, int gesture)
	{
		const Parameter* found = parameter(marker);
		if (found == nullptr || m_track == 0)
			return;
		m_binding.sendAwaited("sampler.zeroCrossings", {{"track", m_track}, {"positions", {value(marker)}}},
			[this, alive = std::weak_ptr<bool>(m_alive), marker, gesture, track = m_track, id = found->id](
				const engine::Result& result)
			{
				if (alive.expired() || !result.ok() || track != m_track)
					return;
				set(id, ordered(marker, result.value.at("positions").at(0).get<double>()), gesture);
				showMarkers();
			});
	}

	// A bar's drag likewise ends with both its markers where the sample crosses zero, in one query and the drag's step.
	void SamplerPanel::snapBarToZero(const std::string& id, int gesture)
	{
		const Bar* bar = barOf(id);
		if (bar == nullptr || m_track == 0)
			return;
		m_binding.sendAwaited("sampler.zeroCrossings",
			{{"track", m_track}, {"positions", {value(bar->from), value(bar->to)}}},
			[this, alive = std::weak_ptr<bool>(m_alive), id, gesture, track = m_track](const engine::Result& result)
			{
				if (alive.expired() || !result.ok() || track != m_track)
					return;
				const Json& positions = result.value.at("positions");
				setBar(id, positions.at(0).get<double>(), positions.at(1).get<double>(), gesture);
				showMarkers();
			});
	}

	// Turns midi.activity on or off for the indicator, and asks which MIDI inputs aren't among all MIDI inputs, which
	// it leaves out.
	void SamplerPanel::listen(bool on)
	{
		if (on == m_listening)
			return;
		m_listening = on;
		m_binding.send("midi.activityListen", {{"on", on}, {"listener", m_stepOwner}});
		if (on)
			m_binding.send("midi.ports", Json::object(),
				[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
				{
					if (!alive.expired() && result.ok())
						readPorts(result.value);
				});
	}

	void SamplerPanel::readPorts(const Json& ports)
	{
		m_notInAllInputs.clear();
		for (const Json& port : ports.value("ports", Json::array()))
			if (port.value("input", false) && !port.value("inAllInputs", true))
				m_notInAllInputs.insert(port.value("backend", std::string()) + " " + port.value("id", std::string()));
	}

	// Lights the indicator at a note-on or a controller that the track's MIDI input, all MIDI inputs or a single port
	// (#641), receives.
	void SamplerPanel::received(const Json& activity)
	{
		if (m_track == 0)
			return;
		const std::string from = m_channel.value("midiInput", std::string("all"));
		for (const Json& message : activity.value("received", Json::array()))
		{
			const auto bytes = message.value("bytes", std::vector<int>());
			const int status = bytes.empty() ? 0 : bytes[0] & 0xF0;
			const bool lights = status == 0xB0 || (status == 0x90 && bytes.size() > 2 && bytes[2] > 0);
			const std::string backend = message.value("backend", std::string());
			const std::string id = message.value("id", std::string());
			if (!lights ||
				(from == "all" ? m_notInAllInputs.count(backend + " " + id) != 0 : from != backend + "/in/" + id))
				continue;
			m_received = true;
			m_receivedAt = now();
			Rml::Element* indicator = find("sampler-received");
			setClass(indicator, "lit", true);
			// The UI updates again then, and update puts it out.
			if (Rml::Context* context = indicator != nullptr ? indicator->GetContext() : nullptr)
				context->RequestNextUpdate(g_receivedSeconds);
			return;
		}
	}

	// Moves the two markers of the bar dragged by as far as the mouse went since the drag began, keeping their
	// distance and both within the sample, in one step.
	void SamplerPanel::dragBar(const Rml::Event& event)
	{
		Rml::Element* display = find("sampler-display");
		const float width = display == nullptr ? 0.0F : display->GetClientWidth();
		if (!(width > 0.0F))
			return;
		const double moved =
			(event.GetParameter<float>("mouse_x", 0.0F) - m_barMouse) / static_cast<double>(width) * m_viewWidth;
		const double shift = std::clamp(value("Reverse") >= 0.5 ? -moved : moved, -std::min(m_barFrom, m_barTo),
			1.0 - std::max(m_barFrom, m_barTo));
		setBar(m_bar, std::clamp(m_barFrom + shift, 0.0, 1.0), std::clamp(m_barTo + shift, 0.0, 1.0), m_gesture);
		showMarkers();
	}

	// Sets the two markers of a bar in one step.
	void SamplerPanel::setBar(const std::string& id, double first, double second, int gesture)
	{
		const Bar* bar = barOf(id);
		const Parameter* from = bar == nullptr ? nullptr : parameter(bar->from);
		const Parameter* to = bar == nullptr ? nullptr : parameter(bar->to);
		if (from == nullptr || to == nullptr || !m_channel.is_object())
			return;
		const auto command = [this](uint32_t parameter, double share)
		{
			return Json{{"command", "parameter.set"},
				{"parameters",
					{{"channel", m_channel.at("id")}, {"target", {{"type", "instrument"}, {"parameter", parameter}}},
						{"value", share}}}};
		};
		m_binding.sendStep(gesture, barKey(bar->id), std::string("Move ") + bar->from + " and " + bar->to,
			Json::array({command(from->id, first), command(to->id, second)}), {},
			Json{{bar->from, first}, {bar->to, second}});
	}

	// Moves the keyboard's handle dragged to the key under the mouse: the key range's low key stays at or below its
	// high one.
	void SamplerPanel::dragKey(const Rml::Event& event)
	{
		Rml::Element* keyboard = find("sampler-keyboard");
		const Handle* found = handleOf(m_handle);
		const float width = keyboard == nullptr ? 0.0F : keyboard->GetClientWidth();
		const Parameter* dragged = found == nullptr ? nullptr : parameter(found->parameter);
		if (dragged == nullptr || !(width > 0.0F))
			return;
		const double x =
			(event.GetParameter<float>("mouse_x", 0.0F) - keyboard->GetAbsoluteLeft()) / static_cast<double>(width);
		double key = std::clamp(std::floor(x * g_keys), 0.0, g_keys - 1.0);
		if (m_handle == "low")
			key = std::min(key, std::round(value("Key high")));
		else if (m_handle == "high")
			key = std::max(key, std::round(value("Key low")));
		if (key == std::round(value(dragged->name)))
			return;
		set(dragged->id, key, m_gesture);
		show();
	}

	void SamplerPanel::showMessage(const std::string& text)
	{
		Rml::Element* message = find("sampler-message");
		setText(message, text);
		setClass(message, "error", !text.empty());
		if (message != nullptr)
			message->SetAttribute("title", text); // the whole message when the toolbar cuts it short
	}

	void SamplerPanel::load(uint64_t track, const std::string& path, bool keepSettings)
	{
		m_binding.send("sampler.load", {{"track", track}, {"path", path}, {"keepSettings", keepSettings}},
			[this, alive = std::weak_ptr<bool>(m_alive), track](const engine::Result& result)
			{
				if (!alive.expired() && track == m_track)
					showMessage(result.ok() ? "" : "Loading the sample failed: " + result.error);
			});
	}

	void SamplerPanel::applySettings(const Json& settings)
	{
		m_settings = settings.is_object() ? settings : Json::object();
		m_lock = m_settings.value("lockParameters", false);
		m_snapToZero = m_settings.value("snapToZero", false);
		m_level = std::clamp(m_settings.value("normalizeLevel", 0.0), -96.0, 0.0);
		const std::string format = m_settings.value("rulerFormat", std::string("seconds"));
		m_rulerFormat = format == "barsBeats" || format == "samples" ? format : "seconds";
		show();
	}

	void SamplerPanel::saveSettings()
	{
		m_settingsKnown = true; // what settings.get answers now is older
		m_settings["lockParameters"] = m_lock;
		m_settings["snapToZero"] = m_snapToZero;
		m_settings["normalizeLevel"] = m_level;
		m_settings["rulerFormat"] = m_rulerFormat;
		++m_savesSent;
		m_binding.send("settings.set", {{"key", "ui.samplerPanel"}, {"value", m_settings}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& /*result*/)
			{
				if (!alive.expired())
					--m_savesSent;
			});
	}

	void SamplerPanel::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		switch (event.GetId())
		{
		case Rml::EventId::Dragstart:
			if (m_track == 0)
				return;
			if (const Rml::Element* marker = withAttribute(target, "marker"); marker != nullptr)
			{
				m_dragged = marker->GetAttribute<Rml::String>("marker", "");
				--m_gesture;
				showMarkers();
			}
			else if (const Rml::Element* bar = withAttribute(target, "bar"); bar != nullptr)
			{
				m_bar = bar->GetAttribute<Rml::String>("bar", "");
				const Bar* found = barOf(m_bar);
				m_barMouse = event.GetParameter<float>("mouse_x", 0.0F);
				m_barFrom = found == nullptr ? 0.0 : value(found->from);
				m_barTo = found == nullptr ? 0.0 : value(found->to);
				--m_gesture;
				showMarkers();
			}
			else if (const Rml::Element* handle = withAttribute(target, "handle"); handle != nullptr)
			{
				m_handle = handle->GetAttribute<Rml::String>("handle", "");
				--m_gesture;
				show();
			}
			return;
		case Rml::EventId::Drag:
			if (!m_dragged.empty())
				drag(event);
			else if (!m_bar.empty())
				dragBar(event);
			else if (!m_handle.empty())
				dragKey(event);
			return;
		case Rml::EventId::Dragend:
			if (m_snapToZero && snapsToZero(m_dragged))
				snapToZero(m_dragged, m_gesture);
			else if (m_snapToZero && !m_bar.empty())
				snapBarToZero(m_bar, m_gesture);
			if (!m_dragged.empty() || !m_bar.empty())
			{
				m_dragged.clear();
				m_bar.clear();
				showMarkers();
			}
			if (!m_handle.empty())
			{
				m_handle.clear();
				show();
			}
			return;
		case Rml::EventId::Click:
		{
			if (target == nullptr || m_track == 0)
				return;
			if (target->GetId() == "sampler-import" && m_importSample)
			{
				m_importSample(m_track, m_lock);
			}
			else if (target->GetId() == "sampler-normalize")
			{
				m_binding.send("sampler.normalize", {{"track", m_track}, {"level", m_level}},
					[this, alive = std::weak_ptr<bool>(m_alive), track = m_track](const engine::Result& result)
					{
						if (!alive.expired() && track == m_track)
							showMessage(result.ok() ? "" : "Normalize Sample failed: " + result.error);
					});
			}
			else if (target->GetId() == "sampler-midi-reset")
			{
				// All Sound Off and Reset All Controllers on every MIDI channel, straight to the sampler (p. 716).
				Json messages = Json::array();
				for (int channel = 0; channel < 16; ++channel)
					for (const int controller : {120, 121})
						messages.push_back({{"position", 0}, {"bytes", {0xB0 + channel, controller, 0}}});
				m_binding.send("midi.inject",
					{{"channel", m_channel.at("id")}, {"direct", true}, {"messages", std::move(messages)}},
					[this, alive = std::weak_ptr<bool>(m_alive), track = m_track](const engine::Result& result)
					{
						if (!alive.expired() && track == m_track)
							showMessage(result.ok() ? "" : "MIDI Reset failed: " + result.error);
					});
			}
			else if (const Rml::Element* key = withAttribute(target, "key"); key != nullptr)
			{
				if (const Parameter* root = parameter("Root key"))
					set(root->id, key->GetAttribute<int>("key", 60), 0);
				show();
			}
			return;
		}
		case Rml::EventId::Change:
		{
			if (!m_showing && target != nullptr && target->GetId() == "sampler-lock")
			{
				m_lock = event.GetParameter<bool>("value", false);
				saveSettings();
				return;
			}
			if (!m_showing && target != nullptr && target->GetId() == "sampler-read" && m_track != 0)
			{
				// Every automation lane of the track reads, or none does, as the track list's button does (p. 712).
				const bool read = event.GetParameter<bool>("value", false);
				Json commands = Json::array();
				for (const uint64_t lane : m_lanes)
					commands.push_back(
						{{"command", "automationLane.set"}, {"parameters", {{"lane", lane}, {"read", read}}}});
				if (!commands.empty())
					m_binding.send("history.transaction",
						{{"description", "Read Automation"}, {"commands", std::move(commands)}},
						[this, alive = std::weak_ptr<bool>(m_alive), track = m_track](const engine::Result& result)
						{
							if (alive.expired() || track != m_track)
								return;
							showMessage(result.ok() ? "" : "Read Automation failed: " + result.error);
							if (!result.ok())
								show(); // the toggle shows the engine's state again: a refusal brings no project read
						});
				return;
			}
			if (!m_showing && target != nullptr && target->GetId() == "sampler-zero")
			{
				m_snapToZero = event.GetParameter<bool>("value", false);
				saveSettings();
				return;
			}
			if (!m_showing && target != nullptr && target->GetId() == "sampler-ruler-format")
			{
				const auto format = event.GetParameter<Rml::String>("value", "seconds");
				if (format == m_rulerFormat)
					return;
				m_rulerFormat = format;
				saveSettings();
				showView();
				return;
			}
			if (!m_showing && target != nullptr &&
				(target->GetId() == "sampler-horizontal-zoom" || target->GetId() == "sampler-scroll" ||
					target->GetId() == "sampler-vertical-zoom"))
			{
				zoom(target->GetId(), event.GetParameter<float>("value", 0.0F));
				return;
			}
			if (!m_showing && target != nullptr && target->GetId() == "sampler-level")
			{
				m_level = std::clamp(static_cast<double>(event.GetParameter<float>("value", 0.0F)), -96.0, 0.0);
				saveSettings();
				return;
			}
			const Rml::Element* control = withAttribute(target, "parameter");
			if (m_showing || control == nullptr || control != target)
				return;
			const auto id = static_cast<uint32_t>(control->GetAttribute<int>("parameter", 0));
			double changed = 0.0;
			if (control->GetTagName() == "button")
				changed = event.GetParameter<bool>("value", false) ? 1.0 : 0.0;
			else if (control->GetTagName() == "select")
				changed = std::strtod(event.GetParameter<Rml::String>("value", "0").c_str(), nullptr);
			else
				changed = event.GetParameter<float>("value", 0.0F);
			set(id, changed, control->GetTagName() == "valuefield" ? event.GetParameter<int>("gesture", 0) : 0);
			showMarkers();
			return;
		}
		default:
			break;
		}
		if (event.GetType() != g_fileDropEvent || m_track == 0)
			return;
		// A file dropped on the panel loads into the sampler (p. 710), the first of several.
		std::istringstream lines(event.GetParameter<Rml::String>("files", ""));
		std::string file;
		std::getline(lines, file);
		if (file.empty())
			return;
		load(m_track, file, m_lock);
	}
}
