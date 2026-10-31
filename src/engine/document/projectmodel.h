// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/document/history.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "platform/rt/snapshotpublisher.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace gnubase::engine
{
	// A controller event from controller.add or controller.set, checked against its type: what the type uses must be
	// there and valid, the rest is left zero (controllerFromJson). Its id is left to the caller. Throws CommandError
	// with messages that start with name, e.g. "controllers[0]".
	Controller controllerFrom(const Json& item, const std::string& name);

	// Automation follows events (projectmodel.cpp): moves the automation of a track from where an event or part was
	// (from to end) to where it is now (to), if automation follows events.
	void followAutomation(Project& project, ObjectId track, const time::Position& from, const time::Position& end,
		const time::Position& to, CommandContext& context);

	// After a command, and after each command of a transaction (eventedits.cpp): the first shared copy of a MIDI part
	// whose notes or controllers changed since before gives them to its other copies, and a copy without others is no
	// longer shared.
	void syncSharedParts(const std::map<ObjectId, MidiPart>& before, Project& project, CommandContext& context);

	// The project document, its commands, its undo history and its render snapshots. The commands run on the engine's
	// command thread, the only thread that touches the document; every command that changes it records an undo step
	// and publishes a new render snapshot for the audio thread.
	class Jobs;

	class ProjectModel
	{
	public:
		using Handler = std::function<Json(Project& project, CommandContext& context)>;

		ProjectModel();

		// Backups run as jobs.
		void registerCommands(Engine& engine, Jobs& jobs);

		// Registers a command defined in another file, such as quantize.* (#132). A command that changes the project
		// is one undo step named action, like the commands here, and transactions can run it.
		void addEdit(Engine& engine, CommandInfo info, std::string action, Handler handler);

		// The project, for commands defined in other files that read it. Only on the command thread.
		const Project& project() const { return m_project; }

		rt::SnapshotPublisher<RenderSnapshot>& snapshots() { return m_snapshots; }

	private:
		// Registers a query, or a command that changes the project. Such a command runs on the project, and if it
		// fails the project is restored from a copy, so a failed command changes nothing. If it succeeds, what it
		// changed becomes an undo step, named after the command (see g_actions in the .cpp) or, for a
		// transaction, after its description, and a new render snapshot is published.
		void add(Engine& engine, CommandInfo info, Handler handler);

		// Registers a command that manages the project or its history and records no undo step.
		static void addControl(Engine& engine, CommandInfo info, std::function<Json(CommandContext& context)> handler);

		void registerAutomationCommands(Engine& engine);
		void registerHistoryCommands(Engine& engine);
		void registerMediaFolderCommands(Engine& engine, Jobs& jobs); // project.backup, media.prepareArchive and more
		void registerMidiEditCommands(Engine& engine);                // midiedits.cpp: note.set and controller.set
		void registerMarkerCommands(Engine& engine);                  // markers.cpp: marker.*
		void registerMidiFileCommands(Engine& engine); // midifilecommands.cpp: midiFile.export and midiFile.import
		Json importMidiFile(Project& project, CommandContext& context);
		void registerAudioFileCommands(Engine& engine, Jobs& jobs); // audiofilecommands.cpp: audioFile.import
		Json importAudioFiles(Project& project, CommandContext& context, Jobs& jobs);
		void registerPoolCommands(Engine& engine); // pool.cpp: pool.*, poolFolder.*, clip.set, media management (#93)
		void registerFadeCommands(Engine& engine); // fadeedits.cpp: fades, crossfades and auto fades
		void registerParameterCommands(Engine& engine); // parameters.cpp: parameter.list, parameter.get, parameter.set
		void registerEventEditCommands(Engine& engine); // eventedits.cpp: event.*, parts and events (#85)
		void record(std::string action, const Project& before, CommandContext& context);
		Json undoOrRedo(CommandContext& context, Direction direction);
		Json transaction(Project& project, CommandContext& context);
		void announceHistory(CommandContext& context) const;
		void publish();

		struct Edit
		{
			CommandInfo info;
			Handler handler;
		};

		Project m_project;
		std::string m_file; // the project file it was last saved as or loaded from, absolute; empty for none
		History m_history;
		std::map<std::string, Edit> m_edits; // the commands a transaction can run
		uint64_t m_revision = 0;
		bool m_automationFollowsEvents = false; // a setting, not part of the project
		rt::SnapshotPublisher<RenderSnapshot> m_snapshots;
	};
}
