// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

// Render in place and freeze (docs/features/render-in-place.md); the commands are registered by the project model
// (ProjectModel::registerRenderCommands).
namespace gnubase::engine
{
	// Whether a freeze keeps a channel's monitor and record enable buttons off: a frozen track's channel, whose input
	// would play past its freeze file, and a MIDI channel that plays a frozen instrument track, whose notes the freeze
	// file plays instead (reference manual p. 220: a frozen track's controls are locked).
	bool lockedByFreeze(const Project& project, const Channel& channel);
}
