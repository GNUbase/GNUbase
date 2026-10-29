// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginhost/vst3/hostobjects.h"

#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <cstring>

namespace gnubase::pluginhost::vst3
{
	// ---- MemoryStream ----

	vst::tresult PLUGIN_API MemoryStream::read(void* buffer, vst::int32 numBytes, vst::int32* numBytesRead)
	{
		if (numBytes < 0 || (buffer == nullptr && numBytes > 0))
			return vst::kInvalidArgument;
		const size_t available = m_data.size() - std::min(m_position, m_data.size());
		const size_t count = std::min(static_cast<size_t>(numBytes), available);
		if (count > 0)
			std::memcpy(buffer, m_data.data() + m_position, count);
		m_position += count;
		if (numBytesRead != nullptr)
			*numBytesRead = static_cast<vst::int32>(count);
		return vst::kResultOk;
	}

	vst::tresult PLUGIN_API MemoryStream::write(void* buffer, vst::int32 numBytes, vst::int32* numBytesWritten)
	{
		if (numBytes < 0 || (buffer == nullptr && numBytes > 0))
			return vst::kInvalidArgument;
		const auto count = static_cast<size_t>(numBytes);
		if (m_position + count > m_data.size())
			m_data.resize(m_position + count);
		if (count > 0)
			std::memcpy(m_data.data() + m_position, buffer, count);
		m_position += count;
		if (numBytesWritten != nullptr)
			*numBytesWritten = numBytes;
		return vst::kResultOk;
	}

	vst::tresult PLUGIN_API MemoryStream::seek(vst::int64 pos, vst::int32 mode, vst::int64* result)
	{
		vst::int64 base = 0;
		if (mode == vst::IBStream::kIBSeekCur)
			base = static_cast<vst::int64>(m_position);
		else if (mode == vst::IBStream::kIBSeekEnd)
			base = static_cast<vst::int64>(m_data.size());
		else if (mode != vst::IBStream::kIBSeekSet)
			return vst::kInvalidArgument;
		const vst::int64 position = base + pos;
		if (position < 0)
			return vst::kInvalidArgument;
		m_position = static_cast<size_t>(position);
		if (result != nullptr)
			*result = position;
		return vst::kResultOk;
	}

	vst::tresult PLUGIN_API MemoryStream::tell(vst::int64* pos)
	{
		if (pos == nullptr)
			return vst::kInvalidArgument;
		*pos = static_cast<vst::int64>(m_position);
		return vst::kResultOk;
	}

	// ---- ParamValueQueue ----

	vst::tresult PLUGIN_API ParamValueQueue::getPoint(
		vst::int32 index, vst::int32& sampleOffset, vst::Vst::ParamValue& value)
	{
		if (index < 0 || static_cast<size_t>(index) >= m_count)
			return vst::kInvalidArgument;
		sampleOffset = m_points[static_cast<size_t>(index)].offset;
		value = m_points[static_cast<size_t>(index)].value;
		return vst::kResultOk;
	}

	vst::tresult PLUGIN_API ParamValueQueue::addPoint(
		vst::int32 sampleOffset, vst::Vst::ParamValue value, vst::int32& index)
	{
		if (m_points.empty())
			return vst::kOutOfMemory;
		// Points are kept in order of their offsets. A point at the offset of the last one replaces it, and so does
		// any later point once the queue is full: the last value of a block is the one that must not get lost.
		if (m_count > 0 && m_points[m_count - 1].offset > sampleOffset)
			return vst::kInvalidArgument;
		if (m_count > 0 && (m_points[m_count - 1].offset == sampleOffset || m_count == m_points.size()))
		{
			m_points[m_count - 1] = {sampleOffset, value};
			index = static_cast<vst::int32>(m_count - 1);
			return vst::kResultOk;
		}
		m_points[m_count] = {sampleOffset, value};
		index = static_cast<vst::int32>(m_count++);
		return vst::kResultOk;
	}

	// ---- ParameterChanges ----

	void ParameterChanges::resize(size_t queues, size_t points)
	{
		m_count = 0;
		m_queues.clear();
		m_queues.reserve(queues);
		for (size_t i = 0; i < queues; ++i)
			m_queues.emplace_back(points);
	}

	bool ParameterChanges::add(vst::Vst::ParamID id, vst::int32 offset, vst::Vst::ParamValue value)
	{
		vst::int32 index = 0;
		vst::Vst::IParamValueQueue* queue = addParameterData(id, index);
		return queue != nullptr && queue->addPoint(offset, value, index) == vst::kResultOk;
	}

	vst::Vst::IParamValueQueue* PLUGIN_API ParameterChanges::getParameterData(vst::int32 index)
	{
		if (index < 0 || static_cast<size_t>(index) >= m_count)
			return nullptr;
		return &m_queues[static_cast<size_t>(index)];
	}

	vst::Vst::IParamValueQueue* PLUGIN_API ParameterChanges::addParameterData(
		const vst::Vst::ParamID& id, vst::int32& index)
	{
		for (size_t i = 0; i < m_count; ++i)
		{
			if (m_queues[i].getParameterId() == id)
			{
				index = static_cast<vst::int32>(i);
				return &m_queues[i];
			}
		}
		if (m_count == m_queues.size())
			return nullptr;
		m_queues[m_count].reset(id);
		index = static_cast<vst::int32>(m_count);
		return &m_queues[m_count++];
	}

	// ---- EventList ----

	vst::tresult PLUGIN_API EventList::getEvent(vst::int32 index, vst::Vst::Event& e)
	{
		if (index < 0 || static_cast<size_t>(index) >= m_count)
			return vst::kInvalidArgument;
		e = m_events[static_cast<size_t>(index)];
		return vst::kResultOk;
	}

	vst::tresult PLUGIN_API EventList::addEvent(vst::Vst::Event& e)
	{
		if (m_count == m_events.size())
			return vst::kOutOfMemory;
		m_events[m_count++] = e;
		return vst::kResultOk;
	}

	// ---- ComponentHandler ----

	namespace
	{
		vst::tresult result(bool ok)
		{
			return ok ? vst::kResultOk : vst::kResultFalse;
		}
	}

	vst::tresult PLUGIN_API ComponentHandler::beginEdit(vst::Vst::ParamID id)
	{
		return result(m_listener.editBegin(id));
	}

	vst::tresult PLUGIN_API ComponentHandler::performEdit(vst::Vst::ParamID id, vst::Vst::ParamValue valueNormalized)
	{
		return result(m_listener.editValue(id, valueNormalized));
	}

	vst::tresult PLUGIN_API ComponentHandler::endEdit(vst::Vst::ParamID id)
	{
		return result(m_listener.editEnd(id));
	}

	vst::tresult PLUGIN_API ComponentHandler::restartComponent(vst::int32 flags)
	{
		return result(m_listener.restart(flags));
	}

	vst::tresult PLUGIN_API ComponentHandler::setDirty(vst::TBool state)
	{
		if (state != 0)
			m_listener.stateDirty();
		return vst::kResultOk;
	}

	vst::tresult PLUGIN_API ComponentHandler::requestOpenEditor(vst::FIDString /*name*/)
	{
		return vst::kNotImplemented; // editors are #69
	}

	vst::tresult PLUGIN_API ComponentHandler::notifyUnitSelection(vst::Vst::UnitID /*unitId*/)
	{
		return vst::kResultOk;
	}

	vst::tresult PLUGIN_API ComponentHandler::notifyProgramListChange(
		vst::Vst::ProgramListID /*listId*/, vst::int32 /*programIndex*/)
	{
		m_listener.programListChanged();
		return vst::kResultOk;
	}

	// ---- HostApplication ----

	vst::tresult PLUGIN_API HostApplication::getName(vst::Vst::String128 name)
	{
		return vst::Vst::StringConvert::convert("GNUbase", name) ? vst::kResultOk : vst::kInternalError;
	}
}
