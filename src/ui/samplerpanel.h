// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/rmlui.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Types.h>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

namespace gnubase::ui
{
	// The sampler panel (#1029, reference manual pp. 710-717, 730, docs/features/sampler-track.md), a panel of the
	// lower zone that shows the selected sampler track: its toolbar (Read Automation, the event received indicator,
	// Snap to Zero Crossing, Import Audio File, the sample's file name, Normalize Sample, Lock Parameter Settings and
	// MIDI Reset), the waveform display with the markers of the sample's start and end, the loop, the fades and the
	// loop crossfade and the bars between start and end and between the loop's ends, with its ruler and zoom sliders
	// (p. 717), the Playback, Pitch, Filter and Amp sections with a control per sampler parameter (from processor.list)
	// and the keyboard section with the pitchbend range, the root key and the key range above a keyboard that shows
	// them, with a handle for each (p. 730). The root gets the class empty while the selected track isn't a sampler
	// track.
	//
	// Controls, markers, bars and handles set the sampler's parameters (parameter.set), a drag or a widget's gesture
	// one undo step (EngineBinding::sendStep); a click on a key sets the root key. A file dropped on the panel, or
	// chosen with Import Audio File, loads into the sampler (sampler.load path), keeping the settings with Lock
	// Parameter Settings; Normalize Sample runs sampler.normalize with the toolbar's Normalization Level (p. 735). Why
	// a load, Normalize Sample or MIDI Reset failed shows in the toolbar (#1122). Lock Parameter Settings, the
	// Normalization Level and Snap to Zero Crossing are the user setting ui.samplerPanel, so they last when the panel
	// is made anew, and so is the ruler's format. Use it on the UI thread; the binding must outlive it.
	class SamplerPanel final : private Rml::EventListener
	{
	public:
		// importSample asks for a file to load into the sampler track, with the settings kept or not.
		SamplerPanel(EngineBinding& binding, Rml::Element& element,
			std::function<void(uint64_t track, bool keepSettings)> importSample = {});
		~SamplerPanel() override;
		SamplerPanel(const SamplerPanel&) = delete;
		SamplerPanel& operator=(const SamplerPanel&) = delete;
		SamplerPanel(SamplerPanel&&) = delete;
		SamplerPanel& operator=(SamplerPanel&&) = delete;

		// Shows a track (0: none) of the project as project.get returned it, the loads-th answer, which the binding
		// handed on as EngineBinding::handed() returned read, as the inspector does (ui/inspector.h).
		void update(uint64_t track, const engine::Json& project, uint64_t loads, uint64_t read);

		// The sampler track it shows, 0 for none.
		uint64_t track() const { return m_track; }

		// Loads the audio file at path into a sampler track, keeping its settings or not, and shows why it failed.
		void load(uint64_t track, const std::string& path, bool keepSettings);

	private:
		struct Parameter
		{
			uint32_t id = 0;
			std::string name;
			double minimum = 0.0;
			double maximum = 1.0;
			double defaultValue = 0.0;
			std::string unit;
			bool stepped = false;
			std::vector<std::string> options;
		};

		void ProcessEvent(Rml::Event& event) override;
		void build(const engine::Json& processors);
		void show();
		void showMarkers();
		double markerShare(const std::string& marker) const;
		double crossfadeLimit() const;
		double ordered(const std::string& marker, double share) const;
		void snapToZero(const std::string& marker, int gesture);
		void snapBarToZero(const std::string& id, int gesture);
		void listen(bool on);
		void readPorts(const engine::Json& ports);
		void received(const engine::Json& activity);
		void ask();
		void showView();
		void zoom(const std::string& slider, double value);
		double viewed(double position) const;
		void set(uint32_t id, double value, int gesture);
		void drag(const Rml::Event& event);
		void dragKey(const Rml::Event& event);
		void dragBar(const Rml::Event& event);
		void setBar(const std::string& id, double first, double second, int gesture);
		void showMessage(const std::string& text);
		void applySettings(const engine::Json& settings);
		void saveSettings();
		double value(const std::string& name) const;
		const Parameter* parameter(const std::string& name) const;
		std::string stepKey(uint32_t id) const;
		std::string barKey(const std::string& bar) const;
		Rml::Element* find(const std::string& id) const;

		EngineBinding& m_binding;
		std::function<void(uint64_t track, bool keepSettings)> m_importSample;
		const std::string m_stepOwner;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::vector<Parameter> m_parameters; // the sampler's, from processor.list
		std::map<uint32_t, double> m_values; // the channel's, the defaults filled in
		uint64_t m_track = 0;
		uint64_t m_selected = 0; // the track update showed last, a sampler track or not
		uint64_t m_loads = 0;
		uint64_t m_read = 0;
		engine::Json m_channel;        // the sampler track's, null without one
		std::vector<uint64_t> m_lanes; // its automation lanes
		bool m_reading = false;        // all of them read: Read Automation is on (p. 712)
		uint64_t m_media = 0;          // the media file its sample plays
		int64_t m_length = 0;          // in its frames
		int m_mediaChannels = 1;
		bool m_asking = false;     // media.peaks is in flight
		bool m_again = false;      // and asks again when it answers
		float m_askedWidth = 0.0F; // the display's width in pixels that it showed the part for
		std::string m_dragged;     // the marker a drag moves: the parameter's name
		std::string m_handle;      // the keyboard's handle a drag moves: root, low or high
		std::string m_bar;         // the bar a drag moves: range (Start and End) or loop
		float m_barMouse = 0.0F;   // where its drag began
		double m_barFrom = 0.0;    // and its markers' values then
		double m_barTo = 0.0;

		// The part of the sample that the display shows (p. 717), shares of it from its start, and the waveform's gain.
		double m_viewStart = 0.0;
		double m_viewWidth = 1.0;
		double m_gain = 1.0;
		float m_rulerWidth = 0.0F;             // the ruler's width in pixels that it showed the part for
		std::array<int64_t, 4> m_asked{};      // media, first and end frame, and pixels of media.peaks' last question
		double m_sampleRate = 48000.0;         // the sample's, which the ruler counts in
		TimeContext m_time;                    // the project's tempo and signatures, for bars and beats
		std::string m_rulerFormat = "seconds"; // barsBeats, seconds or samples, the user setting ui.samplerPanel's

		int m_gesture = 0;      // counts drags down, each one undo step: value fields number theirs up from 1
		bool m_showing = false; // while show sets the controls, whose changes aren't the user's
		engine::Json m_settings = engine::Json::object(); // ui.samplerPanel, with what later versions add to it
		bool m_lock = false;                              // Lock Parameter Settings, the user setting ui.samplerPanel's
		bool m_snapToZero = false;                        // Snap to Zero Crossing, likewise
		double m_level = 0.0;                             // the Normalization Level, dBFS, likewise
		bool m_settingsKnown = false;                     // a change came before settings.get answered: it is newer
		bool m_listening = false; // midi.activityListen is on for the indicator, while it shows a sampler track
		std::set<std::string> m_notInAllInputs; // "backend id" of the MIDI inputs that aren't among all MIDI inputs
		bool m_received = false;                // the event received indicator is lit
		double m_receivedAt = 0.0;              // when it lit up last, by RmlUi's clock
		int m_savesSent = 0; // settings.set sent and not answered: what settings.changed brings meanwhile is older
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
		std::vector<Subscription> m_subscriptions;
		// The file drop event's name, made once, so that removing its listener in the destructor allocates nothing.
		const Rml::String m_dropEvent{g_fileDropEvent};
	};
}
