// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <optional>

// A MIDI track's MIDI channel (#1201, docs/features/mixer.md): the channel it plays its events, patch, MIDI volume and
// pan on.
namespace gnubase::engine::midi
{
	// The MIDI channel a MIDI track plays its events on instead of their own (reference manual p. 351), or none for
	// Any, which keeps theirs, or no such track; a drum sound's own channel still wins.
	inline std::optional<int> trackMidiChannel(const Project& project, ObjectId track)
	{
		const auto found = project.trackById.find(track);
		if (found == project.trackById.end() || found->second.type != TrackType::Midi || found->second.channel == 0)
			return std::nullopt;
		return project.channels.at(found->second.channel).midiChannel;
	}
}
