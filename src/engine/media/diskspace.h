// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

namespace gnubase::engine
{
	// The space available on the disk a folder lies on, measured on a thread of its own (#645): a slow or unreachable
	// folder, such as one on a network drive, holds up that thread, not the caller. The thread shares its state with
	// the DiskSpace, which neither waits for it when it goes nor starts another while one measures.
	class DiskSpace
	{
	public:
		// The available bytes of the folder's disk, 0 if it can't be read. By default the nearest folder above it that
		// is there: a record folder may not be there yet.
		using Measure = std::function<uintmax_t(const std::filesystem::path& folder)>;
		explicit DiskSpace(Measure measure = available);

		// Starts a measurement of the folder unless one runs, and waits for it up to wait. Then the folder's latest
		// measurement, or nullopt if it has none yet.
		std::optional<uintmax_t> measure(const std::filesystem::path& folder, std::chrono::milliseconds wait);

		static uintmax_t available(const std::filesystem::path& folder);

	private:
		struct State
		{
			std::mutex mutex;
			std::condition_variable measured;
			bool running = false;
			uint64_t count = 0; // measurements ended
			std::filesystem::path folder;
			uintmax_t available = 0; // of folder
		};
		Measure m_measure;
		std::shared_ptr<State> m_state = std::make_shared<State>();
	};
}
