// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/meters.h"
#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <string>
#include <vector>

namespace gnubase::ui
{
	// The right zone's meter display (#590, docs/features/metering.md, reference manual pp. 533-538): the master meter,
	// a level meter per channel of the main mix with RMS Max., Peak Max. and the true peak, and the loudness meter,
	// with the momentary and short-term loudness and their values as the engine measures them; the Master and
	// Loudness buttons at its bottom switch between them, and Reset resets them (meter.reset). It reads the meters
	// once a frame (Engine::readMeters), the UI's one reader of them. Use it on the UI thread; the engine and the
	// binding must outlive it.
	class MeterPanel final : private Rml::EventListener
	{
	public:
		MeterPanel(engine::Engine& engine, EngineBinding& binding, Rml::Element& element);
		~MeterPanel() override;
		MeterPanel(const MeterPanel&) = delete;
		MeterPanel& operator=(const MeterPanel&) = delete;
		MeterPanel(MeterPanel&&) = delete;
		MeterPanel& operator=(MeterPanel&&) = delete;

	private:
		void ProcessEvent(Rml::Event& event) override;
		// What the display shows of the meters: each main channel's level and peak maximum, then the texts.
		std::vector<std::string> read();
		void show(const std::vector<std::string>& shown);

		engine::Engine& m_engine;
		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		engine::MeterFrame m_frame;
		Subscription m_meters;
	};
}
