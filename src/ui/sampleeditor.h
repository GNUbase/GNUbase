// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

// The sample editor (#140, reference manual pp. 613-635): docs/features/sample-editor.md describes it.
namespace gnubase::ui
{
	// The sample editor's tools (pp. 629-630, 634-635), in the toolbar's order.
	enum class SampleTool : uint8_t
	{
		Range,
		Zoom,
		Play,
		Scrub,
	};

	// Fills an element with the sample editor of the engine's sampleEditor view: the toolbar with the tools, Snap to
	// Zero Crossing, an element for the quantize functions (#toolbar-quantize, which app::AppUi fills with a
	// ui::QuantizePanel, reference manual p. 367) and the window layout's switches, the info line, the overview of the
	// whole clip, the inspector's Range, Process and Hitpoints sections, the waveform display with the event's edges,
	// the selection range, the snap point and the hitpoints, and the regions list. Its time axis is the clip's sample
	// frames; the display shows m_start on from the left at m_scale frames per dp, and first fits the whole clip. Every
	// change goes to the engine (sampleEditor.set, clip.*Region, clip.*Hitpoint*, event.fromHitpoints,
	// event.setSnapPoint, audition.*, transport.*). Use it on the UI thread; the binding must outlive it.
	class SampleEditor final : private Rml::EventListener
	{
	public:
		SampleEditor(EngineBinding& binding, Rml::Element& root);
		~SampleEditor() override;
		SampleEditor(const SampleEditor&) = delete;
		SampleEditor& operator=(const SampleEditor&) = delete;
		SampleEditor(SampleEditor&&) = delete;
		SampleEditor& operator=(SampleEditor&&) = delete;

		SampleTool tool() const { return m_tool; }
		// The audio event it edits, or 0 for none.
		uint64_t event() const { return hasEvent() ? m_event : 0; }
		void setTool(SampleTool tool);

		// The display's view: the clip frame at its left edge and frames per dp.
		double start() const { return m_start; }
		double scale() const { return m_scale; }

	private:
		enum class Drag : uint8_t
		{
			None,
			Range,
			SnapPoint,
			Play,
			Scrub,
			Overview,
			Hitpoint,
			WarpMarker,
		};

		void ProcessEvent(Rml::Event& event) override;
		void click(Rml::Element& target);
		void press(Rml::Event& event, Rml::Element& target);
		void move(Rml::Event& event);
		void release(Rml::Event& event);

		void requestProject();
		void requestView();
		void show();
		void render();
		void renderRegions();
		void renderHitpoints();
		bool clickHitpoints(Rml::Element& target, const std::string& id);
		void pressHitpoint(Rml::Event& event, Rml::Element& target, double frame);
		bool clickWarp(const std::string& id);
		void renderWarp();
		void pressWarp(Rml::Event& event, Rml::Element& target);
		void changed(Rml::Event& event, Rml::Element& target);
		void ask(bool overview);
		void zoom(double factor, double around);
		void fit(double from, double to);
		void reveal(double from, double to); // scrolls the display to show these frames, at the same zoom
		void editRegion(Rml::Element& cell); // a field instead of a region's name, start or end in the list
		void finishEditing(bool keep);       // Enter (and leaving the field) or Escape
		void auditionClip(int64_t from, std::optional<int64_t> to); // plays the clip, in its frames
		void scrub(double frame);

		float dpRatio() const;
		double displayWidth() const; // dp
		double frameAt(const Rml::Event& event) const;
		int64_t projectSample(double frame) const;
		bool hasEvent() const { return m_length > 0; }
		std::pair<int64_t, int64_t> rangeOrEvent() const; // the selection range, or the event's section
		const engine::Json* region(uint64_t id) const;
		const engine::Json* clip() const;
		double sampleRate() const;

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers after it went
		engine::Json m_project = engine::Json::object();
		engine::Json m_view = engine::Json::object();
		bool m_loading = false;
		bool m_stale = false;
		SampleTool m_tool = SampleTool::Range;

		// What the view shows, from m_view and the project.
		uint64_t m_event = 0;
		uint64_t m_media = 0;
		int64_t m_length = 0; // the clip's frames
		int64_t m_offset = 0;
		int64_t m_eventLength = 0;
		int64_t m_clipStart = 0; // project samples
		double m_ratio = 1.0;
		int64_t m_selectionStart = 0;
		int64_t m_selectionEnd = 0;
		double m_snapPoint = 0.0; // clip frames, where the event snaps

		double m_start = 0.0;
		double m_scale = 0.0;                          // 0 until it has fitted the clip
		uint64_t m_fitted = 0;                         // the event it fitted
		uint64_t m_region = 0;                         // the region selected in the list
		std::string m_regionRows;                      // the regions list's rows, as last built
		bool m_editHitpoints = false;                  // Edit Hitpoints: the display's gestures edit them
		std::string m_hitpointRows;                    // the hitpoints shown, as last built
		int64_t m_hitpoint = 0;                        // the hitpoint dragged, by position
		bool m_freeWarp = false;                       // Free Warp: the display's gestures edit warp markers
		std::string m_markerRows;                      // the warp markers shown, as last built
		int64_t m_marker = -1;                         // the warp marker dragged, by position; -1 for a new one
		std::array<bool, 2> m_asking = {false, false}; // media.peaks in flight: the display's, the overview's
		std::array<bool, 2> m_again = {false, false};  // asked again meanwhile

		// The field a region's name, start or end is typed in, in the regions list.
		Rml::ObserverPtr<Rml::Element> m_editing;
		uint64_t m_editedRegion = 0;
		std::string m_editedField; // name, start or end
		std::string m_edited;      // what it showed

		Drag m_drag = Drag::None;
		double m_pressed = 0.0; // clip frames
		double m_dragged = 0.0;
		std::vector<Subscription> m_subscriptions;
	};
}
