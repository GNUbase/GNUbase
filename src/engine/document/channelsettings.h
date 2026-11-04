// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Copying and pasting channel settings (#112, docs/features/channel-settings.md): channel.copySettings and
// channel.pasteSettings.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	void registerChannelSettingsCommands(ProjectModel& model, Engine& engine);
}
