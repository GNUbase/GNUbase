// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/rt/workerpool.h"

#include "platform/rt/denormals.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <chrono>
#include <climits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <avrt.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#else
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#endif

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#elif defined(_M_ARM64)
#include <intrin.h>
#endif

namespace gnubase::rt
{
#ifdef _WIN32
	struct Semaphore::Impl
	{
		HANDLE handle = CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr);
	};

	Semaphore::Semaphore() : m_impl(std::make_unique<Impl>())
	{
	}

	Semaphore::~Semaphore()
	{
		CloseHandle(m_impl->handle);
	}

	void Semaphore::release(int count) noexcept
	{
		ReleaseSemaphore(m_impl->handle, count, nullptr);
	}

	void Semaphore::wait() noexcept
	{
		WaitForSingleObject(m_impl->handle, INFINITE);
	}
#elif defined(__APPLE__)
	struct Semaphore::Impl
	{
		dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
	};

	Semaphore::Semaphore() : m_impl(std::make_unique<Impl>())
	{
	}

	Semaphore::~Semaphore()
	{
		dispatch_release(m_impl->semaphore);
	}

	void Semaphore::release(int count) noexcept
	{
		for (int i = 0; i < count; ++i)
			dispatch_semaphore_signal(m_impl->semaphore);
	}

	void Semaphore::wait() noexcept
	{
		dispatch_semaphore_wait(m_impl->semaphore, DISPATCH_TIME_FOREVER);
	}
#else
	struct Semaphore::Impl
	{
		Impl() { sem_init(&semaphore, 0, 0); }
		~Impl() { sem_destroy(&semaphore); }
		Impl(const Impl&) = delete;
		Impl& operator=(const Impl&) = delete;

		sem_t semaphore{};
	};

	Semaphore::Semaphore() : m_impl(std::make_unique<Impl>())
	{
	}

	Semaphore::~Semaphore() = default;

	void Semaphore::release(int count) noexcept
	{
		for (int i = 0; i < count; ++i)
			sem_post(&m_impl->semaphore);
	}

	void Semaphore::wait() noexcept
	{
		int result = 0;
		do
			result = sem_wait(&m_impl->semaphore);
		while (result != 0 && errno == EINTR);
	}
#endif

	ScopedRealtimePriority::ScopedRealtimePriority(double periodSeconds) noexcept
	{
#ifdef _WIN32
		(void)periodSeconds;
		DWORD task = 0;
		m_previous.task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
		if (m_previous.task != nullptr)
			AvSetMmThreadPriority(m_previous.task, AVRT_PRIORITY_CRITICAL);
		m_active = m_previous.task != nullptr;
#elif defined(__APPLE__)
		mach_timebase_info_data_t timebase{};
		mach_timebase_info(&timebase);
		const double ticksPerSecond = 1e9 * timebase.denom / timebase.numer;
		thread_time_constraint_policy_data_t policy{};
		policy.period = static_cast<uint32_t>(periodSeconds * ticksPerSecond);
		policy.computation = static_cast<uint32_t>(periodSeconds * 0.5 * ticksPerSecond);
		policy.constraint = policy.period;
		policy.preemptible = 1;
		m_active = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
					   reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
		(void)periodSeconds;
		sched_param previous{};
		if (pthread_getschedparam(pthread_self(), &m_previous.policy, &previous) != 0)
			return;
		m_previous.priority = previous.sched_priority;
		sched_param param{};
		param.sched_priority = std::min(sched_get_priority_min(SCHED_FIFO) + 70, sched_get_priority_max(SCHED_FIFO));
		m_active = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#endif
	}

	ScopedRealtimePriority::~ScopedRealtimePriority()
	{
		if (!m_active)
			return;
#ifdef _WIN32
		AvRevertMmThreadCharacteristics(m_previous.task);
#elif defined(__APPLE__)
		(void)m_previous; // the thread ends with the scope; macOS has no policy to restore
#else
		sched_param param{};
		param.sched_priority = m_previous.priority;
		pthread_setschedparam(pthread_self(), m_previous.policy, &param);
#endif
	}

	void cpuPause() noexcept
	{
#if defined(_M_X64) || defined(__x86_64__)
		_mm_pause();
#elif defined(_M_ARM64)
		__yield();
#elif defined(__aarch64__)
		asm volatile("yield");
#endif
	}

	size_t hardwareThreads() noexcept
	{
		return std::max<size_t>(std::thread::hardware_concurrency(), 1);
	}

	WorkerPool::WorkerPool(size_t workers, double periodSeconds)
		: m_spin(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			  std::chrono::duration<double>(std::clamp(periodSeconds / 2.0, 50e-6, 2e-3))))
	{
		assertNotRealtime("starting a worker pool");
		m_threads.reserve(workers);
		for (size_t i = 0; i < workers; ++i)
			m_threads.emplace_back([this, i, periodSeconds] { loop(i + 1, periodSeconds); });
	}

	WorkerPool::~WorkerPool()
	{
		m_stop.store(true, std::memory_order_seq_cst);
		m_generation.fetch_add(1, std::memory_order_seq_cst);
		m_wake.release(static_cast<int>(m_threads.size()));
		for (std::thread& thread : m_threads)
			thread.join();
	}

	void WorkerPool::run(ParallelJob& job) noexcept
	{
		if (m_threads.empty())
		{
			job.work(0);
			return;
		}
		m_job.store(&job, std::memory_order_seq_cst);
		m_generation.fetch_add(1, std::memory_order_seq_cst);
		// A worker counts itself as a sleeper before it checks the generation one last time, and this thread counts
		// the sleepers after it changed the generation: a worker either sees the new job or is woken.
		if (const size_t sleepers = m_sleepers.exchange(0, std::memory_order_seq_cst); sleepers > 0)
			m_wake.release(static_cast<int>(sleepers));
		job.work(0);
		// A worker increments m_inside before it reads m_job, and this thread clears m_job before it reads m_inside:
		// with sequentially consistent order a worker either sees no job or is counted here.
		m_job.store(nullptr, std::memory_order_seq_cst);
		while (m_inside.load(std::memory_order_seq_cst) != 0)
			cpuPause();
	}

	void WorkerPool::loop(size_t thread, double periodSeconds)
	{
		const ScopedRealtimePriority priority(periodSeconds);
		if (priority.active())
			m_realtime.fetch_add(1, std::memory_order_release);
		const ScopedDenormalsDisabled denormals;
		uint64_t seen = 0;
		for (;;)
		{
			seen = waitForJob(seen);
			if (m_stop.load(std::memory_order_seq_cst))
				return;
			m_inside.fetch_add(1, std::memory_order_seq_cst);
			if (ParallelJob* job = m_job.load(std::memory_order_seq_cst))
			{
				const ScopedRealtimeContext realtime;
				job->work(thread);
			}
			m_inside.fetch_sub(1, std::memory_order_seq_cst);
		}
	}

	uint64_t WorkerPool::waitForJob(uint64_t seen) noexcept
	{
		// Jobs come once per audio block: spinning a little catches the next one without the latency of a wake-up.
		auto spinUntil = std::chrono::steady_clock::now() + m_spin;
		for (;;)
		{
			uint64_t generation = m_generation.load(std::memory_order_seq_cst);
			if (generation != seen)
				return generation;
			if (std::chrono::steady_clock::now() < spinUntil)
			{
				cpuPause();
				continue;
			}
			m_sleepers.fetch_add(1, std::memory_order_seq_cst);
			generation = m_generation.load(std::memory_order_seq_cst);
			if (generation != seen)
				return generation; // counted anyway: the extra release only wakes this worker once for nothing
			m_wake.wait();
			spinUntil = std::chrono::steady_clock::now() + m_spin;
		}
	}
}
