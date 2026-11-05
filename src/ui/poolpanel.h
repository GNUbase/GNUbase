// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace gnubase::ui
{
	// The pool (#93, docs/features/pool.md): a toolbar, a message line, the info line and a tree of the pool's folders
	// and clips from pool.get, with the columns Media, Used, Status, Info and Path. The names of folders and clips are
	// fields that rename them (poolFolder.set, clip.set) with Enter or when they lose the focus; clips dragged onto a
	// folder's row move there. The toolbar's Import and Find Missing Files run the UI commands pool.import and
	// pool.findMissing through run, which own the file dialogs; its other buttons send engine commands. It follows
	// history.changed and project.saved, whichever client changed the pool, but reads the pool only while shown says
	// it shows: the app calls update in every step, which reads it once it shows again. Use it on the UI thread; the
	// binding must outlive it.
	class PoolPanel final : private Rml::EventListener
	{
	public:
		using Run = std::function<void(const std::string& command)>;

		PoolPanel(EngineBinding& binding, Rml::Element& element, Run run, std::function<bool()> shown);
		~PoolPanel() override;
		PoolPanel(const PoolPanel&) = delete;
		PoolPanel& operator=(const PoolPanel&) = delete;
		PoolPanel(PoolPanel&&) = delete;
		PoolPanel& operator=(PoolPanel&&) = delete;

		// Reads the pool if it changed while the panel didn't show and it shows now.
		void update();

		// Shows in the message line (#pool-message, with the class error for a failure) what a pool command did:
		// pool.removeUnused, media.locate, media.removeMissing or media.conform, wherever in the UI it was sent.
		// media.locate and media.conform start jobs: the line follows the job's progress and shows its result once it
		// is done.
		void report(const std::string& command, const engine::Result& result);

	private:
		void ProcessEvent(Rml::Event& event) override;
		void poolChanged();
		void request();
		void show(const engine::Json& pool);
		void rename(Rml::Element& field);
		void drop(const std::string& folder);
		void addFolder();
		// Sends a command of the panel's own; a failure shows in the message line as the action's.
		void send(const char* action, const std::string& command, const engine::Json& parameters);
		void showMessage(const std::string& text, bool error);
		// A job of a pool command ended (job.finished, or job.get for one that ended before report heard of it).
		void jobEnded(const engine::Json& job);
		std::vector<std::string> selection() const;

		EngineBinding& m_binding;
		Run m_run;
		std::function<bool()> m_shown;
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::ObserverPtr<Rml::Element> m_view; // the <listview>
		engine::Json m_pool = engine::Json::object();
		bool m_stale = true;                      // the pool changed since it was last read
		std::vector<std::string> m_values;        // the rows' values as shown
		std::set<std::string> m_collapsed;        // folders the user closed
		std::vector<std::string> m_dragged;       // clips a mouse button went down on, until it goes up
		Rml::ObserverPtr<Rml::Element> m_pressed; // what it went down on
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
		std::map<uint64_t, std::string> m_jobs; // the running jobs of pool commands it reports, with their commands
		Subscription m_changed;
		Subscription m_saved;
		Subscription m_jobProgress;
		Subscription m_jobFinished;
	};
}
