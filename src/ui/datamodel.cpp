// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/datamodel.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>

#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	DataModel::DataModel(Rml::Context& context, std::string name)
		: m_context(context.GetName()), m_name(std::move(name)), m_constructor(context.CreateDataModel(m_name))
	{
		if (!m_constructor)
			throw std::runtime_error("the window already has a data model named " + m_name);
		m_handle = m_constructor.GetModelHandle();
	}

	DataModel::~DataModel()
	{
		if (Rml::Context* model = context())
			model->RemoveDataModel(m_name);
	}

	void DataModel::changed(const std::string& variable)
	{
		if (Rml::Context* model = context())
		{
			m_handle.DirtyVariable(variable);
			model->RequestNextUpdate(0);
		}
	}

	void DataModel::changedAll()
	{
		if (Rml::Context* model = context())
		{
			m_handle.DirtyAllVariables();
			model->RequestNextUpdate(0);
		}
	}

	Rml::Context* DataModel::context() const
	{
		return Rml::GetContext(m_context);
	}
}
