// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/document/history.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "platform/rt/snapshotpublisher.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gnubase::engine
{
	namespace settings
	{
		class Store;
	}

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

	// What syncSharedParts needs of the parts before a command (eventedits.cpp): copies of the shared ones only. It
	// compares a part with its earlier self only if that was in the same group, so a part that isn't shared yet needn't
	// be copied: one that becomes shared in the command gives no edit to its new group either way.
	std::map<ObjectId, MidiPart> sharedPartsOf(const std::map<ObjectId, MidiPart>& parts);

	// What range.copy and range.cut keep for range.paste (eventedits.cpp): the events, parts and automation of a
	// selection range, cut at its edges, where they were. Not part of the project or of undo.
	struct RangeClipboard
	{
		time::Samples start = 0; // the range, in project samples and in ticks
		time::Samples end = 0;
		time::Ticks startTicks = 0;
		time::Ticks endTicks = 0;
		std::vector<ObjectId> tracks; // the tracks it was copied from, from top to bottom
		std::vector<AudioEvent> audioEvents;
		std::vector<MidiPart> midiParts;
		std::vector<AutomationLane> automation; // the points in the range, with the curve's values at its edges
	};

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
		// The project file it was last saved as or loaded from, absolute; empty for none. Only on the command thread.
		const std::string& file() const { return m_file; }
		// Identifies the open project: the revision at which it was loaded or started (project.revision's project).
		uint64_t openedRevision() const { return m_openedRevision; }

		rt::SnapshotPublisher<RenderSnapshot>& snapshots() { return m_snapshots; }

		// What the plugin.* commands report as a slot's status: the audio engine's (AudioEngine::slotStatus).
		void setSlotStatus(std::function<std::string(ObjectId slot)> status) { m_slotStatus = std::move(status); }
		// A slot of the project as the plugin.* commands and their plugin.changed events describe it, with its index
		// and status; null for no such slot. Only on the command thread.
		Json describeSlot(const Project& project, ObjectId slot) const;

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
		void registerRecordingCommands(Engine& engine); // recording.cpp: recording.add
		Json addRecording(Project& project, CommandContext& context);
		void registerPoolCommands(Engine& engine); // pool.cpp: pool.*, poolFolder.*, clip.set, media management (#93)
		void registerFadeCommands(Engine& engine); // fadeedits.cpp: fades, crossfades and auto fades
		void registerParameterCommands(Engine& engine); // parameters.cpp: parameter.list, parameter.get, parameter.set
		void registerSlotCommands(Engine& engine);      // slotcommands.cpp: plugin.*
		void registerEventEditCommands(Engine& engine); // eventedits.cpp: event.*, parts and events (#85)
		void registerRangeEditCommands(Engine& engine); // eventedits.cpp: range.*, range editing (#88)
		// projectfiles.cpp (#77): save new version, revert, auto save, recent projects and templates.
		void registerProjectFileCommands(Engine& engine);
		// After the project was replaced by a new or loaded one (from file, or none): clears the undo history,
		// announces project.reset, counts as saved and adds the file to the recent projects. An auto save (a .bak in
		// an Auto Saves folder) opens without a file instead, as unsaved and not a recent project.
		void opened(Project project, const std::string& file, CommandContext& context);
		// After the project was saved as file: it is the project's file now and a recent project. Announces
		// project.saved.
		void saved(const std::string& file, CommandContext& context);
		// Makes the file the first of the recent projects.
		void remember(const std::string& file);
		// The project of the template with this name. Throws CommandError if there is none or the name isn't a file
		// name.
		Project loadTemplate(const std::string& name) const;
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
		uint64_t m_savedRevision = 0;     // when it was last saved, loaded or made: unchanged since if m_revision
		uint64_t m_autoSavedRevision = 0; // when it was last saved or auto saved
		uint64_t m_openedRevision = 0;    // when it was loaded or made: openedRevision
		const Clock* m_clock = nullptr;   // the engine's, which auto saves go by
		Clock::TimePoint m_lastSave;      // or auto save, or failed auto save
		bool m_autoSaveFailed = false;    // the last auto save since the project was saved or opened failed
		std::filesystem::path m_settingsDirectory; // the user settings' folder, empty without one
		std::shared_ptr<settings::Store> m_recent; // the recent projects
		bool m_automationFollowsEvents = false;    // a setting, not part of the project
		std::optional<RangeClipboard> m_clipboard; // range.copy and range.cut
		std::function<std::string(ObjectId)> m_slotStatus;
		rt::SnapshotPublisher<RenderSnapshot> m_snapshots;
	};
}
