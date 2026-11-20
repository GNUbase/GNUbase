// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/meterpanel.h"

#include "engine/analysis/analysis.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace gnubase::ui
{
	namespace
	{
		// The meters' attribute values for silence: below every scale.
		constexpr const char* g_silence = "-200";

		std::string decibels(float level)
		{
			const std::optional<double> db = engine::analysis::decibels(level);
			return db ? widgets::format(static_cast<float>(*db), 1) : g_silence;
		}

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

		// The display's elements, in the panel's; returns the display. The loudness meter's scale is EBU +9 (p. 536):
		// -41 to -14 LUFS.
		Rml::Element& build(Rml::Element& element)
		{
			element.SetInnerRML(
				R"(<div class="meter-display">)"
				R"(<div class="meter-page meter-master"><div class="meter-bars"/><div class="meter-values">)"
				R"(<div>RMS Max.<span class="rms-max"/></div>)"
				R"(<div>Peak Max.<span class="peak-max"/></div></div></div>)"
				R"(<div class="meter-page meter-loudness"><div class="meter-bars">)"
				R"(<meter class="momentary" min="-41" max="-14"/><meter class="short-term" min="-41" max="-14"/>)"
				R"(</div><div class="meter-values">)"
				R"(<div>Momentary Max.<span class="momentary-max"/></div>)"
				R"(<div>Short-Term<span class="short-term-max"/></div>)"
				R"(<div>Integrated<span class="integrated"/></div>)"
				R"(<div>Range<span class="range"/></div>)"
				R"(<div>True Peak<span class="true-peak-max"/></div>)"
				R"(<div>Time<span class="time"/></div></div></div>)"
				R"(<div class="meter-buttons"><button class="meter-tab checked" value="master">Master</button>)"
				R"(<button class="meter-tab" value="loudness">Loudness</button>)"
				R"(<button class="meter-reset">Reset</button></div></div>)");
			return *element.GetFirstChild();
		}
	}

	MeterPanel::MeterPanel(engine::Engine& engine, EngineBinding& binding, Rml::Element& element)
		: m_engine(engine), m_binding(binding), m_root(build(element).GetObserverPtr()),
		  m_meters(binding.watch<std::vector<std::string>>(
			  m_root.get(), [this] { return read(); }, [this](const std::vector<std::string>& shown) { show(shown); }))
	{
		m_root->AddEventListener(Rml::EventId::Click, this);
	}

	MeterPanel::~MeterPanel()
	{
		if (m_root)
			m_root->RemoveEventListener(Rml::EventId::Click, this);
	}

	void MeterPanel::ProcessEvent(Rml::Event& event)
	{
		const Rml::Element* target = event.GetTargetElement();
		if (target == nullptr || !m_root)
			return;
		if (target->IsClassSet("meter-reset"))
		{
			m_binding.send("meter.reset");
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
		m_engine.readMeters(m_frame);
		const engine::LevelReading& main = m_frame.main;
		const engine::LoudnessReading& measured = m_frame.loudness;
		// The main channels' peaks and peak maxima, the loudness meter's bars, then the readouts.
		std::vector<std::string> shown;
		for (size_t c = 0; c < static_cast<size_t>(main.channels); ++c)
		{
			shown.push_back(decibels(main.peak[c]));
			shown.push_back(decibels(main.peakMax[c]));
		}
		shown.push_back(loudness(measured.momentary));
		shown.push_back(loudness(measured.integrated));
		shown.push_back(loudness(measured.shortTerm));
		shown.push_back(loudness(measured.shortTermMax));
		shown.push_back(levelText(largest(main.rmsMax, main.channels), "dB"));
		shown.push_back(levelText(largest(main.peakMax, main.channels), "dB"));
		shown.push_back(text(measured.momentaryMax, "LUFS"));
		shown.push_back(text(measured.shortTermMax, "LUFS"));
		shown.push_back(text(measured.integrated, "LUFS"));
		shown.push_back(text(measured.range, "LU"));
		shown.push_back(levelText(largest(measured.truePeakMax, measured.channels), "dBTP"));
		const auto seconds = static_cast<int64_t>(measured.seconds);
		shown.push_back(
			std::to_string(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60));
		return shown;
	}

	void MeterPanel::show(const std::vector<std::string>& shown)
	{
		if (!m_root)
			return;
		const size_t channels = (shown.size() - g_readouts.size() - 4) / 2;
		Rml::Element* bars = m_root->QuerySelector(".meter-master .meter-bars");
		if (bars->GetNumChildren() != static_cast<int>(channels))
		{
			std::string rml;
			for (size_t c = 0; c < channels; ++c)
				rml += R"(<meter min="-60" max="6"/>)";
			bars->SetInnerRML(rml);
		}
		for (size_t c = 0; c < channels; ++c)
		{
			Rml::Element* meter = bars->GetChild(static_cast<int>(c));
			meter->SetAttribute("value", shown[2 * c]);
			meter->SetAttribute("peak", shown[(2 * c) + 1]);
		}
		size_t at = 2 * channels;
		for (const char* bar : {".momentary", ".short-term"})
		{
			Rml::Element* meter = m_root->QuerySelector(bar);
			meter->SetAttribute("value", shown[at++]);
			meter->SetAttribute("peak", shown[at++]);
		}
		for (const char* readout : g_readouts)
			setText(m_root->QuerySelector(std::string(".") + readout), shown[at++]);
	}
}
