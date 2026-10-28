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
	// The project document, its commands, its undo history and its render snapshots. The commands run on the engine's
	// command thread, the only thread that touches the document; every command that changes it records an undo step
	// and publishes a new render snapshot for the audio thread.
	class ProjectModel
	{
	public:
		ProjectModel();

		void registerCommands(Engine& engine);

		rt::SnapshotPublisher<RenderSnapshot>& snapshots() { return m_snapshots; }

	private:
		using Handler = std::function<Json(Project& project, CommandContext& context)>;

		// Registers a query, or a command that changes the project. Such a command runs on the project, and if it
		// fails the project is restored from a copy, so a failed command changes nothing. If it succeeds, what it
		// changed becomes an undo step, named after the command (see g_actions in the .cpp) or, for a
		// transaction, after its description, and a new render snapshot is published.
		void add(Engine& engine, CommandInfo info, Handler handler);

		// Registers a command that manages the project or its history and records no undo step.
		static void addControl(Engine& engine, CommandInfo info, std::function<Json(CommandContext& context)> handler);

		void registerHistoryCommands(Engine& engine);
		void registerMidiEditCommands(Engine& engine); // midiedits.cpp: note.set and controller.set
		void registerMarkerCommands(Engine& engine);   // markers.cpp: marker.*
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
		History m_history;
		std::map<std::string, Edit> m_edits; // the commands a transaction can run
		uint64_t m_revision = 0;
		rt::SnapshotPublisher<RenderSnapshot> m_snapshots;
	};
}
