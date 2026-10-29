// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <string>

namespace gnubase::ui
{
	// A data model in a window's RmlUi context: the variables and events that its documents bind to with data-model,
	// {{...}}, data-value, data-event-click and the like. Bind them through constructor() before loading the documents
	// that use the model. When a bound variable changes, call changed(): the views update and the window draws again.
	// The model is removed with the object, which must not outlive RmlUi (ui/rmlui.h); the variables it binds must
	// outlive it. Documents loaded again (ui/livereload.h) keep their model, since it lives in the context.
	class DataModel
	{
	public:
		// Throws std::runtime_error if the context has a model with that name.
		DataModel(Rml::Context& context, std::string name);
		~DataModel();
		DataModel(const DataModel&) = delete;
		DataModel& operator=(const DataModel&) = delete;
		DataModel(DataModel&&) = delete;
		DataModel& operator=(DataModel&&) = delete;

		const std::string& name() const { return m_name; }
		Rml::DataModelConstructor& constructor() { return m_constructor; }

		// The variable changed: its views update in the window's next step. Does nothing once the window is closed.
		void changed(const std::string& variable);
		void changedAll();

	private:
		// The model's context, or null once its window is closed.
		Rml::Context* context() const;

		std::string m_context; // by name, since the window may close first
		std::string m_name;
		Rml::DataModelConstructor m_constructor;
		Rml::DataModelHandle m_handle;
	};
}
