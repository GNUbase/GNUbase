// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/rmlui.h"

#include "platform/log.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Log.h>

#include <stdexcept>

namespace gnubase::ui
{
	double SystemInterface::GetElapsedTime()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
	}

	bool SystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message)
	{
		switch (type)
		{
		case Rml::Log::LT_ERROR:
		case Rml::Log::LT_ASSERT:
			logging::error("RmlUi: {}", message);
			break;
		case Rml::Log::LT_WARNING:
			logging::warning("RmlUi: {}", message);
			break;
		case Rml::Log::LT_INFO:
		case Rml::Log::LT_ALWAYS:
			logging::info("RmlUi: {}", message);
			break;
		default:
			logging::debug("RmlUi: {}", message);
			break;
		}
		return true; // never break into the debugger
	}

	RmlUi::RmlUi()
	{
		Rml::SetSystemInterface(&m_system);
		Rml::SetFontEngineInterface(&m_fonts);
		if (!Rml::Initialise())
			throw std::runtime_error("RmlUi cannot start");
	}

	RmlUi::~RmlUi()
	{
		Rml::Shutdown();
	}
}
