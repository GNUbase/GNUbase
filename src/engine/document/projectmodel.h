// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "platform/rt/snapshotpublisher.h"

#include <cstdint>
#include <functional>

namespace gnubase::engine
{
	// The project document, its commands and its render snapshots. The commands run on the engine's command thread,
	// the only thread that touches the document; every command that changes it publishes a new render snapshot for
	// the audio thread.
	class ProjectModel
	{
	public:
		ProjectModel();

		void registerCommands(Engine& engine);

		rt::SnapshotPublisher<RenderSnapshot>& snapshots() { return m_snapshots; }

	private:
		using Handler = std::function<Json(Project& project, CommandContext& context)>;

		// Registers a command whose handler checks everything before it changes anything, so a failed command
		// leaves the project as it was. Unless it is a query, a new render snapshot is published after it ran.
		void add(Engine& engine, CommandInfo info, Handler handler);

		Project m_project;
		uint64_t m_revision = 0;
		rt::SnapshotPublisher<RenderSnapshot> m_snapshots;
	};
}
