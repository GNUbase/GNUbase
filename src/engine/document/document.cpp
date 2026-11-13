// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/document.h"

#include "engine/midi/drummaps.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <utility>

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

	std::vector<ProjectColor> factoryColors()
	{
		return {{"Red", "#d0453e"}, {"Orange", "#e07a35"}, {"Amber", "#e0a834"}, {"Yellow", "#d8cf3c"},
			{"Lime", "#9ccc3e"}, {"Green", "#4caf50"}, {"Mint", "#3fbf8f"}, {"Teal", "#2fa8a8"}, {"Cyan", "#3ab4d8"},
			{"Sky", "#4a90e0"}, {"Blue", "#4a62d8"}, {"Indigo", "#6a52cc"}, {"Violet", "#9050c8"},
			{"Magenta", "#c04cb4"}, {"Pink", "#d84c86"}, {"Grey", "#8c8c8c"}};
	}

	bool isColor(const std::string& text)
	{
		return text.size() == 7 && text[0] == '#' &&
			std::all_of(
				text.begin() + 1, text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
	}

	std::vector<ObjectId> Project::trackOrder() const
	{
		std::vector<ObjectId> order;
		order.reserve(trackById.size());
		appendTracks(*this, tracks, order);
		return order;
	}

	bool operator==(const TrackVersion& a, const TrackVersion& b)
	{
		const auto sameParts = [](const MidiPart& x, const MidiPart& y)
		{ return sameProperties(x, y) && x.notes == y.notes && x.controllers == y.controllers; };
		return a.id == b.id && a.name == b.name && a.audioEvents == b.audioEvents && a.audioParts == b.audioParts &&
			std::equal(a.midiParts.begin(), a.midiParts.end(), b.midiParts.begin(), b.midiParts.end(), sameParts);
	}

	std::vector<TrackVersion> versionsOf(const Track& track)
	{
		return track.versions.empty() ? std::vector<TrackVersion>(1) : track.versions;
	}

	void renumber(Project& project, MidiPart& part)
	{
		std::map<ObjectId, Note> notes;
		for (auto& [id, note] : part.notes)
		{
			note.id = project.newId();
			notes[note.id] = note;
		}
		part.notes = std::move(notes);
		std::map<ObjectId, Controller> controllers;
		for (auto& [id, controller] : part.controllers)
		{
			controller.id = project.newId();
			controllers[controller.id] = std::move(controller);
		}
		part.controllers = std::move(controllers);
	}

	void detach(Project& project, MidiPart& part)
	{
		if (part.shared == 0)
			return;
		part.shared = 0;
		renumber(project, part);
	}

	Project newProject()
	{
		Project project;
		project.busses.push_back(
			{project.newId(), "Stereo In", BusDirection::Input, ChannelFormat::Stereo, {}, 0, {}, false});
		project.busses.push_back(
			{project.newId(), "Stereo Out", BusDirection::Output, ChannelFormat::Stereo, {}, 0, {}, false});
		keepMainMix(project);
		DrumMap gm = midi::gmDrumMap(); // initially the only drum map (reference manual p. 1239)
		gm.id = project.newId();
		project.drumMaps[gm.id] = std::move(gm);
		return project;
	}

	const LinkGroup* linkGroupOf(const Project& project, ObjectId channel)
	{
		for (const auto& [id, group] : project.linkGroups)
			if (std::find(group.channels.begin(), group.channels.end(), channel) != group.channels.end())
				return &group;
		return nullptr;
	}

	const LinkGroup* controlledBy(const Project& project, ObjectId vca)
	{
		if (vca == 0)
			return nullptr;
		for (const auto& [id, group] : project.linkGroups)
			if (group.vca == vca)
				return &group;
		return nullptr;
	}

	std::vector<ObjectId> vcaChain(const Project& project, ObjectId channel)
	{
		std::vector<ObjectId> chain;
		for (const LinkGroup* group = linkGroupOf(project, channel); group != nullptr && group->vca != 0 &&
			group->vca != channel && std::find(chain.begin(), chain.end(), group->vca) == chain.end();
			group = linkGroupOf(project, group->vca))
			chain.push_back(group->vca);
		return chain;
	}

	time::Ticks longestProjectTicks(const Project& project)
	{
		return project.tempo.toTicks(
			static_cast<double>(time::g_maxPosition) / std::max(project.sampleRate, g_lowestSampleRate));
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

	bool keepMinimumLength(const Project& project, ArrangerEvent& event)
	{
		Marker marker; // a cycle marker has the same rule
		marker.type = MarkerType::Cycle;
		marker.position = event.position;
		marker.end = event.end;
		if (!keepMinimumLength(project, marker))
			return false;
		event.end = marker.end;
		return true;
	}

	const ArrangerChain* activeArrangerChain(const Project& project)
	{
		const std::vector<ArrangerChain>& chains = project.arranger.chains;
		const auto found = std::find_if(chains.begin(), chains.end(),
			[&project](const ArrangerChain& chain) { return chain.id == project.arrangerChain; });
		if (found != chains.end())
			return &*found;
		return chains.empty() ? nullptr : &chains.front();
	}

	const std::vector<std::string>& rootKeyNames()
	{
		static const std::vector<std::string> names = {
			"", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
		return names;
	}

	std::string rootKeyName(int key)
	{
		return key >= 0 && key < 12 ? rootKeyNames().at(static_cast<size_t>(key) + 1) : std::string();
	}

	std::optional<int> rootKeyFromName(const std::string& name)
	{
		const std::vector<std::string>& names = rootKeyNames();
		const auto found = std::find(names.begin(), names.end(), name);
		if (found == names.end())
			return std::nullopt;
		return static_cast<int>(found - names.begin()) - 1;
	}

	int rootKeyTranspose(const Project& project, const MidiPart& part)
	{
		if (part.independent || project.rootKey < 0 || part.rootKey < 0)
			return 0;
		const int semitones = (((project.rootKey - part.rootKey) % 12) + 12) % 12; // 0 to 11 up
		return semitones > 6 ? semitones - 12 : semitones;
	}

	int globalTranspose(const Project& project, const MidiPart& part, time::Samples at)
	{
		if (part.independent)
			return 0;
		int semitones = rootKeyTranspose(project, part);
		const TransposeTrack& track = project.transpose;
		if (track.mute)
			return semitones;
		// ponytail: a scan of the events per note; a song has a handful of them.
		int value = 0;
		for (const TransposeEvent& event : track.events)
		{
			if (time::toSamples(event.position, project.tempo, project.sampleRate) > at)
				break;
			value = event.value;
		}
		semitones += value;
		if (track.keepInOctave)
		{
			while (semitones > 7)
				semitones -= 12;
			while (semitones < -7)
				semitones += 12;
		}
		return semitones;
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

	bool isSendDestination(const Project& project, ObjectId channel)
	{
		const auto found = project.channels.find(channel);
		if (found == project.channels.end())
			return false;
		const TrackType type = project.trackById.at(found->second.track).type;
		return type == TrackType::Group || type == TrackType::Effect || type == TrackType::Folder;
	}

	bool midiSendLoops(const Project& project, ObjectId channel, ObjectId to)
	{
		std::vector<ObjectId> pending{to};
		std::set<ObjectId> seen;
		while (!pending.empty())
		{
			const ObjectId next = pending.back();
			pending.pop_back();
			if (next == channel)
				return true;
			const auto found = project.channels.find(next);
			if (!seen.insert(next).second || found == project.channels.end())
				continue;
			for (const midi::EffectSlot& slot : found->second.midiSends.slots)
				if (slot.output != 0)
					pending.push_back(slot.output);
		}
		return false;
	}

	bool routingLoops(const Project& project, ObjectId channel, ObjectId to)
	{
		std::vector<ObjectId> pending{to};
		std::set<ObjectId> seen;
		while (!pending.empty())
		{
			const ObjectId next = pending.back();
			pending.pop_back();
			if (next == channel)
				return true;
			const auto found = project.channels.find(next);
			if (found == project.channels.end() || !seen.insert(next).second)
				continue;
			pending.push_back(found->second.output);
			for (const Send& send : found->second.sends)
				pending.push_back(send.destination);
		}
		return false;
	}
}
