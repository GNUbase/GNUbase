// SPDX-License-Identifier: GPL-3.0-or-later
#include "ipc/doorbell.h"

#include "platform/rt/realtime.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Windows 10 1803 and later; older systems refuse it, and the wait falls back to whole milliseconds.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#elif defined(__APPLE__)
// The compare-and-wait system calls behind macOS's own locks, available since macOS 10.12 (xnu bsd/sys/ulock.h).
// ponytail: switch to os_sync_wait_on_address (public, macOS 14.4) once that is the oldest supported macOS.
extern "C" int __ulock_wait(uint32_t operation, void* address, uint64_t value, uint32_t timeoutMicroseconds);
extern "C" int __ulock_wake(uint32_t operation, void* address, uint64_t wakeValue);
#else
#include <climits>
#include <ctime>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace gnubase::ipc
{
	namespace
	{
#ifdef _WIN32
		std::wstring eventName(const std::string& name)
		{
			return L"Local\\" + std::wstring(name.begin(), name.end());
		}

		// Null if the system has no high-resolution timers.
		HANDLE createTimer() noexcept
		{
			return CreateWaitableTimerExW(
				nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
		}
#elif defined(__APPLE__)
		constexpr uint32_t g_compareAndWaitShared = 3; // UL_COMPARE_AND_WAIT_SHARED
		constexpr uint32_t g_wakeAll = 0x00000100;     // ULF_WAKE_ALL
		constexpr uint32_t g_noErrno = 0x01000000;     // ULF_NO_ERRNO
#else
		long futex(std::atomic<uint32_t>* word, int operation, uint32_t value, const timespec* timeout) noexcept
		{
			// Not FUTEX_PRIVATE_FLAG: the word is shared with another process.
			return syscall(SYS_futex, reinterpret_cast<uint32_t*>(word), operation, value, timeout, nullptr, 0);
		}
#endif
	}

	Doorbell::~Doorbell()
	{
		release();
	}

	Doorbell::Doorbell(Doorbell&& other) noexcept
		: m_word(std::exchange(other.m_word, nullptr)), m_event(std::exchange(other.m_event, nullptr)),
		  m_timer(std::exchange(other.m_timer, nullptr))
	{
	}

	Doorbell& Doorbell::operator=(Doorbell&& other) noexcept
	{
		if (this != &other)
		{
			release();
			m_word = std::exchange(other.m_word, nullptr);
			m_event = std::exchange(other.m_event, nullptr);
			m_timer = std::exchange(other.m_timer, nullptr);
		}
		return *this;
	}

	Doorbell Doorbell::create(std::atomic<uint32_t>& word, const std::string& name)
	{
		rt::assertNotRealtime("Doorbell::create");
		Doorbell doorbell;
		doorbell.m_word = &word;
#ifdef _WIN32
		doorbell.m_event = CreateEventW(nullptr, FALSE, FALSE, eventName(name).c_str());
		if (doorbell.m_event == nullptr)
			throw std::system_error(
				static_cast<int>(GetLastError()), std::system_category(), "Cannot create the wake-up event " + name);
		if (GetLastError() == ERROR_ALREADY_EXISTS)
			throw std::system_error(EEXIST, std::generic_category(), "The wake-up event " + name + " exists already");
		doorbell.m_timer = createTimer();
#else
		static_cast<void>(name);
#endif
		return doorbell;
	}

	Doorbell Doorbell::open(std::atomic<uint32_t>& word, const std::string& name)
	{
		rt::assertNotRealtime("Doorbell::open");
		Doorbell doorbell;
		doorbell.m_word = &word;
#ifdef _WIN32
		doorbell.m_event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, eventName(name).c_str());
		if (doorbell.m_event == nullptr)
			throw std::system_error(
				static_cast<int>(GetLastError()), std::system_category(), "Cannot open the wake-up event " + name);
		doorbell.m_timer = createTimer();
#else
		static_cast<void>(name);
#endif
		return doorbell;
	}

	void Doorbell::ring() noexcept
	{
		if (m_word == nullptr)
			return;
		m_word->fetch_add(1, std::memory_order_seq_cst);
#ifdef _WIN32
		SetEvent(m_event);
#elif defined(__APPLE__)
		__ulock_wake(g_compareAndWaitShared | g_wakeAll | g_noErrno, m_word, 0);
#else
		futex(m_word, FUTEX_WAKE, INT_MAX, nullptr);
#endif
	}

	bool Doorbell::wait(uint32_t seen, std::chrono::nanoseconds timeout) const noexcept
	{
		using namespace std::chrono;
		if (m_word == nullptr)
			return false;
		const auto deadline = steady_clock::now() + timeout;
		for (;;)
		{
			if (m_word->load(std::memory_order_seq_cst) != seen)
				return true;
			const nanoseconds remaining = deadline - steady_clock::now();
			if (remaining <= nanoseconds::zero())
				return false;
#ifdef _WIN32
			// Whole milliseconds, rounded up so the wait never ends early. With the timer they are only a backstop.
			const auto timeoutMs = std::min<int64_t>(ceil<milliseconds>(remaining).count(), INFINITE - 2);
			LARGE_INTEGER due{};
			due.QuadPart = -std::max<int64_t>(1, (remaining.count() + 99) / 100); // relative, in 100 ns
			if (m_timer != nullptr && SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE)
			{
				const std::array<HANDLE, 2> handles{m_event, m_timer};
				WaitForMultipleObjects(2, handles.data(), FALSE, static_cast<DWORD>(timeoutMs + 1));
			}
			else
			{
				WaitForSingleObject(m_event, static_cast<DWORD>(timeoutMs));
			}
#elif defined(__APPLE__)
			// A timeout of 0 would wait forever; ceil makes it at least 1.
			const auto micro =
				std::min<int64_t>(ceil<microseconds>(remaining).count(), std::numeric_limits<uint32_t>::max());
			__ulock_wait(g_compareAndWaitShared | g_noErrno, m_word, seen, static_cast<uint32_t>(micro));
#else
			// Returns when woken, when the word no longer equals `seen`, on a signal or at the timeout; the loop sorts
			// these out.
			const seconds whole = duration_cast<seconds>(remaining);
			const timespec relative{static_cast<time_t>(whole.count()), static_cast<long>((remaining - whole).count())};
			futex(m_word, FUTEX_WAIT, seen, &relative);
#endif
		}
	}

	void Doorbell::release() noexcept
	{
#ifdef _WIN32
		if (m_event != nullptr)
			CloseHandle(m_event);
		if (m_timer != nullptr)
			CloseHandle(m_timer);
#endif
		m_event = nullptr;
		m_timer = nullptr;
		m_word = nullptr;
	}
}
