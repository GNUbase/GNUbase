// SPDX-License-Identifier: GPL-3.0-or-later
// The widgets that show many things (ui/widgets.h): list, tree and table views, and splitters between views.
#include "ui/widgets.h"

#include "ui/rmlui.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using widgets::appendPart;
		using widgets::format;
		using widgets::number;

		// How far a tree row's level indents it, in dp.
		constexpr float g_indent = 16.0F;

		// A drag past a list view's edge (dragscroll) scrolls it this many times as far as the mouse is past the edge
		// each second, counting at most this long between two updates.
		constexpr float g_dragScrollRate = 10.0F;
		constexpr double g_dragScrollStep = 0.1; // seconds
		constexpr std::array<Rml::EventId, 4> g_dragScrollEvents = {
			Rml::EventId::Drag, Rml::EventId::Dragend, Rml::EventId::Mouseup, Rml::EventId::Blur};

		// <listview>: a list, tree or table of rows that the user selects. Its rows are its <listitem> descendants,
		// or else the rows (<tr>) of the tables in it, but not those of their <thead>s, in the document's order; each
		// has a value attribute.
		//
		// A click selects a row. With the multiple attribute, Ctrl-click (Cmd-click on macOS) adds or removes a row
		// and Shift-click selects the rows from the last one clicked; with shiftdeselects too, Shift-click on a
		// selected row deselects it, as in the track list. Up and Down move the selection (Shift extends
		// it), Page Up and Page Down by one less than the rows the view shows, Home and End go to the first and last
		// row. Selected rows have the state class .selected. The view's
		// value attribute is the selected rows' values, separated by commas, and each change sends a change event
		// with it, so data-value binds the selection; setting the attribute selects the rows. Rows that code adds,
		// removes or replaces with new elements, all or some of them, are selected by the value too, also when a
		// click or a key comes before the next update.
		//
		// Rows with a level attribute (0 at the top) make a tree: the rows that follow a row with a higher level are
		// its children, shown while it has the expanded attribute. The view gives tree rows an indent and a
		// .tree-toggle part, and parents the state class .parent; a click on the toggle, or Right and Left, expand
		// and collapse a parent, which sends a toggle event with the row's value and "expanded". Rows inside a
		// collapsed parent have the state class .hidden.
		//
		// With the dragscroll attribute, a drag of an element in the view (drag: drag) scrolls it while the mouse is
		// above or below it, faster the further the mouse is past its edge, e.g. so that the edit history's separator
		// reaches rows the view doesn't show.
		class ListView final : public Rml::Element
		{
		public:
			explicit ListView(const Rml::String& tag) : Rml::Element(tag)
			{
				for (const Rml::EventId id : g_dragScrollEvents)
					AddEventListener(id, &m_dragListener);
			}

			~ListView() override
			{
				for (const Rml::EventId id : g_dragScrollEvents)
					RemoveEventListener(id, &m_dragListener);
			}

			ListView(const ListView&) = delete;
			ListView& operator=(const ListView&) = delete;
			ListView(ListView&&) = delete;
			ListView& operator=(ListView&&) = delete;

			void ProcessDefaultAction(Rml::Event& event) override
			{
				Rml::Element::ProcessDefaultAction(event);
				if (HasAttribute("disabled") || inControl(event.GetTargetElement()))
					return;
				if (event == Rml::EventId::Click)
					click(event);
				else if (event == Rml::EventId::Keydown)
					key(event);
			}

		protected:
			// Rows come and go with data models, and their attributes change: keep the view in step with them.
			void OnUpdate() override
			{
				Rml::Element::OnUpdate();
				if (m_dragScrolling)
					scrollWhileDragged(); // before the rows are looked at: a scroll can move them, as in the edit
										  // history
				refresh(rows());
			}

		private:
			// Catches up with what code changed since the view last looked: rows added, removed or replaced (also with
			// as many rows and the same value) and a value that code set select the rows again, and tree rows get their
			// parts and state classes. Runs on every update and before every click and key, since those can come
			// before the next update.
			void refresh(const std::vector<Rml::Element*>& all)
			{
				const auto value = GetAttribute<Rml::String>("value", "");
				if (value != m_value || rowsReplaced(all))
					select(all, value);
				shapeTree(all);
			}

			// A drag in the view goes on (with dragscroll), or is over: it ends, the button goes up, or the view loses
			// the focus, as when RmlUi drops a drag whose element leaves the document.
			void dragged(const Rml::Event& event)
			{
				if (event == Rml::EventId::Drag && HasAttribute("dragscroll"))
				{
					if (!m_dragScrolling)
					{
						m_dragScrolling = true;
						m_dragTime = Rml::GetSystemInterface()->GetElapsedTime();
						m_dragRemainder = 0.0F;
					}
					m_dragMouse = event.GetParameter("mouse_y", 0.0F);
					if (Rml::Context* context = GetContext())
						context->RequestNextUpdate(0);
				}
				else if (event.GetId() != Rml::EventId::Drag)
				{
					m_dragScrolling = false;
				}
			}

			// With the mouse past the top or bottom edge, scrolls on by the time since the last update.
			void scrollWhileDragged()
			{
				const double now = Rml::GetSystemInterface()->GetElapsedTime();
				const auto seconds = static_cast<float>(std::min(now - m_dragTime, g_dragScrollStep));
				m_dragTime = now;
				const float top = GetAbsoluteOffset(Rml::BoxArea::Padding).y;
				const float bottom = top + GetClientHeight();
				float past = 0.0F;
				if (m_dragMouse < top)
					past = m_dragMouse - top;
				else if (m_dragMouse > bottom)
					past = m_dragMouse - bottom;
				const float scroll = GetScrollTop();
				const bool more =
					past < 0.0F ? scroll > 0.0F : (past > 0.0F && scroll < GetScrollHeight() - GetClientHeight());
				if (!more)
				{
					m_dragRemainder = 0.0F;
					return;
				}
				m_dragRemainder += past * g_dragScrollRate * seconds;
				const float whole = std::trunc(m_dragRemainder);
				m_dragRemainder -= whole;
				if (whole != 0.0F)
					SetScrollTop(scroll + whole);
				GetContext()->RequestNextUpdate(0);
			}

			// Whether the element is in a form control in a row, such as a text field, whose clicks and keys are its
			// own.
			bool inControl(Rml::Element* element) const
			{
				for (; element != nullptr && element != this; element = element->GetParentNode())
					if (const Rml::String& tag = element->GetTagName();
						tag == "input" || tag == "select" || tag == "textarea")
						return true;
				return false;
			}

			// The rows in the document's order, also from several tables at different depths (RmlUi's
			// GetElementsByTagName goes breadth first).
			std::vector<Rml::Element*> rows()
			{
				std::vector<Rml::Element*> found;
				collect(*this, "listitem", found);
				if (!found.empty())
					return found;
				collect(*this, "tr", found);
				found.erase(std::remove_if(found.begin(), found.end(),
								[](const Rml::Element* row) { return row->GetParentNode()->GetTagName() == "thead"; }),
					found.end());
				return found;
			}

			static void collect(Rml::Element& parent, const Rml::String& tag, std::vector<Rml::Element*>& found)
			{
				for (int i = 0; i < parent.GetNumChildren(); ++i)
				{
					Rml::Element* child = parent.GetChild(i);
					if (child->GetTagName() == tag)
						found.push_back(child);
					collect(*child, tag, found);
				}
			}

			// The row that the event's target is in, or null.
			Rml::Element* rowOf(Rml::Element* element, const std::vector<Rml::Element*>& all)
			{
				for (; element != nullptr && element != this; element = element->GetParentNode())
					if (std::find(all.begin(), all.end(), element) != all.end())
						return element;
				return nullptr;
			}

			static std::vector<Rml::Element*> visible(const std::vector<Rml::Element*>& all)
			{
				std::vector<Rml::Element*> shown;
				std::copy_if(all.begin(), all.end(), std::back_inserter(shown),
					[](const Rml::Element* row) { return !row->IsClassSet("hidden"); });
				return shown;
			}

			void click(Rml::Event& event)
			{
				const std::vector<Rml::Element*> all = rows();
				Rml::Element* row = rowOf(event.GetTargetElement(), all);
				if (row == nullptr)
					return;
				refresh(all);
				for (const Rml::Element* part = event.GetTargetElement(); part != row; part = part->GetParentNode())
				{
					if (part->IsClassSet("tree-toggle"))
					{
						toggle(*row, !row->HasAttribute("expanded"));
						return;
					}
					// A control that opens a menu, such as the track versions arrow, leaves the selection as it is:
					// its menu works on the rows that are selected.
					if (part->HasAttribute("menu"))
						return;
				}
				const bool multiple = HasAttribute("multiple");
				const bool shift = event.GetParameter("shift_key", false);
				if (multiple && (event.GetParameter("ctrl_key", false) || event.GetParameter("meta_key", false)))
				{
					row->SetClass("selected", !row->IsClassSet("selected"));
					m_anchor = row->GetObserverPtr();
				}
				else if (multiple && shift && HasAttribute("shiftdeselects") && row->IsClassSet("selected"))
				{
					row->SetClass("selected", false); // the range keeps going from the last row clicked
				}
				else if (multiple && shift && m_anchor)
				{
					selectRange(visible(all), *row);
				}
				else
				{
					selectOnly(all, *row);
					m_anchor = row->GetObserverPtr();
				}
				setCurrent(row);
				changed(all);
			}

			void key(Rml::Event& event)
			{
				const std::vector<Rml::Element*> all = rows();
				refresh(all);
				const std::vector<Rml::Element*> shown = visible(all);
				if (shown.empty())
					return;
				const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
				const auto at = std::find(shown.begin(), shown.end(), m_current.get());
				const std::ptrdiff_t index = at == shown.end() ? -1 : at - shown.begin();
				std::ptrdiff_t next = 0;
				switch (key)
				{
				case Rml::Input::KI_UP:
					next = std::max<std::ptrdiff_t>(index - 1, 0);
					break;
				case Rml::Input::KI_DOWN:
					next = std::min<std::ptrdiff_t>(index + 1, static_cast<std::ptrdiff_t>(shown.size()) - 1);
					break;
				case Rml::Input::KI_HOME:
					next = 0;
					break;
				case Rml::Input::KI_END:
					next = static_cast<std::ptrdiff_t>(shown.size()) - 1;
					break;
				case Rml::Input::KI_PRIOR:
					next = std::max<std::ptrdiff_t>(index - pageRows(shown), 0);
					break;
				case Rml::Input::KI_NEXT:
					next = std::min<std::ptrdiff_t>(std::max<std::ptrdiff_t>(index, 0) + pageRows(shown),
						static_cast<std::ptrdiff_t>(shown.size()) - 1);
					break;
				case Rml::Input::KI_RIGHT:
				case Rml::Input::KI_LEFT:
					if (m_current && m_current->IsClassSet("parent"))
						toggle(*m_current.get(), key == Rml::Input::KI_RIGHT);
					event.StopPropagation();
					return;
				default:
					return;
				}
				event.StopPropagation(); // the keys move the selection, not the focus or a scroll position
				Rml::Element* row = shown[static_cast<size_t>(std::max<std::ptrdiff_t>(next, 0))];
				if (HasAttribute("multiple") && event.GetParameter("shift_key", false) && m_anchor)
				{
					selectRange(shown, *row);
				}
				else
				{
					selectOnly(all, *row);
					m_anchor = row->GetObserverPtr();
				}
				setCurrent(row);
				row->ScrollIntoView(false);
				changed(all);
			}

			// How many rows Page Up and Page Down move: one less than the view shows, at least one.
			std::ptrdiff_t pageRows(const std::vector<Rml::Element*>& shown)
			{
				const float row = shown.front()->GetOffsetHeight();
				if (row <= 0.0F)
					return 1;
				return std::max<std::ptrdiff_t>(static_cast<std::ptrdiff_t>(GetClientHeight() / row) - 1, 1);
			}

			static void selectOnly(const std::vector<Rml::Element*>& all, const Rml::Element& chosen)
			{
				for (Rml::Element* row : all)
					row->SetClass("selected", row == &chosen);
			}

			// Selects the rows from the anchor to the row, and only those.
			void selectRange(const std::vector<Rml::Element*>& shown, const Rml::Element& row)
			{
				const auto anchor = std::find(shown.begin(), shown.end(), m_anchor.get());
				const auto end = std::find(shown.begin(), shown.end(), &row);
				if (anchor == shown.end() || end == shown.end())
					return;
				const auto [first, last] = std::minmax(anchor, end);
				for (auto it = shown.begin(); it != shown.end(); ++it)
					(*it)->SetClass("selected", it >= first && it <= last);
			}

			// The selection changed: the value attribute and a change event say so.
			void changed(const std::vector<Rml::Element*>& all)
			{
				std::string value;
				for (const Rml::Element* row : all)
					if (row->IsClassSet("selected"))
						value += (value.empty() ? "" : ",") + row->GetAttribute<Rml::String>("value", "");
				m_value = value;
				SetAttribute("value", value);
				Rml::Dictionary parameters;
				parameters["value"] = value;
				DispatchEvent(Rml::EventId::Change, parameters);
			}

			// Selects the rows whose values the comma-separated list has. The keys then move from the first of them,
			// also when the rows were rebuilt or the selection was set from code, e.g. after an engine event.
			void select(const std::vector<Rml::Element*>& all, const std::string& value)
			{
				m_value = value;
				m_rows.clear();
				m_rows.reserve(all.size());
				for (Rml::Element* row : all)
					m_rows.push_back(row->GetObserverPtr());
				setCurrent(nullptr);
				m_anchor.reset();
				std::vector<std::string> values;
				std::istringstream split(value);
				for (std::string one; std::getline(split, one, ',');)
					values.push_back(one);
				for (Rml::Element* row : all)
				{
					const bool selected = std::find(values.begin(), values.end(),
											  row->GetAttribute<Rml::String>("value", "")) != values.end();
					row->SetClass("selected", selected);
					if (selected && !m_current)
					{
						setCurrent(row);
						m_anchor = row->GetObserverPtr();
					}
				}
			}

			void toggle(Rml::Element& row, bool expanded)
			{
				if (expanded == row.HasAttribute("expanded"))
					return;
				if (expanded)
					row.SetAttribute("expanded", "");
				else
					row.RemoveAttribute("expanded");
				shapeTree(rows());
				Rml::Dictionary parameters;
				parameters["value"] = row.GetAttribute<Rml::String>("value", "");
				parameters["expanded"] = expanded;
				DispatchEvent(g_treeToggleEvent, parameters);
			}

			// Gives tree rows their parts, indents and state classes from their levels and expanded attributes.
			static void shapeTree(const std::vector<Rml::Element*>& all)
			{
				std::vector<std::pair<int, bool>> open; // the levels of the rows above and whether they're expanded
				for (size_t i = 0; i < all.size(); ++i)
				{
					Rml::Element& row = *all[i];
					if (!row.HasAttribute("level"))
						continue;
					const int level = static_cast<int>(number(row, "level", 0.0F));
					while (!open.empty() && open.back().first >= level)
						open.pop_back();
					const bool hidden =
						std::any_of(open.begin(), open.end(), [](const auto& above) { return !above.second; });
					const bool parent =
						i + 1 < all.size() && number(*all[i + 1], "level", 0.0F) > static_cast<float>(level);
					row.SetClass("hidden", hidden);
					row.SetClass("parent", parent);
					row.SetClass("expanded", parent && row.HasAttribute("expanded"));
					if (row.GetNumChildren() == 0 || !row.GetChild(0)->IsClassSet("tree-indent"))
					{
						Rml::Element* indent = appendPart(row, "tree-indent");
						row.InsertBefore(row.RemoveChild(indent), row.GetFirstChild());
						Rml::Element* toggle = appendPart(row, "tree-toggle");
						row.InsertBefore(row.RemoveChild(toggle), row.GetChild(1));
					}
					if (Rml::Element* indent = row.GetChild(0); indent->GetAttribute<int>("level", -1) != level)
					{
						indent->SetAttribute("level", level);
						indent->SetProperty("width", format(static_cast<float>(level) * g_indent) + "dp");
					}
					open.emplace_back(level, row.HasAttribute("expanded"));
				}
			}

			void setCurrent(Rml::Element* row)
			{
				m_current = row != nullptr ? row->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
			}

			// Whether the rows aren't those the view last selected among, row by row: code added, removed, moved or
			// replaced some, also any one of them with as many rows and the same value, which neither tells the view.
			// A replaced row's observer pointer is null, so a new row at the old one's address doesn't count as it.
			bool rowsReplaced(const std::vector<Rml::Element*>& all) const
			{
				return !std::equal(all.begin(), all.end(), m_rows.begin(), m_rows.end(),
					[](const Rml::Element* row, const Rml::ObserverPtr<Rml::Element>& known)
					{ return row == known.get(); });
			}

			std::string m_value; // the value attribute as the view last applied or set it
			// Rows can be destroyed at any time; observer pointers become null with them.
			std::vector<Rml::ObserverPtr<Rml::Element>> m_rows; // the rows when the view last selected among them
			Rml::ObserverPtr<Rml::Element> m_anchor;            // where a Shift-click's range starts
			Rml::ObserverPtr<Rml::Element> m_current;           // the row the keys move from
			// While a drag in it scrolls it (dragscroll): the mouse's last y in the window, when it last scrolled, and
			// the part of a pixel it has yet to scroll.
			bool m_dragScrolling = false;
			float m_dragMouse = 0.0F;
			double m_dragTime = 0.0;
			float m_dragRemainder = 0.0F;
			widgets::Listener m_dragListener{[this](const Rml::Event& event) { dragged(event); }};
		};

		// <splitter>: between two elements of a flex row or column, dragging it moves the boundary between them. It
		// sets the size of the element before it, or with resize="next" of the element after it (its width or height
		// in a row or a column, without padding and borders), between its min-width or min-height and taking all of
		// the row's or column's flexible elements (flex-grow) down to their minimums, or of the other element next to
		// it if none is flexible, and sends a change event with that size in dp, e.g. for a workspace to keep. So a
		// zone next to another fixed one, such as the Channel zone next to the left zone, takes its room from the main
		// zone. A splitter in a column has the state class .column; the theme's grip shows the direction.
		class Splitter final : public Rml::Element
		{
		public:
			explicit Splitter(const Rml::String& tag) : Rml::Element(tag)
			{
				appendPart(*this, "splitter-grip-vertical");
				appendPart(*this, "splitter-grip-horizontal");
			}

			void ProcessDefaultAction(Rml::Event& event) override
			{
				Rml::Element::ProcessDefaultAction(event);
				const bool next = GetAttribute<Rml::String>("resize", "") == "next";
				Rml::Element* sized = next ? GetNextSibling() : GetPreviousSibling();
				Rml::Element* other = next ? GetPreviousSibling() : GetNextSibling();
				if (sized == nullptr || other == nullptr || GetContext() == nullptr)
					return;
				const bool column = IsClassSet("column");
				const float mouse = event.GetParameter(column ? "mouse_y" : "mouse_x", 0.0F);
				if (event == Rml::EventId::Dragstart)
				{
					m_start = mouse;
					m_size = size(*sized, column, Rml::BoxArea::Content);
					m_least = least(*sized, column);
					// Its size can grow by what the flexible elements have, padding and borders included, down to
					// their minimums.
					float room = 0.0F;
					bool flexible = false;
					for (int i = 0; i < GetParentNode()->GetNumChildren(); ++i)
					{
						Rml::Element* child = GetParentNode()->GetChild(i);
						const Rml::Style::ComputedValues& values = child->GetComputedValues();
						if (child != this && child != sized && values.display() != Rml::Style::Display::None &&
							values.flex_grow() > 0.0F)
						{
							room += size(*child, column, Rml::BoxArea::Border) - least(*child, column);
							flexible = true;
						}
					}
					if (!flexible)
						room = size(*other, column, Rml::BoxArea::Border) - least(*other, column);
					m_most = std::max(m_least, m_size + room);
				}
				else if (event == Rml::EventId::Drag)
				{
					const float dpRatio = GetContext()->GetDensityIndependentPixelRatio();
					const float moved = next ? m_start - mouse : mouse - m_start;
					const float pixels = std::clamp(m_size + moved, m_least, m_most);
					const std::string size = format(pixels / dpRatio) + "dp";
					if (column)
						sized->SetProperty("height", size);
					else
						sized->SetProperty("width", size);
					Rml::Dictionary parameters;
					parameters["value"] = pixels / dpRatio;
					DispatchEvent(Rml::EventId::Change, parameters);
				}
			}

		protected:
			void OnUpdate() override
			{
				Rml::Element::OnUpdate();
				if (const Rml::Element* parent = GetParentNode())
				{
					const Rml::Style::FlexDirection direction = parent->GetComputedValues().flex_direction();
					SetClass("column",
						direction == Rml::Style::FlexDirection::Column ||
							direction == Rml::Style::FlexDirection::ColumnReverse);
				}
			}

		private:
			static float size(Rml::Element& element, bool column, Rml::BoxArea area)
			{
				const Rml::Vector2f box = element.GetBox().GetSize(area);
				return column ? box.y : box.x;
			}

			// The element's min-width or min-height in pixels.
			static float least(Rml::Element& element, bool column)
			{
				Rml::Element* parent = element.GetParentNode();
				const float base = parent == nullptr ? 0.0F : size(*parent, column, Rml::BoxArea::Content);
				const Rml::Style::ComputedValues& values = element.GetComputedValues();
				return Rml::ResolveValue(column ? values.min_height() : values.min_width(), base);
			}

			float m_start = 0.0F; // where the drag started
			float m_size = 0.0F;  // the size of the sized element, then, without padding and borders
			float m_least = 0.0F; // the least it can have
			float m_most = 0.0F;  // the most it can have
		};
	}

	void registerViewWidgets(RmlUi& rmlui)
	{
		Rml::RegisterEventType(g_treeToggleEvent, false, true, Rml::DefaultActionPhase::None);
		rmlui.registerElement<ListView>("listview");
		rmlui.registerElement<Splitter>("splitter");
	}
}
