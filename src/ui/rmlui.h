// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include <chrono>

namespace gnubase::ui
{
	class WindowSystem;

	// The RmlUi event that the element under the mouse gets when files are dropped on a window. It bubbles up, and
	// its parameters are "files" (the files' UTF-8 paths, separated by newlines), "count" (how many), and "mouse_x"
	// and "mouse_y" (where they were dropped, in the window's pixels).
	inline constexpr const char* g_fileDropEvent = "filedrop";

	// RmlUi's clock, log and clipboard: elapsed time since start, messages into GNUbase's log, and the window
	// system's clipboard, which text fields copy to and paste from.
	class SystemInterface final : public Rml::SystemInterface
	{
	public:
		explicit SystemInterface(WindowSystem& windows) : m_windows(windows) {}

		double GetElapsedTime() override;
		bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
		void SetClipboardText(const Rml::String& text) override;
		void GetClipboardText(Rml::String& text) override;

	private:
		WindowSystem& m_windows;
		std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
	};

	// RmlUi, set up for GNUbase while the object lives: Rml::Initialise and Rml::Shutdown. One at a time. Windows'
	// contexts (Shell) and the renderer's render interface must outlive it, since Rml::Shutdown releases their
	// resources; the window system must outlive it too. Throws std::runtime_error if RmlUi can't start.
	class RmlUi
	{
	public:
		explicit RmlUi(WindowSystem& windows);
		~RmlUi();
		RmlUi(const RmlUi&) = delete;
		RmlUi& operator=(const RmlUi&) = delete;
		RmlUi(RmlUi&&) = delete;
		RmlUi& operator=(RmlUi&&) = delete;

	private:
		SystemInterface m_system;
		// ponytail: no text until FreeType arrives (#207); RmlUi needs a font engine to start.
		Rml::FontEngineInterface m_fonts;
	};
}
