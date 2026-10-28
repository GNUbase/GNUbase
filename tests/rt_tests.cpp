// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/rt/denormals.h"
#include "platform/rt/mpmcqueue.h"
#include "platform/rt/snapshotpublisher.h"
#include "platform/rt/spscqueue.h"
#include "platform/rt/triplebuffer.h"
#include "platform/rt/workerpool.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

using namespace gnubase::rt;

TEST_CASE("SpscQueue delivers values in order and reports full and empty", "[unit]")
{
	SpscQueue<int> queue(3); // rounded up to 4
	CHECK(queue.capacity() == 4);

	int value = 0;
	CHECK_FALSE(queue.tryPop(value));
	for (int i = 0; i < 4; ++i)
		CHECK(queue.tryPush(i));
	CHECK_FALSE(queue.tryPush(99));

	for (int i = 0; i < 4; ++i)
	{
		REQUIRE(queue.tryPop(value));
		CHECK(value == i);
	}
	CHECK_FALSE(queue.tryPop(value));
}

TEST_CASE("SpscQueue keeps the order between a producer and a consumer thread", "[unit]")
{
	constexpr int count = 100000;
	SpscQueue<int> queue(64);

	std::thread producer(
		[&]
		{
			for (int i = 0; i < count; ++i)
				while (!queue.tryPush(i))
					std::this_thread::yield();
		});

	int expected = 0;
	bool inOrder = true;
	while (expected < count)
	{
		int value = 0;
		if (!queue.tryPop(value))
		{
			std::this_thread::yield();
			continue;
		}
		inOrder = inOrder && value == expected;
		++expected;
	}
	producer.join();
	CHECK(inOrder);
}

TEST_CASE("MpmcQueue delivers every value exactly once from several producers", "[unit]")
{
	constexpr int producers = 4;
	constexpr int perProducer = 20000;
	MpmcQueue<int> queue(128);

	std::vector<std::thread> threads;
	threads.reserve(producers);
	for (int p = 0; p < producers; ++p)
	{
		threads.emplace_back(
			[&queue, p]
			{
				for (int i = 0; i < perProducer; ++i)
					while (!queue.tryPush((p * perProducer) + i))
						std::this_thread::yield();
			});
	}

	constexpr int total = producers * perProducer;
	std::vector<int> seen(static_cast<size_t>(total), 0);
	for (int received = 0; received < total;)
	{
		int value = 0;
		if (queue.tryPop(value))
		{
			++seen[static_cast<size_t>(value)];
			++received;
		}
		else
		{
			std::this_thread::yield();
		}
	}
	for (auto& thread : threads)
		thread.join();

	bool exactlyOnce = true;
	for (const int s : seen)
		exactlyOnce = exactlyOnce && s == 1;
	CHECK(exactlyOnce);

	int value = 0;
	CHECK_FALSE(queue.tryPop(value));
}

TEST_CASE("MpmcQueue reports full", "[unit]")
{
	MpmcQueue<int> queue(2);
	CHECK(queue.tryPush(1));
	CHECK(queue.tryPush(2));
	CHECK_FALSE(queue.tryPush(3));
}

TEST_CASE("TripleBuffer hands the latest value to the reader", "[unit]")
{
	TripleBuffer<int> buffer;
	CHECK_FALSE(buffer.update());

	buffer.writeBuffer() = 1;
	buffer.publish();
	buffer.writeBuffer() = 2;
	buffer.publish();

	REQUIRE(buffer.update());
	CHECK(buffer.readBuffer() == 2);
	CHECK_FALSE(buffer.update());
	CHECK(buffer.readBuffer() == 2);
}

TEST_CASE("TripleBuffer never shows a half-written value", "[unit]")
{
	struct Pair
	{
		int a = 0;
		int b = 0;
	};
	constexpr int count = 100000;
	TripleBuffer<Pair> buffer;

	std::thread writer(
		[&]
		{
			for (int i = 1; i <= count; ++i)
			{
				buffer.writeBuffer() = {i, -i};
				buffer.publish();
			}
		});

	bool consistent = true;
	int last = 0;
	while (last < count)
	{
		if (!buffer.update())
		{
			std::this_thread::yield();
			continue;
		}
		const Pair& pair = buffer.readBuffer();
		consistent = consistent && pair.a == -pair.b && pair.a > last;
		last = pair.a;
	}
	writer.join();
	CHECK(consistent);
}

TEST_CASE("ScopedDenormalsDisabled flushes subnormal numbers to zero and restores the mode", "[unit]")
{
	const volatile float subnormal = std::numeric_limits<float>::denorm_min() * 4.0f;
	const volatile float one = 1.0f;
	{
		const ScopedDenormalsDisabled noDenormals;
		CHECK(subnormal * one == 0.0f);
	}
	CHECK(subnormal * one != 0.0f);
}

namespace
{
	std::atomic<int> g_liveSnapshots{0};

	// Counts how many exist, to see when SnapshotPublisher frees them.
	struct Snapshot
	{
		explicit Snapshot(int value) : value(value), check(-value) { ++g_liveSnapshots; }
		~Snapshot() { --g_liveSnapshots; }
		Snapshot(const Snapshot&) = delete;
		Snapshot& operator=(const Snapshot&) = delete;

		int value;
		int check;
	};
}

TEST_CASE("SnapshotPublisher hands the newest snapshot to the reader", "[unit]")
{
	SnapshotPublisher<Snapshot> publisher(std::make_unique<Snapshot>(1));
	CHECK(publisher.acquire().value == 1);
	publisher.publish(std::make_unique<Snapshot>(2));
	publisher.publish(std::make_unique<Snapshot>(3));
	CHECK(publisher.latest().value == 3);
	CHECK(publisher.acquire().value == 3);
}

TEST_CASE("SnapshotPublisher frees a snapshot once the reader has moved on", "[unit]")
{
	g_liveSnapshots = 0;
	{
		SnapshotPublisher<Snapshot> publisher(std::make_unique<Snapshot>(1));
		const Snapshot& first = publisher.acquire();
		publisher.publish(std::make_unique<Snapshot>(2));
		publisher.publish(std::make_unique<Snapshot>(3));
		CHECK(g_liveSnapshots == 2); // 1 is in use, 2 was never read, 3 is the newest
		CHECK(first.value == 1);

		CHECK(publisher.acquire().value == 3);
		publisher.collect();
		CHECK(g_liveSnapshots == 1);

		publisher.release();
		publisher.publish(std::make_unique<Snapshot>(4));
		CHECK(g_liveSnapshots == 1);
	}
	CHECK(g_liveSnapshots == 0);
}

TEST_CASE("SnapshotPublisher keeps the reader's snapshot alive while publishing continues", "[unit]")
{
	constexpr int count = 20000;
	g_liveSnapshots = 0;
	{
		SnapshotPublisher<Snapshot> publisher(std::make_unique<Snapshot>(0));
		std::atomic<bool> done{false};
		std::thread writer(
			[&]
			{
				for (int i = 1; i <= count; ++i)
					publisher.publish(std::make_unique<Snapshot>(i));
				done = true;
			});

		bool consistent = true;
		int last = 0;
		while (!done || last < count)
		{
			const Snapshot& snapshot = publisher.acquire();
			consistent = consistent && snapshot.check == -snapshot.value && snapshot.value >= last;
			last = snapshot.value;
		}
		writer.join();
		CHECK(consistent);
		CHECK(last == count);
	}
	CHECK(g_liveSnapshots == 0);
}

TEST_CASE("Semaphore wakes a waiting thread once per release", "[unit]")
{
	Semaphore semaphore;
	semaphore.release(2);
	semaphore.wait();
	semaphore.wait(); // both releases counted: neither wait blocks

	std::atomic<int> woken{0};
	std::thread waiter(
		[&]
		{
			semaphore.wait();
			++woken;
			semaphore.wait();
			++woken;
		});
	semaphore.release();
	while (woken < 1)
		std::this_thread::yield();
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	CHECK(woken == 1);
	semaphore.release();
	waiter.join();
	CHECK(woken == 2);
}

namespace
{
	// Hands out tasks 0 to count - 1 to whichever thread asks, and records which thread ran each.
	class Tasks final : public ParallelJob
	{
	public:
		explicit Tasks(size_t count) : m_ranBy(count) {}

		void reset()
		{
			m_next = 0;
			m_done = 0;
			for (std::atomic<int>& ranBy : m_ranBy)
				ranBy = -1;
		}

		void work(size_t thread) noexcept override
		{
			for (size_t task = m_next++; task < m_ranBy.size(); task = m_next++)
			{
				volatile double sum = 0.0; // a little work, so several threads get some
				for (int i = 0; i < 2000; ++i)
					sum = sum + (i * 0.5);
				m_ranBy[task] = static_cast<int>(thread);
				++m_done;
			}
			calls[std::min<size_t>(thread, calls.size() - 1)]++;
		}

		size_t done() const { return m_done; }
		int ranBy(size_t task) const { return m_ranBy[task]; }

		std::array<std::atomic<int>, 9> calls{};

	private:
		std::vector<std::atomic<int>> m_ranBy;
		std::atomic<size_t> m_next{0};
		std::atomic<size_t> m_done{0};
	};
}

TEST_CASE("WorkerPool runs a job on the calling thread and its workers until it is done", "[unit]")
{
	for (const size_t workers : {size_t{0}, size_t{1}, size_t{3}, size_t{7}})
	{
		CAPTURE(workers);
		WorkerPool pool(workers);
		CHECK(pool.workers() == workers);
		Tasks tasks(500);
		std::array<int, 9> ran{};
		for (int run = 0; run < 200; ++run)
		{
			tasks.reset();
			pool.run(tasks);
			REQUIRE(tasks.done() == 500); // finished when run returns
			for (size_t task = 0; task < 500; ++task)
			{
				const int thread = tasks.ranBy(task);
				REQUIRE(thread >= 0);
				REQUIRE(static_cast<size_t>(thread) <= workers);
				++ran[static_cast<size_t>(thread)];
			}
		}
		CHECK(tasks.calls[0] == 200); // the calling thread works on every run
		if (workers > 0)
			CHECK(ran[0] < 200 * 500); // and the workers helped
	}
}

TEST_CASE("WorkerPool never runs a job after run returned", "[unit]")
{
	class Probe final : public ParallelJob
	{
	public:
		void work(size_t /*thread*/) noexcept override
		{
			if (closed)
				++late;
		}

		std::atomic<bool> closed{false};
		std::atomic<int> late{0};
	};

	WorkerPool pool(4);
	std::vector<std::unique_ptr<Probe>> probes;
	for (int run = 0; run < 500; ++run)
	{
		probes.push_back(std::make_unique<Probe>());
		pool.run(*probes.back());
		probes.back()->closed = true;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50)); // late wake-ups have happened by now
	int late = 0;
	for (const auto& probe : probes)
		late += probe->late;
	CHECK(late == 0);
}

TEST_CASE("hardwareThreads counts at least one thread", "[unit]")
{
	CHECK(hardwareThreads() >= 1);
}
