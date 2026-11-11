// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/samplerpanel.h"

#include "ui/canvas.h"
#include "ui/rmlui.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr std::array<Rml::EventId, 3> g_events = {
			Rml::EventId::Mousedown, Rml::EventId::Click, Rml::EventId::Change};

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
				{"playback", "Playback", {"Playback mode", "Loop mode", "Start", "End", "Loop start", "Loop end"}},
				{"voices", "", {"Reverse", "One shot", "Monophonic", "Legato", "Slices", "Sample gain"}},
				{"pitch", "Pitch", {"Fixed pitch", "Octave", "Coarse", "Fine"}},
				{"filter", "Filter", {"Filter", "Filter type", "Cutoff", "Resonance", "Drive", "Cutoff key follow"}},
				{"amp", "Amp", {"Volume", "Pan", "Attack", "Decay", "Sustain", "Release"}},
				{"keyboard", "Keyboard", {"Pitchbend", "Root key", "Key low", "Key high"}},
			};
			return list;
		}

		// The markers of the waveform display and the parameters they set (p. 717).
		constexpr std::array<const char*, 4> g_markers = {"Start", "End", "Loop start", "Loop end"};

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
		for (const char* marker : g_markers)
			markers += std::string(R"(<div class="sampler-marker)") +
				(std::string(marker).rfind("Loop", 0) == 0 ? " loop" : "") + R"(" marker=")" + marker +
				R"("><div class="sampler-marker-line"/><div class="sampler-marker-flag"/></div>)";
		element.SetInnerRML(
			R"(<div class="sampler-panel empty" id="sampler-panel">)"
			R"(<div class="sampler-hint">Select a sampler track</div>)"
			R"(<div class="sampler-toolbar">)"
			R"(<button id="sampler-import" title="Import Audio File">Import</button>)"
			R"(<span id="sampler-file" class="sampler-file"/>)"
			R"(<span id="sampler-message" class="sampler-message"/>)"
			R"(<valuefield id="sampler-level" class="sampler-level" title="Normalization Level" min="-96" max="0")"
			R"( default="0" value="0" decimals="1" unit="dB"/>)"
			R"(<button id="sampler-normalize" title="Normalize Sample">Normalize</button>)"
			R"(<button id="sampler-lock" type="toggle" title="Lock Parameter Settings">Lock</button>)"
			R"(</div><div class="sampler-display" id="sampler-display"><waveform class="sampler-wave"/>)"
			R"(<div class="sampler-outside sampler-before"/><div class="sampler-outside sampler-after"/>)"
			R"(<div class="sampler-loop"/>)" +
			markers + R"(</div><div class="sampler-handles" id="sampler-handles">)" + handles +
			R"(</div><div class="sampler-keyboard" id="sampler-keyboard">)" + keys +
			R"(</div><div class="sampler-sections" id="sampler-sections"/></div>)");
		for (const Rml::EventId id : g_events)
			element.AddEventListener(id, this);
		element.AddEventListener(m_dropEvent, this);
		if (Rml::ElementDocument* document = element.GetOwnerDocument(); document != nullptr)
		{
			document->AddEventListener(Rml::EventId::Mousemove, this);
			document->AddEventListener(Rml::EventId::Mouseup, this);
			m_document = document->GetObserverPtr();
		}
		for (const char* name : {"media.peaksProgress", "media.peaksReady"})
			m_subscriptions.push_back(m_binding.on(name,
				[this](const engine::Event& event)
				{
					if (m_media != 0 && event.data.value("media", Json()) == m_media)
						ask();
				}));
		// Lock Parameter Settings and the Normalization Level are a user setting, so they outlast the panel.
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto settings = values.find("ui.samplerPanel"); settings != values.end())
				{
					m_settingsKnown = true;
					applySettings(*settings);
				}
			}));
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
		if (Rml::Element* root = m_root.get())
		{
			for (const Rml::EventId id : g_events)
				root->RemoveEventListener(id, this);
			root->RemoveEventListener(m_dropEvent, this);
		}
		if (Rml::Element* document = m_document.get())
		{
			document->RemoveEventListener(Rml::EventId::Mousemove, this);
			document->RemoveEventListener(Rml::EventId::Mouseup, this);
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
		const auto set = m_values.find(found->id);
		return set != m_values.end() ? set->second : found->defaultValue;
	}

	std::string SamplerPanel::stepKey(uint32_t id) const
	{
		return m_stepOwner + "channel " + m_channel.value("id", Json()).dump() + " instrument " + std::to_string(id);
	}

	void SamplerPanel::update(uint64_t track, const Json& project, uint64_t loads, uint64_t read)
	{
		m_read = read;
		// The waveform's columns follow the display's width once it is laid out, and whenever it changes.
		if (Rml::Element* wave = m_root ? m_root->QuerySelector(".sampler-wave") : nullptr;
			wave != nullptr && m_media != 0 && wave->GetClientWidth() != m_askedWidth)
			ask();
		if (track == m_selected && loads == m_loads)
			return;
		m_selected = track;
		m_loads = loads;
		const Json* found = findIn(project, "tracks", track);
		const Json* channel = found == nullptr ? nullptr : findIn(project, "channels", found->value("channel", Json()));
		if (channel != nullptr && channel->value("instrument", std::string()) != "sampler")
			channel = nullptr;
		if ((channel != nullptr ? track : 0) != m_track)
			showMessage(""); // what failed concerned the track shown before
		m_track = channel != nullptr ? track : 0;
		m_channel = channel != nullptr ? *channel : Json();
		// A parameter whose change is on its way keeps what the user gave it, not the engine's older value.
		std::map<uint32_t, double> values;
		if (channel != nullptr)
			for (const Json& item : channel->value("instrumentParameters", Json::array()))
				values[item.at("parameter").get<uint32_t>()] = item.at("value").get<double>();
		for (const auto& [id, set] : m_values)
			if (channel != nullptr && m_binding.ahead(stepKey(id), m_read))
				values[id] = set;
		m_values = std::move(values);
		uint64_t media = 0;
		int64_t length = 0;
		std::string file = "No sample";
		const Json* clip = channel == nullptr ? nullptr : findIn(project, "clips", channel->value("sample", Json()));
		if (const Json* medium = clip == nullptr ? nullptr : findIn(project, "media", clip->at("media")))
		{
			media = medium->at("id").get<uint64_t>();
			length = medium->at("length").get<int64_t>();
			m_mediaChannels = std::max(medium->value("channels", 1), 1);
			file = clip->value("name", std::string());
		}
		setText(find("sampler-file"), file);
		if (media != m_media || length != m_length)
		{
			m_media = media;
			m_length = length;
			ask();
		}
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
		showMarkers();
	}

	// Places the markers, the loop and the shades outside the start and end, in percent of the sample: geometry from
	// the parameters. Reversed, the sampler counts them from the sample's end (docs/features/sampler-track.md).
	void SamplerPanel::showMarkers()
	{
		Rml::Element* display = find("sampler-display");
		if (display == nullptr)
			return;
		const bool reverse = value("Reverse") >= 0.5;
		const auto at = [&](const char* name)
		{
			const double share = std::clamp(value(name), 0.0, 1.0);
			return reverse ? 1.0 - share : share;
		};
		Rml::ElementList markers;
		display->QuerySelectorAll(markers, ".sampler-marker");
		const bool looping = std::lround(value("Loop mode")) != 0;
		for (Rml::Element* marker : markers)
		{
			const auto name = marker->GetAttribute<Rml::String>("marker", "");
			marker->SetProperty(Rml::PropertyId::Left, percent(at(name.c_str())));
			setClass(marker, "dragged", name == m_dragged);
			setClass(marker, "off", !looping && name.rfind("Loop", 0) == 0);
		}
		const double start = std::min(at("Start"), at("End"));
		const double end = std::max(at("Start"), at("End"));
		if (Rml::Element* before = display->QuerySelector(".sampler-before"))
			before->SetProperty(Rml::PropertyId::Width, percent(start));
		if (Rml::Element* after = display->QuerySelector(".sampler-after"))
		{
			after->SetProperty(Rml::PropertyId::Left, percent(end));
			after->SetProperty(Rml::PropertyId::Width, percent(1.0 - end));
		}
		if (Rml::Element* loop = display->QuerySelector(".sampler-loop"))
		{
			const double from = std::min(at("Loop start"), at("Loop end"));
			loop->SetProperty(Rml::PropertyId::Left, percent(from));
			loop->SetProperty(Rml::PropertyId::Width, percent(std::max(at("Loop start"), at("Loop end")) - from));
			setClass(loop, "off", !looping);
		}
	}

	// Asks media.peaks for the whole sample, a column per pixel of the display; one question in flight, and what
	// changed meanwhile is asked once it is answered.
	void SamplerPanel::ask()
	{
		Rml::Element* root = m_root.get();
		auto* wave = dynamic_cast<Waveform*>(root == nullptr ? nullptr : root->QuerySelector(".sampler-wave"));
		if (wave == nullptr)
			return;
		if (m_media == 0 || m_length < 1)
		{
			wave->setPeaks({}, {});
			return;
		}
		if (m_asking)
		{
			m_again = true;
			return;
		}
		const float width = wave->GetClientWidth();
		m_askedWidth = width;
		const Rml::Context* context = wave->GetContext();
		const float ratio = context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
		if (!(width > 0.0F))
			return;
		wave->SetAttribute("start", 0.0);
		wave->SetAttribute("scale", static_cast<double>(m_length) * ratio / width);
		const int64_t pixels =
			std::clamp<int64_t>(std::lround(width), 1, std::min(g_maxPixels, g_maxColumnValues / m_mediaChannels));
		m_asking = true;
		m_binding.send("media.peaks", {{"media", m_media}, {"start", 0}, {"frames", m_length}, {"pixels", pixels}},
			[this, alive = std::weak_ptr<bool>(m_alive), media = m_media, length = m_length](
				const engine::Result& result)
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
					waveform->setPeaks({0.0, static_cast<double>(length)}, std::move(columns));
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
		m_values[id] = value;
		const Json step = {{"command", "parameter.set"},
			{"parameters",
				{{"channel", m_channel.at("id")}, {"target", {{"type", "instrument"}, {"parameter", id}}},
					{"value", value}}}};
		m_binding.sendStep(gesture, stepKey(id), "Change " + (found != m_parameters.end() ? found->name : "Sampler"),
			Json::array({step}));
	}

	// Moves the marker dragged to the mouse: Start stays before End, and the loop's start before its end.
	void SamplerPanel::drag(const Rml::Event& event)
	{
		Rml::Element* display = find("sampler-display");
		const Parameter* dragged = parameter(m_dragged);
		const float width = display == nullptr ? 0.0F : display->GetClientWidth();
		if (dragged == nullptr || !(width > 0.0F))
			return;
		const double x = std::clamp(
			(event.GetParameter<float>("mouse_x", 0.0F) - display->GetAbsoluteLeft()) / static_cast<double>(width), 0.0,
			1.0);
		double share = value("Reverse") >= 0.5 ? 1.0 - x : x;
		if (m_dragged == "Start")
			share = std::min(share, value("End"));
		else if (m_dragged == "End")
			share = std::max(share, value("Start"));
		else if (m_dragged == "Loop start")
			share = std::min(share, value("Loop end"));
		else
			share = std::max(share, value("Loop start"));
		set(dragged->id, share, m_gesture);
		showMarkers();
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
		m_level = std::clamp(m_settings.value("normalizeLevel", 0.0), -96.0, 0.0);
		show();
	}

	void SamplerPanel::saveSettings()
	{
		m_settingsKnown = true; // what settings.get answers now is older
		m_settings["lockParameters"] = m_lock;
		m_settings["normalizeLevel"] = m_level;
		m_binding.send("settings.set", {{"key", "ui.samplerPanel"}, {"value", m_settings}});
	}

	void SamplerPanel::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
			if (m_track == 0 || event.GetParameter<int>("button", 0) != 0)
				return;
			if (const Rml::Element* marker = withAttribute(target, "marker"); marker != nullptr)
			{
				m_dragged = marker->GetAttribute<Rml::String>("marker", "");
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
		case Rml::EventId::Mousemove:
			if (!m_dragged.empty())
				drag(event);
			else if (!m_handle.empty())
				dragKey(event);
			return;
		case Rml::EventId::Mouseup:
			if (!m_dragged.empty())
			{
				m_dragged.clear();
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
