// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Undo: every command that changes the project records the objects it changed, before and after, as one step in the
// History. Undo and redo put the recorded states back, so they restore the project exactly.
namespace gnubase::engine
{
	// One object before and after a change. Without before it was added; without after it was removed.
	template <typename T> struct ObjectChange
	{
		ObjectId id = 0;
		std::optional<T> before;
		std::optional<T> after;
	};

	// A MIDI part's own properties (with its notes and controllers left out) and its changed notes and controllers,
	// so that editing one note doesn't store the whole part.
	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct MidiPartChange
	{
		ObjectId id = 0;
		std::optional<MidiPart> before;
		std::optional<MidiPart> after;
		std::vector<ObjectChange<Note>> notes;
		std::vector<ObjectChange<Controller>> controllers;
	};

	// An automation lane's own properties (with its points left out) and its changed points.
	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct AutomationLaneChange
	{
		ObjectId id = 0;
		std::optional<AutomationLane> before;
		std::optional<AutomationLane> after;
		std::vector<ObjectChange<AutomationPoint>> points;
	};

	// The parts of a project that are not objects with ids, or keep their order: name, sample rate, tempo, time
	// signatures, auto fades, busses, the top-level tracks and the active marker track.
	struct ProjectSettings
	{
		std::string name;
		double sampleRate = 48000.0;
		time::TempoMap tempo{120.0};
		time::SignatureMap signatures;
		AutoFades autoFades;
		PanLaw panLaw = PanLaw::Minus3;
		double maxVolume = 6.0;
		std::vector<Bus> busses;
		std::vector<ObjectId> tracks;
		ObjectId activeMarkerTrack = 0;
	};

	// How a project changed, object by object. The id counter is left out on purpose: undoing an addition doesn't
	// make its ids available again, so ids stay unique.
	struct ProjectChange
	{
		std::optional<ProjectSettings> settingsBefore; // both set if the settings changed
		std::optional<ProjectSettings> settingsAfter;
		std::vector<ObjectChange<Track>> tracks;
		std::vector<ObjectChange<Channel>> channels;
		std::vector<ObjectChange<MediaFile>> media;
		std::vector<ObjectChange<Clip>> clips;
		std::vector<ObjectChange<PoolFolder>> poolFolders;
		std::vector<ObjectChange<AudioEvent>> audioEvents;
		std::vector<MidiPartChange> midiParts;
		std::vector<ObjectChange<Marker>> markers;
		std::vector<AutomationLaneChange> automation;

		bool empty() const;
	};

	ProjectChange difference(const Project& before, const Project& after);

	enum class Direction : uint8_t
	{
		Undo, // back to the state before
		Redo, // forward to the state after
	};

	void apply(Project& project, const ProjectChange& change, Direction direction);

	struct HistoryStep
	{
		std::string action;  // what was done, e.g. "Add track"
		std::string time;    // when, ISO 8601 in UTC
		std::string details; // notes the user can add
		ProjectChange change;
	};

	// The steps before position() are done; the ones after it were undone and can be redone.
	class History
	{
	public:
		// Adds a done step. Steps that could be redone are discarded, the oldest ones beyond the limit dropped.
		void record(HistoryStep step);

		// Moves the position back and returns the step to undo. Requires position() > 0.
		const HistoryStep& undo();

		// Moves the position forward and returns the step to redo. Requires position() < steps().size().
		const HistoryStep& redo();

		void clear();

		// 0 means no limit. Drops the oldest steps beyond a new limit.
		void setMaxSteps(size_t maxSteps);
		size_t maxSteps() const { return m_maxSteps; }

		size_t position() const { return m_position; }
		const std::vector<HistoryStep>& steps() const { return m_steps; }
		HistoryStep& step(size_t index) { return m_steps.at(index); }

	private:
		void trim();

		std::vector<HistoryStep> m_steps;
		size_t m_position = 0;
		size_t m_maxSteps = 0;
	};
}
