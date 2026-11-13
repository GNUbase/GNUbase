// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <mutex>

namespace gnubase::test
{
	// The runs of a command that holds the engine's command thread while it waits for the test to let it go: what
	// the test sends meanwhile queues behind it, and the engine answers it only once the hold is released, however
	// busy the machine is (#1272). The n-th run of the command waits for the n-th release.
	struct CommandHolds
	{
		CommandHolds() = default;
		CommandHolds(const CommandHolds&) = delete;
		CommandHolds& operator=(const CommandHolds&) = delete;
		// Lets every run go, so that a test that fails on the way doesn't leave the engine's thread waiting.
		~CommandHolds() { release(std::numeric_limits<size_t>::max()); }

		// The command's handler, on the engine's command thread. It gives up after 30 s, so that an engine that is
		// stopping can end even if the test never releases it.
		engine::Json hold()
		{
			std::unique_lock lock(mutex);
			const size_t own = entered++;
			changed.notify_all();
			changed.wait_for(lock, std::chrono::seconds(30), [this, own] { return released > own; });
			return engine::Json::object();
		}

		// Whether count runs have entered hold(), waiting up to 30 s for them.
		[[nodiscard]] bool waitEntered(size_t count)
		{
			std::unique_lock lock(mutex);
			return changed.wait_for(lock, std::chrono::seconds(30), [this, count] { return entered >= count; });
		}

		// Lets the first count runs go on.
		void release(size_t count)
		{
			{
				const std::scoped_lock lock(mutex);
				released = count;
			}
			changed.notify_all();
		}

		std::mutex mutex;
		std::condition_variable changed;
		size_t entered = 0;  // the runs that have entered hold()
		size_t released = 0; // the runs let go: release(n) frees the n-th
	};
}
