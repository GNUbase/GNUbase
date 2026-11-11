// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

namespace gnubase::ui
{
	// The sampler panel (#1029, reference manual pp. 710-717, 730, docs/features/sampler-track.md), a panel of the
	// lower zone that shows the selected sampler track: its toolbar (Import Audio File, the sample's file name,
	// Normalize Sample and Lock Parameter Settings), the waveform display with the sample's start and end and the
	// loop's markers, the Playback, Pitch, Filter and Amp sections with a control per sampler parameter (from
	// processor.list) and the keyboard section with the pitchbend range, the root key and the key range above a
	// keyboard that shows them. The root gets the class empty while the selected track isn't a sampler track.
	//
	// Controls and markers set the sampler's parameters (parameter.set), a drag or a widget's gesture one undo step
	// (EngineBinding::sendStep); a click on a key sets the root key. A file dropped on the panel, or chosen with
	// Import Audio File, loads into the sampler (sampler.load path), keeping the settings with Lock Parameter
	// Settings; Normalize Sample runs sampler.normalize. Use it on the UI thread; the binding must outlive it.
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
		void ask();
		void set(uint32_t id, double value, int gesture);
		void drag(const Rml::Event& event);
		double value(const std::string& name) const;
		const Parameter* parameter(const std::string& name) const;
		std::string stepKey(uint32_t id) const;
		Rml::Element* find(const std::string& id) const;

		EngineBinding& m_binding;
		std::function<void(uint64_t track, bool keepSettings)> m_importSample;
		const std::string m_stepOwner;
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::ObserverPtr<Rml::Element> m_document; // for drags that leave the panel
		std::vector<Parameter> m_parameters;       // the sampler's, from processor.list
		std::map<uint32_t, double> m_values;       // the channel's, the defaults filled in
		uint64_t m_track = 0;
		uint64_t m_selected = 0; // the track update showed last, a sampler track or not
		uint64_t m_loads = 0;
		uint64_t m_read = 0;
		engine::Json m_channel; // the sampler track's, null without one
		uint64_t m_media = 0;   // the media file its sample plays
		int64_t m_length = 0;   // in its frames
		int m_mediaChannels = 1;
		bool m_asking = false;     // media.peaks is in flight
		bool m_again = false;      // and asks again when it answers
		float m_askedWidth = 0.0F; // the display's width in pixels that it asked for
		std::string m_dragged;     // the marker a drag moves: the parameter's name
		int m_gesture = 0;         // counts drags down, each one undo step: value fields number theirs up from 1
		bool m_showing = false;    // while show sets the controls, whose changes aren't the user's
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
		std::vector<Subscription> m_subscriptions;
	};
}
