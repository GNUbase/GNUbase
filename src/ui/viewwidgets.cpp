// SPDX-License-Identifier: GPL-3.0-or-later
// The widgets that show many things (ui/widgets.h): list, tree and table views, and splitters between views.
#include "ui/widgets.h"

#include "ui/rmlui.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>

#include <algorithm>
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

		// <listview>: a list, tree or table of rows that the user selects. Its rows are its <listitem> descendants,
		// or else the rows (<tr>) of a table in it, but not those of its <thead>; each has a value attribute.
		//
		// A click selects a row. With the multiple attribute, Ctrl-click (Cmd-click on macOS) adds or removes a row
		// and Shift-click selects the rows from the last one clicked. Up and Down move the selection (Shift extends
		// it), Home and End go to the first and last row. Selected rows have the state class .selected. The view's
		// value attribute is the selected rows' values, separated by commas, and each change sends a change event
		// with it, so data-value binds the selection; setting the attribute selects the rows.
		//
		// Rows with a level attribute (0 at the top) make a tree: the rows that follow a row with a higher level are
		// its children, shown while it has the expanded attribute. The view gives tree rows an indent and a
		// .tree-toggle part, and parents the state class .parent; a click on the toggle, or Right and Left, expand
		// and collapse a parent, which sends a toggle event with the row's value and "expanded". Rows inside a
		// collapsed parent have the state class .hidden.
		class ListView final : public Rml::Element
		{
		public:
			using Rml::Element::Element;

			void ProcessDefaultAction(Rml::Event& event) override
			{
				Rml::Element::ProcessDefaultAction(event);
				if (HasAttribute("disabled"))
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
				const std::vector<Rml::Element*> all = rows();
				if (all.size() != m_rowCount || GetAttribute<Rml::String>("value", "") != m_value)
				{
					m_rowCount = all.size();
					select(all, GetAttribute<Rml::String>("value", ""));
				}
				shapeTree(all);
			}

		private:
			std::vector<Rml::Element*> rows()
			{
				Rml::ElementList found;
				GetElementsByTagName(found, "listitem");
				if (!found.empty())
					return found;
				GetElementsByTagName(found, "tr");
				found.erase(std::remove_if(found.begin(), found.end(),
								[](const Rml::Element* row) { return row->GetParentNode()->GetTagName() == "thead"; }),
					found.end());
				return found;
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
				for (const Rml::Element* part = event.GetTargetElement(); part != row; part = part->GetParentNode())
					if (part->IsClassSet("tree-toggle"))
					{
						toggle(*row, !row->HasAttribute("expanded"));
						return;
					}
				const bool multiple = HasAttribute("multiple");
				if (multiple && (event.GetParameter("ctrl_key", false) || event.GetParameter("meta_key", false)))
				{
					row->SetClass("selected", !row->IsClassSet("selected"));
					m_anchor = row;
				}
				else if (multiple && event.GetParameter("shift_key", false) && m_anchor != nullptr)
				{
					selectRange(visible(all), *row);
				}
				else
				{
					selectOnly(all, *row);
					m_anchor = row;
				}
				m_current = row;
				changed(all);
			}

			void key(Rml::Event& event)
			{
				const std::vector<Rml::Element*> all = rows();
				const std::vector<Rml::Element*> shown = visible(all);
				if (shown.empty())
					return;
				const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
				const auto at = std::find(shown.begin(), shown.end(), m_current);
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
				case Rml::Input::KI_RIGHT:
				case Rml::Input::KI_LEFT:
					if (m_current != nullptr && m_current->IsClassSet("parent"))
						toggle(*m_current, key == Rml::Input::KI_RIGHT);
					event.StopPropagation();
					return;
				default:
					return;
				}
				event.StopPropagation(); // the keys move the selection, not the focus or a scroll position
				Rml::Element* row = shown[static_cast<size_t>(std::max<std::ptrdiff_t>(next, 0))];
				if (HasAttribute("multiple") && event.GetParameter("shift_key", false) && m_anchor != nullptr)
				{
					selectRange(shown, *row);
				}
				else
				{
					selectOnly(all, *row);
					m_anchor = row;
				}
				m_current = row;
				row->ScrollIntoView(false);
				changed(all);
			}

			static void selectOnly(const std::vector<Rml::Element*>& all, const Rml::Element& chosen)
			{
				for (Rml::Element* row : all)
					row->SetClass("selected", row == &chosen);
			}

			// Selects the rows from the anchor to the row, and only those.
			void selectRange(const std::vector<Rml::Element*>& shown, const Rml::Element& row)
			{
				const auto anchor = std::find(shown.begin(), shown.end(), m_anchor);
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

			// Selects the rows whose values the comma-separated list has.
			void select(const std::vector<Rml::Element*>& all, const std::string& value)
			{
				m_value = value;
				std::vector<std::string> values;
				std::istringstream split(value);
				for (std::string one; std::getline(split, one, ',');)
					values.push_back(one);
				for (Rml::Element* row : all)
				{
					const bool selected = std::find(values.begin(), values.end(),
											  row->GetAttribute<Rml::String>("value", "")) != values.end();
					row->SetClass("selected", selected);
					if (selected && m_current == nullptr)
						m_current = m_anchor = row;
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

			size_t m_rowCount = 0;
			std::string m_value;               // the value attribute as the view last applied or set it
			Rml::Element* m_anchor = nullptr;  // where a Shift-click's range starts
			Rml::Element* m_current = nullptr; // the row the keys move from
		};

		// <splitter>: between two elements of a flex row or column, dragging it moves the boundary between them. It
		// sets the size of the element before it (its width or height in a row or a column, without padding and
		// borders), between none and taking all of the element after it, and sends a change event with that size in
		// dp, e.g. for a workspace to keep. A splitter in a
		// column has the state class .column; the theme's grip shows the direction.
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
				Rml::Element* before = GetPreviousSibling();
				Rml::Element* after = GetNextSibling();
				if (before == nullptr || after == nullptr || GetContext() == nullptr)
					return;
				const bool column = IsClassSet("column");
				const float mouse = event.GetParameter(column ? "mouse_y" : "mouse_x", 0.0F);
				if (event == Rml::EventId::Dragstart)
				{
					m_start = mouse;
					m_size = size(*before, column, Rml::BoxArea::Content);
					// Its size can grow by what the element after it has, padding and borders included.
					m_most = m_size + size(*after, column, Rml::BoxArea::Border);
				}
				else if (event == Rml::EventId::Drag)
				{
					const float dpRatio = GetContext()->GetDensityIndependentPixelRatio();
					const float pixels = std::clamp(m_size + mouse - m_start, 0.0F, m_most);
					const std::string size = format(pixels / dpRatio) + "dp";
					if (column)
						before->SetProperty("height", size);
					else
						before->SetProperty("width", size);
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

			float m_start = 0.0F; // where the drag started
			float m_size = 0.0F;  // the size of the element before, then, without padding and borders
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
