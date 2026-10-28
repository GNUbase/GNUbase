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
	// SCHED_FIFO on Linux. If the OS refuses, for example without real-time privileges on Linux, the thread keeps
	// its priority and active() is false.
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

	// Real-time worker threads that help the audio thread with a job. After a job, workers spin for half a period
	// (50 us to 2 ms) waiting for the next one, then sleep on a semaphore; run wakes the sleepers, works on the job
	// itself as thread 0 and returns once no worker is inside the job any more. A worker that wakes late finds the
	// job done, so a slow worker never delays the audio thread by more than the job's own work. Workers run in a
	// real-time context with denormals disabled.
	class WorkerPool
	{
	public:
		// Starts workers threads with real-time priority for periods of about periodSeconds.
		explicit WorkerPool(size_t workers, double periodSeconds = 0.005);
		~WorkerPool(); // joins the workers
		WorkerPool(const WorkerPool&) = delete;
		WorkerPool& operator=(const WorkerPool&) = delete;

		size_t workers() const { return m_threads.size(); }

		// Whether every worker got real-time priority.
		bool realtime() const { return m_realtime.load(std::memory_order_acquire) == m_threads.size(); }

		// Runs the job on the calling thread (thread 0) and on the workers (threads 1 to workers()). Real-time safe:
		// it neither allocates, locks nor blocks; it spins until the last worker has left the job.
		void run(ParallelJob& job) noexcept;

	private:
		void loop(size_t thread, double periodSeconds);
		uint64_t waitForJob(uint64_t seen) noexcept; // returns the new generation

		std::chrono::steady_clock::duration m_spin;
		Semaphore m_wake;
		std::atomic<ParallelJob*> m_job{nullptr};
		std::atomic<uint64_t> m_generation{0}; // counts jobs
		std::atomic<size_t> m_sleepers{0};     // workers that sleep or are about to
		std::atomic<size_t> m_inside{0};       // workers inside the current job
		std::atomic<size_t> m_realtime{0};
		std::atomic<bool> m_stop{false};
		std::vector<std::thread> m_threads;
	};
}
