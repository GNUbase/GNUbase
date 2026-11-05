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

	// The clip's tempo in musical mode: its own, or without one the project's at the start of the first event that
	// plays it (at the project start without one).
	double definitionTempo(const Project& project, const Clip& clip);

	// How many project samples after an audio event's start the clip frame plays: through the clip's warp markers,
	// and in musical mode through its definition and the project's tempo. For frames within the event.
	double warpedOffset(const Project& project, const AudioEvent& event, double frame);

	// The warp map an audio event of a warped clip plays by: its clip frames from offset to offset + length, to frames
	// at the clip's sample rate from the event's start, a point every 4096 clip frames and at every warp marker and
	// edited beat.
	std::vector<dsp::WarpPoint> warpMap(const Project& project, const AudioEvent& event);
}
