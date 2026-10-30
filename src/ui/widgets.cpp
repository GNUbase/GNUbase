// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/widgets.h"

#include "ui/rmlui.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

namespace gnubase::ui
{
	namespace
	{
		// <button>: clicked with the mouse, or with Space or Enter while it has the focus (RmlUi clicks the focused
		// element). The disabled attribute gives it the :disabled state, and the shared style sheet lets clicks pass
		// through it and takes its focus. With type="toggle" a click turns it on or off: the checked attribute, shown
		// by the state class .checked, and a change event with the new state as a bool, so data-checked binds it.
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

	void registerWidgets(RmlUi& rmlui)
	{
		rmlui.registerElement<Button>("button");
		registerValueWidgets(rmlui);
	}
}
