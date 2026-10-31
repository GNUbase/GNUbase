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
		project.busses.push_back(
			{project.newId(), "Stereo In", BusDirection::Input, ChannelFormat::Stereo, {}, 0, {}, false});
		project.busses.push_back(
			{project.newId(), "Stereo Out", BusDirection::Output, ChannelFormat::Stereo, {}, 0, {}, false});
		keepMainMix(project);
		return project;
	}

	ObjectId mainMix(const Project& project)
	{
		const auto bus = std::find_if(
			project.busses.begin(), project.busses.end(), [](const Bus& candidate) { return candidate.mainMix; });
		return bus != project.busses.end() ? bus->id : 0;
	}

	std::string busProblem(const Project& project, const Bus& bus)
	{
		const int channels = channelCount(bus.format);
		if (channels == 0)
			return "a bus needs audio channels";
		if (bus.parent == 0)
		{
			const auto ports = static_cast<int>(bus.ports.size());
			if (ports != 0 && ports != channels)
				return std::string("a ") + formatName(bus.format) + " bus needs " + std::to_string(channels) +
					(channels == 1 ? " port" : " ports") + ", one per channel";
			if (!bus.parentChannels.empty())
				return "only child busses have parentChannels";
			if (bus.mainMix && bus.direction != BusDirection::Output)
				return "only an output bus can be the main mix";
			return {};
		}
		const auto parent = std::find_if(project.busses.begin(), project.busses.end(),
			[&bus](const Bus& candidate) { return candidate.id == bus.parent; });
		if (parent == project.busses.end() || parent->parent != 0)
			return "a child bus's parent must be a top-level bus";
		if (parent->direction != bus.direction)
			return "a child bus has its parent's direction";
		if (!bus.ports.empty())
			return "a child bus uses its parent's ports";
		if (bus.mainMix)
			return "a child bus can't be the main mix";
		const int parentChannels = channelCount(parent->format);
		if (channels >= parentChannels)
			return "a child bus has fewer channels than its parent";
		std::vector<int> used = bus.parentChannels;
		std::sort(used.begin(), used.end());
		if (static_cast<int>(used.size()) != channels || std::adjacent_find(used.begin(), used.end()) != used.end() ||
			used.front() < 0 || used.back() >= parentChannels)
			return "parentChannels must be " + std::to_string(channels) + " different channels of the parent, 0 to " +
				std::to_string(parentChannels - 1);
		return {};
	}

	void keepMainMix(Project& project)
	{
		if (mainMix(project) != 0)
			return;
		const auto bus = std::find_if(project.busses.begin(), project.busses.end(),
			[](const Bus& candidate) { return candidate.direction == BusDirection::Output && candidate.parent == 0; });
		if (bus != project.busses.end())
			bus->mainMix = true;
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

	bool poolFolderOutside(const Project& project, ObjectId folder, ObjectId excluded)
	{
		// More steps than there are folders means a loop.
		for (size_t steps = 0; steps <= project.poolFolders.size(); ++steps)
		{
			if (folder == 0)
				return true;
			const auto found = project.poolFolders.find(folder);
			if (folder == excluded || found == project.poolFolders.end())
				return false;
			folder = found->second.parent;
		}
		return false;
	}
}
