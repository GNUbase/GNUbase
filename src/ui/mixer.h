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
#include <set>
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

// The mixer (#106): the lower zone's mixer and the mixer windows. docs/features/mixer-window.md describes it.
namespace gnubase::ui
{
	class Docking;

	// A channel as the mixer shows it, from project.get: an input bus, a track's channel or an output bus.
	struct MixerChannel
	{
		uint64_t id = 0;    // the channel's or the bus's
		uint64_t track = 0; // 0 for busses
		std::string type;   // as mixer.set names channel types: input, audio, instrument, midi, group, effect, output
		std::string name;
		const engine::Json* channel = nullptr; // the channel's JSON in the project, null for busses
		std::string picture;                   // the track's
	};

	// The channel.set parameters of a click on a channel's solo button (reference manual p. 466), from the channel's
	// JSON: solo on or off, exclusive turns the other channels' solo off as it goes on, defeat switches solo defeat
	// instead. The mixer and the track list use it.
	engine::Json soloClick(const engine::Json& channel, bool exclusive, bool defeat);

	// Every channel of a project in the mixer's order: the input busses, the tracks' channels from top to bottom, then
	// the output busses.
	std::vector<MixerChannel> mixerChannels(const engine::Json& project);

	// Fills an element with a mixer: a toolbar and the fader section, a strip per channel that the mixer shows, which
	// follows the project whichever client changes it. view is the mixer's name in mixer.get and mixer.set (lower,
	// window1 to window4), which keep what it hides: the toolbar's channel type filter (#mixer-filter-menu) and
	// visibility agents (#mixer-agents-menu) change it, and in a window the Visibility tab of the left zone, which
	// showVisibility fills. Each strip (.mixer-channel, with the channel's id in channel and its type in type) is a
	// container of the docking system's set of sections "channel": the picture, the notepad and the fader, which the
	// Set up Window Layout pane (#mixer-layout-pane) shows, hides and reorders. A channel name dragged onto another
	// channel moves its track there with track.move; busses keep their places. choosePicture runs on a double click on
	// a channel's picture section, to choose its track's picture. Use it on the UI thread; the binding and the docking
	// system must outlive it.
	class Mixer final : private Rml::EventListener
	{
	public:
		Mixer(EngineBinding& binding, Docking& docking, Rml::Element& root, std::string view, bool window,
			std::function<void(uint64_t track)> choosePicture);
		~Mixer() override;
		Mixer(const Mixer&) = delete;
		Mixer& operator=(const Mixer&) = delete;
		Mixer(Mixer&&) = delete;
		Mixer& operator=(Mixer&&) = delete;

		// Fills an element with the Visibility tab (reference manual p. 442): a check box per channel of the project,
		// checked while the mixer shows it. It shows nothing more once the element goes.
		void showVisibility(Rml::Element& element);

		// The ids of the channels the strips show, in order.
		std::vector<uint64_t> shown() const { return m_shown; }

	private:
		void ProcessEvent(Rml::Event& event) override;
		void requestProject();
		void requestView();
		void show();
		void buildStrips(const std::vector<const MixerChannel*>& strips);
		void updateStrip(Rml::Element& strip, const MixerChannel& channel);
		void showVisibilityList();
		void setHidden(uint64_t id, bool hidden);
		void setTypeHidden(const std::string& type, bool hidden);
		void sendView(engine::Json parameters);
		void drop(uint64_t onto);
		const MixerChannel* channelOf(Rml::Element* element) const;

		EngineBinding& m_binding;
		Docking& m_docking;
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::ObserverPtr<Rml::Element> m_strips;             // the fader section
		Rml::ObserverPtr<Rml::Element> m_filter;             // the channel type filter's button
		Rml::ObserverPtr<Rml::Element> m_visibility;         // the Visibility tab's element
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus; // in the document's body
		std::string m_view;
		bool m_window = false;
		std::function<void(uint64_t track)> m_choosePicture;

		engine::Json m_project = engine::Json::object();
		std::vector<MixerChannel> m_channels;
		std::set<uint64_t> m_hidden;
		std::set<std::string> m_hiddenTypes;
		std::vector<uint64_t> m_shown;
		std::map<uint64_t, Rml::Element*> m_stripOf; // by channel id, while m_strips lives
		bool m_loading = false;
		bool m_stale = false;
		uint64_t m_dragged = 0;          // the channel whose name a mouse button went down on
		bool m_updating = false;         // while the strips show the project's values
		double m_defaultSendLevel = 0.0; // the user setting audio.defaultSendLevel

		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
	};
}
