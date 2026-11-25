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
	// and clips from pool.get, with the columns Media, Used, Status, Key, Info and Path. The toolbar's View/Attributes
	// menu shows and hides the columns but Media (the user setting ui.poolColumns; the Key column only once its Root
	// Key turns it on, reference manual p. 417). The names of folders and clips are fields that rename them
	// (poolFolder.set, clip.set) with Enter or when they lose the focus; clips dragged onto a folder's row move there.
	// The toolbar's Import and Find Missing Files run the UI commands pool.import and pool.findMissing through run,
	// which own the file dialogs; its other buttons send engine commands. It follows history.changed and project.saved,
	// whichever client changed the pool, but reads the pool only while shown says it shows: the app calls update in
	// every step, which reads it once it shows again. Use it on the UI thread; the binding must outlive it.
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
		// Selected clip ids for the audio functions, excluding folder rows.
		std::string analysisClips() const;

		// Shows in the message line (#pool-message, with the class error for a failure) what a pool command did:
		// pool.removeUnused, media.locate, media.removeMissing, media.conform or media.prepareArchive, wherever in the
		// UI it was sent, or media.cleanup, which the panel sends itself. media.locate, media.conform and
		// media.prepareArchive start jobs: the line follows the job's progress and shows its result once it is done.
		// While one runs, the line's row (.pool-status) has the class running, which shows Stop (#pool-stop), and Stop
		// cancels it (reference manual p. 751).
		void report(const std::string& command, const engine::Result& result);

		// Media > Prepare Archive (pool.prepareArchive): asks in the panel's window before media.prepareArchive copies
		// files from outside the project folder into it (prepare-archive.rml, reference manual p. 135), and sends it
		// at once if there are none, or if the project has no file, which the engine refuses.
		void prepareArchive();

		// Media > Clean Up Media Folders (pool.cleanup, #1827): lists the files media.cleanup finds unused in
		// cleanup.rml, which asks before media.cleanup removes them from the disk, or says there are none.
		void cleanup();

	private:
		void ProcessEvent(Rml::Event& event) override;
		void poolChanged();
		void request();
		void show(const engine::Json& pool);
		void rename(Rml::Element& field);
		// The Key column's change of a clip's row: clip.set rootKey, unless the clip shows that key.
		void setKey(const std::string& row, const std::string& key);
		// Gives a clip's Key list all its choices, when it gets the focus.
		void fillKey(Rml::Element& list);
		// Shows the columns the user setting ui.poolColumns shows (the root's state classes hide-used and so on) and
		// checks their items in the View/Attributes menu.
		void showColumns(const engine::Json& columns);
		// The View/Attributes menu's item of a column turned it on or off: ui.poolColumns.
		void setColumn(const std::string& column, bool shown);
		void drop(const std::string& folder);
		void addFolder();
		// Sends a command of the panel's own; a failure shows in the message line as the action's.
		void send(const char* action, const std::string& command, const engine::Json& parameters);
		void showMessage(const std::string& text, bool error);
		// Opens prepare-archive.rml, which asks whether to copy the external files that are there, as many as
		// external, and says how many others are missing.
		void askPrepareArchive(int external, int missing);
		// Opens the dialog file from the folder of the panel's document, modal, in the panel's window; proceed runs if
		// it closes with the result ok. Null if it is asking already or the dialog can't open.
		Rml::ElementDocument* ask(const char* file, std::function<void()> proceed);
		// A job of a pool command ended (job.finished, or job.get for one that ended before report heard of it).
		void jobEnded(const engine::Json& job);
		// Cancels the jobs of pool commands that run.
		void stop();
		// Marks the message line's row running while a job of a pool command runs.
		void showRunning();
		std::vector<std::string> selection() const;

		EngineBinding& m_binding;
		Run m_run;
		std::function<bool()> m_shown;
		Rml::ObserverPtr<Rml::Element> m_root;
		const std::string m_closeEvent;
		const std::string m_treeEvent;
		Rml::ObserverPtr<Rml::Element> m_view;       // the <listview>
		Rml::ObserverPtr<Rml::Element> m_attributes; // the View/Attributes menu, in the document's body
		Rml::ObserverPtr<Rml::Element> m_analysis;
		engine::Json m_columns = engine::Json::object(); // which columns show, every one of ui.poolColumns
		engine::Json m_pool = engine::Json::object();
		bool m_stale = true;                       // the pool changed since it was last read
		std::vector<std::string> m_values;         // the rows' values as shown
		std::set<std::string> m_collapsed;         // folders the user closed
		std::vector<std::string> m_dragged;        // clips a mouse button went down on, until it goes up
		Rml::ObserverPtr<Rml::Element> m_pressed;  // what it went down on
		Rml::ObserverPtr<Rml::Element> m_question; // the dialog of ask while it asks
		std::function<void()> m_proceed;           // what its ok does
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
		std::map<uint64_t, std::string> m_jobs; // the running jobs of pool commands it reports, with their commands
		LevelSteps m_levelSteps;                // whether a step changed only channels' levels (#1881)
		Subscription m_changed;
		Subscription m_channelChanged;
		Subscription m_saved;
		Subscription m_settings;
		Subscription m_jobProgress;
		Subscription m_jobFinished;
	};
}
