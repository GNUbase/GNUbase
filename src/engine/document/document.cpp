// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/document.h"

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
}
