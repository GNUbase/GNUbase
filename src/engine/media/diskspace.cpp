// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/media/diskspace.h"

#include <system_error>
#include <thread>
#include <utility>

namespace gnubase::engine
{
	DiskSpace::DiskSpace(Measure measure) : m_measure(std::move(measure))
	{
	}

	std::optional<uintmax_t> DiskSpace::measure(const std::filesystem::path& folder, std::chrono::milliseconds wait)
	{
		State& state = *m_state;
		std::unique_lock lock(state.mutex);
		if (!state.running)
		{
			state.running = true;
			const uint64_t before = state.count;
			std::thread(
				[shared = m_state, measure = m_measure, folder]
				{
					const uintmax_t available = measure(folder);
					const std::scoped_lock done(shared->mutex);
					shared->running = false;
					++shared->count;
					shared->folder = folder;
					shared->available = available;
					shared->measured.notify_all();
				})
				.detach(); // a hanging disk keeps it, not the caller or the engine's end
			state.measured.wait_for(lock, wait, [&state, before] { return state.count != before; });
		}
		if (state.count == 0 || state.folder != folder)
			return std::nullopt;
		return state.available;
	}

	uintmax_t DiskSpace::available(const std::filesystem::path& folder)
	{
		std::filesystem::path existing = folder;
		std::error_code error;
		while (!existing.empty() && !std::filesystem::exists(existing, error) && existing.has_parent_path() &&
			existing.parent_path() != existing)
			existing = existing.parent_path();
		const std::filesystem::space_info space = std::filesystem::space(existing, error);
		return error ? 0 : space.available;
	}
}
