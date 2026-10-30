// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/widgets.h"

#include "ui/rmlui.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>

namespace gnubase::ui
{
	namespace
	{
		// <button>, and <menuitem> the same way: clicked with the mouse, or with Space or Enter while it has the focus
		// (RmlUi clicks the focused element). The disabled attribute gives it the :disabled state, and the shared style
		// sheet lets clicks pass through it and takes its focus. With type="toggle" a click turns it on or off: the
		// checked attribute, shown by the state class .checked, and a change event with the new state as a bool, so
		// data-checked binds it.
		class Button final : public Rml::Element
		{
		public:
			using Rml::Element::Element;

			void ProcessDefaultAction(Rml::Event& event) override
			{
				Rml::Element::ProcessDefaultAction(event);
				if (event == Rml::EventId::Click && !HasAttribute("disabled") &&
					GetAttribute<Rml::String>("type", "") == "toggle")
				{
					const bool checked = !HasAttribute("checked");
					if (checked)
						SetAttribute("checked", "");
					else
						RemoveAttribute("checked");
					Rml::Dictionary parameters;
					parameters["value"] = checked;
					DispatchEvent(Rml::EventId::Change, parameters);
				}
			}

		protected:
			void OnAttributeChange(const Rml::ElementAttributes& changed) override
			{
				Rml::Element::OnAttributeChange(changed);
				if (changed.count("disabled") != 0)
					SetPseudoClass("disabled", HasAttribute("disabled"));
				if (changed.count("checked") != 0)
					SetClass("checked", HasAttribute("checked"));
			}
		};
	}

	namespace widgets
	{
		std::optional<float> parse(const std::string& text)
		{
			std::istringstream in(text);
			in.imbue(std::locale::classic());
			float value = 0.0F;
			if (!(in >> value) || !std::isfinite(value))
				return std::nullopt;
			return value;
		}

		std::string format(float value, std::optional<int> decimals)
		{
			std::ostringstream out;
			out.imbue(std::locale::classic());
			if (decimals)
				out << std::fixed << std::setprecision(*decimals);
			else
				out << std::setprecision(7);
			out << value;
			return out.str();
		}

		float number(const Rml::Element& element, const std::string& attribute, float fallback)
		{
			return parse(element.GetAttribute<Rml::String>(attribute, "")).value_or(fallback);
		}

		Rml::Element* appendPart(Rml::Element& parent, const std::string& className)
		{
			Rml::ElementPtr part = Rml::Factory::InstanceElement(&parent, "*", "div", Rml::XMLAttributes());
			part->SetClass(className, true);
			return parent.AppendChild(std::move(part));
		}
	}

	void registerWidgets(RmlUi& rmlui)
	{
		rmlui.registerElement<Button>("button");
		rmlui.registerElement<Button>("menuitem");
		registerValueWidgets(rmlui);
		registerPopupWidgets(rmlui);
	}
}
