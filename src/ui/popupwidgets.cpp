// SPDX-License-Identifier: GPL-3.0-or-later
// The widgets that pop up (ui/widgets.h): menus and context menus, tooltips and dialogs.
#include "ui/widgets.h"

#include "ui/rmlui.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Plugin.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using widgets::format;
		using widgets::Listener;

		// How long the mouse rests on an element before its tooltip shows, in seconds.
		constexpr double g_tooltipDelay = 0.6;
		// How far below the mouse a tooltip shows, in dp.
		constexpr float g_tooltipOffset = 20.0F;

		// The element or its nearest ancestor with the attribute, or null.
		Rml::Element* closestWith(Rml::Element* element, const char* attribute)
		{
			while (element != nullptr && !element->HasAttribute(attribute))
				element = element->GetParentNode();
			return element;
		}

		// The element or its nearest ancestor with the tag, below the end; null if there is none.
		Rml::Element* closestTag(Rml::Element* element, const std::string& tag, const Rml::Element* end = nullptr)
		{
			while (element != nullptr && element != end && element->GetTagName() != tag)
				element = element->GetParentNode();
			return element != end ? element : nullptr;
		}

		double now()
		{
			return Rml::GetSystemInterface()->GetElapsedTime();
		}

		// Moves an absolutely positioned popup to a point in the window's pixels.
		void moveTo(Rml::Element& popup, Rml::Vector2f at)
		{
			Rml::Element* parent = popup.GetOffsetParent();
			const Rml::Vector2f origin =
				parent != nullptr ? parent->GetAbsoluteOffset(Rml::BoxArea::Padding) : Rml::Vector2f(0.0F, 0.0F);
			popup.SetProperty("left", format(at.x - origin.x) + "px");
			popup.SetProperty("top", format(at.y - origin.y) + "px");
		}

		// The point, moved so that a popup of that size there lies inside the window.
		Rml::Vector2f inside(Rml::Element& popup, Rml::Vector2f at)
		{
			const Rml::Vector2f size = popup.GetBox().GetSize(Rml::BoxArea::Border);
			const Rml::Vector2f window(popup.GetContext()->GetDimensions());
			return {
				std::max(0.0F, std::min(at.x, window.x - size.x)), std::max(0.0F, std::min(at.y, window.y - size.y))};
		}

		// <menu>: <menuitem>s and <menuseparator>s, hidden until it opens: at the mouse on a right click on an element
		// whose contextmenu attribute names the menu by id, or under a button whose menu attribute names it. A menu
		// inside an item is its submenu, shown while the mouse is over the item. A click on an item without a submenu
		// closes the menu, as do Escape and a click outside it. Up and Down move the focus between the items, and Enter
		// clicks the focused one. An open menu has the state class .open.
		class Menu final : public Rml::Element
		{
		public:
			using Rml::Element::Element;

			// Opens the menu at a point in the window's pixels, inside the window.
			void open(Rml::Vector2f at)
			{
				SetClass("open", true);
				moveTo(*this, at);
				GetOwnerDocument()->UpdateDocument(); // for the menu's size
				moveTo(*this, inside(*this, at));
				Focus();
			}

			void ProcessDefaultAction(Rml::Event& event) override
			{
				Rml::Element::ProcessDefaultAction(event);
				if (event == Rml::EventId::Click)
				{
					Rml::Element* item = closestTag(event.GetTargetElement(), "menuitem", this);
					if (item != nullptr && !item->HasAttribute("disabled") && item->QuerySelector("menu") == nullptr)
						SetClass("open", false); // every menu the click bubbles through closes
				}
				else if (event == Rml::EventId::Keydown && IsClassSet("open"))
				{
					const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
					if (key == Rml::Input::KI_DOWN || key == Rml::Input::KI_UP)
					{
						moveFocus(key == Rml::Input::KI_DOWN ? 1 : -1);
						event.StopPropagation();
					}
				}
			}

		private:
			// Focuses the next or previous item that isn't disabled.
			void moveFocus(int direction)
			{
				std::vector<Rml::Element*> items;
				for (int i = 0; i < GetNumChildren(); ++i)
					if (GetChild(i)->GetTagName() == "menuitem" && !GetChild(i)->HasAttribute("disabled"))
						items.push_back(GetChild(i));
				if (items.empty())
					return;
				const auto focused = std::find(items.begin(), items.end(), GetContext()->GetFocusElement());
				const int count = static_cast<int>(items.size());
				int index = direction > 0 ? 0 : count - 1;
				if (focused != items.end())
					index = (static_cast<int>(focused - items.begin()) + direction + count) % count;
				items[static_cast<size_t>(index)]->Focus(true);
			}
		};

		// A document's tooltip: the title attribute of the element under the mouse, or of its nearest ancestor with
		// one, shown under the mouse once the mouse has rested there; the state class .shown while it's shown.
		class Tooltip final : public Rml::Element
		{
		public:
			using Rml::Element::Element;

			// The mouse is over the element now, at a point in the window's pixels.
			void hover(Rml::Element* element, Rml::Vector2f at)
			{
				Rml::Element* titled = closestWith(element, "title");
				if (titled == m_titled)
					return;
				hide();
				m_titled = titled;
				if (titled == nullptr)
					return;
				m_at = at + Rml::Vector2f(0.0F, g_tooltipOffset * GetContext()->GetDensityIndependentPixelRatio());
				m_due = now() + g_tooltipDelay;
				GetContext()->RequestNextUpdate(g_tooltipDelay);
			}

			void hide()
			{
				m_titled = nullptr;
				SetClass("shown", false);
			}

		protected:
			void OnUpdate() override
			{
				Rml::Element::OnUpdate();
				if (m_titled == nullptr || IsClassSet("shown"))
					return;
				if (const double wait = m_due - now(); wait > 0.0)
				{
					GetContext()->RequestNextUpdate(wait);
					return;
				}
				SetInnerRML(Rml::StringUtilities::EncodeRml(m_titled->GetAttribute<Rml::String>("title", "")));
				SetClass("shown", true);
				moveTo(*this, m_at);
			}

			// Once laid out with its text, the tooltip moves inside the window if it isn't.
			void OnResize() override
			{
				Rml::Element::OnResize();
				if (IsClassSet("shown"))
					if (const Rml::Vector2f at = inside(*this, m_at); at != m_at)
						moveTo(*this, at);
			}

		private:
			Rml::Element* m_titled = nullptr; // whose title is shown, or waits to be
			Rml::Vector2f m_at;
			double m_due = 0.0;
		};

		// <dialog>: a dialog in a document of its own, which the app shows, usually modal. It opens in the middle of
		// the window, and dragging its .dialog-titlebar moves it. Escape closes it with the result "cancel", Enter
		// clicks its button.primary (also from a text field), and a click on an element with a result attribute closes
		// it with that result. Closing sends a close event with the parameter "result", then closes the document.
		class Dialog final : public Rml::Element
		{
		public:
			explicit Dialog(const Rml::String& tag) : Rml::Element(tag)
			{
				for (const Rml::EventId id : {Rml::EventId::Dragstart, Rml::EventId::Drag})
					AddEventListener(id, &m_listener);
			}

			~Dialog() override
			{
				for (const Rml::EventId id : {Rml::EventId::Dragstart, Rml::EventId::Drag})
					RemoveEventListener(id, &m_listener);
			}

			Dialog(const Dialog&) = delete;
			Dialog& operator=(const Dialog&) = delete;
			Dialog(Dialog&&) = delete;
			Dialog& operator=(Dialog&&) = delete;

			void ProcessDefaultAction(Rml::Event& event) override
			{
				Rml::Element::ProcessDefaultAction(event);
				if (event.GetId() != Rml::EventId::Click)
					return;
				if (const Rml::Element* button = closestWith(event.GetTargetElement(), "result");
					button != nullptr && !button->HasAttribute("disabled"))
					close(button->GetAttribute<Rml::String>("result", ""));
			}

			// A key pressed anywhere in the dialog's document, before text fields keep Enter to themselves.
			void key(Rml::Event& event)
			{
				// A multi-line text field takes Enter, and a value field being edited both keys.
				if (closestTag(event.GetTargetElement(), "textarea", this) != nullptr ||
					closestTag(event.GetTargetElement(), "valuefield", this) != nullptr)
					return;
				const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
				if (key == Rml::Input::KI_ESCAPE)
				{
					event.StopPropagation();
					close("cancel");
				}
				else if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER)
				{
					Rml::Element* primary = QuerySelector("button.primary");
					if (primary == nullptr || primary->HasAttribute("disabled"))
						return;
					event.StopPropagation();
					primary->Click();
				}
			}

		protected:
			// The first layout puts the dialog in the middle of the window.
			void OnResize() override
			{
				Rml::Element::OnResize();
				if (m_placed || GetContext() == nullptr)
					return;
				m_placed = true;
				const Rml::Vector2f window(GetContext()->GetDimensions());
				moveTo(*this, ((window - GetBox().GetSize(Rml::BoxArea::Border)) * 0.5F).Round());
			}

		private:
			void listen(Rml::Event& event)
			{
				const Rml::Vector2f mouse(event.GetParameter("mouse_x", 0.0F), event.GetParameter("mouse_y", 0.0F));
				switch (event.GetId())
				{
				case Rml::EventId::Dragstart:
					m_grab = mouse - GetAbsoluteOffset(Rml::BoxArea::Border);
					break;
				case Rml::EventId::Drag:
					moveTo(*this, mouse - m_grab);
					break;
				default:
					break;
				}
			}

			void close(const std::string& result)
			{
				if (m_closed)
					return;
				m_closed = true;
				Rml::Dictionary parameters;
				parameters["result"] = result;
				DispatchEvent(g_dialogCloseEvent, parameters);
				GetOwnerDocument()->Close();
			}

			Listener m_listener{[this](Rml::Event& event) { listen(event); }};
			Rml::Vector2f m_grab; // where the mouse holds the dialog
			bool m_placed = false;
			bool m_closed = false;
		};

		// What every document gets: the behaviour of attributes that any element can have (contextmenu, menu and
		// title), menus that close on a click elsewhere or on Escape, the keys of its dialog, and a tooltip. It listens
		// in the capture phase, before the elements do.
		class Popups final : public Rml::Plugin
		{
		public:
			int GetEventClasses() override { return EVT_DOCUMENT; }

			void OnDocumentLoad(Rml::ElementDocument* document) override
			{
				document->AppendChild(document->CreateElement("tooltip"));
				for (const Rml::EventId id : g_events)
					document->AddEventListener(id, &m_listener, true);
			}

		private:
			static constexpr std::array<Rml::EventId, 6> g_events = {Rml::EventId::Mousedown, Rml::EventId::Click,
				Rml::EventId::Keydown, Rml::EventId::Mouseover, Rml::EventId::Mousescroll, Rml::EventId::Mouseout};

			static void handle(Rml::Event& event)
			{
				Rml::ElementDocument* document = event.GetCurrentElement()->GetOwnerDocument();
				Rml::Element* target = event.GetTargetElement();
				const Rml::Vector2f mouse(event.GetParameter("mouse_x", 0.0F), event.GetParameter("mouse_y", 0.0F));
				Tooltip* tooltip = tooltipOf(*document);
				switch (event.GetId())
				{
				case Rml::EventId::Mouseover:
					// What the mouse is over now: the elements it entered get the event innermost first.
					if (tooltip != nullptr)
						tooltip->hover(document->GetContext()->GetHoverElement(), mouse);
					break;
				case Rml::EventId::Mouseout:
					// The mouse left the document; Mouseover reports where it goes within it.
					if (tooltip != nullptr && target == document)
						tooltip->hide();
					break;
				case Rml::EventId::Mousescroll:
					if (tooltip != nullptr)
						tooltip->hide();
					break;
				case Rml::EventId::Mousedown:
				{
					if (tooltip != nullptr)
						tooltip->hide();
					// A click outside the open menus closes them, except on the button of a menu, which toggles it.
					const Rml::Element* button = closestWith(target, "menu");
					for (Menu* menu : openMenus(*document))
						if (!contains(*menu, target) && (button == nullptr || menuOf(*button) != menu))
							menu->SetClass("open", false);
					if (event.GetParameter("button", 0) == 1)
						if (const Rml::Element* owner = closestWith(target, "contextmenu"))
							if (Menu* menu = byId(*document, owner->GetAttribute<Rml::String>("contextmenu", "")))
								menu->open(mouse);
					break;
				}
				case Rml::EventId::Click:
					if (Rml::Element* button = closestWith(target, "menu");
						button != nullptr && !button->HasAttribute("disabled"))
						if (Menu* menu = menuOf(*button))
						{
							if (menu->IsClassSet("open"))
								menu->SetClass("open", false);
							else
								menu->open(button->GetAbsoluteOffset(Rml::BoxArea::Border) +
									Rml::Vector2f(0.0F, button->GetBox().GetSize(Rml::BoxArea::Border).y));
						}
					break;
				case Rml::EventId::Keydown:
					if (tooltip != nullptr)
						tooltip->hide();
					if (event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN) == Rml::Input::KI_ESCAPE)
						if (const std::vector<Menu*> open = openMenus(*document); !open.empty())
						{
							for (Menu* menu : open)
								menu->SetClass("open", false);
							event.StopPropagation(); // Escape closed the menu, not the dialog around it
							break;
						}
					if (Dialog* dialog = dialogOf(*document))
						dialog->key(event);
					break;
				default:
					break;
				}
			}

			static Tooltip* tooltipOf(Rml::ElementDocument& document)
			{
				Rml::ElementList found;
				document.GetElementsByTagName(found, "tooltip");
				return found.empty() ? nullptr : dynamic_cast<Tooltip*>(found.back());
			}

			static Dialog* dialogOf(Rml::ElementDocument& document)
			{
				Rml::ElementList found;
				document.GetElementsByTagName(found, "dialog");
				return found.empty() ? nullptr : dynamic_cast<Dialog*>(found.front());
			}

			static Menu* byId(Rml::ElementDocument& document, const std::string& id)
			{
				return id.empty() ? nullptr : dynamic_cast<Menu*>(document.GetElementById(id));
			}

			static Menu* menuOf(const Rml::Element& button)
			{
				return byId(*button.GetOwnerDocument(), button.GetAttribute<Rml::String>("menu", ""));
			}

			static std::vector<Menu*> openMenus(Rml::ElementDocument& document)
			{
				Rml::ElementList found;
				document.QuerySelectorAll(found, "menu.open");
				std::vector<Menu*> menus;
				for (Rml::Element* element : found)
					if (auto* menu = dynamic_cast<Menu*>(element))
						menus.push_back(menu);
				return menus;
			}

			static bool contains(const Rml::Element& ancestor, const Rml::Element* element)
			{
				while (element != nullptr && element != &ancestor)
					element = element->GetParentNode();
				return element != nullptr;
			}

			Listener m_listener{[](Rml::Event& event) { handle(event); }};
		};
	}

	void registerPopupWidgets(RmlUi& rmlui)
	{
		Rml::RegisterEventType(g_dialogCloseEvent, false, true, Rml::DefaultActionPhase::None);
		rmlui.registerElement<Menu>("menu");
		rmlui.registerElement<Tooltip>("tooltip");
		rmlui.registerElement<Dialog>("dialog");
		rmlui.registerPlugin(std::make_unique<Popups>());
	}
}
