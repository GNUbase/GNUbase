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
			return std::tie(a.name, a.sampleRate, a.tempo, a.busses, a.tracks) ==
				std::tie(b.name, b.sampleRate, b.tempo, b.busses, b.tracks);
		}

		ProjectSettings settings(const Project& project)
		{
			return {project.name, project.sampleRate, project.tempo, project.busses, project.tracks};
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
			return copy;
		}
	}

	bool ProjectChange::empty() const
	{
		return !settingsBefore && tracks.empty() && channels.empty() && media.empty() && clips.empty() &&
			audioEvents.empty() && midiParts.empty();
	}

	ProjectChange difference(const Project& before, const Project& after)
	{
		ProjectChange change;
		ProjectSettings old = settings(before);
		ProjectSettings now = settings(after);
		if (!(old == now))
		{
			change.settingsBefore = std::move(old);
			change.settingsAfter = std::move(now);
		}
		change.tracks = differences(before.trackById, after.trackById);
		change.channels = differences(before.channels, after.channels);
		change.media = differences(before.media, after.media);
		change.clips = differences(before.clips, after.clips);
		change.audioEvents = differences(before.audioEvents, after.audioEvents);

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
		return change;
	}

	void apply(Project& project, const ProjectChange& change, Direction direction)
	{
		if (change.settingsBefore && change.settingsAfter)
		{
			const ProjectSettings& target =
				direction == Direction::Redo ? *change.settingsAfter : *change.settingsBefore;
			project.name = target.name;
			project.sampleRate = target.sampleRate;
			project.tempo = target.tempo;
			project.busses = target.busses;
			project.tracks = target.tracks;
		}
		applyObjects(project.trackById, change.tracks, direction);
		applyObjects(project.channels, change.channels, direction);
		applyObjects(project.media, change.media, direction);
		applyObjects(project.clips, change.clips, direction);
		applyObjects(project.audioEvents, change.audioEvents, direction);
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
			applyObjects(part.notes, partChange.notes, direction);
			applyObjects(part.controllers, partChange.controllers, direction);
		}
	}

	void History::record(HistoryStep step)
	{
		m_steps.erase(m_steps.begin() + static_cast<std::ptrdiff_t>(m_position), m_steps.end());
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
