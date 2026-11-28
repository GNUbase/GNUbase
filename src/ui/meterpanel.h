// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/meters.h"
#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	// A level as a <meter> shows it: in dB with one decimal, or below every scale for silence.
	std::string meterValue(float level);

	// A level meter's scale (reference manual pp. 534 and 1614): its bars' range, its marks, each a level and its
	// label, and where its headroom starts, all in dBFS; aligned for the broadcast scales, which an alignment level
	// moves.
	struct MeterScale
	{
		float min;
		float max;
		float headroom;
		bool aligned;
		std::vector<std::pair<float, std::string>> marks;
	};

	// A Peak Program Meter standard's scale by the user setting meter.scale's names: digital, din, ebu, british,
	// nordic, k20, k14, k12, digital3 (the channel meters'), digital6 and digital12; Digital for others. Broadcast
	// scales put their alignment level (meter.alignmentLevel) at alignment dBFS.
	MeterScale meterScale(const std::string& name, float alignment = -18.0F);

	// A scale's marks beside its bars, each at its level's height, and its headroom's line (.meter-headroom).
	std::string meterScaleMarks(const MeterScale& scale);

	// The UI's one reader of the meters (Engine::readMeters, docs/features/metering.md), which every view that shows
	// meters shares, since a second reader would take the first one's peaks: while any view shows meters, it reads
	// them once a frame, the channels of all views at once. Views read frame() and level() in their own
	// EngineBinding::watch, which they make after they called show(): watches run in the order they were made, so a
	// watch made before the reader's reads the previous frame. Use it on the UI thread; the engine and the binding must
	// outlive it.
	class MeterReader
	{
	public:
		MeterReader(engine::Engine& engine, EngineBinding& binding);

		// A view shows meters while it keeps what this returns: the channels whose levels it shows, which it may
		// change; none for only the main mix and the loudness meter.
		[[nodiscard]] std::shared_ptr<std::vector<uint64_t>> show();

		const engine::MeterFrame& frame() const { return m_frame; }
		// A channel's level in the last frame, or null if no view showed it then.
		const engine::LevelReading* level(uint64_t channel) const;

	private:
		void read();

		engine::Engine& m_engine;
		EngineBinding& m_binding;
		engine::MeterFrame m_frame;
		std::vector<std::weak_ptr<std::vector<uint64_t>>> m_views;
		std::optional<Subscription> m_reading; // while views show meters
		uint64_t m_reads = 0;
	};

	// The right zone's meter display (#590, docs/features/metering.md, reference manual pp. 533-538): the master meter,
	// a true peak meter per channel of the main mix with its RMS level, RMS Max. and Peak Max., and the loudness meter,
	// with the momentary and short-term loudness and their values as the engine measures them; the Master and
	// Loudness buttons at its bottom switch between them, and each page's reset resets its own values (meter.reset
	// with meters "main" or "loudness"), and a Ctrl-click on RMS Max. or Peak Max. resets that one alone. Configure
	// Meter Settings and Configure Loudness Settings open the meter settings pane (pp. 538-540) with the user settings
	// meter.loudnessUnit, meter.loudnessScale, meter.resetOnStart, meter.holdOnStop, meter.rmsAes17 and
	// meter.rmsResolution, and the thresholds above which Momentary Max., Short-Term, Integrated and True Peak turn red
	// (.over), and the measurement presets of those (meter.presets); Measure Loudness switches meter.measureLoudness,
	// the unit switch meter.loudnessUnit. The master meter's scale (meter.scale, meter.alignmentLevel, p. 534) and the
	// loudness meter's are left of the bars, and the master meter's bars fall back by meter.fallback (p. 1614). It
	// reads the meters once a frame through the reader. Use it on the UI thread; the reader and the binding must
	// outlive it.
	class MeterPanel final : private Rml::EventListener
	{
	public:
		MeterPanel(MeterReader& meters, EngineBinding& binding, Rml::Element& element);
		~MeterPanel() override;
		MeterPanel(const MeterPanel&) = delete;
		MeterPanel& operator=(const MeterPanel&) = delete;
		MeterPanel(MeterPanel&&) = delete;
		MeterPanel& operator=(MeterPanel&&) = delete;

	private:
		void ProcessEvent(Rml::Event& event) override;
		// What the display shows: each main channel's true peak, RMS level and peak maximum, then the texts.
		std::vector<std::string> read();
		void show(const std::vector<std::string>& shown);
		// A user setting's value in the meter settings pane.
		void showSetting(const std::string& key, const engine::Json& value);
		// The pages' scales, by the settings meter.scale, meter.alignmentLevel, meter.loudnessScale and
		// meter.loudnessUnit.
		void showScales();
		// The measurement presets in their list (the setting meter.presets).
		void showPresets(const engine::Json& presets);

		MeterReader& m_reader;
		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::shared_ptr<std::vector<uint64_t>> m_shows; // before m_meters, so the reader reads first
		Subscription m_meters;
		std::optional<Subscription> m_settings;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(); // for the answers to settings.get
		bool m_showing = false;                                   // while showSetting sets a control
		std::set<std::string> m_shown;   // the settings whose values arrived: their lists send only after that
		bool m_lu = false;               // the user setting meter.loudnessUnit is lu
		bool m_wide = false;             // meter.loudnessScale is ebu18
		std::string m_scale = "digital"; // meter.scale
		float m_alignment = -18.0F;      // meter.alignmentLevel, in dBFS
		double m_fallback = 20.0;        // meter.fallback, in dB per second
		std::vector<float> m_bars;       // the master meter's bars as they fall back
		std::chrono::steady_clock::time_point m_read;    // when read() read the meters last
		engine::Json m_presets = engine::Json::object(); // meter.presets
		std::map<std::string, double> m_thresholds;      // the settings meter.<name>Reference and meter.<name>Tolerance
	};
}
