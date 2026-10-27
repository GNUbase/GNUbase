// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/rt/denormals.h"
#include "engine/rt/mpmcqueue.h"
#include "engine/rt/spscqueue.h"
#include "engine/rt/triplebuffer.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>
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
