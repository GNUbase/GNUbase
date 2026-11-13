// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

// The pattern editor (#181, reference manual pp. 895-937): docs/features/pattern-editor.md describes it.
namespace gnubase::ui
{
	// Fills an element of a MidiEditor with the pattern editor of the view's active part: its toolbar (mode, tools,
	// pattern management, Number of Steps, Step Resolution, Pattern Play Direction and the edit functions), the step
	// display with a step lane per drum sound or pitch, and the parameter lane of the selected lane. show() gives it
	// the project and the engine's editor view whenever they change. A gesture edits a copy of the pattern, which goes
	// to the engine as one pattern.set when it ends, with the lanes it didn't edit as the project has them then; the
	// engine's pattern shows again if that is refused, or if the project changed while the gesture went on and it
	// changed nothing. Use it on the UI thread; the binding must
	// outlive it.
	class PatternEditor final : private Rml::EventListener
	{
	public:
		PatternEditor(EngineBinding& binding, Rml::Element& root, std::string view);
		~PatternEditor() override;
		PatternEditor(const PatternEditor&) = delete;
		PatternEditor& operator=(const PatternEditor&) = delete;
		PatternEditor(PatternEditor&&) = delete;
		PatternEditor& operator=(PatternEditor&&) = delete;

		void show(const engine::Json& project, const engine::Json& view);
		// The project as show() gave it last.
		const engine::Json& project() const { return m_project; }

	private:
		enum class Gesture : uint8_t
		{
			None,
			Add,    // the draw tool's drag adds steps
			Remove, // or removes them
			Values, // a drag in the parameter lane
		};

		void ProcessEvent(Rml::Event& event) override;
		void press(Rml::Event& event);
		void drag(Rml::Event& event);
		void release();
		void click(Rml::Element& target);
		void choose(Rml::Element& item);
		void listen(bool document);
		// Takes the active part's pattern from the project shown and renders it.
		void load();
		void render();
		// Shows the steps' state, the selected lane and the parameter lane without making the step display's elements
		// anew, as a gesture changes them.
		void refresh();
		std::string barsRml() const;

		// The pitches of the step display's rows, from the top.
		std::vector<int> rows() const;
		int laneLength(int pitch) const;
		engine::Json* lane(int pitch);
		const engine::Json* lane(int pitch) const;
		// The step starting at the index, or with covering the step whose ties cover it.
		const engine::Json* step(int pitch, int index, bool covering = false) const;
		void addStep(int pitch, int index);
		void removeStep(int pitch, int index);
		void setValue(int index, double value);
		// The lanes a gesture didn't edit as the project has them now: a script, an MCP client or an undo may have
		// changed them while it went on.
		void takeOtherLanes();
		// Sends the lanes as they are now in one pattern.set, an undo step named description; if it is refused, the
		// engine's pattern shows again.
		void sendLanes(const std::string& description);
		void set(engine::Json changes);
		// Tie Adjacent Steps and Split Tied Steps (p. 910), of the selected steps or else of all.
		void tie(bool split);
		void doubleLength(bool resolution);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::string m_name;
		engine::Json m_project = engine::Json::object();
		engine::Json m_pattern; // the active part's, as last shown or as a gesture changed it; null for none
		uint64_t m_part = 0;
		std::string m_tool = "draw";
		std::string m_parameter = "velocity";
		int m_lane = -1;                           // the selected lane's pitch
		std::set<std::pair<int, int>> m_selection; // the selected steps: pitch and index
		Gesture m_gesture = Gesture::None;
		bool m_changed = false; // by the gesture going on
		bool m_stale = false;   // the project changed while it went on
		std::set<int> m_edited; // the pitches of the lanes it changed
		Rml::ObserverPtr<Rml::Element> m_document;
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it's gone
	};
}
