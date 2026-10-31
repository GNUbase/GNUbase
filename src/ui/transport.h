// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

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

	// Drives the transport controls in an element and follows the transport, the project's tempo and signature and the
	// time formats through the engine binding. The element's markup says what each control is:
	//
	//   command="transport.toggle" parameters='{"setting": "cycle"}'
	//                                a button that runs the command with the parameters (default none)
	//   state="cycle"                checked while the transport's value is true: cycle, punchIn, punchOut,
	//                                metronome, playing or recording
	//   time="position|left|right|length" format="primary|secondary|timeDisplay"
	//                                the project cursor, a locator or the locator range in the time format of that slot
	//                                (default primary); a <textdisplay> takes a position or length typed into it
	//   field="tempo"                a <valuefield> with the first tempo in BPM
	//   field="signature"            a <textdisplay> with the first time signature, "4/4"
	//   menuitem slot="primary" format="seconds"
	//                                anywhere in the document: picks the slot's time format, checked while it has it
	//
	// The time formats are the user setting ui.timeFormats, so every transport control shows the same. Use it on the UI
	// thread; the engine and the binding must outlive it. If the element goes, it shows nothing more.
	class TransportControls final : private Rml::EventListener
	{
	public:
		TransportControls(engine::Engine& engine, EngineBinding& binding, Rml::Element& root, RunCommand run);
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

	private:
		void ProcessEvent(Rml::Event& event) override;
		void listen(bool on);
		bool alive() const { return static_cast<bool>(m_root); }
		std::vector<Rml::Element*> all(const std::string& selector) const; // in the root
		void requestProject();
		void followTransport(const engine::Json& status);
		void followFormats(const engine::Json& formats);
		void changed(Rml::Element& element, const std::string& text);
		void show();     // everything
		void showTime(); // the displays with times, e.g. once a frame while playing
		int64_t samples(double seconds) const;

		engine::Engine& m_engine;
		EngineBinding& m_binding;
		RunCommand m_run;
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::ObserverPtr<Rml::Element> m_document;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for the binding's answers after it went

		TimeContext m_time;
		engine::Json m_status = engine::Json::object(); // as transport.get returns it
		int64_t m_position = 0;                         // the cursor, in samples
		std::map<std::string, TimeFormat> m_formats;    // by slot
		engine::Json m_formatsSaved = engine::Json::object();
		bool m_loading = false;
		bool m_stale = false;

		std::vector<Subscription> m_subscriptions;
		std::optional<Subscription> m_playback; // while playing
	};
}
