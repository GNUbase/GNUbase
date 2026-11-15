// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/midi/quantize.h"

#include <optional>
#include <vector>

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	// Audio quantizing (#145, docs/features/audio-quantize.md, reference manual pp. 367-380). quantize.apply and
	// quantize.reset (midi/quantizecommands.cpp) quantize audio events with these functions, on the command thread.

	// Moves an audio event so that its snap point, or without one its start, moves toward the grid as quantizeStart
	// moves a note's start (midi/quantize.h). Quantizing starts from the event's original position while it still is
	// where quantizing put it, unless it is soft; the randomizing depends on the position, so events at the same
	// place, such as the slices of several tracks, move alike. Locked events stay. Returns whether it moved.
	bool quantizeEventStart(const Project& project, AudioEvent& event, const midi::QuantizeGrid& grid);

	// Puts an event back to the position, offset, length and crossfade it had before it was quantized, unless it
	// was moved by other means since or is locked. A piece of a split past the end of its event's original (#1435)
	// has nothing of it and comes back with no length, for the caller to remove.
	bool resetEventQuantize(AudioEvent& event);

	// A selection range, in project samples.
	struct SampleRange
	{
		time::Samples start = 0;
		time::Samples end = 0;
	};

	// Warp quantizing (p. 370): moves the warp markers inside each event (clip frames within it) so that they
	// play on the grid, by time stretch. A clip without markers in the event gets one at each of its hitpoints that
	// count there. For each grid position the marker nearest to it goes toward it; the others keep their place
	// relative to those around them. Quantizing starts from the clip's markers from before it first quantized them,
	// unless it is soft. With a range only the markers inside it move, and the audio outside it stays where it plays
	// (#1579): quantizing starts from the markers as they are, so that another range quantized before stays so.
	// Returns the clips changed.
	std::vector<ObjectId> warpQuantize(Project& project, const std::vector<ObjectId>& events,
		const midi::QuantizeGrid& grid, const std::optional<SampleRange>& range = std::nullopt);

	// Puts back a clip's warp markers from before warp quantizing, while they still are what it made of them.
	bool resetWarpQuantize(Clip& clip);

	// Quantizing several audio tracks (pp. 370-372, 378-380): quantize.slice, quantize.createWarpMarkers and
	// quantize.crossfade, quantize.nudgeCrossfade, and the Reset buttons quantize.resetSlices and
	// quantize.resetWarpMarkers.
	void registerAudioQuantizeCommands(ProjectModel& model, Engine& engine);
}
