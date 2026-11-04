// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/enginebinding.h"

#include <RmlUi/Core/ObserverPtr.h>

#include <memory>

namespace Rml
{
	class Element;
}

// The project window's status line (#407): docs/features/status-line.md describes it.
namespace gnubase::ui
{
	// Fills the status line's fields in root from audio.get and audio.changed: #record-format with the project's
	// sample rate, with the class mismatch while the device runs at another one, and #lost-device with the name of
	// the device that went away and the reason and its id as its title, with the class hidden while none is lost; and
	// #record-time with the remaining record time from recording.remainingTime, read again by update(). Use it on the
	// UI thread; the binding must outlive it. Fields that root lacks, or that go, show nothing.
	class StatusLine
	{
	public:
		StatusLine(EngineBinding& binding, Rml::Element& root);

		// Reads the remaining record time again, which the disk filling up changes too; the app calls it every
		// second.
		void update();

	private:
		void show(const engine::Json& status);
		void showRecordTime(const engine::Json& time);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_recordFormat;
		Rml::ObserverPtr<Rml::Element> m_lostDevice;
		Rml::ObserverPtr<Rml::Element> m_recordTime;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for audio.get's answer
		Subscription m_changed;
	};
}
