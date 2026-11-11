// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/processors/sampler.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace gnubase::engine
{
	// Reads the samples of sampler tracks on a thread of its own (#1122), so that a long file holds up neither the
	// command thread nor a project load. The command thread asks for files and takes the samples read; the loader
	// calls back on its thread after each file, e.g. to have the command thread hand it to the samplers.
	class SampleLoader
	{
	public:
		using Read = std::function<std::shared_ptr<const processors::Sample>(const std::string& path)>;

		// loaded runs on the loader's thread after each file is read; read reads one (processors::sharedSample).
		explicit SampleLoader(std::function<void()> loaded = {}, Read read = processors::sharedSample);

		// Waits for the file it reads, if any, and lets the others go unread.
		~SampleLoader();
		SampleLoader(const SampleLoader&) = delete;
		SampleLoader& operator=(const SampleLoader&) = delete;
		SampleLoader(SampleLoader&&) = delete;
		SampleLoader& operator=(SampleLoader&&) = delete;

		// Reads the file at path after those asked for before, unless it is waiting or read already.
		void request(const std::string& path);

		// The sample read from path, null until it is.
		std::shared_ptr<const processors::Sample> sample(const std::string& path) const;

		// Lets go of the samples read, except those of paths, and forgets the files waiting to be read that aren't
		// among them.
		void keepOnly(const std::set<std::string>& paths);

		// Waits until every file asked for is read and its callback has run, at most timeout; false if one isn't.
		bool waitUntilRead(std::chrono::milliseconds timeout);

	private:
		void run();

		const std::function<void()> m_loaded;
		const Read m_read;
		mutable std::mutex m_mutex;
		std::condition_variable m_wake;  // a file asked for, or stop
		std::condition_variable m_done;  // a file read
		std::deque<std::string> m_queue; // waiting; the first is being read while the thread runs
		std::map<std::string, std::shared_ptr<const processors::Sample>> m_samples;
		bool m_reading = false; // the first file waiting is being read
		bool m_stopping = false;
		std::thread m_thread; // started by the first request
	};
}
