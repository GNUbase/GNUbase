// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/rt/workerpool.h"

#include "platform/rt/denormals.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

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
#include <os/workgroup.h>
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
		param.sched_priority = sched_get_priority_min(SCHED_FIFO);
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

	namespace
	{
		std::atomic<size_t> g_threadLimit{SIZE_MAX};
		std::atomic<bool> g_refuseRealtime{false};
		std::atomic<bool> g_pretendRealtime{false};

		// The tickets of the job of a generation, for count more workers; see WorkerPool::admit.
		constexpr uint64_t tickets(uint64_t generation, size_t count) noexcept
		{
			return (generation << 8) | count;
		}

		// Spinning between jobs only catches the next one when the load is close to the full period; otherwise the
		// worker sleeps.
		constexpr auto g_minSpin = std::chrono::microseconds(20);
		constexpr auto g_maxSpin = std::chrono::microseconds(200);

		// Linux: the workers' priority for the calling thread; 0 for their initial one if it has no real-time priority.
		int prioritySuitingCaller() noexcept
		{
#ifdef __linux__
			// Once per thread that runs jobs: these are short system calls that never block.
			sched_param param{};
			const int policy = sched_getscheduler(0);
			if ((policy == SCHED_FIFO || policy == SCHED_RR) && sched_getparam(0, &param) == 0)
				return workerPriority(param.sched_priority, sched_get_priority_min(SCHED_FIFO));
#endif
			return 0;
		}
	}

	void setWorkerPoolFaults(const WorkerPoolFaults& faults) noexcept
	{
		g_threadLimit.store(faults.threadLimit);
		g_refuseRealtime.store(faults.refuseRealtime);
		g_pretendRealtime.store(faults.pretendRealtime);
	}

	WorkerPool::WorkerPool(size_t workers, double periodSeconds, void* workgroup)
		: m_spin(std::clamp(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
								std::chrono::duration<double>(periodSeconds / 16.0)),
			  std::chrono::duration_cast<std::chrono::steady_clock::duration>(g_minSpin),
			  std::chrono::duration_cast<std::chrono::steady_clock::duration>(g_maxSpin)))
	{
		assertNotRealtime("starting a worker pool");
		if (workers > g_maxWorkers)
			throw std::invalid_argument("a worker pool takes at most " + std::to_string(g_maxWorkers) + " workers");
		m_wakes = std::make_unique<Semaphore[]>(workers);
		m_threads.reserve(workers);
		try
		{
			for (size_t i = 0; i < workers; ++i)
			{
				if (i >= g_threadLimit.load())
					throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again),
						"starting worker thread " + std::to_string(i + 1));
				m_threads.emplace_back([this, i, periodSeconds, workgroup] { loop(i + 1, periodSeconds, workgroup); });
			}
		}
		catch (...)
		{
			stopWorkers(); // or the threads' destructors would terminate the program
			throw;
		}
		// Each worker tries to get real-time priority before it waits for jobs: afterwards realtime() is final.
		for (size_t i = 0; i < workers; ++i)
			m_started.wait();
	}

	WorkerPool::~WorkerPool()
	{
		stopWorkers();
	}

	void WorkerPool::stopWorkers() noexcept
	{
		m_stop.store(true, std::memory_order_seq_cst);
		m_generation.fetch_add(1, std::memory_order_seq_cst);
		wake(g_maxWorkers);
		for (std::thread& thread : m_threads)
			thread.join();
		m_threads.clear();
	}

	void WorkerPool::run(ParallelJob& job, size_t helpers) noexcept
	{
		helpers = std::min(helpers, m_threads.size());
		if (helpers == 0)
		{
			job.work(0);
			return;
		}
		followCaller();
		const uint64_t generation = m_generation.load(std::memory_order_relaxed) + 1; // only this thread changes it
		m_floatMode.store(floatMode(), std::memory_order_relaxed);                    // published by the store of m_job
		m_tickets.store(tickets(generation, helpers), std::memory_order_seq_cst);
		m_job.store(&job, std::memory_order_seq_cst);
		m_generation.store(generation, std::memory_order_seq_cst);
		wake(helpers);
		job.work(0);
		// A worker increments m_inside before it takes a ticket, and this thread takes the tickets back before it
		// reads m_inside: with sequentially consistent order a worker either gets no ticket or is counted here, so a
		// worker never takes a ticket of this job after run returned. Clearing m_job lets the admitted ones go.
		m_tickets.store(tickets(generation, 0), std::memory_order_seq_cst);
		m_job.store(nullptr, std::memory_order_seq_cst);
		while (m_inside.load(std::memory_order_seq_cst) != 0)
			cpuPause();
	}

	bool WorkerPool::admit(uint64_t generation) noexcept
	{
		// The job's generation and its tickets are one word: a worker takes a ticket only from the job it saw, so a
		// worker that comes late for one job can't take one of the next job's tickets as well.
		uint64_t current = m_tickets.load(std::memory_order_seq_cst);
		while ((current >> 8) == generation && (current & 0xFF) != 0)
			if (m_tickets.compare_exchange_weak(current, current - 1, std::memory_order_seq_cst))
				return true;
		return false;
	}

	void WorkerPool::wake(size_t count) noexcept
	{
		// A worker sets its sleeping bit before it checks the generation one last time, and this thread takes the bits
		// after it changed the generation: a worker either sees the new job or is woken. Each taken bit gets one
		// release of that worker's own semaphore, so no other worker can take the wake-up. Sleepers the job doesn't
		// need keep their bits for the next one.
		uint64_t sleeping = m_sleeping.load(std::memory_order_seq_cst);
		uint64_t taken = 0;
		do
		{
			taken = 0;
			uint64_t rest = sleeping;
			for (size_t i = 0; i < count && rest != 0; ++i)
			{
				taken |= rest & (~rest + 1); // the lowest bit
				rest &= rest - 1;
			}
		} while (
			taken != 0 && !m_sleeping.compare_exchange_weak(sleeping, sleeping & ~taken, std::memory_order_seq_cst));
		for (size_t worker = 0; taken != 0; ++worker, taken >>= 1)
			if ((taken & 1) != 0)
				m_wakes[worker].release();
	}

	void WorkerPool::followCaller() noexcept
	{
		const std::thread::id caller = std::this_thread::get_id();
		if (caller == m_caller)
			return;
		m_caller = caller;
		m_priority.store(prioritySuitingCaller(), std::memory_order_relaxed); // published by the new generation
	}

	void WorkerPool::loop(size_t thread, double periodSeconds, void* workgroup)
	{
		// A refused worker runs at normal priority, as on Linux without real-time privileges.
		std::optional<ScopedRealtimePriority> priority;
		if (!g_refuseRealtime.load())
			priority.emplace(periodSeconds);
		const bool realtime = priority && priority->active();
		if (realtime || g_pretendRealtime.load())
			m_realtime.fetch_add(1, std::memory_order_release);
#ifdef __APPLE__
		// The device's workgroup tells the scheduler that this thread works for its audio thread's deadline.
		auto* const group = static_cast<os_workgroup_t>(workgroup);
		os_workgroup_join_token_s token{};
		const bool joined = group != nullptr && os_workgroup_join(group, &token) == 0;
		if (joined)
			m_joined.fetch_add(1, std::memory_order_release);
#else
		(void)workgroup;
#endif
		m_started.release();

		const uint64_t initialMode = floatMode();
		uint64_t mode = initialMode;
#ifdef __linux__
		int ownPriority = 0;
#endif
		uint64_t seen = 0;
		bool tookPart = false;
		for (;;)
		{
			seen = waitForJob(thread - 1, seen, tookPart);
			tookPart = false;
			if (m_stop.load(std::memory_order_seq_cst))
				break;
			m_inside.fetch_add(1, std::memory_order_seq_cst);
			if (admit(seen))
				if (ParallelJob* job = m_job.load(std::memory_order_seq_cst))
				{
					if (const uint64_t callerMode = m_floatMode.load(std::memory_order_relaxed); callerMode != mode)
					{
						setFloatMode(callerMode);
						mode = callerMode;
					}
#ifdef __linux__
					if (const int wanted = m_priority.load(std::memory_order_relaxed);
						realtime && wanted != ownPriority)
					{
						// Rarely: when another real-time thread starts running jobs.
						sched_param param{};
						param.sched_priority = wanted != 0 ? wanted : sched_get_priority_min(SCHED_FIFO);
						sched_setscheduler(0, SCHED_FIFO, &param);
						ownPriority = wanted;
					}
#endif
					const ScopedRealtimeContext realtimeContext;
					job->work(thread);
					tookPart = true;
				}
			m_inside.fetch_sub(1, std::memory_order_seq_cst);
		}
		setFloatMode(initialMode);
#ifdef __APPLE__
		if (joined)
			os_workgroup_leave(group, &token);
#endif
	}

	uint64_t WorkerPool::waitForJob(size_t worker, uint64_t seen, bool spin) noexcept
	{
		const uint64_t bit = uint64_t{1} << worker;
		// Jobs come once per audio block: spinning a little after taking part in one catches the next one without the
		// latency of a wake-up when the load is high.
		auto spinUntil = std::chrono::steady_clock::now() + (spin ? m_spin : std::chrono::steady_clock::duration{});
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
			m_sleeping.fetch_or(bit, std::memory_order_seq_cst);
			generation = m_generation.load(std::memory_order_seq_cst);
			// A new job: unless run took the bit already, and with it releases the semaphore, the worker goes to it.
			if (generation != seen && (m_sleeping.fetch_and(~bit, std::memory_order_seq_cst) & bit) != 0)
				return generation;
			m_wakes[worker].wait();
			spinUntil = std::chrono::steady_clock::now() + m_spin;
		}
	}
}
