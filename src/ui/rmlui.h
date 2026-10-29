// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi/Core/SystemInterface.h>

#include <chrono>

namespace gnubase::ui
{
	// RmlUi's clock and log: elapsed time since start, messages into GNUbase's log.
	class SystemInterface final : public Rml::SystemInterface
	{
	public:
		double GetElapsedTime() override;
		bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

	private:
		std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
	};

	// RmlUi, set up for GNUbase while the object lives: Rml::Initialise and Rml::Shutdown. One at a time. Windows'
	// contexts (Shell) and the renderer's render interface must outlive it, since Rml::Shutdown releases their
	// resources. Throws std::runtime_error if RmlUi can't start.
	class RmlUi
	{
	public:
		RmlUi();
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
