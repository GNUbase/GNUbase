// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <string>

namespace gnubase::platform
{
	// Named shared memory that another process can map: a file mapping in the Local namespace on Windows, a POSIX
	// shared memory object (shm_open) on macOS and Linux. Creating and opening allocate and make system calls, so
	// neither may run on an audio thread; the mapped memory itself is ordinary memory.
	class SharedMemory
	{
	public:
		// macOS allows 31 characters including the leading slash that POSIX names need.
		static constexpr size_t g_maxNameLength = 30;

		SharedMemory() = default;
		~SharedMemory();
		SharedMemory(SharedMemory&& other) noexcept;
		SharedMemory& operator=(SharedMemory&& other) noexcept;
		SharedMemory(const SharedMemory&) = delete;
		SharedMemory& operator=(const SharedMemory&) = delete;

		// Creates a region of at least `size` bytes, filled with zeros. Names are 1 to g_maxNameLength letters, digits,
		// '-' and '_'. Throws std::system_error if the name is taken or invalid, or the system refuses.
		static SharedMemory create(const std::string& name, size_t size);

		// Maps a region another process created. Throws std::system_error if there is none under the name.
		static SharedMemory open(const std::string& name);

		void* data() const noexcept { return m_data; }
		// The mapped size, at least the size it was created with (rounded up to whole pages).
		size_t size() const noexcept { return m_size; }
		const std::string& name() const noexcept { return m_name; }

		// Removes the name, so no other process can open the region any more; mappings stay valid. On POSIX the
		// creator removes it at the latest when it is destroyed. On Windows a name lives as long as a mapping does.
		void removeName() noexcept;

	private:
		void release() noexcept;

		void* m_data = nullptr;
		size_t m_size = 0;
		std::string m_name;
		void* m_handle = nullptr; // the file mapping on Windows
		bool m_owner = false;     // the creator removes the name (POSIX)
	};
}
