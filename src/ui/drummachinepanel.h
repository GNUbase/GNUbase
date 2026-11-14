// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/rmlui.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

namespace gnubase::ui
{
	// The drum machine panel (#182, reference manual pp. 938-977, docs/features/drum-track.md), a panel of the lower
	// zone that shows the selected drum track: its toolbar with the editing pages (Instrument, Delay and Reverb) and
	// Bypass All Effects, the pad section with the eight groups of 16 pads, and the page shown. The root gets the class
	// empty while the selected track isn't a drum track, and it then shows Add Drum Track (p. 146).
	//
	// A click on a pad selects it and plays it while the button is down (midi.inject); the Instrument page shows the
	// selected pad's name and a control per pad parameter (from processor.list), the Delay and Reverb pages the global
	// ones. The pad tools load an audio file onto the pad (loadSample asks for it), and copy, cut, paste and reset it;
	// a file dropped on a pad loads onto that pad. Controls set the drum machine's parameters (parameter.set), a
	// widget's gesture one undo step (EngineBinding::sendStep). Pads show their names, and the classes assigned (they
	// play something), selected, muted and soloed. Use it on the UI thread; the binding must outlive it.
	class DrumMachinePanel final : private Rml::EventListener
	{
	public:
		// loadSample asks for a file to load onto a pad of a drum track; addTrack adds one (Add Drum Track, the
		// button of the empty panel).
		DrumMachinePanel(EngineBinding& binding, Rml::Element& element,
			std::function<void(uint64_t track, int pad)> loadSample = {}, std::function<void()> addTrack = {});
		~DrumMachinePanel() override;
		DrumMachinePanel(const DrumMachinePanel&) = delete;
		DrumMachinePanel& operator=(const DrumMachinePanel&) = delete;
		DrumMachinePanel(DrumMachinePanel&&) = delete;
		DrumMachinePanel& operator=(DrumMachinePanel&&) = delete;

		// Shows a track (0: none) of the project as project.get returned it, the loads-th answer, which the binding
		// handed on as EngineBinding::handed() returned read, as the inspector does (ui/inspector.h).
		void update(uint64_t track, const engine::Json& project, uint64_t loads, uint64_t read);

		// The drum track it shows, 0 for none, and its selected pad.
		uint64_t track() const { return m_track; }
		int pad() const { return m_pad; }

		// Loads the audio file at path onto a pad of a drum track, and shows why it failed.
		void load(uint64_t track, int pad, const std::string& path);

	private:
		struct Parameter
		{
			uint32_t id = 0; // pad parameters: of pad 0
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
		void buildPage();
		void show();
		void set(uint32_t id, double value, int gesture);
		void showMessage(const std::string& text);
		double value(uint32_t id) const;
		std::string padName(int pad) const;
		void sound(int pad);
		uint32_t idOf(const Parameter& parameter) const;
		std::string stepKey(uint32_t id) const;
		Rml::Element* find(const std::string& id) const;

		EngineBinding& m_binding;
		std::function<void(uint64_t track, int pad)> m_loadSample;
		std::function<void()> m_addTrack;
		const std::string m_stepOwner;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::vector<Parameter> m_padParameters;    // pad 0's, from processor.list
		std::vector<Parameter> m_globalParameters; // the drum machine's own
		std::map<uint32_t, double> m_values;       // the channel's
		std::map<int, std::string> m_names;        // the pads' names (drum.setPad), else their clips'
		std::map<int, int> m_sounds;               // the notes whose General MIDI sounds copies go by (DrumPad::sound)
		std::map<int, std::string> m_colors;       // the pads' colors, "#rrggbb"
		std::vector<std::pair<std::string, std::string>> m_colorSet; // No Color and the project's color set (Set Color)
		std::vector<std::string> m_padNames; // without these: the General MIDI sounds', from processor.list
		uint64_t m_track = 0;
		std::optional<std::pair<uint64_t, int>> m_sounding; // the channel and pad that a click plays
		uint64_t m_selected = 0;                            // the track update showed last, a drum track or not
		uint64_t m_loads = 0;
		uint64_t m_read = 0;
		engine::Json m_channel; // the drum track's, null without one
		int m_pad = 36;         // the selected pad
		int m_group = 2;        // the group of pads shown, 0-7
		std::string m_page = "instrument";
		int m_copied = -1;      // the pad copied or cut, -1 for none
		bool m_cut = false;     // and whether it was cut
		bool m_showing = false; // while show sets the controls, whose changes aren't the user's
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
		// The file drop event's name, made once, so that removing its listener in the destructor allocates nothing.
		const Rml::String m_dropEvent{g_fileDropEvent};
	};
}
