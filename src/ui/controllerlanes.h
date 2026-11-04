// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

// The controller lanes (#128, reference manual pp. 1157-1188): docs/features/controller-lanes.md describes them.
namespace gnubase::ui
{
	// Fills the key editor's controller display: a lane per entry of the editor view's lanes, each showing the notes'
	// velocities, off velocities, play probabilities or velocity variances as bars, or controller events of a type
	// as steps, under the note display and on its time axis; a menu per lane to change its type, select its events,
	// remove it and apply or reset play probability and velocity variance; and buttons for the Create Controller Lane
	// and Controller Lane Setup menus, with the lane presets of the user setting ui.controllerLanePresets. The key
	// editor's tool edits the lanes: Object Selection selects controller events and drags values, Draw paints values
	// and events, Erase removes events, Line draws in its mode with controller.line. Every edit goes to the engine.
	// Use it on the UI thread; the binding must outlive it.
	class ControllerLanes final : private Rml::EventListener
	{
	public:
		ControllerLanes(EngineBinding& binding, Rml::Element& root, std::string view);
		~ControllerLanes() override;
		ControllerLanes(const ControllerLanes&) = delete;
		ControllerLanes& operator=(const ControllerLanes&) = delete;
		ControllerLanes(ControllerLanes&&) = delete;
		ControllerLanes& operator=(ControllerLanes&&) = delete;

		// The project, the editor view and the time axis of the note display, project ticks.
		void show(const engine::Json& project, const engine::Json& view, int64_t from, int64_t to);
		// The key editor's tool by name (keyToolName) and the line tool's mode: line, parabola, sine, triangle, square
		// or paint.
		void setTool(std::string tool, std::string lineMode);

	private:
		struct Part
		{
			uint64_t id = 0;
			int64_t start = 0; // project ticks
			int64_t end = 0;
			bool editable = false;
		};

		// A bar or step of a lane: a note's value or a controller event's, 0 to 1 from the lane's bottom to its top.
		struct Item
		{
			uint64_t part = 0;
			uint64_t id = 0;
			int64_t start = 0; // project ticks
			int64_t end = 0;   // controller events: the next event of the lane, or the part's end
			double value = 0.0;
			bool editable = false;
			bool selected = false;
		};

		struct Lane
		{
			std::string type;
			int number = 0;
		};

		enum class Gesture : uint8_t
		{
			None,
			Value, // drags the values of the pressed item, and of the selected ones with it
			Paint, // paints values or events
			Line,  // draws a line in the line tool's mode
		};

		void ProcessEvent(Rml::Event& event) override;
		void press(Rml::Event& event);
		void drag(Rml::Event& event);
		void release(Rml::Event& event);
		void menuItem(Rml::Element& menu, Rml::Element& item);
		void openPresetDialog();
		void dialogEvent(Rml::Event& event);

		void render();
		std::vector<Item> items(const Lane& lane) const;
		// The lane under the event's target and the point in its area: project ticks and a value from 0 to 1.
		std::optional<size_t> laneAt(Rml::Element* target) const;
		std::pair<double, double> at(const Rml::Event& event, size_t lane) const;
		int64_t snap(double ticks, bool off) const;
		static int64_t cell(double ticks, int64_t step); // the start of the step the ticks are in
		int64_t grid() const;
		int64_t lengthQuantize() const;
		const Part* partAt(int64_t ticks) const;
		const engine::Json* partJson(uint64_t id) const;
		bool noteSelected(uint64_t part, uint64_t note) const;

		// Edits: the field a note lane changes from a display value, and the change of a controller event to one.
		static engine::Json noteChange(const Lane& lane, uint64_t note, double value);
		static engine::Json controllerChange(const Lane& lane, uint64_t controller, double value);
		static engine::Json laneSpec(const Lane& lane);
		void edit(const std::string& description, engine::Json commands);
		void setLanes(const std::vector<Lane>& lanes);
		void setView(engine::Json parameters);
		void finishValue(size_t lane, double value);
		void finishPaint(size_t lane);
		void finishLine(size_t lane, const std::pair<double, double>& end, bool snapOff);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::string m_name;
		std::string m_tool = "objectSelection";
		std::string m_lineMode = "line";
		engine::Json m_project = engine::Json::object();
		engine::Json m_view = engine::Json::object();
		std::vector<Part> m_parts;
		std::vector<Lane> m_lanes;
		int64_t m_from = 0;
		int64_t m_to = 1;
		std::string m_rml; // the lanes' elements as last set
		engine::Json m_presets = engine::Json::array();
		bool m_presetsKnown = false;
		std::vector<Subscription> m_subscriptions;
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus; // lane, create and setup menus
		std::optional<size_t> m_menuLane;                    // the lane whose menu opened
		Rml::ObserverPtr<Rml::Element> m_dialog;

		Gesture m_gesture = Gesture::None;
		size_t m_lane = 0;
		std::pair<double, double> m_pressedAt;
		std::optional<Item> m_pressed;
		bool m_snapOff = false;
		std::vector<std::pair<int64_t, double>> m_painted; // positions and values, as painted
	};
}
