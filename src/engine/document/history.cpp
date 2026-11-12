// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/history.h"

#include "platform/log.h"

#include <algorithm>
#include <iterator>
#include <tuple>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		bool operator==(const ProjectSettings& a, const ProjectSettings& b)
		{
			return std::tie(a.name, a.sampleRate, a.tempo, a.tempoTrackActive, a.otherTempo, a.signatures, a.autoFades,
					   a.panLaw, a.maxVolume, a.colors, a.setup, a.busses, a.tracks, a.activeMarkerTrack, a.arranger,
					   a.rootKey, a.transpose, a.chords, a.chordPads, a.inputTransformer) ==
				std::tie(b.name, b.sampleRate, b.tempo, b.tempoTrackActive, b.otherTempo, b.signatures, b.autoFades,
					b.panLaw, b.maxVolume, b.colors, b.setup, b.busses, b.tracks, b.activeMarkerTrack, b.arranger,
					b.rootKey, b.transpose, b.chords, b.chordPads, b.inputTransformer);
		}

		ProjectSettings settings(const Project& project)
		{
			ProjectSettings result{project.name, project.sampleRate, project.tempo, project.tempoTrackActive,
				project.otherTempo, project.signatures, project.autoFades, project.panLaw, project.maxVolume,
				project.colors, project.setup, project.busses, project.tracks, project.activeMarkerTrack,
				project.arranger, project.rootKey, project.transpose, project.chords, project.chordPads,
				project.inputTransformer};
			// A display choice must survive replaying other settings while merging or joining history steps.
			result.setup.timeFormat.clear();
			return result;
		}

		// Walks two maps in id order and calls visit(id, before, after) with nullptr for a missing object.
		template <typename T, typename Visit>
		void compare(const std::map<ObjectId, T>& before, const std::map<ObjectId, T>& after, Visit visit)
		{
			auto b = before.begin();
			auto a = after.begin();
			while (b != before.end() || a != after.end())
			{
				if (a == after.end() || (b != before.end() && b->first < a->first))
				{
					visit(b->first, &b->second, nullptr);
					++b;
				}
				else if (b == before.end() || a->first < b->first)
				{
					visit(a->first, nullptr, &a->second);
					++a;
				}
				else
				{
					visit(b->first, &b->second, &a->second);
					++b;
					++a;
				}
			}
		}

		template <typename T> std::optional<T> optional(const T* object)
		{
			return object != nullptr ? std::optional<T>(*object) : std::nullopt;
		}

		template <typename T> std::vector<ObjectChange<T>> differences(
			const std::map<ObjectId, T>& before, const std::map<ObjectId, T>& after)
		{
			std::vector<ObjectChange<T>> changes;
			compare(before, after,
				[&changes](ObjectId id, const T* old, const T* now)
				{
					if (old == nullptr || now == nullptr || !(*old == *now))
						changes.push_back({id, optional(old), optional(now)});
				});
			return changes;
		}

		template <typename T> void applyObjects(
			std::map<ObjectId, T>& objects, const std::vector<ObjectChange<T>>& changes, Direction direction)
		{
			for (const ObjectChange<T>& change : changes)
			{
				const std::optional<T>& target = direction == Direction::Redo ? change.after : change.before;
				if (target)
					objects[change.id] = *target;
				else
					objects.erase(change.id);
			}
		}

		// A part without its notes and controllers.
		std::optional<MidiPart> properties(const MidiPart* part)
		{
			if (part == nullptr)
				return std::nullopt;
			MidiPart copy;
			copy.id = part->id;
			copy.track = part->track;
			copy.name = part->name;
			copy.position = part->position;
			copy.length = part->length;
			copy.mute = part->mute;
			copy.lock = part->lock;
			copy.shared = part->shared;
			copy.color = part->color;
			copy.lane = part->lane;
			copy.transpose = part->transpose;
			copy.velocity = part->velocity;
			copy.rootKey = part->rootKey;
			copy.independent = part->independent;
			copy.pattern = part->pattern;
			copy.patternOffset = part->patternOffset;
			return copy;
		}

		// A lane without its points.
		std::optional<AutomationLane> properties(const AutomationLane* lane)
		{
			if (lane == nullptr)
				return std::nullopt;
			AutomationLane copy;
			copy.id = lane->id;
			copy.track = lane->track;
			copy.target = lane->target;
			copy.read = lane->read;
			return copy;
		}
	}

	bool ProjectChange::empty() const
	{
		return !settingsBefore && !timeFormatBefore && tracks.empty() && channels.empty() && slots.empty() &&
			media.empty() && clips.empty() && poolFolders.empty() && audioEvents.empty() && audioParts.empty() &&
			midiParts.empty() && markers.empty() && automation.empty() && linkGroups.empty() && drumMaps.empty() &&
			articulationMaps.empty() && patterns.empty();
	}

	ProjectChange difference(const Project& before, const Project& after)
	{
		ProjectChange change;
		if (before.setup.timeFormat != after.setup.timeFormat)
		{
			change.timeFormatBefore = before.setup.timeFormat;
			change.timeFormatAfter = after.setup.timeFormat;
		}
		ProjectSettings old = settings(before);
		ProjectSettings now = settings(after);
		if (!(old == now))
		{
			change.settingsBefore = std::move(old);
			change.settingsAfter = std::move(now);
		}
		change.tracks = differences(before.trackById, after.trackById);
		change.channels = differences(before.channels, after.channels);
		change.slots = differences(before.slots, after.slots);
		change.media = differences(before.media, after.media);
		change.clips = differences(before.clips, after.clips);
		change.poolFolders = differences(before.poolFolders, after.poolFolders);
		change.audioEvents = differences(before.audioEvents, after.audioEvents);
		change.audioParts = differences(before.audioParts, after.audioParts);
		change.markers = differences(before.markers, after.markers);
		change.linkGroups = differences(before.linkGroups, after.linkGroups);
		change.drumMaps = differences(before.drumMaps, after.drumMaps);
		change.articulationMaps = differences(before.articulationMaps, after.articulationMaps);
		change.patterns = differences(before.patterns, after.patterns);

		const MidiPart empty;
		compare(before.midiParts, after.midiParts,
			[&change, &empty](ObjectId id, const MidiPart* old, const MidiPart* now)
			{
				const MidiPart& from = old != nullptr ? *old : empty;
				const MidiPart& to = now != nullptr ? *now : empty;
				MidiPartChange part{id, properties(old), properties(now), differences(from.notes, to.notes),
					differences(from.controllers, to.controllers)};
				const bool propertiesChanged = old == nullptr || now == nullptr || !sameProperties(*old, *now);
				if (propertiesChanged || !part.notes.empty() || !part.controllers.empty())
					change.midiParts.push_back(std::move(part));
			});

		const AutomationLane noLane;
		compare(before.automation, after.automation,
			[&change, &noLane](ObjectId id, const AutomationLane* old, const AutomationLane* now)
			{
				const AutomationLane& from = old != nullptr ? *old : noLane;
				const AutomationLane& to = now != nullptr ? *now : noLane;
				AutomationLaneChange lane{id, properties(old), properties(now), differences(from.points, to.points)};
				if (old == nullptr || now == nullptr || !sameProperties(*old, *now) || !lane.points.empty())
					change.automation.push_back(std::move(lane));
			});
		return change;
	}

	void apply(Project& project, const ProjectChange& change, Direction direction)
	{
		if (change.timeFormatBefore && change.timeFormatAfter)
			project.setup.timeFormat =
				direction == Direction::Redo ? *change.timeFormatAfter : *change.timeFormatBefore;
		if (change.settingsBefore && change.settingsAfter)
		{
			const ProjectSettings& target =
				direction == Direction::Redo ? *change.settingsAfter : *change.settingsBefore;
			project.name = target.name;
			project.sampleRate = target.sampleRate;
			project.tempo = target.tempo;
			project.tempoTrackActive = target.tempoTrackActive;
			project.otherTempo = target.otherTempo;
			project.signatures = target.signatures;
			project.autoFades = target.autoFades;
			project.panLaw = target.panLaw;
			project.maxVolume = target.maxVolume;
			project.colors = target.colors;
			ProjectSetup setup = target.setup;
			setup.timeFormat = project.setup.timeFormat;
			project.setup = std::move(setup);
			project.busses = target.busses;
			project.tracks = target.tracks;
			project.activeMarkerTrack = target.activeMarkerTrack;
			project.arranger = target.arranger;
			project.rootKey = target.rootKey;
			project.transpose = target.transpose;
			project.chords = target.chords;
			project.chordPads = target.chordPads;
			project.inputTransformer = target.inputTransformer;
		}
		applyObjects(project.trackById, change.tracks, direction);
		applyObjects(project.channels, change.channels, direction);
		applyObjects(project.slots, change.slots, direction);
		applyObjects(project.media, change.media, direction);
		applyObjects(project.clips, change.clips, direction);
		applyObjects(project.poolFolders, change.poolFolders, direction);
		applyObjects(project.audioEvents, change.audioEvents, direction);
		applyObjects(project.audioParts, change.audioParts, direction);
		applyObjects(project.markers, change.markers, direction);
		applyObjects(project.linkGroups, change.linkGroups, direction);
		applyObjects(project.drumMaps, change.drumMaps, direction);
		applyObjects(project.articulationMaps, change.articulationMaps, direction);
		applyObjects(project.patterns, change.patterns, direction);
		for (const MidiPartChange& partChange : change.midiParts)
		{
			const std::optional<MidiPart>& target = direction == Direction::Redo ? partChange.after : partChange.before;
			if (!target)
			{
				project.midiParts.erase(partChange.id);
				continue;
			}
			MidiPart& part = project.midiParts[partChange.id];
			part.id = target->id;
			part.track = target->track;
			part.name = target->name;
			part.position = target->position;
			part.length = target->length;
			part.mute = target->mute;
			part.lock = target->lock;
			part.shared = target->shared;
			part.color = target->color;
			part.lane = target->lane;
			part.transpose = target->transpose;
			part.velocity = target->velocity;
			part.rootKey = target->rootKey;
			part.independent = target->independent;
			part.pattern = target->pattern;
			part.patternOffset = target->patternOffset;
			applyObjects(part.notes, partChange.notes, direction);
			applyObjects(part.controllers, partChange.controllers, direction);
		}
		for (const AutomationLaneChange& laneChange : change.automation)
		{
			const std::optional<AutomationLane>& target =
				direction == Direction::Redo ? laneChange.after : laneChange.before;
			if (!target)
			{
				project.automation.erase(laneChange.id);
				continue;
			}
			AutomationLane& lane = project.automation[laneChange.id];
			lane.id = target->id;
			lane.track = target->track;
			lane.target = target->target;
			lane.read = target->read;
			applyObjects(lane.points, laneChange.points, direction);
		}
	}

	void History::record(HistoryStep step)
	{
		m_steps.erase(m_steps.begin() + static_cast<std::ptrdiff_t>(m_position), m_steps.end());
		step.id = ++m_lastId;
		m_steps.push_back(std::move(step));
		m_position = m_steps.size();
		trim();
	}

	const HistoryStep& History::undo()
	{
		GNUBASE_ASSERT(m_position > 0);
		return m_steps[--m_position];
	}

	const HistoryStep& History::redo()
	{
		GNUBASE_ASSERT(m_position < m_steps.size());
		return m_steps[m_position++];
	}

	void History::removeLast()
	{
		GNUBASE_ASSERT(!m_steps.empty());
		GNUBASE_ASSERT(m_position == m_steps.size());
		m_steps.pop_back();
		m_position = m_steps.size();
	}

	void History::join(size_t first, std::optional<HistoryStep> step)
	{
		GNUBASE_ASSERT(first <= m_position);
		const auto begin = m_steps.begin() + static_cast<std::ptrdiff_t>(first);
		const auto end = m_steps.begin() + static_cast<std::ptrdiff_t>(m_position);
		if (step)
			step->id = first < m_position ? std::prev(end)->id : ++m_lastId;
		m_steps.erase(begin, end);
		m_position = first;
		if (step)
		{
			m_steps.insert(m_steps.begin() + static_cast<std::ptrdiff_t>(first), std::move(*step));
			++m_position;
		}
	}

	void History::clear()
	{
		m_steps.clear();
		m_position = 0;
	}

	void History::setMaxSteps(size_t maxSteps)
	{
		m_maxSteps = maxSteps;
		trim();
	}

	void History::trim()
	{
		if (m_maxSteps == 0 || m_steps.size() <= m_maxSteps)
			return;
		// Drop the oldest done steps first; steps that can be redone go only when nothing done is left.
		const size_t excess = m_steps.size() - m_maxSteps;
		const size_t dropped = std::min(excess, m_position);
		m_steps.erase(m_steps.begin(), m_steps.begin() + static_cast<std::ptrdiff_t>(dropped));
		m_position -= dropped;
		if (m_steps.size() > m_maxSteps)
			m_steps.resize(m_maxSteps);
	}
}
