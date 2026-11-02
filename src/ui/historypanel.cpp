// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/historypanel.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr std::array<Rml::EventId, 4> g_events = {
			Rml::EventId::Change, Rml::EventId::Dragstart, Rml::EventId::Drag, Rml::EventId::Dragend};

		// A step's time, ISO 8601 in UTC as history.get has it, as the local time of day, e.g. "14:03:12".
		std::string localTime(const std::string& utc)
		{
			std::tm time{};
			std::istringstream in(utc);
			in >> std::get_time(&time, "%Y-%m-%dT%H:%M:%S");
			if (in.fail())
				return utc;
#ifdef _WIN32
			const std::time_t seconds = _mkgmtime(&time);
			localtime_s(&time, &seconds);
#else
			const std::time_t seconds = timegm(&time);
			localtime_r(&seconds, &time);
#endif
			char text[16];
			std::strftime(text, sizeof text, "%H:%M:%S", &time);
			return text;
		}

		void setText(Rml::Element& cell, const std::string& text)
		{
			const std::string rml = Rml::StringUtilities::EncodeRml(text);
			if (cell.GetInnerRML() != rml)
				cell.SetInnerRML(rml);
		}

		// The table bodies before and after the separator: the rows of the initial state and the done steps, and those
		// of the undone steps.
		struct Bodies
		{
			Rml::Element* done = nullptr;
			Rml::Element* undone = nullptr;

			int rows() const { return done->GetNumChildren() + undone->GetNumChildren(); }

			Rml::Element* row(int index) const
			{
				const int before = done->GetNumChildren();
				return index < before ? done->GetChild(index) : undone->GetChild(index - before);
			}
		};

		std::optional<Bodies> bodies(Rml::Element* view)
		{
			Rml::ElementList found;
			if (view != nullptr)
				view->QuerySelectorAll(found, "tbody");
			if (found.size() != 2)
				return std::nullopt;
			return Bodies{found[0], found[1]};
		}

		// The row of the initial state, then a row per step, filled by show.
		std::string rows(size_t steps)
		{
			std::string rml = R"(<tr value="0"><td class="action">Initial state</td><td class="time"/>)"
							  R"(<td class="state"/><td class="details"/></tr>)";
			for (size_t i = 1; i <= steps; ++i)
				rml += R"(<tr value=")" + std::to_string(i) +
					R"("><td class="action"/><td class="time"/><td class="state"/>)"
					R"(<td class="details"><input type="text"/></td></tr>)";
			return rml;
		}
	}

	HistoryPanel::HistoryPanel(EngineBinding& binding, Rml::Element& element)
		: m_binding(binding), m_changed(binding.on("history.changed", [this](const engine::Event&) { request(); }))
	{
		// The rows move between the tables when the position changes, so that the separator stays between them; RmlUi
		// lays both tables' columns out the same from their first rows. Both bodies are at the same depth, so the list
		// view, which looks for rows breadth first, finds them in order.
		element.SetInnerRML(R"(<listview id="history" class="history"><table><thead><tr><td class="action">Action</td>)"
							R"(<td class="time">Time</td><td class="state">State</td><td class="details">Details</td>)"
							R"(</tr></thead><tbody/></table><div class="history-separator"/><table><tbody/></table>)"
							R"(</listview>)");
		Rml::Element* view = element.GetFirstChild();
		m_view = view->GetObserverPtr();
		m_separator = view->GetChild(1)->GetObserverPtr();
		for (const Rml::EventId id : g_events)
			view->AddEventListener(id, this);
		view->AddEventListener(Rml::EventId::Blur, this, true); // blur doesn't bubble
		request();
	}

	HistoryPanel::~HistoryPanel()
	{
		if (!m_view)
			return;
		for (const Rml::EventId id : g_events)
			m_view->RemoveEventListener(id, this);
		m_view->RemoveEventListener(Rml::EventId::Blur, this, true);
	}

	void HistoryPanel::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == m_view.get())
		{
			if (event == Rml::EventId::Change)
				jump(event.GetParameter<int>("value", -1));
		}
		else if (target != nullptr && target == m_separator.get())
		{
			drag(event);
		}
		else if (target != nullptr && target->GetTagName() == "input" && !m_placing &&
			(event == Rml::EventId::Blur || event.GetParameter<bool>("linebreak", false)))
		{
			describe(*target);
		}
	}

	void HistoryPanel::request()
	{
		m_binding.send("history.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					show(result.value);
			});
	}

	void HistoryPanel::show(const Json& history)
	{
		Rml::Element* view = m_view.get();
		const std::optional<Bodies> body = bodies(view);
		if (!body)
			return;
		m_steps = history.at("steps");
		m_position = history.at("position").get<int>();
		const auto count = static_cast<int>(m_steps.size());
		if (body->rows() != count + 1)
		{
			body->undone->SetInnerRML("");
			body->done->SetInnerRML(rows(m_steps.size()));
			m_dragging = false; // the rows it measured are gone, and the steps it would undo or redo may be too
		}
		// RmlUi drops a drag without a dragend when the separator leaves the document, e.g. as the panel moves to a
		// window of its own; the separator stays :active as long as its drag goes on.
		if (m_dragging && (!m_separator || !m_separator->IsPseudoClassSet("active")))
			m_dragging = false;
		for (int i = 1; i <= count; ++i)
		{
			const Rml::Element* row = body->row(i);
			const Json& step = m_steps[static_cast<size_t>(i - 1)];
			setText(*row->GetChild(0), step.at("action").get<std::string>());
			setText(*row->GetChild(1), localTime(step.at("time").get<std::string>()));
			// What the user is typing stays until it is sent.
			if (Rml::Element* field = row->GetChild(3)->GetFirstChild(); !field->IsPseudoClassSet("focus"))
				field->SetAttribute("value", step.at("details").get<std::string>());
		}
		place(m_dragging ? m_dragTo : m_position);
		view->SetAttribute("value", std::to_string(m_position));
		// Answers arrive outside input handling: ask the window to update and draw, also so that the list view moves
		// its selection before the next key.
		if (Rml::Context* context = view->GetContext())
			context->RequestNextUpdate(0);
	}

	void HistoryPanel::place(int position)
	{
		const std::optional<Bodies> body = bodies(m_view.get());
		if (!body)
			return;
		// The initial state's row and those of the steps up to the position go before the separator. RmlUi moves the
		// focus from a row it removes to its parent: a details field the user is typing in gets it back, without
		// sending what it has so far.
		Rml::ObserverPtr<Rml::Element> field;
		if (Rml::Context* context = m_view->GetContext(); context != nullptr)
			for (const Rml::Element* parent = context->GetFocusElement(); parent != nullptr;
				parent = parent->GetParentNode())
				if (parent == m_view.get())
					field = context->GetFocusElement()->GetObserverPtr();
		m_placing = true;
		while (body->done->GetNumChildren() > position + 1)
			body->undone->InsertBefore(
				body->done->RemoveChild(body->done->GetLastChild()), body->undone->GetFirstChild());
		while (body->done->GetNumChildren() < position + 1 && body->undone->HasChildNodes())
			body->done->AppendChild(body->undone->RemoveChild(body->undone->GetFirstChild()));
		if (field && !field->IsPseudoClassSet("focus"))
			field->Focus();
		m_placing = false;
		for (int i = 1; i < body->rows(); ++i)
		{
			Rml::Element* row = body->row(i);
			const bool done = i <= position;
			row->SetClass("undone", !done);
			setText(*row->GetChild(2), done ? "Done" : "Undone");
		}
	}

	void HistoryPanel::drag(const Rml::Event& event)
	{
		const std::optional<Bodies> body = bodies(m_view.get());
		if (!body)
			return;
		if (event == Rml::EventId::Dragstart)
		{
			m_middles.clear();
			for (int i = 1; i < body->rows(); ++i)
			{
				Rml::Element* row = body->row(i);
				m_middles.push_back(row->GetAbsoluteOffset(Rml::BoxArea::Border).y +
					(row->GetBox().GetSize(Rml::BoxArea::Border).y / 2.0F));
			}
			m_dragging = true;
			m_dragTo = m_position;
		}
		else if (event == Rml::EventId::Drag && m_dragging)
		{
			// The separator goes after the steps whose rows' middles are above the mouse, where they were when the
			// drag started; the rows show what it will undo or redo.
			const float mouse = event.GetParameter("mouse_y", 0.0F);
			const auto to = static_cast<int>(
				std::count_if(m_middles.begin(), m_middles.end(), [mouse](float middle) { return middle < mouse; }));
			if (to != m_dragTo)
			{
				m_dragTo = to;
				place(m_dragTo);
			}
		}
		else if (event == Rml::EventId::Dragend && m_dragging)
		{
			m_dragging = false;
			jump(m_dragTo);
			place(m_position);
		}
	}

	void HistoryPanel::jump(int position)
	{
		if (position < 0 || position > static_cast<int>(m_steps.size()) || position == m_position)
			return;
		const int steps = position - m_position;
		m_position = position; // so that a second click before the answer counts from here
		m_binding.send(steps < 0 ? "history.undo" : "history.redo", {{"steps", std::abs(steps)}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && !result.ok())
					request(); // e.g. another client changed the history meanwhile: show it as it is
			});
	}

	void HistoryPanel::describe(Rml::Element& field)
	{
		const Rml::Element* row = field.GetParentNode() == nullptr ? nullptr : field.GetParentNode()->GetParentNode();
		const int step = row == nullptr ? 0 : row->GetAttribute<int>("value", 0) - 1;
		if (step < 0 || step >= static_cast<int>(m_steps.size()))
			return;
		const auto details = field.GetAttribute<Rml::String>("value", "");
		if (details == m_steps[static_cast<size_t>(step)].at("details").get<std::string>())
			return;
		m_steps[static_cast<size_t>(step)]["details"] = details;
		m_binding.send("history.describe", {{"step", step}, {"details", details}});
	}
}
