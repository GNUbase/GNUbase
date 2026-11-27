// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/meterpanel.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <chrono>
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
		std::string color;                     // the track's, "#rrggbb", or empty for the theme's
	};

	// The channel.set parameters of a click on a channel's solo button (reference manual p. 466), from the channel's
	// JSON: solo on or off, exclusive turns the other channels' solo off as it goes on, defeat switches solo defeat
	// instead. The mixer, the track list and the inspector use it.
	engine::Json soloClick(const engine::Json& channel, bool exclusive, bool defeat);

	// Whether any of the project's channels has the state, mute, solo (solo defeat too) or listen: what the Deactivate
	// All buttons show (reference manual pp. 58, 447).
	bool anyChannelHas(const engine::Json& project, const std::string& state);

	// Whether the audio of channel from reaches channel to through outputs and sends, as the engine checks for loops
	// (engine::routingLoops): a channel can't play into one whose audio comes back to it. The mixer's send destination
	// lists and the inspector's output list leave such channels out.
	bool routesTo(const engine::Json& project, uint64_t from, uint64_t to);

	// Where the channel of a track of the type can play: output busses, group and effect channels whose audio doesn't
	// come back to it (routesTo), or for MIDI tracks the instrument channels, as in the add track dialog; and where an
	// audio track records from. The inspector's and the mixer's routing lists (reference manual pp. 471-473).
	widgets::Options outputChoices(const engine::Json& project, const std::string& type, uint64_t channel);
	widgets::Options inputChoices(const engine::Json& project);

	// Click and hold on a solo button switches solo defeat, as Alt-click does (p. 466): a view tells it where the mouse
	// went down, and asks at the click on a solo button (control="solo") whether the press there was that long.
	class SoloHold
	{
	public:
		static constexpr double g_seconds = 0.5;

		void pressed(Rml::Element* target);
		bool held(const Rml::Element& button) const;

	private:
		Rml::ObserverPtr<Rml::Element> m_button; // the solo button pressed
		double m_at = 0.0;                       // RmlUi's clock at the press
	};

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
		// A double click on a channel's picture section: choose its track's picture, in the window of the element.
		std::function<void(const Rml::Element& from, uint64_t track)> choosePicture;
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
	// showVisibility fills. Find Track/Channel (#mixer-find, its selector #mixer-find-menu) shows a channel. Each strip
	// (.mixer-channel, with the channel's id in channel and its type in type) is a container of the docking system's
	// set of sections "channel": the picture, the notepad and the fader, which the Set up Window Layout pane
	// (#mixer-layout-pane) shows, hides and reorders. A channel name dragged onto another channel moves its track there
	// with track.move; busses keep their places. Each strip of a track's channel has Edit Channel Settings
	// (button.mixer-edit), and its sends' destination lists Add Effect Track to Send. An audio channel's strip has its
	// meter next to the fader (.mixer-meter, a <meter> per audio channel with the peak since the last frame and its
	// peak mark, read once a frame through the meter reader) and its peak level under it
	// (.mixer-peak, reference manual p. 468), which a click resets, every channel's (meter.reset channels).
	//
	// As the channel settings window's (MixerKind::ChannelSettings, view unused) it shows one track's channel, with the
	// pre section, the equalizer, the strip, the sends and the fader shown by default, and a toolbar of its own: Go to
	// Previous and Next Mixer Channel (#settings-previous, #settings-next), the channel's name (.settings-name), Go to
	// Output (#settings-output) and the Functions menu (#settings-functions-menu) with Copy and Paste Channel Settings
	// and Follow Edit Buttons or Selection Changes. Use it on the UI thread; the binding, the meter reader and the
	// docking system must outlive it.
	class Mixer final : private Rml::EventListener
	{
	public:
		Mixer(EngineBinding& binding, MeterReader& meters, Docking& docking, Rml::Element& root, std::string view,
			MixerKind kind, MixerActions actions);
		~Mixer() override;
		Mixer(const Mixer&) = delete;
		Mixer& operator=(const Mixer&) = delete;
		Mixer(Mixer&&) = delete;
		Mixer& operator=(Mixer&&) = delete;

		// Fills an element with the Visibility tab (reference manual p. 442): a check box per channel of the project,
		// checked while the mixer shows it, and Sync Visibility of Project and Mixer (#mixer-visibility-sync, .checked
		// while the track visibility's sync is this mixer, docs/features/track-visibility.md). It shows nothing more
		// once the element goes.
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
		// For the steps of a gesture of what on a channel: once the last one is answered, the strip shows the engine's
		// value, which the step's channel.changed brought; a refused one reads the project, so the control shows the
		// engine's value again.
		std::function<void(const engine::Result&)> whenAnswered(std::string what, uint64_t channel);
		void requestView();
		void show();
		// One channel's JSON changed in m_project: its strip and what else shows it, not every strip (#1881).
		void showChannel(uint64_t id);
		void showStates();
		void buildStrips(const std::vector<const MixerChannel*>& strips);
		void updateStrip(Rml::Element& strip, const MixerChannel& channel);
		void showVisibilityList();
		void setHidden(uint64_t id, bool hidden);
		void setTypeHidden(const std::string& type, bool hidden);
		void sendView(engine::Json parameters);
		// Leaves the notepad being typed in, whose blur sends its text: before its strip or the mixer goes, and on
		// project.reset, which drops it.
		void leaveNotes();
		void drop(uint64_t onto);
		void showSettingsToolbar();
		void goTo(uint64_t track);
		const MixerChannel* channelOf(Rml::Element* element) const;
		// The meters' values, per channel in m_metered: each audio channel's peak and its peak mark, held as the
		// Global Meter Settings say (empty without Hold Peaks), then the peak level.
		std::vector<std::vector<std::string>> readMeters();
		// A user setting the mixer shows: audio.defaultSendLevel, or a Global Meter Setting.
		void setting(const std::string& key, const engine::Json& value);
		void showMeters(const std::vector<std::vector<std::string>>& shown);
		// The display line's pop-up menu (reference manual p. 459), filled for the channel as it opens; link does what
		// its item says.
		void showLinkMenu(uint64_t channel);
		// Find Track/Channel (reference manual pp. 449-450): fills its selector with a search field and the channels
		// that the type filter doesn't hide; find narrows it to the names with the text, reveal shows the channel
		// picked, again if it was hidden, and scrolls its strip into view once it is there.
		void showFindMenu();
		void find(const std::string& text);
		void reveal(uint64_t channel);
		void scrollToRevealed();
		void link(const std::string& item);
		// The Link Group Settings dialog (link-group-settings.rml, reference manual pp. 457-459) for a link group,
		// filled from it; OK sends what it shows with linkGroup.set. showLinkSettings enables its fields for Use VCA
		// Fader, which turns the volume off and, when vcaChanged, on again without it (p. 517).
		void openLinkSettings(uint64_t group);
		void showLinkSettings(bool vcaChanged);
		void linkSettingsEvent(Rml::Event& event);

		EngineBinding& m_binding;
		const std::string m_stepOwner; // begins the whats of its steps (EngineBinding::stepOwner)
		Docking& m_docking;
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::ObserverPtr<Rml::Element> m_strips;             // the fader section
		Rml::ObserverPtr<Rml::Element> m_filter;             // the channel type filter's button
		Rml::ObserverPtr<Rml::Element> m_visibility;         // the Visibility tab's element
		Rml::ObserverPtr<Rml::Element> m_window;             // its context's root, which every mouseup reaches
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus; // in the document's body
		std::string m_view;
		MixerKind m_kind = MixerKind::LowerZone;
		MixerActions m_actions;
		uint64_t m_track = 0;  // the channel settings window's
		bool m_follows = true; // the channel settings window's Follow
		bool m_syncs = false;  // its channel visibility follows the track visibility (#166)

		engine::Json m_project = engine::Json::object();
		ChannelSteps m_channelSteps; // channel.changed until their step's history.changed (#1881)
		std::vector<MixerChannel> m_channels;
		std::set<uint64_t> m_hidden;
		std::set<std::string> m_hiddenTypes;
		std::vector<uint64_t> m_shown;
		std::map<uint64_t, Rml::Element*> m_stripOf; // by channel id, while m_strips lives
		MeterReader& m_meterReader;
		std::shared_ptr<std::vector<uint64_t>> m_metered; // the channels whose strips have meters
		bool m_loading = false;
		bool m_stale = false;
		uint64_t m_dragged = 0;     // the channel whose name a mouse button went down on
		int m_viewSends = 0;        // its mixer.set commands not answered yet
		uint64_t m_linkChannel = 0; // the channel whose display line's menu opened last
		uint64_t m_revealing = 0;   // the channel Find Track/Channel picked, until its strip is in view
		Rml::ObserverPtr<Rml::Element> m_linkDialog; // link-group-settings.rml while open
		uint64_t m_linkDialogGroup = 0;              // the group it is for
		bool m_updating = false;                     // while the strips show the project's values
		bool m_dropping = false;                     // while another project drops the notes being typed
		SoloHold m_soloHold;
		double m_defaultSendLevel = 0.0; // the user setting audio.defaultSendLevel
		// The Global Meter Settings (reference manual p. 467): meter.holdPeaks, meter.holdForever, meter.peakHoldTime;
		// and meter.fallback (p. 1614).
		bool m_holdPeaks = true;
		bool m_holdForever = false;
		std::chrono::milliseconds m_peakHoldTime{2000};
		double m_fallback = 20.0; // dB per second
		struct HeldPeak
		{
			float level = 0.0f;
			std::chrono::steady_clock::time_point at;
			float bar = 0.0f; // the level the bar shows, which falls back from the peaks
		};
		std::map<uint64_t, std::vector<HeldPeak>> m_held;   // each metered channel's peak marks and bars, per side
		std::chrono::steady_clock::time_point m_metersRead; // when readMeters read them last
		Rml::ObserverPtr<Rml::Element> m_meterMenu;

		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
	};
}
