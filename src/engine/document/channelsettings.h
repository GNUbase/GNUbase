// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <vector>

// Copying and pasting channel settings (#112, docs/features/channel-settings.md): channel.copySettings and
// channel.pasteSettings.
namespace gnubase::engine
{
	class CommandContext;
	class Engine;
	class ProjectModel;

	// Replaces an audio channel's inserts by copies of slots, with new ids: the automation of the plugins it had goes
	// with them (automationLane.removed, plugin.removed). The caller announces the channel and the new slots.
	void replaceSlots(Project& project, CommandContext& context, ObjectId id, const std::vector<Slot>& slots);

	void registerChannelSettingsCommands(ProjectModel& model, Engine& engine);
}
