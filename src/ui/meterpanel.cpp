// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/meterpanel.h"

#include "engine/analysis/analysis.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		// The meters' attribute values for silence: below every scale.
		constexpr const char* g_silence = "-200";

		// A value of the display, or -inf for silence and -- for nothing measured.
		std::string text(std::optional<double> value, const char* unit)
		{
			return value ? widgets::format(static_cast<float>(*value), 1) + " " + unit : "--";
		}

		std::string levelText(float level, const char* unit)
		{
			const std::optional<double> db = engine::analysis::decibels(level);
			return db ? text(db, unit) : std::string("-inf ") + unit;
		}

		std::string loudness(const std::optional<double>& value)
		{
			return value ? widgets::format(static_cast<float>(*value), 1) : g_silence;
		}

		template <typename Levels> float largest(const Levels& levels, int channels)
		{
			return *std::max_element(levels.begin(), levels.begin() + std::max(channels, 1));
		}

		void setText(Rml::Element* element, const std::string& value)
		{
			const std::string rml = Rml::StringUtilities::EncodeRml(value);
			if (element != nullptr && element->GetInnerRML() != rml)
				element->SetInnerRML(rml);
		}

		// The readouts, in the order read() gives their texts after the bars.
		constexpr std::array<const char*, 8> g_readouts = {
			"rms-max", "peak-max", "momentary-max", "short-term-max", "integrated", "range", "true-peak-max", "time"};
		// The user settings that the meter settings pane shows, and Measure Loudness on the loudness page (p. 537).
		constexpr std::array<const char*, 7> g_settings = {"meter.loudnessUnit", "meter.loudnessScale",
			"meter.resetOnStart", "meter.holdOnStop", "meter.measureLoudness", "meter.rmsAes17", "meter.rmsResolution"};

		// The readouts with thresholds (p. 539), as the settings meter.<name>Reference and meter.<name>Tolerance name
		// them, and their elements.
		constexpr std::array<std::pair<const char*, const char*>, 4> g_thresholds = {{{"momentaryMax", "momentary-max"},
			{"shortTerm", "short-term-max"}, {"integrated", "integrated"}, {"truePeak", "true-peak-max"}}};

		// EBU R 128's reference level: 0 LU (p. 538).
		constexpr double g_referenceLufs = -23.0;

		// A Peak Program Meter standard (p. 534): its range, headroom and marks in its own dB, whose 0 is at zero dBFS,
		// or for the broadcast scales zero dB above the alignment level. DIN reads the alignment level at -9 dB, EBU
		// and Nordic at TEST (0 dB), with the permitted maximum level 9 dB above it, British at 4 with 4 dB a mark and
		// the permitted maximum level at 6; the K-System's headroom starts 4 dB above its 0.
		struct Standard
		{
			const char* name;
			float zero;
			bool aligned;
			float min;
			float max;
			float headroom;
			std::vector<float> marks;
		};

		const std::vector<Standard>& standards()
		{
			static const std::vector<Standard> all = {
				{"digital", 0.0F, false, -60.0F, 0.0F, 0.0F, {0, -5, -10, -15, -20, -30, -40, -50, -60}},
				{"din", 9.0F, true, -50.0F, 5.0F, 0.0F, {5, 0, -5, -10, -20, -30, -40, -50}},
				{"ebu", 0.0F, true, -12.0F, 12.0F, 9.0F, {12, 8, 4, 0, -4, -8, -12}},
				{"british", 0.0F, true, -12.0F, 12.0F, 8.0F, {12, 8, 4, 0, -4, -8, -12}},
				{"nordic", 0.0F, true, -36.0F, 12.0F, 9.0F, {12, 6, 0, -6, -12, -18, -24, -30, -36}},
				{"k20", -20.0F, false, -40.0F, 20.0F, 4.0F, {20, 16, 12, 8, 4, 0, -10, -20, -30, -40}},
				{"k14", -14.0F, false, -40.0F, 14.0F, 4.0F, {14, 12, 8, 4, 0, -10, -20, -30, -40}},
				{"k12", -12.0F, false, -40.0F, 12.0F, 4.0F, {12, 8, 4, 0, -10, -20, -30, -40}},
				{"digital3", 0.0F, false, -60.0F, 3.0F, 0.0F, {0, -6, -12, -18, -24, -30, -40, -50}},
				{"digital6", 0.0F, false, -60.0F, 6.0F, 0.0F, {6, 3, 0, -6, -12, -18, -24, -30, -40, -50}},
				{"digital12", 0.0F, false, -60.0F, 12.0F, 0.0F, {12, 6, 0, -6, -12, -18, -24, -30, -40, -50}}};
			return all;
		}

		// A scale mark's label: signed above 0.
		std::string markLabel(float level)
		{
			return (level > 0.0F ? "+" : "") + widgets::format(level);
		}

		// The loudness meter's scale (p. 536): EBU +9 every 3 LU or EBU +18 every 6 LU, labelled in LUFS or in LU.
		MeterScale loudnessScale(bool wide, bool lu)
		{
			MeterScale scale = {wide ? -59.0F : -41.0F, wide ? -5.0F : -14.0F, 0.0F, false, {}};
			scale.headroom = scale.max; // none
			const float step = wide ? 6.0F : 3.0F;
			for (int mark = 0; mark <= static_cast<int>((scale.max - scale.min) / step); ++mark)
			{
				const float level = scale.max - (static_cast<float>(mark) * step);
				scale.marks.emplace_back(
					level, lu ? markLabel(level - static_cast<float>(g_referenceLufs)) : widgets::format(level));
			}
			return scale;
		}

		// The settings pane's rows of reference and tolerance for each threshold (p. 539).
		std::string thresholds()
		{
			const auto field = [](const std::string& key, const char* title, const char* min, const char* max)
			{
				return R"(<valuefield setting="meter.)" + key + R"(" title=")" + title + R"(" min=")" + min +
					R"(" max=")" + max + R"(" step="0.1" decimals="1"/>)";
			};
			std::string rml;
			for (const auto& [name, label] :
				{std::pair{"momentaryMax", "Momentary Max."}, std::pair{"shortTerm", "Short-Term"},
					std::pair{"integrated", "Integrated"}, std::pair{"truePeak", "True Peak"}})
			{
				const bool peak = std::string(name) == "truePeak";
				rml += std::string(R"(<div class="meter-threshold"><span>)") + label + "</span>" +
					field(std::string(name) + "Reference", "Reference", peak ? "-20" : "-70", peak ? "3" : "0") +
					field(std::string(name) + "Tolerance", "Tolerance", "0", "10") + "</div>";
			}
			return rml;
		}

		// A list's or a field's value as its setting takes it: the alignment level list's and the fields' as numbers,
		// whole ones for fields without decimals.
		engine::Json settingValue(const Rml::Element& control, const Rml::Variant& value)
		{
			if (control.GetTagName() == "select")
			{
				const auto text = value.Get<Rml::String>();
				const std::optional<float> number = widgets::parse(text);
				return number ? engine::Json(*number) : engine::Json(text);
			}
			if (control.GetAttribute<Rml::String>("decimals", "") == "0")
				return std::lround(value.Get<double>());
			return value.Get<double>();
		}

		// The scale list's options (p. 534).
		std::string scales()
		{
			std::string rml;
			for (const auto& [value, label] :
				{std::pair{"digital", "Digital"}, std::pair{"din", "DIN"}, std::pair{"ebu", "EBU"},
					std::pair{"british", "British"}, std::pair{"nordic", "Nordic"}, std::pair{"k20", "K-20"},
					std::pair{"k14", "K-14"}, std::pair{"k12", "K-12"}, std::pair{"digital3", "+3 dB Digital"},
					std::pair{"digital6", "+6 dB Digital"}, std::pair{"digital12", "+12 dB Digital"}})
				rml += std::string(R"(<option value=")") + value + R"(">)" + label + "</option>";
			return rml;
		}

		// The alignment level list's options (p. 534), in dBFS.
		std::string alignments()
		{
			std::string rml;
			for (const int level : {-9, -12, -14, -15, -18, -20, -22})
				rml +=
					R"(<option value=")" + std::to_string(level) + R"(">)" + std::to_string(level) + " dBFS</option>";
			return rml;
		}

		// The display's elements, in the panel's; returns the display. The loudness meter's scale is EBU +9 (p. 536)
		// until the setting meter.loudnessScale arrives: -41 to -14 LUFS.
		Rml::Element& build(Rml::Element& element)
		{
			element.SetInnerRML(
				R"(<div class="meter-display">)"
				R"(<div class="meter-page meter-master"><div class="meter-scales"><select setting="meter.scale">)" +
				scales() + R"(</select><select setting="meter.alignmentLevel" disabled="">)" + alignments() +
				R"(</select></div><div class="meter-scale"/><div class="meter-bars"/><div class="meter-values">)"
				R"(<div reset="mainRmsMax">RMS Max.<span class="rms-max"/></div>)"
				R"(<div reset="mainTruePeakHold">Peak Max.<span class="peak-max"/></div></div>)"
				R"(<button class="meter-reset" value="main">Reset RMS Max. and Peak Max.</button>)"
				R"(<button class="meter-configure">Configure Meter Settings</button></div>)"
				R"(<div class="meter-page meter-loudness"><button class="meter-unit">LUFS</button>)"
				R"(<div class="meter-scale"/><div class="meter-bars">)"
				R"(<meter class="momentary" min="-41" max="-14"/><meter class="short-term" min="-41" max="-14"/>)"
				R"(</div><div class="meter-values">)"
				R"(<div>Momentary Max.<span class="momentary-max"/></div>)"
				R"(<div>Short-Term<span class="short-term-max"/></div>)"
				R"(<div>Integrated<span class="integrated"/></div>)"
				R"(<div>Range<span class="range"/></div>)"
				R"(<div>True Peak<span class="true-peak-max"/></div>)"
				R"(<div>Time<span class="time"/></div></div>)"
				R"(<button class="meter-measure" setting="meter.measureLoudness">Measure Loudness</button>)"
				R"(<button class="meter-reset" value="loudness">Reset Loudness</button>)"
				R"(<button class="meter-configure">Configure Loudness Settings</button></div>)"
				R"(<div class="meter-settings"><div><span>Preset</span><select class="meter-presets">)"
				R"(<option value="">No Preset</option></select></div><div class="meter-preset">)"
				R"(<input type="text" class="meter-preset-name"/><button class="meter-preset-save">Save</button>)"
				R"(<button class="meter-preset-remove">Remove</button></div><div class="meter-settings-title">Loudness</div>)" +
				thresholds() +
				R"(<div><span>Unit</span><select setting="meter.loudnessUnit"><option value="lufs">LUFS</option>)"
				R"(<option value="lu">LU</option></select></div>)"
				R"(<div><span>EBU Scale</span><select setting="meter.loudnessScale"><option value="ebu9">EBU +9</option>)"
				R"(<option value="ebu18">EBU +18</option></select></div>)"
				R"(<div><span>Reset on Start</span><input type="checkbox" setting="meter.resetOnStart"/></div>)"
				R"(<div><span>Hold on Stop</span><input type="checkbox" setting="meter.holdOnStop"/></div>)"
				R"(<div class="meter-settings-title">RMS</div>)"
				R"(<div><span>AES 17</span><input type="checkbox" setting="meter.rmsAes17"/></div>)"
				R"(<div><span>Resolution</span><valuefield setting="meter.rmsResolution" min="1" max="1000" step="1" )"
				R"(decimals="0" unit="ms"/></div></div>)"
				R"(<div class="meter-buttons"><button class="meter-tab checked" value="master">Master</button>)"
				R"(<button class="meter-tab" value="loudness">Loudness</button></div></div>)");
			return *element.GetFirstChild();
		}
	}

	std::string meterValue(float level)
	{
		const std::optional<double> db = engine::analysis::decibels(level);
		return db ? widgets::format(static_cast<float>(*db), 1) : g_silence;
	}

	MeterScale meterScale(const std::string& name, float alignment)
	{
		const std::vector<Standard>& all = standards();
		const auto found =
			std::find_if(all.begin(), all.end(), [&name](const Standard& standard) { return name == standard.name; });
		const Standard& standard = found != all.end() ? *found : all.front();
		const float zero = standard.zero + (standard.aligned ? alignment : 0.0F);
		MeterScale scale = {standard.min + zero, standard.max + zero, standard.headroom + zero, standard.aligned, {}};
		const bool british = std::string(standard.name) == "british";
		for (const float mark : standard.marks)
			scale.marks.emplace_back(
				mark + zero, british ? std::to_string(4 + static_cast<int>(mark / 4.0F)) : markLabel(mark));
		return scale;
	}

	std::string meterScaleMarks(const MeterScale& scale)
	{
		const auto top = [&scale](float level)
		{ return std::to_string(100.0 * (scale.max - level) / (scale.max - scale.min)) + "%"; };
		std::string rml;
		for (const auto& [level, label] : scale.marks)
			rml += R"(<span style="top: )" + top(level) + R"(">)" + label + "</span>";
		if (scale.headroom < scale.max)
			rml += R"(<div class="meter-headroom" style="top: )" + top(scale.headroom) + R"("/>)";
		return rml;
	}

	MeterReader::MeterReader(engine::Engine& engine, EngineBinding& binding) : m_engine(engine), m_binding(binding)
	{
	}

	std::shared_ptr<std::vector<uint64_t>> MeterReader::show()
	{
		auto view = std::make_shared<std::vector<uint64_t>>();
		m_views.push_back(view);
		if (!m_reading) // the watch keeps the shell stepping once a frame, so only while views show meters
			m_reading.emplace(m_binding.watch<uint64_t>(
				nullptr,
				[this]
				{
					read();
					return ++m_reads;
				},
				[](uint64_t) { return false; }));
		return view;
	}

	void MeterReader::read()
	{
		m_views.erase(std::remove_if(m_views.begin(), m_views.end(), [](const auto& view) { return view.expired(); }),
			m_views.end());
		m_frame.channels.clear();
		if (m_views.empty())
		{
			m_reading.reset();
			return;
		}
		for (const auto& view : m_views)
		{
			const std::shared_ptr<std::vector<uint64_t>> channels = view.lock();
			m_frame.channels.insert(m_frame.channels.end(), channels->begin(), channels->end());
		}
		m_engine.readMeters(m_frame);
	}

	const engine::LevelReading* MeterReader::level(uint64_t channel) const
	{
		const auto found = std::find(m_frame.channels.begin(), m_frame.channels.end(), channel);
		return found != m_frame.channels.end() && m_frame.levels.size() == m_frame.channels.size()
			? &m_frame.levels[static_cast<size_t>(found - m_frame.channels.begin())]
			: nullptr;
	}

	MeterPanel::MeterPanel(MeterReader& meters, EngineBinding& binding, Rml::Element& element)
		: m_reader(meters), m_binding(binding), m_root(build(element).GetObserverPtr()), m_shows(meters.show()),
		  m_meters(binding.watch<std::vector<std::string>>(
			  m_root.get(), [this] { return read(); }, [this](const std::vector<std::string>& shown) { show(shown); }))
	{
		m_root->AddEventListener(Rml::EventId::Click, this);
		m_root->AddEventListener(Rml::EventId::Change, this);
		m_settings = m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				for (const auto& [key, value] : event.data.at("values").items())
					showSetting(key, value);
			});
		std::vector<std::string> keys(g_settings.begin(), g_settings.end());
		keys.insert(keys.end(), {"meter.scale", "meter.alignmentLevel", "meter.fallback", "meter.presets"});
		for (const auto& threshold : g_thresholds)
			for (const char* part : {"Reference", "Tolerance"})
				keys.push_back(std::string("meter.") + threshold.first + part);
		for (const std::string& key : keys)
			m_binding.send("settings.get", {{"key", key}},
				[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
				{
					if (!alive.expired() && result.ok())
						showSetting(result.value.at("key").get<std::string>(), result.value.at("value"));
				});
		showScales();
	}

	MeterPanel::~MeterPanel()
	{
		if (!m_root)
			return;
		m_root->RemoveEventListener(Rml::EventId::Click, this);
		m_root->RemoveEventListener(Rml::EventId::Change, this);
	}

	void MeterPanel::showSetting(const std::string& key, const engine::Json& value)
	{
		m_shown.insert(key);
		if (key == "meter.fallback")
			m_fallback = value.get<double>();
		else if (key == "meter.scale")
			m_scale = value.get<std::string>();
		else if (key == "meter.alignmentLevel")
			m_alignment = value.get<float>();
		else if (key == "meter.loudnessUnit")
			m_lu = value == "lu";
		else if (key == "meter.loudnessScale")
			m_wide = value == "ebu18";
		if (key == "meter.presets")
			showPresets(value);
		if (key == "meter.scale" || key == "meter.alignmentLevel" || key == "meter.loudnessUnit" ||
			key == "meter.loudnessScale")
			showScales();
		Rml::Element* control = m_root ? m_root->QuerySelector("[setting=\"" + key + "\"]") : nullptr;
		if (control == nullptr)
			return;
		// A check box sends a change for this, which mustn't go back to the engine: an older value, such as the answer
		// to settings.get, would undo a click whose settings.set is still on its way.
		m_showing = true;
		if (auto* select = rmlui_dynamic_cast<Rml::ElementFormControl*>(control);
			select != nullptr && control->GetTagName() == "select")
			select->SetValue(value.is_number() ? widgets::format(value.get<float>()) : value.get<std::string>());
		else if (control->GetTagName() == "button")
			control->SetClass("checked", value.is_boolean() && value.get<bool>());
		else if (value.is_boolean() && value.get<bool>())
			control->SetAttribute("checked", "");
		else if (value.is_boolean())
			control->RemoveAttribute("checked");
		else if (value.is_number())
			control->SetAttribute("value", widgets::format(value.get<float>()));
		m_showing = false;
		if (value.is_number())
			m_thresholds[key] = value.get<double>();
	}

	void MeterPanel::showPresets(const engine::Json& presets)
	{
		// The measurement presets (p. 539), by name, after No Preset.
		auto* list = m_root
			? rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(m_root->QuerySelector(".meter-presets"))
			: nullptr;
		if (list == nullptr || !presets.is_object())
			return;
		m_presets = presets;
		m_showing = true;
		list->RemoveAll();
		list->Add("No Preset", "");
		for (const auto& [name, values] : presets.items())
			list->Add(Rml::StringUtilities::EncodeRml(name), name);
		m_showing = false;
	}

	void MeterPanel::showScales()
	{
		if (!m_root)
			return;
		// The master meter's scale (p. 534); the alignment level only for the broadcast scales.
		const MeterScale master = meterScale(m_scale, m_alignment);
		m_root->QuerySelector(".meter-master .meter-scale")->SetInnerRML(meterScaleMarks(master));
		Rml::ElementList bars;
		m_root->QuerySelectorAll(bars, ".meter-master .meter-bars meter");
		for (Rml::Element* bar : bars)
		{
			bar->SetAttribute("min", widgets::format(master.min));
			bar->SetAttribute("max", widgets::format(master.max));
		}
		Rml::Element* alignment = m_root->QuerySelector("[setting=\"meter.alignmentLevel\"]");
		if (master.aligned)
			alignment->RemoveAttribute("disabled");
		else
			alignment->SetAttribute("disabled", "");
		// The loudness unit and scale (pp. 536-539): values and marks in LU are relative to -23 LUFS; the bars stay in
		// LUFS.
		const MeterScale loudness = loudnessScale(m_wide, m_lu);
		m_root->QuerySelector(".meter-loudness .meter-scale")->SetInnerRML(meterScaleMarks(loudness));
		for (const char* bar : {".momentary", ".short-term"})
		{
			m_root->QuerySelector(bar)->SetAttribute("min", widgets::format(loudness.min));
			m_root->QuerySelector(bar)->SetAttribute("max", widgets::format(loudness.max));
		}
		setText(m_root->QuerySelector(".meter-unit"), m_lu ? "LU" : "LUFS");
	}

	void MeterPanel::ProcessEvent(Rml::Event& event)
	{
		const Rml::Element* target = event.GetTargetElement();
		if (target == nullptr || !m_root)
			return;
		if (event == Rml::EventId::Change)
		{
			if (m_showing)
				return;
			const auto given = event.GetParameters().find("value");
			if (target->IsClassSet("meter-presets") && given != event.GetParameters().end())
			{
				// Loading a measurement preset (p. 539) sets its thresholds.
				const auto preset = m_presets.find(given->second.Get<Rml::String>());
				if (preset != m_presets.end())
					m_binding.send("settings.set", {{"values", *preset}});
				return;
			}
			// The meter settings pane's check boxes, the Unit and EBU Scale lists, the threshold fields and the RMS
			// resolution in ms (pp. 539-540).
			// A list selects its first option before its setting's value arrived, which mustn't overwrite it.
			const auto key = target->GetAttribute<Rml::String>("setting", "");
			if (key.empty() || (target->GetTagName() == "select" && m_shown.count(key) == 0))
				return;
			if (target->GetAttribute<Rml::String>("type", "") == "checkbox")
				m_binding.send("settings.set", {{"key", key}, {"value", target->HasAttribute("checked")}});
			else if (given != event.GetParameters().end())
				m_binding.send("settings.set", {{"key", key}, {"value", settingValue(*target, given->second)}});
			return;
		}
		if (target->IsClassSet("meter-measure"))
		{
			// Measure Loudness (p. 537) switches the measurement on and off.
			m_binding.send(
				"settings.set", {{"key", "meter.measureLoudness"}, {"value", !target->IsClassSet("checked")}});
			return;
		}
		if (target->IsClassSet("meter-preset-save") || target->IsClassSet("meter-preset-remove"))
		{
			// Save keeps the thresholds as they are under the typed name, Remove removes the listed preset (p. 539).
			const bool save = target->IsClassSet("meter-preset-save");
			const auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(
				m_root->QuerySelector(save ? ".meter-preset-name" : ".meter-presets"));
			const std::string name = control != nullptr ? control->GetValue() : std::string();
			if (!name.empty() && (save || m_presets.contains(name)))
				m_binding.send(save ? "meter.savePreset" : "meter.removePreset", {{"name", name}});
			return;
		}
		if (target->IsClassSet("meter-unit"))
		{
			// The switch between LU and LUFS (p. 537).
			m_binding.send("settings.set", {{"key", "meter.loudnessUnit"}, {"value", m_lu ? "lufs" : "lu"}});
			return;
		}
		if (target->IsClassSet("meter-configure"))
		{
			// Configure Meter Settings and Configure Loudness Settings open and close the settings pane (p. 538).
			const bool open = !m_root->IsClassSet("settings");
			m_root->SetClass("settings", open);
			Rml::ElementList buttons;
			m_root->QuerySelectorAll(buttons, ".meter-configure");
			for (Rml::Element* button : buttons)
				button->SetClass("checked", open);
			return;
		}
		// Ctrl-click (Cmd on macOS) on RMS Max. or Peak Max. resets it alone (p. 535).
		const Rml::Element* value = target->HasAttribute("reset") ? target : target->Closest("[reset]");
		if (value != nullptr &&
			(event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0))
		{
			m_binding.send("meter.reset", {{"meters", value->GetAttribute<Rml::String>("reset", "")}});
		}
		else if (target->IsClassSet("meter-reset"))
		{
			// Each page resets only its own values (pp. 535 and 538).
			m_binding.send("meter.reset", {{"meters", target->GetAttribute<Rml::String>("value", "")}});
		}
		else if (target->IsClassSet("meter-tab"))
		{
			const bool loudness = target->GetAttribute<Rml::String>("value", "") == "loudness";
			m_root->SetClass("loudness", loudness);
			Rml::ElementList tabs;
			m_root->QuerySelectorAll(tabs, ".meter-tab");
			for (Rml::Element* tab : tabs)
				tab->SetClass("checked", tab == target);
		}
	}

	std::vector<std::string> MeterPanel::read()
	{
		const engine::LevelReading& main = m_reader.frame().main;
		const engine::LoudnessReading& measured = m_reader.frame().loudness;
		// The main channels' true peaks, RMS levels and true peak maxima, the loudness meter's bars, then the readouts.
		// The bars fall back from the peaks by meter.fallback (p. 1614), and below their scale to silence.
		const auto now = std::chrono::steady_clock::now();
		const auto fall = static_cast<float>(
			std::pow(10.0, -m_fallback * std::chrono::duration<double>(now - m_read).count() / 20.0));
		m_read = now;
		const float end = std::pow(10.0F, meterScale(m_scale, m_alignment).min / 20.0F);
		m_bars.resize(static_cast<size_t>(main.channels));
		std::vector<std::string> shown;
		for (size_t c = 0; c < static_cast<size_t>(main.channels); ++c)
		{
			float& bar = m_bars[c];
			bar = std::max(measured.truePeak[c], bar * fall);
			if (bar < end)
				bar = measured.truePeak[c];
			shown.push_back(meterValue(bar));
			shown.push_back(meterValue(main.rms[c]));
			shown.push_back(meterValue(measured.truePeakHold[c]));
		}
		shown.push_back(loudness(measured.momentary));
		shown.push_back(loudness(measured.integrated));
		shown.push_back(loudness(measured.shortTerm));
		shown.push_back(loudness(measured.shortTermMax));
		shown.push_back(levelText(largest(main.rmsMax, main.channels), "dB"));
		shown.push_back(levelText(largest(measured.truePeakHold, main.channels), "dB"));
		const auto relative = [this](std::optional<double> value)
		{ return m_lu ? text(value ? std::optional(*value - g_referenceLufs) : value, "LU") : text(value, "LUFS"); };
		shown.push_back(relative(measured.momentaryMax));
		shown.push_back(relative(measured.shortTermMax));
		shown.push_back(relative(measured.integrated));
		shown.push_back(text(measured.range, "LU"));
		shown.push_back(levelText(largest(measured.truePeakMax, measured.channels), "dBTP"));
		const auto seconds = static_cast<int64_t>(measured.seconds);
		shown.push_back(
			std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60));
		// Whether each value with a threshold is above its reference plus its tolerance (p. 539).
		const float truePeak = largest(measured.truePeakMax, measured.channels);
		const std::array<std::optional<double>, g_thresholds.size()> values = {
			measured.momentaryMax, measured.shortTermMax, measured.integrated, engine::analysis::decibels(truePeak)};
		for (size_t i = 0; i < values.size(); ++i)
		{
			const std::string key = std::string("meter.") + g_thresholds.at(i).first;
			const auto reference = m_thresholds.find(key + "Reference");
			const auto tolerance = m_thresholds.find(key + "Tolerance");
			const std::optional<double>& value = values.at(i);
			shown.emplace_back(value.has_value() && reference != m_thresholds.end() &&
						tolerance != m_thresholds.end() && value.value_or(0.0) > reference->second + tolerance->second
					? "over"
					: "");
		}
		return shown;
	}

	void MeterPanel::show(const std::vector<std::string>& shown)
	{
		if (!m_root)
			return;
		const size_t channels = (shown.size() - g_readouts.size() - g_thresholds.size() - 4) / 3;
		Rml::Element* bars = m_root->QuerySelector(".meter-master .meter-bars");
		if (bars->GetNumChildren() != static_cast<int>(channels))
		{
			std::string rml;
			for (size_t c = 0; c < channels; ++c)
				rml += "<meter/>";
			bars->SetInnerRML(rml);
			showScales();
		}
		for (size_t c = 0; c < channels; ++c)
		{
			Rml::Element* meter = bars->GetChild(static_cast<int>(c));
			meter->SetAttribute("value", shown[3 * c]);
			meter->SetAttribute("rms", shown[(3 * c) + 1]);
			meter->SetAttribute("peak", shown[(3 * c) + 2]);
		}
		size_t at = 3 * channels;
		for (const char* bar : {".momentary", ".short-term"})
		{
			Rml::Element* meter = m_root->QuerySelector(bar);
			meter->SetAttribute("value", shown[at++]);
			meter->SetAttribute("peak", shown[at++]);
		}
		for (const char* readout : g_readouts)
			setText(m_root->QuerySelector(std::string(".") + readout), shown[at++]);
		for (const auto& [name, element] : g_thresholds)
			m_root->QuerySelector(std::string(".") + element)->SetClass("over", shown[at++] == "over");
	}
}
