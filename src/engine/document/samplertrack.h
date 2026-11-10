// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Sampler tracks (#180, docs/features/sampler-track.md): instrument tracks that play the built-in sampler, with the
// pool clip it plays in their channel.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	// sampler.create, sampler.load and sampler.normalize.
	void registerSamplerCommands(ProjectModel& model, Engine& engine);
}
