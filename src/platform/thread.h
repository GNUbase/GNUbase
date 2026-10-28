// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace gnubase::platform
{
	// Gives the calling thread real-time priority for audio processing: time-critical priority on Windows, SCHED_FIFO
	// on Linux and macOS. Returns false if the system refuses, e.g. without the right to real-time scheduling on Linux;
	// the thread then keeps running at its normal priority. Not for audio threads: it is a system call.
	bool setRealtimePriority() noexcept;
}
