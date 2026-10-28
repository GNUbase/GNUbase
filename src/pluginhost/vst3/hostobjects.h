// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "pluginhost/vst3/vst3ns.h"

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// What a VST3 host gives its plugins: streams, parameter changes, event lists, the component handler and the host
// application. The parameter changes and event lists have a fixed capacity, allocated up front, so filling and reading
// them on the audio thread is real-time safe. The adapter owns these objects; their reference counts don't delete them.
namespace gnubase::pluginhost::vst3
{
	// A stream in memory, for component and controller states.
	class MemoryStream final : public vst::U::ImplementsNonDestroyable<vst::U::Directly<vst::IBStream>>
	{
	public:
		MemoryStream() = default;
		MemoryStream(const uint8_t* data, size_t size) : m_data(data, data + size) {}

		const std::vector<uint8_t>& data() const { return m_data; }
		void rewind() { m_position = 0; }

		vst::tresult PLUGIN_API read(void* buffer, vst::int32 numBytes, vst::int32* numBytesRead) override;
		vst::tresult PLUGIN_API write(void* buffer, vst::int32 numBytes, vst::int32* numBytesWritten) override;
		vst::tresult PLUGIN_API seek(vst::int64 pos, vst::int32 mode, vst::int64* result) override;
		vst::tresult PLUGIN_API tell(vst::int64* pos) override;

	private:
		std::vector<uint8_t> m_data;
		size_t m_position = 0;
	};

	// The points of one parameter in one block.
	class ParamValueQueue final : public vst::U::ImplementsNonDestroyable<vst::U::Directly<vst::Vst::IParamValueQueue>>
	{
	public:
		explicit ParamValueQueue(size_t capacity = 0) : m_points(capacity) {}

		void reset(vst::Vst::ParamID id)
		{
			m_id = id;
			m_count = 0;
		}

		vst::Vst::ParamID PLUGIN_API getParameterId() override { return m_id; }
		vst::int32 PLUGIN_API getPointCount() override { return static_cast<vst::int32>(m_count); }
		vst::tresult PLUGIN_API getPoint(
			vst::int32 index, vst::int32& sampleOffset, vst::Vst::ParamValue& value) override;
		vst::tresult PLUGIN_API addPoint(
			vst::int32 sampleOffset, vst::Vst::ParamValue value, vst::int32& index) override;

	private:
		struct Point
		{
			vst::int32 offset = 0;
			vst::Vst::ParamValue value = 0.0;
		};

		vst::Vst::ParamID m_id = 0;
		std::vector<Point> m_points;
		size_t m_count = 0;
	};

	// The parameter changes of one block: up to `queues` parameters with up to `points` points each.
	class ParameterChanges final
		: public vst::U::ImplementsNonDestroyable<vst::U::Directly<vst::Vst::IParameterChanges>>
	{
	public:
		ParameterChanges(size_t queues, size_t points);

		void clear() { m_count = 0; }
		// [audio] Appends a point to the parameter's queue, creating it. False if there is no room.
		bool add(vst::Vst::ParamID id, vst::int32 offset, vst::Vst::ParamValue value);

		vst::int32 PLUGIN_API getParameterCount() override { return static_cast<vst::int32>(m_count); }
		vst::Vst::IParamValueQueue* PLUGIN_API getParameterData(vst::int32 index) override;
		vst::Vst::IParamValueQueue* PLUGIN_API addParameterData(
			const vst::Vst::ParamID& id, vst::int32& index) override;

	private:
		std::vector<ParamValueQueue> m_queues;
		size_t m_count = 0;
	};

	// The events of one block.
	class EventList final : public vst::U::ImplementsNonDestroyable<vst::U::Directly<vst::Vst::IEventList>>
	{
	public:
		explicit EventList(size_t capacity) : m_events(capacity) {}

		void clear() { m_count = 0; }
		size_t size() const { return m_count; }
		const vst::Vst::Event& operator[](size_t index) const { return m_events[index]; }

		vst::int32 PLUGIN_API getEventCount() override { return static_cast<vst::int32>(m_count); }
		vst::tresult PLUGIN_API getEvent(vst::int32 index, vst::Vst::Event& e) override;
		vst::tresult PLUGIN_API addEvent(vst::Vst::Event& e) override;

	private:
		std::vector<vst::Vst::Event> m_events;
		size_t m_count = 0;
	};

	// What the component handler passes on to the adapter, in GNUbase's types.
	class ComponentHandlerListener
	{
	public:
		ComponentHandlerListener() = default;
		ComponentHandlerListener(const ComponentHandlerListener&) = delete;
		ComponentHandlerListener& operator=(const ComponentHandlerListener&) = delete;

		// [main] An edit in the plugin's editor: a gesture begins or ends, or the value changes.
		virtual bool editBegin(uint32_t id) = 0;
		virtual bool editValue(uint32_t id, double normalized) = 0;
		virtual bool editEnd(uint32_t id) = 0;
		// [main] VST3's RestartFlags.
		virtual bool restart(int32_t flags) = 0;
		virtual void stateDirty() = 0;
		virtual void programListChanged() = 0;

	protected:
		~ComponentHandlerListener() = default;
	};

	// The host's side of a controller: edits, restarts, the dirty state and program list changes.
	class ComponentHandler final
		: public vst::U::ImplementsNonDestroyable<
			  vst::U::Directly<vst::Vst::IComponentHandler, vst::Vst::IComponentHandler2, vst::Vst::IUnitHandler>>
	{
	public:
		explicit ComponentHandler(ComponentHandlerListener& listener) : m_listener(listener) {}

		vst::tresult PLUGIN_API beginEdit(vst::Vst::ParamID id) override;
		vst::tresult PLUGIN_API performEdit(vst::Vst::ParamID id, vst::Vst::ParamValue valueNormalized) override;
		vst::tresult PLUGIN_API endEdit(vst::Vst::ParamID id) override;
		vst::tresult PLUGIN_API restartComponent(vst::int32 flags) override;

		vst::tresult PLUGIN_API setDirty(vst::TBool state) override;
		vst::tresult PLUGIN_API requestOpenEditor(vst::FIDString name) override;
		vst::tresult PLUGIN_API startGroupEdit() override { return vst::kResultOk; }
		vst::tresult PLUGIN_API finishGroupEdit() override { return vst::kResultOk; }

		vst::tresult PLUGIN_API notifyUnitSelection(vst::Vst::UnitID unitId) override;
		vst::tresult PLUGIN_API notifyProgramListChange(
			vst::Vst::ProgramListID listId, vst::int32 programIndex) override;

	private:
		ComponentHandlerListener& m_listener;
	};

	// The host application: GNUbase's name, and messages and attribute lists for plugins that talk between their
	// component and controller (the SDK's host classes).
	class HostApplication final : public vst::Vst::HostApplication
	{
	public:
		vst::tresult PLUGIN_API getName(vst::Vst::String128 name) override;
	};
}
