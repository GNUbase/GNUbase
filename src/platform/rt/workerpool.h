// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace gnubase::rt
{
	// A counting semaphore. release never blocks, so audio threads may call it; wait blocks and is for the threads
	// that sleep on it.
	class Semaphore
	{
	public:
		Semaphore();
		~Semaphore();
		Semaphore(const Semaphore&) = delete;
		Semaphore& operator=(const Semaphore&) = delete;

		void release(int count = 1) noexcept;
		void wait() noexcept;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};

	// Gives the current thread real-time priority for the lifetime of the object, for audio work in periods of about
	// periodSeconds: the Multimedia Class Scheduler's "Pro Audio" task on Windows, a time-constraint policy on macOS,
	// SCHED_FIFO at its lowest priority on Linux (WorkerPool raises it to just below its caller's). If the OS refuses,
	// for example without real-time privileges on Linux, the thread keeps its priority and active() is false.
	class ScopedRealtimePriority
	{
	public:
		explicit ScopedRealtimePriority(double periodSeconds) noexcept;
		~ScopedRealtimePriority();
		ScopedRealtimePriority(const ScopedRealtimePriority&) = delete;
		ScopedRealtimePriority& operator=(const ScopedRealtimePriority&) = delete;

		bool active() const { return m_active; }

	private:
		struct Previous
		{
			void* task = nullptr; // Windows: the MMCSS task
			int policy = 0;       // Linux: the policy and priority to restore
			int priority = 0;
		};

		bool m_active = false;
		Previous m_previous;
	};

	// Linux: the SCHED_FIFO priority of workers that help an audio thread with real-time priority audioPriority. One
	// below it, so the device's own thread always goes first and a spinning worker never outranks it, but at least
	// minimum.
	constexpr int workerPriority(int audioPriority, int minimum) noexcept
	{
		return audioPriority - 1 > minimum ? audioPriority - 1 : minimum;
	}

	// Tells the CPU that the thread spins, so a sibling hyperthread or the bus gets the time.
	void cpuPause() noexcept;

	// The number of hardware threads, at least 1.
	size_t hardwareThreads() noexcept;

	// Work that the threads of a WorkerPool share. work runs on every participating thread at once and returns when
	// that thread finds nothing more to do; it must cope with being called while or after the others finish.
	class ParallelJob
	{
	public:
		virtual void work(size_t thread) noexcept = 0;

	protected:
		ParallelJob() = default;
		~ParallelJob() = default;
		ParallelJob(const ParallelJob&) = default;
		ParallelJob& operator=(const ParallelJob&) = default;
	};

	// Real-time worker threads that help the audio thread with a job. run wakes as many sleeping workers as the job
	// can use, works on the job itself as thread 0 and returns once no worker is inside the job any more. A worker
	// that wakes late finds the job done, so a slow worker never delays the audio thread by more than the job's own
	// work, as long as the workers have real-time priority (realtime). After a job, a worker that took part spins
	// briefly for the next one (a sixteenth of a period, 20 to 200 us), then sleeps on a semaphore; a worker the job
	// didn't need sleeps at once.
	//
	// Every job runs in its caller's floating-point mode on every thread (rounding, flush to zero, denormals are
	// zero), so a job's results don't depend on which thread computed them. On Linux workers take SCHED_FIFO priority
	// one below their caller's (workerPriority) once a real-time thread runs a job.
	class WorkerPool
	{
	public:
		// Starts workers threads with real-time priority for periods of about periodSeconds and waits until each has
		// tried to get it. Throws std::system_error if a thread can't be started, after ending the ones that were.
		explicit WorkerPool(size_t workers, double periodSeconds = 0.005);
		~WorkerPool(); // joins the workers
		WorkerPool(const WorkerPool&) = delete;
		WorkerPool& operator=(const WorkerPool&) = delete;

		size_t workers() const { return m_threads.size(); }

		// The workers that got real-time priority, and whether all did. Final once the constructor returned.
		size_t realtimeWorkers() const { return m_realtime.load(std::memory_order_acquire); }
		bool realtime() const { return realtimeWorkers() == m_threads.size(); }

		// Runs the job on the calling thread (thread 0) and on up to helpers workers (threads 1 to workers()). Wakes
		// no more sleeping workers than that. Real-time safe: it neither allocates, locks nor blocks; it spins until
		// the last worker has left the job.
		void run(ParallelJob& job, size_t helpers = SIZE_MAX) noexcept;

	private:
		void loop(size_t thread, double periodSeconds);
		uint64_t waitForJob(uint64_t seen, bool spin) noexcept; // returns the new generation
		void stopWorkers() noexcept;
		void followCaller() noexcept; // Linux: publishes the worker priority for the calling thread

		std::chrono::steady_clock::duration m_spin;
		Semaphore m_wake;
		Semaphore m_started; // released once by each worker after it tried to get real-time priority
		std::atomic<ParallelJob*> m_job{nullptr};
		std::atomic<uint64_t> m_generation{0}; // counts jobs
		std::atomic<size_t> m_helpers{0};      // how many workers the current job takes
		std::atomic<size_t> m_admitted{0};     // workers that came for the current job
		std::atomic<uint64_t> m_floatMode{0};  // the current job's caller's floating-point mode
		std::atomic<int> m_priority{0};        // Linux: the workers' SCHED_FIFO priority; 0 for their initial one
		std::atomic<size_t> m_sleepers{0};     // workers that sleep or are about to
		std::atomic<size_t> m_inside{0};       // workers inside the current job
		std::atomic<size_t> m_realtime{0};
		std::atomic<bool> m_stop{false};
		std::thread::id m_caller; // the thread that ran the last job; on Linux the workers follow its priority
		std::vector<std::thread> m_threads;
	};

	// For tests: the pools constructed from now on meet an OS that refuses them, as with a process limit or without
	// real-time privileges. Call it while no pool is being constructed.
	struct WorkerPoolFaults
	{
		size_t threadLimit = SIZE_MAX; // starting more worker threads than this throws std::system_error
		bool refuseRealtime = false;   // no worker gets real-time priority
	};
	void setWorkerPoolFaults(const WorkerPoolFaults& faults) noexcept;
}
