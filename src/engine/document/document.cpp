// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/document.h"

#include <algorithm>

namespace gnubase::engine
{
	namespace
	{
		void appendTracks(const Project& project, const std::vector<ObjectId>& tracks, std::vector<ObjectId>& order)
		{
			for (const ObjectId id : tracks)
			{
				order.push_back(id);
				appendTracks(project, project.trackById.at(id).children, order);
			}
		}
	}

	std::vector<ObjectId> Project::trackOrder() const
	{
		std::vector<ObjectId> order;
		order.reserve(trackById.size());
		appendTracks(*this, tracks, order);
		return order;
	}

	Project newProject()
	{
		Project project;
		project.busses.push_back({project.newId(), "Stereo In", BusDirection::Input, 2});
		project.busses.push_back({project.newId(), "Stereo Out", BusDirection::Output, 2});
		return project;
	}

	bool hasMinimumLength(const Project& project, const Marker& marker)
	{
		return marker.type == MarkerType::Position ||
			time::toSamples(marker.end, project.tempo, project.sampleRate) >
			time::toSamples(marker.position, project.tempo, project.sampleRate);
	}

	bool keepMinimumLength(const Project& project, Marker& marker)
	{
		if (hasMinimumLength(project, marker))
			return false;
		const time::Samples start = time::toSamples(marker.position, project.tempo, project.sampleRate);
		const time::Position end =
			time::rebase({time::TimeBase::Linear, start + 1}, marker.position.base, project.tempo, project.sampleRate);
		marker.end = {marker.position.base, std::max(end.value, marker.position.value + 1)};
		// Ticks round to the nearest one, which can still be in the same sample.
		while (!hasMinimumLength(project, marker))
			++marker.end.value;
		return true;
	}
}
