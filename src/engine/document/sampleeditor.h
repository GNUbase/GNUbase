// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The sample editor (#140, docs/features/sample-editor.md): which audio event it shows and its selection range, view
// state like the MIDI editors' (neither in the project nor in undo), and the regions of clips, which are in the
// project.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	// sampleEditor.get, sampleEditor.open, sampleEditor.set, sampleEditor.close, clip.addRegion, clip.setRegion and
	// clip.removeRegion.
	void registerSampleEditorCommands(ProjectModel& model, Engine& engine);
}
