// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

namespace gnubase::ui
{
	// The quantize panel (#132, reference manual pp. 372-378, docs/features/quantize.md), or the project window
	// toolbar's quantize section (p. 368), in an element. Both show the project's quantize settings (quantize.get,
	// quantize.changed) and the quantize presets (quantize.presets, quantize.presetsChanged), whichever client changes
	// them, and change them with quantize.set.
	//
	// The panel: the Quantize Presets menu (#quantize-presets), which shows the preset the settings match, or No
	// Preset, with a name field to save the settings as a preset and to rename or remove the user preset shown; Soft
	// Quantize and the strength (Position for a groove); for a grid its type, Quantize Tuplet, Quantize Swing (straight
	// grids without tuplets only) and Catch Range, for a groove Velocity, Length, Pre-Q, Max. Move and Orig.
	// Position; Safe Range and Rough Quantize; the grid display, a bar of the grid or groove's positions with the
	// catch range around them; Auto Apply Quantize, Reset Quantize and Apply Quantize. The toolbar's section: Apply
	// Quantize, the Quantize Presets menu, Soft Quantize and Open Quantize Panel.
	//
	// Apply and Reset run the UI commands edit.quantize and edit.resetQuantize through run, as the Edit menu does, so
	// they quantize what it quantizes; with Auto Apply on, every change of the settings runs edit.quantize too. Open
	// Quantize Panel runs window.quantizePanel. Use it on the UI thread; the binding must outlive it.
	class QuantizePanel final : private Rml::EventListener
	{
	public:
		enum class Layout : uint8_t
		{
			Panel,
			Toolbar,
		};

		QuantizePanel(
			EngineBinding& binding, Rml::Element& element, Layout layout, std::function<void(const std::string&)> run);
		~QuantizePanel() override;
		QuantizePanel(const QuantizePanel&) = delete;
		QuantizePanel& operator=(const QuantizePanel&) = delete;
		QuantizePanel(QuantizePanel&&) = delete;
		QuantizePanel& operator=(QuantizePanel&&) = delete;

	private:
		void ProcessEvent(Rml::Event& event) override;
		void request();
		void show();
		void showGrid();
		std::string presetsMenu() const;
		// The preset whose settings the project's are, or "": with several, m_chosen, else a user preset.
		std::string currentPreset() const;
		bool isUserPreset(const std::string& name) const;
		void set(engine::Json parameters);
		void savePreset(bool rename);
		std::string presetName() const;
		Rml::Element* find(const std::string& id) const;

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus; // in the document's body
		Layout m_layout;
		std::string m_prefix;          // of the ids: quantize or toolbar-quantize
		std::string m_presetAttribute; // of the presets menu's items: preset or toolbar-preset
		std::function<void(const std::string&)> m_run;
		engine::Json m_settings = engine::Json::object();
		engine::Json m_presets = engine::Json::array();
		std::string m_chosen; // the preset last chosen, saved or renamed in this panel
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
		std::vector<Subscription> m_subscriptions;
	};
}
