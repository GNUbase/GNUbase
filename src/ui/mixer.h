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

	// Where a mixer shows: in the project window's lower zone, in a mixer window, or in the channel settings window
	// (#112, docs/features/channel-settings.md), which shows one channel with all its sections.
	enum class MixerKind : uint8_t
	{
		LowerZone,
		Window,
		ChannelSettings,
	};

	// What a mixer asks the app to do; each may be empty.
	struct MixerActions
	{
		// A double click on a channel's picture section: choose its track's picture.
		std::function<void(uint64_t track)> choosePicture;
		// Edit Channel Settings (reference manual p. 500): open the channel settings window for the track's channel.
		std::function<void(uint64_t track)> editChannel;
		// Add Effect Track to Send (p. 554): the add track dialog for an effect track that the send goes to, in the
		// window of the element.
		std::function<void(Rml::Element& from, uint64_t track, int send)> addEffectTrack;
		// The channel settings window went to another track's channel by itself (previous, next, Go to Output).
		std::function<void(uint64_t track)> wentTo;
	};

	// Fills an element with a mixer: a toolbar and the fader section, a strip per channel that the mixer shows, which
	// follows the project whichever client changes it. view is the mixer's name in mixer.get and mixer.set (lower,
	// window1 to window4), which keep what it hides: the toolbar's channel type filter (#mixer-filter-menu) and
	// visibility agents (#mixer-agents-menu) change it, and in a window the Visibility tab of the left zone, which
	// showVisibility fills. Each strip (.mixer-channel, with the channel's id in channel and its type in type) is a
	// container of the docking system's set of sections "channel": the picture, the notepad and the fader, which the
	// Set up Window Layout pane (#mixer-layout-pane) shows, hides and reorders. A channel name dragged onto another
	// channel moves its track there with track.move; busses keep their places. Each strip of a track's channel has
	// Edit Channel Settings (button.mixer-edit), and its sends' destination lists Add Effect Track to Send.
	//
	// As the channel settings window's (MixerKind::ChannelSettings, view unused) it shows one track's channel, with the
	// pre section, the equalizer, the strip, the sends and the fader shown by default, and a toolbar of its own: Go to
	// Previous and Next Mixer Channel (#settings-previous, #settings-next), the channel's name (.settings-name), Go to
	// Output (#settings-output) and the Functions menu (#settings-functions-menu) with Copy and Paste Channel Settings
	// and Follow Edit Buttons or Selection Changes. Use it on the UI thread; the binding and the docking system must
	// outlive it.
	class Mixer final : private Rml::EventListener
	{
	public:
		Mixer(EngineBinding& binding, Docking& docking, Rml::Element& root, std::string view, MixerKind kind,
			MixerActions actions);
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

		// The channel settings window's: shows the track's channel, or nothing for a track without one.
		void showTrack(uint64_t track);
		uint64_t track() const { return m_track; }
		// Whether it follows the Edit Channel Settings buttons and the track selection (on by default).
		bool follows() const { return m_follows; }

	private:
		void ProcessEvent(Rml::Event& event) override;
		void requestProject();
		// For the steps of a gesture: a refused one reads the project, so the control shows the engine's value again.
		std::function<void(const engine::Result&)> whenRefused();
		void requestView();
		void show();
		void buildStrips(const std::vector<const MixerChannel*>& strips);
		void updateStrip(Rml::Element& strip, const MixerChannel& channel);
		void showVisibilityList();
		void setHidden(uint64_t id, bool hidden);
		void setTypeHidden(const std::string& type, bool hidden);
		void sendView(engine::Json parameters);
		void drop(uint64_t onto);
		void showSettingsToolbar();
		void goTo(uint64_t track);
		const MixerChannel* channelOf(Rml::Element* element) const;

		EngineBinding& m_binding;
		Docking& m_docking;
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::ObserverPtr<Rml::Element> m_strips;             // the fader section
		Rml::ObserverPtr<Rml::Element> m_filter;             // the channel type filter's button
		Rml::ObserverPtr<Rml::Element> m_visibility;         // the Visibility tab's element
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus; // in the document's body
		std::string m_view;
		MixerKind m_kind = MixerKind::LowerZone;
		MixerActions m_actions;
		uint64_t m_track = 0;  // the channel settings window's
		bool m_follows = true; // the channel settings window's Follow

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
