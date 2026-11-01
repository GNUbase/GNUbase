// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>

#include <functional>
#include <optional>
#include <string>
#include <utility>

// What the widget library's source files share (ui/widgets.h).
namespace gnubase::ui::widgets
{
	// A number as documents write it and users type it, "0.5" or "-6 dB", whatever the C locale is.
	std::optional<float> parse(const std::string& text);

	// The number with that many decimals, or as short as it goes without losing precision.
	std::string format(float value, std::optional<int> decimals = std::nullopt);

	// The attribute as a number, or the fallback if it's missing or not a number.
	float number(const Rml::Element& element, const std::string& attribute, float fallback);

	// A part of a widget, which the themes draw by its class, e.g. a knob's pointer.
	Rml::Element* appendPart(Rml::Element& parent, const std::string& className);

	// Hands events to a function: a widget listens with a member, since an element can't be its own listener
	// without a second base class. Listeners must go before the elements they listen to, so a widget removes them in
	// its destructor.
	class Listener final : public Rml::EventListener
	{
	public:
		explicit Listener(std::function<void(Rml::Event&)> handle) : m_handle(std::move(handle)) {}
		void ProcessEvent(Rml::Event& event) override { m_handle(event); }

	private:
		std::function<void(Rml::Event&)> m_handle;
	};
}
