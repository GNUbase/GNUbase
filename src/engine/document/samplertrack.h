// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/engine.h"

#include <string>

// Sampler tracks (#180, docs/features/sampler-track.md): instrument tracks that play the built-in sampler, with the
// pool clip it plays in their channel.
namespace gnubase::engine
{
	class ProjectModel;

	// sampler.create, sampler.load and sampler.normalize.
	void registerSamplerCommands(ProjectModel& model, Engine& engine);

	// Sampler 01, Sampler 02 and so on: prefix and the first number that no track's name has.
	std::string numberedTrackName(const Project& project, const std::string& prefix);

	// The clip that a command's parameters name with clip or event, and the section of it to play in its frames:
	// start and end if given, else the event's or the whole clip. Throws CommandError for anything else.
	struct SampleSection
	{
		ObjectId clip = 0;
		time::Samples start = 0;
		time::Samples end = 0;
		time::Samples length = 0; // the clip's
	};
	SampleSection sampleSection(const Project& project, const Json& parameters);

	// The parameters with a file (path) put into the pool as a clip (Import Audio File, or a file dropped on the
	// sampler panel or a drum pad, reference manual pp. 710, 713, 944): clip in place of path, in the same undo step.
	Json withSampleClip(ProjectModel& model, Project& project, const Json& parameters, CommandContext& context);
}
