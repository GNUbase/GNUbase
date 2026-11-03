// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// What the MIDI editors show (#126, docs/features/midi-editors.md): the lower zone's editor and the editor window each
// have their parts, active part, part editing mode, Keep Editor Contents and global tracks. View state, like the event
// display's selection: neither in the project nor in undo.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	// editor.get, editor.open, editor.set and editor.close.
	void registerEditorCommands(ProjectModel& model, Engine& engine);
}
