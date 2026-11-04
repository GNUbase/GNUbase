// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

// The transport controls (#90): the transport bar, the transport panel and the time display window.
// docs/features/transport-bar.md describes them.
namespace gnubase::ui
{
	// Runs a command, the engine's or the UI's, with its parameters as a key would (KeyCommands::run).
	using RunCommand = std::function<void(const std::string& command, const engine::Json& parameters)>;
	// Ends a command that runs while its button is held, as its key going up would (KeyCommands::release).
	using ReleaseCommand = std::function<void(const std::string& command)>;
	// Checks that a control can run the command with its parameters: throws std::invalid_argument if it can't
	// (KeyCommands::check).
	using CheckCommand = std::function<void(const std::string& command, const engine::Json& parameters)>;

	// Drives the transport controls in an element and follows the transport, the project's tempo and signature and the
	// time formats through the engine binding. The element's markup says what each control is:
	//
	//   command="transport.toggle" parameters='{"setting": "cycle"}'
	//                                a button that runs the command with the parameters (default none)
	//   hold                         with command: runs it when the mouse button goes down on it and ends it when the
	//                                button goes up, as a key held would, e.g. Forward, which winds while held
	//   state="cycle"                checked while the transport's value is true: cycle, punchIn, punchOut,
	//                                metronome, playing or recording
	//   time="position|left|right|length" format="primary|secondary|timeDisplay"
	//                                the project cursor, a locator or the locator range in the time format of that slot
	//                                (default primary); a <textdisplay> takes a position or length typed into it,
	//                                and the mouse wheel moves it by a beat (bars and beats), a frame (timecode,
	//                                user frames) or a second. Those in the slot timeDisplay follow ui.timeDisplay:
	//                                only the beat in its bar with beatCountOnly, and the state class size-0 to
	//                                size-4 of its size
	//   field="tempo"                a <valuefield> with Current Tempo in BPM (ui::currentTempo): the fixed tempo,
	//                                or in tempo track mode the tempo at the project cursor, also while playing,
	//                                which it changes on the tempo event at or before the cursor (reference manual p.
	//                                1446); a drag or a run of wheel turns on it sets the tempo once, at its end, one
	//                                undo step
	//   field="signature"            a <textdisplay> with the first time signature, "4/4"
	//   field="tempoTrack"           a button that switches between fixed tempo and tempo track mode (Activate Tempo
	//                                Track), checked in tempo track mode
	//   field="tap"                  Tap Tempo: clicks on it in time set the tempo (tempo.tap, by the user setting
	//                                transport.tapTempoMode) once they stop for two seconds; meanwhile the tempo
	//                                fields show the tapped tempo, it has the state class .tapping, and each tap shows
	//                                where the tempo goes (tapTempoMessage) in the document's
	//                                notification.tap-tempo-message elements (p. 1455)
	//   menuitem slot="primary" format="seconds"
	//                                anywhere in the document: picks the slot's time format, checked while it has it
	//   menuitem tap-mode="cursor"   anywhere in the document: picks the Tap Tempo Mode (transport.tapTempoMode, p.
	//                                1458), checked while the setting has it; transport.rml puts them in Tap Tempo's
	//                                context menu
	//   option="beatCountOnly"       anywhere in the document: checked while ui.timeDisplay has it
	//
	// The time formats are the user setting ui.timeFormats, so every transport control shows the same. Use it on the UI
	// thread; the engine and the binding must outlive it. If the element goes, it shows nothing more.
	// The tempo that taps at these times (seconds, in order) beat: the average of the times between them, 1 to 999
	// BPM. None for fewer than two taps.
	std::optional<double> tappedTempo(const std::vector<double>& taps);

	// The message that taps on Tap Tempo show (reference manual p. 1455): where the tempo they beat goes by the Tap
	// Tempo Mode (transport.tapTempoMode), in the tempo mode, with or without a selection, playing or stopped, as
	// tempo.tap applies it.
	std::string tapTempoMessage(const std::string& mode, bool tempoTrackActive, bool selection, bool playing);

	class TransportControls final : private Rml::EventListener
	{
	public:
		// Throws std::invalid_argument for a control in the element whose parameters aren't a JSON object, or whose
		// command and parameters check refuses (#446).
		TransportControls(engine::Engine& engine, EngineBinding& binding, Rml::Element& root, RunCommand run,
			ReleaseCommand release = {}, const CheckCommand& check = {});
		~TransportControls() override;
		TransportControls(const TransportControls&) = delete;
		TransportControls& operator=(const TransportControls&) = delete;
		TransportControls(TransportControls&&) = delete;
		TransportControls& operator=(TransportControls&&) = delete;

		// Starts editing a display as a double click does: left, right, position, length (by their time attribute,
		// the primary one) or tempo or signature. False if the element has none that shows.
		bool edit(const std::string& field);

		// The time format of a slot (primary, secondary, timeDisplay), and changing it in ui.timeFormats.
		TimeFormat format(const std::string& slot) const;
		void setFormat(const std::string& slot, TimeFormat format);
		// Switches the primary and secondary formats.
		void exchangeFormats();

		// The time display window's options, in ui.timeDisplay: Show Beat Count Only on or off, and its size, 0 to 4,
		// changed by steps. onSize hears of the size whenever the setting says it, e.g. to size the window.
		void toggleBeatCountOnly();
		void resize(int steps);
		int size() const { return m_size; }
		void onSize(std::function<void(int size)> sized) { m_sized = std::move(sized); }
		// Where the project window's selection starts, in ticks, which Tap Tempo hands tempo.tap as its selection.
		void followSelection(std::function<std::optional<int64_t>()> start) { m_selectionStart = std::move(start); }

	private:
		void ProcessEvent(Rml::Event& event) override;
		void listen(bool on);
		bool alive() const { return static_cast<bool>(m_root); }
		std::vector<Rml::Element*> all(const std::string& selector) const; // in the root
		void requestProject();
		void followTransport(const engine::Json& status);
		void followFormats(const engine::Json& formats);
		void followTimeDisplay(const engine::Json& options);
		void saveTimeDisplay();
		void changed(Rml::Element& element, const std::string& text);
		void wheel(Rml::Element& element, float delta);
		void wheelTempo();
		void endTempoGesture();
		void tap();
		void endTap();
		void followTapMode(const engine::Json& mode);
		void release();
		void show();      // everything
		void showTime();  // the displays with times, e.g. once a frame while playing
		void showTempo(); // the tempo fields, also once a frame while playing
		int64_t samples(double seconds) const;

		engine::Engine& m_engine;
		EngineBinding& m_binding;
		RunCommand m_run;
		ReleaseCommand m_release;
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::ObserverPtr<Rml::Element> m_document;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for the binding's answers after it went

		TimeContext m_time;
		engine::Json m_status = engine::Json::object(); // as transport.get returns it
		int64_t m_position = 0;                         // the cursor, in samples
		std::map<std::string, TimeFormat> m_formats;    // by slot
		bool m_loading = false;
		bool m_stale = false;
		bool m_beatCountOnly = false;
		int m_size = 1;
		std::function<void(int)> m_sized;
		std::string m_holding; // the command of the button held

		// A gesture on the tempo field: a drag, or wheel turns until a pause. It sets the tempo at its end.
		enum class Gesture : uint8_t
		{
			None,
			Drag,
			Wheel,
		};
		Gesture m_gesture = Gesture::None;
		std::optional<float> m_tempo; // where the gesture took it
		std::optional<float> m_sent;  // what the gesture's end set, until the project's tempo is read again
		std::chrono::steady_clock::time_point m_wheeled;
		std::optional<Subscription> m_wheelPause;
		bool m_tempoTrackActive = false;
		std::vector<double> m_taps;     // when Tap was clicked, by RmlUi's clock, in seconds
		std::string m_tapMode = "auto"; // transport.tapTempoMode
		std::optional<Subscription> m_tapPause;
		std::function<std::optional<int64_t>()> m_selectionStart;

		std::vector<Subscription> m_subscriptions;
		std::optional<Subscription> m_playback; // while playing
	};

	// Rewind and Forward, and their fast variants, while their key or button is held (reference manual pp. 297,
	// 300): start moves the project cursor by the step at once, as a click does, and after a moment winds on at four
	// steps a second until stop. It sends transport.locate with a distance each frame; step returns whether it
	// winds, for Shell::eachStep. Use it on the UI thread; the binding must outlive it.
	class Winding
	{
	public:
		explicit Winding(EngineBinding& binding) : m_binding(binding) {}

		void start(double seconds); // the step: negative to rewind
		void stop() { m_step = 0.0; }
		bool step();

	private:
		using Clock = std::chrono::steady_clock;

		EngineBinding& m_binding;
		double m_step = 0.0;
		Clock::time_point m_last; // until when it wound
	};
}
