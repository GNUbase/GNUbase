// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/dsp/warp.h"

#include <vector>

// Tempo matching and warping (#144, docs/features/warp.md): musical mode, a clip's definition grid (Auto Adjust and
// Manual Adjust), free warp markers, and where that makes an audio event's audio play.
namespace gnubase::engine
{
	// Whether a clip's audio plays warped: in musical mode, or with free warp markers.
	bool warped(const Clip& clip);

	// Sorts a clip's free warp markers and throws a CommandError unless each is within the clip, none passes another
	// and no section between two of them, or one and the clip's ends, plays more than 4 times or less than a quarter
	// as long, the real-time stretcher's range (dsp::WarpReader). length is the clip's in sample frames.
	void checkMarkers(ClipWarp& warp, time::Samples length);

	// The clip's tempo in musical mode: its own, or without one the project's at the start of the first event that
	// plays it (at the project start without one).
	double definitionTempo(const Project& project, const Clip& clip);

	// How many project samples after an audio event's start the clip frame plays: through the clip's warp markers,
	// and in musical mode through its definition and the project's tempo. For frames within the event.
	double warpedOffset(const Project& project, const AudioEvent& event, double frame);

	// The inverse of warpedOffset: the clip frame that plays offset project samples after the event's start.
	double warpedFrameAt(const Project& project, const AudioEvent& event, double offset);

	// The warp map an audio event of a warped clip plays by: its clip frames from offset to offset + length, to frames
	// at the clip's sample rate from the event's start, a point every 4096 clip frames and at every warp marker and
	// edited beat.
	std::vector<dsp::WarpPoint> warpMap(const Project& project, const AudioEvent& event);

	// Where an event that plays by a warp map plays its clip's start and end and the inner ends of the clip's fades,
	// in frames from the event's start at the clip's sample rate: through the map, and beyond it with the slope of its
	// end segments. Playback fades the clip in and out there, and flattening renders those fades.
	// ponytail: before and after the event the map's end slopes stand in for the warp, which may differ there.
	struct WarpedClipEdges
	{
		time::Samples start = 0;
		time::Samples fadeInEnd = 0;
		time::Samples fadeOutStart = 0;
		time::Samples end = 0;
	};
	WarpedClipEdges warpedClipEdges(const Clip& clip, time::Samples length, const std::vector<dsp::WarpPoint>& map);
}
