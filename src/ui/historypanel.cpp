// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/historypanel.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
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
		element.SetInnerRML(R"(<listview id="history" class="history"><table><thead><tr><td class="action">Action</td>)"
							R"(<td class="time">Time</td><td class="state">State</td><td class="details">Details</td>)"
							R"(</tr></thead><tbody/></table></listview>)");
		Rml::Element* view = element.GetFirstChild();
		m_view = view->GetObserverPtr();
		view->AddEventListener(Rml::EventId::Change, this);
		view->AddEventListener(Rml::EventId::Blur, this, true); // blur doesn't bubble
		request();
	}

	HistoryPanel::~HistoryPanel()
	{
		if (!m_view)
			return;
		m_view->RemoveEventListener(Rml::EventId::Change, this);
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
		else if (target != nullptr && target->GetTagName() == "input" &&
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
		Rml::Element* body = view == nullptr ? nullptr : view->QuerySelector("tbody");
		if (body == nullptr)
			return;
		m_steps = history.at("steps");
		m_position = history.at("position").get<int>();
		const auto count = static_cast<int>(m_steps.size());
		if (body->GetNumChildren() != count + 1)
			body->SetInnerRML(rows(m_steps.size()));
		for (int i = 1; i <= count; ++i)
		{
			Rml::Element* row = body->GetChild(i);
			const Json& step = m_steps[static_cast<size_t>(i - 1)];
			const bool done = i <= m_position;
			row->SetClass("undone", !done);
			setText(*row->GetChild(0), step.at("action").get<std::string>());
			setText(*row->GetChild(1), localTime(step.at("time").get<std::string>()));
			setText(*row->GetChild(2), done ? "Done" : "Undone");
			// What the user is typing stays until it is sent.
			if (Rml::Element* field = row->GetChild(3)->GetFirstChild(); !field->IsPseudoClassSet("focus"))
				field->SetAttribute("value", step.at("details").get<std::string>());
		}
		view->SetAttribute("value", std::to_string(m_position));
		// Answers arrive outside input handling: ask the window to update and draw, also so that the list view moves
		// its selection before the next key.
		if (Rml::Context* context = view->GetContext())
			context->RequestNextUpdate(0);
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
