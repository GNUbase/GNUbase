// SPDX-License-Identifier: GPL-3.0-or-later
// The widgets with a number (ui/widgets.h): knobs, faders, sliders, value fields and meters.
#include "ui/widgets.h"

#include "ui/rmlui.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>

namespace gnubase::ui
{
	namespace
	{
		using widgets::appendPart;
		using widgets::format;
		using widgets::number;
		using widgets::parse;

		// How far the mouse moves for the whole range of a knob or a value field, in dp.
		constexpr float g_dragRange = 200.0F;
		// Shift makes drags, wheel turns and arrow keys this much finer.
		constexpr float g_fine = 0.1F;

		// What a ValueControl hears to change its value or to end a gesture.
		constexpr std::array<Rml::EventId, 6> g_gestureEvents = {Rml::EventId::Mousescroll, Rml::EventId::Mouseout,
			Rml::EventId::Mousedown, Rml::EventId::Mouseup, Rml::EventId::Dragend, Rml::EventId::Keyup};

		// The last gesture of any ValueControl, so that no two gestures have the same number. Only on the UI thread.
		int g_lastGesture = 0;

		std::string percent(float fraction)
		{
			return format(fraction * 100.0F) + "%";
		}

		// A widget with a number: the attributes value, min (0), max (1), default (min) and step (a hundredth of the
		// range). Dragging changes the value, up or to the right for more, the wheel and the arrow keys by a step; all
		// of them ten times finer with Shift. Ctrl-click (Cmd-click on macOS) sets the default. Each change sets the
		// value attribute and sends a change event with the value as a float and the gesture it belongs to as an
		// integer: the changes of one drag, of wheel turns until the mouse leaves, of an arrow key until it is released
		// and a typed value have the same gesture, and the next gesture has a higher one, so that a document can make
		// each gesture one undo step (history.transaction's merge).
		class ValueControl : public Rml::Element
		{
		public:
			explicit ValueControl(const Rml::String& tag) : Rml::Element(tag)
			{
				for (const Rml::EventId id : g_gestureEvents)
					AddEventListener(id, &m_listener);
			}

			~ValueControl() override
			{
				for (const Rml::EventId id : g_gestureEvents)
					RemoveEventListener(id, &m_listener);
			}

			ValueControl(const ValueControl&) = delete;
			ValueControl& operator=(const ValueControl&) = delete;
			ValueControl(ValueControl&&) = delete;
			ValueControl& operator=(ValueControl&&) = delete;

			// The events that the widget listens to rather than handling them as default actions: the wheel, which
			// has none, and those of its parts.
			virtual void listen(Rml::Event& event)
			{
				// What ends a gesture: a click, the end of a drag, the mouse leaving the widget outside a drag, a
				// released key.
				const Rml::EventId kind = event.GetId();
				if (kind == Rml::EventId::Mousedown || kind == Rml::EventId::Mouseup || kind == Rml::EventId::Dragend ||
					kind == Rml::EventId::Keyup ||
					(kind == Rml::EventId::Mouseout && event.GetTargetElement() == this && !m_dragging))
				{
					if (kind == Rml::EventId::Dragend)
						m_dragging = false;
					endGesture();
					return;
				}
				if (kind != Rml::EventId::Mousescroll || !active())
					return;
				change(value() - (event.GetParameter("wheel_delta_y", 0.0F) * step() * fineness(event)));
				event.StopPropagation(); // the wheel turns the widget, it doesn't scroll what's around it
			}

			void ProcessDefaultAction(Rml::Event& event) override
			{
				Rml::Element::ProcessDefaultAction(event);
				if (!active())
					return;
				switch (event.GetId())
				{
				case Rml::EventId::Mousedown:
					if (event.GetParameter("button", 0) == 0 &&
						(event.GetParameter("ctrl_key", false) || event.GetParameter("meta_key", false)))
						change(number(*this, "default", minimum()));
					break;
				case Rml::EventId::Dragstart:
					m_last = position(event);
					m_dragging = true;
					break;
				case Rml::EventId::Drag:
				{
					const float at = position(event);
					const float pixels = std::max(travel(), 1.0F);
					change(value() + ((at - m_last) / pixels * (maximum() - minimum()) * fineness(event)));
					m_last = at;
					break;
				}
				case Rml::EventId::Keydown:
				{
					const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
					if (key == Rml::Input::KI_UP || key == Rml::Input::KI_RIGHT)
						change(value() + (step() * fineness(event)));
					else if (key == Rml::Input::KI_DOWN || key == Rml::Input::KI_LEFT)
						change(value() - (step() * fineness(event)));
					else
						break;
					event.StopPropagation(); // the widget used the key: no key command runs for it
					break;
				}
				default:
					break;
				}
			}

		protected:
			float minimum() const { return number(*this, "min", 0.0F); }
			float maximum() const { return number(*this, "max", 1.0F); }
			float step() const { return number(*this, "step", (maximum() - minimum()) / 100.0F); }
			float value() const
			{
				return std::clamp(number(*this, "value", number(*this, "default", minimum())),
					std::min(minimum(), maximum()), std::max(minimum(), maximum()));
			}

			// Where the value is between min and max, 0 to 1.
			float fraction() const
			{
				const float range = maximum() - minimum();
				return range == 0.0F ? 0.0F : std::clamp((value() - minimum()) / range, 0.0F, 1.0F);
			}

			// Sets the value, within min and max, and tells about it if it changed.
			void change(float to)
			{
				to = std::clamp(to, std::min(minimum(), maximum()), std::max(minimum(), maximum()));
				if (to == value() && HasAttribute("value"))
					return;
				SetAttribute("value", format(to));
				if (!m_gesturing)
				{
					m_gesture = ++g_lastGesture;
					m_gesturing = true;
				}
				Rml::Dictionary parameters;
				parameters["value"] = to;
				parameters["gesture"] = m_gesture;
				DispatchEvent(Rml::EventId::Change, parameters);
			}

			// The next change starts another gesture.
			void endGesture() { m_gesturing = false; }

			// Whether the user can change it now.
			virtual bool active() const { return !HasAttribute("disabled"); }
			// Drags are vertical unless this says they're horizontal.
			virtual bool horizontal() const { return false; }
			// How many pixels the mouse moves for the whole range.
			virtual float travel() const
			{
				return g_dragRange * (GetContext() != nullptr ? GetContext()->GetDensityIndependentPixelRatio() : 1.0F);
			}
			// Shows the value: the position of a part, a text.
			virtual void show() = 0;

			void OnAttributeChange(const Rml::ElementAttributes& changed) override
			{
				Rml::Element::OnAttributeChange(changed);
				if (changed.count("disabled") != 0)
					SetPseudoClass("disabled", HasAttribute("disabled"));
				show();
			}

		private:
			float position(const Rml::Event& event) const
			{
				return horizontal() ? event.GetParameter("mouse_x", 0.0F) : -event.GetParameter("mouse_y", 0.0F);
			}

			static float fineness(const Rml::Event& event)
			{
				return event.GetParameter("shift_key", false) ? g_fine : 1.0F;
			}

			float m_last = 0.0F;      // where the mouse was in the drag
			bool m_dragging = false;  // from dragstart to dragend
			int m_gesture = 0;        // the gesture of the last change
			bool m_gesturing = false; // the next change belongs to it

		protected:
			widgets::Listener m_listener{[this](Rml::Event& event) { listen(event); }};
		};

		// <knob>: a rotary control whose pointer turns from -135 to 135 degrees, dragged up and down.
		class Knob final : public ValueControl
		{
		public:
			explicit Knob(const Rml::String& tag) : ValueControl(tag)
			{
				appendPart(*this, "knob");
				m_pointer = appendPart(*this, "knob-pointer");
				show();
			}

		protected:
			void show() override
			{
				if (m_pointer != nullptr)
					m_pointer->SetProperty("transform", "rotate(" + format((fraction() * 270.0F) - 135.0F) + "deg)");
			}

		private:
			Rml::Element* m_pointer = nullptr;
		};

		// <fader>: a vertical fader whose cap follows the mouse.
		class Fader final : public ValueControl
		{
		public:
			explicit Fader(const Rml::String& tag) : ValueControl(tag)
			{
				appendPart(*this, "fader");
				m_travel = appendPart(*this, "fader-travel");
				m_cap = appendPart(*m_travel, "fader-cap");
				show();
			}

		protected:
			float travel() const override { return m_travel->GetClientHeight(); }

			void show() override
			{
				if (m_cap != nullptr)
					m_cap->SetProperty("top", percent(1.0F - fraction()));
			}

		private:
			Rml::Element* m_travel = nullptr; // where the cap's centre goes
			Rml::Element* m_cap = nullptr;
		};

		// <slider>: a horizontal slider whose thumb follows the mouse, with the track filled up to it.
		class Slider final : public ValueControl
		{
		public:
			explicit Slider(const Rml::String& tag) : ValueControl(tag)
			{
				m_fill = appendPart(*appendPart(*this, "slider-track"), "slider-fill");
				m_travel = appendPart(*this, "slider-travel");
				m_thumb = appendPart(*m_travel, "slider-thumb");
				show();
			}

		protected:
			bool horizontal() const override { return true; }
			float travel() const override { return m_travel->GetClientWidth(); }

			void show() override
			{
				if (m_thumb == nullptr)
					return;
				m_fill->SetProperty("width", percent(fraction()));
				m_thumb->SetProperty("left", percent(fraction()));
			}

		private:
			Rml::Element* m_fill = nullptr;
			Rml::Element* m_travel = nullptr; // where the thumb's centre goes
			Rml::Element* m_thumb = nullptr;
		};

		// What a value field or a text display hears from its text field: change tells that the user typed.
		constexpr std::array<Rml::EventId, 3> g_inputEvents = {
			Rml::EventId::Blur, Rml::EventId::Keydown, Rml::EventId::Change};

		// Whether a change of the text field is typing, rather than the line break that Enter sends without changing
		// the text.
		bool typed(const Rml::Event& change)
		{
			return !change.GetParameter("linebreak", false);
		}

		// <valuefield>: the value as text, with the decimals (2) and the unit that its attributes give. Besides
		// dragging, a double click, or Enter while it has the focus, edits it as text, with the state class .editing:
		// Enter or leaving the field takes the number typed, Escape keeps the value, and so does a field nothing was
		// typed into, so that a value with more decimals than it shows isn't rounded to them. With autofocus, it is
		// edited, all of it selected so that the number typed replaces it, whenever it gets a visible focus: when its
		// document shows it focused, or Tab moves to it, but not on a click.
		class ValueField final : public ValueControl
		{
		public:
			explicit ValueField(const Rml::String& tag) : ValueControl(tag)
			{
				m_text = appendPart(*this, "valuefield-text");
				Rml::XMLAttributes attributes;
				attributes["type"] = "text";
				m_input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(
					AppendChild(Rml::Factory::InstanceElement(this, "input", "input", attributes)));
				for (const Rml::EventId id : g_inputEvents)
					m_input->AddEventListener(id, &m_listener);
				show();
			}

			~ValueField() override
			{
				for (const Rml::EventId id : g_inputEvents)
					m_input->RemoveEventListener(id, &m_listener);
			}

			ValueField(const ValueField&) = delete;
			ValueField& operator=(const ValueField&) = delete;
			ValueField(ValueField&&) = delete;
			ValueField& operator=(ValueField&&) = delete;

			void listen(Rml::Event& event) override
			{
				if (event.GetTargetElement() != m_input)
				{
					ValueControl::listen(event);
					return;
				}
				switch (event.GetId())
				{
				case Rml::EventId::Blur:
					finishEditing(true);
					break;
				case Rml::EventId::Change:
					m_typed = m_typed || typed(event);
					break;
				case Rml::EventId::Keydown:
				{
					// Before the text field, which keeps Enter to itself.
					const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
					if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER ||
						key == Rml::Input::KI_ESCAPE)
					{
						finishEditing(key != Rml::Input::KI_ESCAPE);
						event.StopPropagation();
					}
					break;
				}
				default:
					break;
				}
			}

			void ProcessDefaultAction(Rml::Event& event) override
			{
				// A key before that update, typed while the document was loading, starts editing too, so that the text
				// it types goes into the text field.
				if (event == Rml::EventId::Keydown && m_focused)
					startFocused();
				ValueControl::ProcessDefaultAction(event);
				const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
				const bool enter = event == Rml::EventId::Keydown &&
					(key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER);
				if ((event == Rml::EventId::Dblclick || enter) && active())
				{
					event.StopPropagation(); // Enter edits the field, it doesn't click what's around it
					startEditing();
				}
				else if (event == Rml::EventId::Focus && event.GetTargetElement() == this &&
					HasAttribute("autofocus") && event.GetParameter("focus_visible", false))
				{
					m_focused = true;
				}
			}

		protected:
			// Editing starts on the update after the focus event, whose focus change is still under way.
			void OnUpdate() override
			{
				ValueControl::OnUpdate();
				if (m_focused)
					startFocused();
			}

			bool active() const override { return ValueControl::active() && !m_editing; }

			void show() override
			{
				if (m_text == nullptr)
					return;
				std::string text = format(value(), decimals());
				if (const auto unit = GetAttribute<Rml::String>("unit", ""); !unit.empty())
					text += " " + unit;
				m_text->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
			}

		private:
			int decimals() const { return static_cast<int>(number(*this, "decimals", 2.0F)); }

			// Starts editing after a visible focus, if the field still has it.
			void startFocused()
			{
				m_focused = false;
				if (GetContext() != nullptr && GetContext()->GetFocusElement() == this && active())
					startEditing();
			}

			void startEditing()
			{
				m_editing = true;
				SetClass("editing", true);
				m_input->SetValue(format(value(), decimals()));
				m_typed = false; // after SetValue, whose text the text field may clean up with a change
				m_input->Focus(true);
				m_input->Select();
			}

			void finishEditing(bool take)
			{
				if (!m_editing)
					return;
				m_editing = false;
				SetClass("editing", false);
				if (take && m_typed)
					if (const std::optional<float> typed = parse(m_input->GetValue()))
					{
						endGesture(); // a typed value is a gesture of its own
						change(*typed);
						endGesture();
					}
				if (GetContext() != nullptr && GetContext()->GetFocusElement() == m_input)
					m_input->Blur(); // the field keeps the focus
			}

			Rml::Element* m_text = nullptr;
			Rml::ElementFormControlInput* m_input = nullptr;
			bool m_editing = false;
			bool m_typed = false;   // the user typed while editing, even if the text is the same again
			bool m_focused = false; // it has autofocus and got a visible focus, and editing hasn't started yet
		};

		// <textdisplay>: its value attribute as text, drawn like a display, such as a position in a time format. A
		// double click edits it as text, with the state class .editing: Enter, or leaving the field, sends change with
		// the text typed as value (a string), for the document to read and set, if anything was typed, even the text it
		// started with (the display may round, or have moved on, such as a position during playback); Escape keeps the
		// value.
		class TextDisplay final : public Rml::Element
		{
		public:
			explicit TextDisplay(const Rml::String& tag) : Rml::Element(tag)
			{
				m_text = appendPart(*this, "textdisplay-text");
				Rml::XMLAttributes attributes;
				attributes["type"] = "text";
				m_input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(
					AppendChild(Rml::Factory::InstanceElement(this, "input", "input", attributes)));
				for (const Rml::EventId id : g_inputEvents)
					m_input->AddEventListener(id, &m_listener);
				show();
			}

			~TextDisplay() override
			{
				for (const Rml::EventId id : g_inputEvents)
					m_input->RemoveEventListener(id, &m_listener);
			}

			TextDisplay(const TextDisplay&) = delete;
			TextDisplay& operator=(const TextDisplay&) = delete;
			TextDisplay(TextDisplay&&) = delete;
			TextDisplay& operator=(TextDisplay&&) = delete;

			void ProcessDefaultAction(Rml::Event& event) override
			{
				Rml::Element::ProcessDefaultAction(event);
				if (event == Rml::EventId::Dblclick && !m_editing && !HasAttribute("disabled"))
				{
					m_editing = true;
					SetClass("editing", true);
					m_input->SetValue(GetAttribute<Rml::String>("value", ""));
					m_typed = false; // after SetValue, whose text the text field may clean up with a change
					m_input->Focus(true);
					m_input->Select();
				}
			}

		protected:
			void OnAttributeChange(const Rml::ElementAttributes& changed) override
			{
				Rml::Element::OnAttributeChange(changed);
				if (changed.count("value") != 0)
					show();
			}

		private:
			void listen(Rml::Event& event)
			{
				if (event == Rml::EventId::Blur)
				{
					finishEditing(true);
					return;
				}
				if (event == Rml::EventId::Change)
				{
					m_typed = m_typed || typed(event);
					return;
				}
				// Before the text field, which keeps Enter to itself.
				const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
				if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER || key == Rml::Input::KI_ESCAPE)
				{
					finishEditing(key != Rml::Input::KI_ESCAPE);
					event.StopPropagation();
				}
			}

			void show()
			{
				if (m_text != nullptr)
					m_text->SetInnerRML(Rml::StringUtilities::EncodeRml(GetAttribute<Rml::String>("value", "")));
			}

			void finishEditing(bool take)
			{
				if (!m_editing)
					return;
				m_editing = false;
				SetClass("editing", false);
				if (GetContext() != nullptr && GetContext()->GetFocusElement() == m_input)
					m_input->Blur(); // the field keeps the focus
				if (take && m_typed)
				{
					Rml::Dictionary parameters;
					parameters["value"] = m_input->GetValue();
					DispatchEvent(Rml::EventId::Change, parameters);
				}
			}

			widgets::Listener m_listener{[this](Rml::Event& event) { listen(event); }};
			Rml::Element* m_text = nullptr;
			Rml::ElementFormControlInput* m_input = nullptr;
			bool m_editing = false;
			bool m_typed = false; // the user typed while editing, even if the text is the same again
		};

		// <meter>: a level meter, value and peak in dB between min (-60) and max (6). The level shows the lower part
		// of the theme's meter fill, and the peak sits at its height. Engine meters arrive once a frame (#111).
		class Meter final : public Rml::Element
		{
		public:
			explicit Meter(const Rml::String& tag) : Rml::Element(tag)
			{
				appendPart(*this, "meter");
				m_level = appendPart(*this, "meter-level");
				m_fill = appendPart(*m_level, "meter-fill");
				m_peak = appendPart(*this, "meter-peak");
				show();
			}

		protected:
			void OnAttributeChange(const Rml::ElementAttributes& changed) override
			{
				Rml::Element::OnAttributeChange(changed);
				show();
			}

			// The fill is as tall as the meter, so that the level crops it rather than squeezing it.
			void OnResize() override
			{
				Rml::Element::OnResize();
				m_fill->SetProperty("height", format(GetClientHeight()) + "px");
			}

		private:
			float fraction(const std::string& attribute) const
			{
				const float minimum = number(*this, "min", -60.0F);
				const float range = number(*this, "max", 6.0F) - minimum;
				const float level = number(*this, attribute, minimum);
				return range <= 0.0F ? 0.0F : std::clamp((level - minimum) / range, 0.0F, 1.0F);
			}

			void show()
			{
				if (m_peak == nullptr)
					return;
				m_level->SetProperty("height", percent(fraction("value")));
				m_peak->SetProperty("bottom", percent(fraction("peak")));
			}

			Rml::Element* m_level = nullptr;
			Rml::Element* m_fill = nullptr;
			Rml::Element* m_peak = nullptr;
		};
	}

	void registerValueWidgets(RmlUi& rmlui)
	{
		rmlui.registerElement<Knob>("knob");
		rmlui.registerElement<Fader>("fader");
		rmlui.registerElement<Slider>("slider");
		rmlui.registerElement<ValueField>("valuefield");
		rmlui.registerElement<Meter>("meter");
		rmlui.registerElement<TextDisplay>("textdisplay");
	}
}
