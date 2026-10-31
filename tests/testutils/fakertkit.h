// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace gnubase::test
{
	// Linux: a fake rtkit (org.freedesktop.RealtimeKit1) on a D-Bus server of its own, which the code under test
	// reaches as the system bus (DBUS_SYSTEM_BUS_ADDRESS) while the object lives. It answers the bus's Hello, rtkit's
	// MaxRealtimePriority and RTTimeUSecMax, and records MakeThreadRealtime requests, which it grants or refuses.
	// Missing answers as a bus without rtkit; NoBus leaves no bus at that address.
	class FakeRtkit
	{
	public:
		enum class Mode : uint8_t
		{
			Grants,
			Refuses,
			Missing,
			NoBus
		};
		struct Request
		{
			uint64_t thread = 0; // a kernel thread id
			uint32_t priority = 0;
		};
		static constexpr int32_t g_maxPriority = 20;
		static constexpr int64_t g_rttimeMax = 3'600'000'000; // one hour in microseconds

		explicit FakeRtkit(Mode mode = Mode::Grants);
		~FakeRtkit();
		FakeRtkit(const FakeRtkit&) = delete;
		FakeRtkit& operator=(const FakeRtkit&) = delete;

		std::vector<Request> requests() const;

	private:
		struct Impl;
		std::unique_ptr<Impl> m_impl;
	};
}
