// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/sharedmemory.h"

#include "platform/rt/realtime.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gnubase::platform
{
	namespace
	{
		void checkName(const std::string& name)
		{
			const bool valid = !name.empty() && name.size() <= SharedMemory::g_maxNameLength &&
				std::all_of(name.begin(), name.end(),
					[](char c)
					{
						return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
							c == '_';
					});
			if (!valid)
				throw std::system_error(EINVAL, std::generic_category(), "Invalid shared memory name '" + name + "'");
		}

#ifndef __linux__
		// Touches every page, so that audio threads take no page faults on the memory later; Linux maps them with
		// MAP_POPULATE instead. Only the creator writes: it is the only one using the memory yet. Reading is enough to
		// map a page of shared memory.
		void prefault(void* data, size_t size, bool write) noexcept
		{
			constexpr size_t smallestPage = 4096;
			auto* bytes = static_cast<volatile unsigned char*>(data);
			for (size_t offset = 0; offset < size; offset += smallestPage)
			{
				if (write)
					bytes[offset] = 0;
				else
					static_cast<void>(bytes[offset]);
			}
		}
#endif

#ifdef _WIN32
		std::wstring objectName(const std::string& name)
		{
			// Names are ASCII (checkName), so widening each character is enough.
			return L"Local\\" + std::wstring(name.begin(), name.end());
		}

		[[noreturn]] void throwLastError(const std::string& what)
		{
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
		}
#else
#ifdef __linux__
		// MAP_POPULATE maps every page at once.
		constexpr int g_mapFlags = MAP_SHARED | MAP_POPULATE;
#else
		constexpr int g_mapFlags = MAP_SHARED;
#endif

		std::string objectName(const std::string& name)
		{
			return "/" + name;
		}

		[[noreturn]] void throwErrno(const std::string& what)
		{
			throw std::system_error(errno, std::generic_category(), what);
		}
#endif
	}

	SharedMemory::~SharedMemory()
	{
		release();
	}

	SharedMemory::SharedMemory(SharedMemory&& other) noexcept
		: m_data(std::exchange(other.m_data, nullptr)), m_size(std::exchange(other.m_size, 0)),
		  m_name(std::move(other.m_name)), m_handle(std::exchange(other.m_handle, nullptr)),
		  m_owner(std::exchange(other.m_owner, false))
	{
	}

	SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept
	{
		if (this != &other)
		{
			release();
			m_data = std::exchange(other.m_data, nullptr);
			m_size = std::exchange(other.m_size, 0);
			m_name = std::move(other.m_name);
			m_handle = std::exchange(other.m_handle, nullptr);
			m_owner = std::exchange(other.m_owner, false);
		}
		return *this;
	}

	SharedMemory SharedMemory::create(const std::string& name, size_t size)
	{
		rt::assertNotRealtime("SharedMemory::create");
		checkName(name);
		SharedMemory memory;
		memory.m_name = name;
		memory.m_size = size;
#ifdef _WIN32
		const auto size64 = static_cast<uint64_t>(size);
		HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
			static_cast<DWORD>(size64 >> 32), static_cast<DWORD>(size64 & 0xffffffffU), objectName(name).c_str());
		if (mapping == nullptr)
			throwLastError("Cannot create shared memory '" + name + "'");
		if (GetLastError() == ERROR_ALREADY_EXISTS)
		{
			CloseHandle(mapping);
			throw std::system_error(EEXIST, std::generic_category(), "Shared memory '" + name + "' exists already");
		}
		memory.m_handle = mapping;
		memory.m_data = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size);
		if (memory.m_data == nullptr)
			throwLastError("Cannot map shared memory '" + name + "'");
		// The pages are committed already: a full page file makes creating fail, not the first touch.
		prefault(memory.m_data, size, true);
#else
		const int file = shm_open(objectName(name).c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
		if (file < 0)
			throwErrno("Cannot create shared memory '" + name + "'");
		memory.m_owner = true; // from here on the destructor removes the name again
		if (ftruncate(file, static_cast<off_t>(size)) != 0)
		{
			const int error = errno;
			close(file);
			throw std::system_error(error, std::generic_category(), "Cannot size shared memory '" + name + "'");
		}
#ifdef __linux__
		// ftruncate reserves no pages on tmpfs. Without them, a full /dev/shm (64 MiB in a default Docker container)
		// would be a SIGBUS on first touch, on an audio thread, instead of an error here.
		int reserved = 0;
		do
			reserved = posix_fallocate(file, 0, static_cast<off_t>(size));
		while (reserved == EINTR);
		if (reserved != 0)
		{
			close(file);
			throw std::system_error(reserved, std::generic_category(), "Cannot reserve shared memory '" + name + "'");
		}
#endif
		void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, g_mapFlags, file, 0);
		const int error = errno;
		close(file);
		if (data == MAP_FAILED) // NOLINT(performance-no-int-to-ptr): MAP_FAILED is defined as ((void*)-1)
			throw std::system_error(error, std::generic_category(), "Cannot map shared memory '" + name + "'");
		memory.m_data = data;
#ifndef __linux__
		prefault(data, size, true);
#endif
#endif
		return memory;
	}

	SharedMemory SharedMemory::open(const std::string& name)
	{
		rt::assertNotRealtime("SharedMemory::open");
		checkName(name);
		SharedMemory memory;
		memory.m_name = name;
#ifdef _WIN32
		HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, objectName(name).c_str());
		if (mapping == nullptr)
			throwLastError("Cannot open shared memory '" + name + "'");
		memory.m_handle = mapping;
		memory.m_data = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);
		if (memory.m_data == nullptr)
			throwLastError("Cannot map shared memory '" + name + "'");
		MEMORY_BASIC_INFORMATION info{};
		if (VirtualQuery(memory.m_data, &info, sizeof(info)) == 0)
			throwLastError("Cannot query shared memory '" + name + "'");
		memory.m_size = info.RegionSize;
		prefault(memory.m_data, memory.m_size, false);
#else
		const int file = shm_open(objectName(name).c_str(), O_RDWR, 0);
		if (file < 0)
			throwErrno("Cannot open shared memory '" + name + "'");
		struct stat status{};
		if (fstat(file, &status) != 0 || status.st_size <= 0)
		{
			const int error = status.st_size <= 0 ? EINVAL : errno;
			close(file);
			throw std::system_error(error, std::generic_category(), "Cannot size shared memory '" + name + "'");
		}
		const auto size = static_cast<size_t>(status.st_size);
		void* data = mmap(nullptr, size, PROT_READ | PROT_WRITE, g_mapFlags, file, 0);
		const int error = errno;
		close(file);
		if (data == MAP_FAILED) // NOLINT(performance-no-int-to-ptr): MAP_FAILED is defined as ((void*)-1)
			throw std::system_error(error, std::generic_category(), "Cannot map shared memory '" + name + "'");
		memory.m_data = data;
		memory.m_size = size;
#ifndef __linux__
		prefault(data, size, false);
#endif
#endif
		return memory;
	}

	void SharedMemory::removeName() noexcept
	{
#ifndef _WIN32
		if (m_owner)
		{
			// Without allocating: names are short (checkName).
			std::array<char, SharedMemory::g_maxNameLength + 2> path{};
			path[0] = '/';
			std::memcpy(path.data() + 1, m_name.data(), std::min(m_name.size(), SharedMemory::g_maxNameLength));
			shm_unlink(path.data());
		}
#endif
		m_owner = false;
	}

	void SharedMemory::release() noexcept
	{
		removeName();
#ifdef _WIN32
		if (m_data != nullptr)
			UnmapViewOfFile(m_data);
		if (m_handle != nullptr)
			CloseHandle(m_handle);
#else
		if (m_data != nullptr)
			munmap(m_data, m_size);
#endif
		m_data = nullptr;
		m_handle = nullptr;
		m_size = 0;
	}
}
