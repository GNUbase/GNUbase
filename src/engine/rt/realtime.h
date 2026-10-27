// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace gnubase::rt
{
	// Called when real-time code allocates, locks or blocks. The default handler prints the reason and aborts.
	using ViolationHandler = void (*)(const char* what) noexcept;

	// Installs a handler and returns the previous one. Tests use this to count violations instead of aborting.
	ViolationHandler setViolationHandler(ViolationHandler handler) noexcept;

	// True while the current thread runs real-time code, i.e. inside a ScopedRealtimeContext.
	bool isRealtimeContext() noexcept;

	// Reports a violation to the handler. Used by the RT checker and by GNUbase functions that may block.
	void reportViolation(const char* what) noexcept;

	// Call at the start of every GNUbase function that may allocate, lock, block or do I/O.
	inline void assertNotRealtime(const char* what) noexcept
	{
		if (isRealtimeContext())
			reportViolation(what);
	}

	// Marks the current thread as running real-time code for the lifetime of the object. Scopes nest.
	class ScopedRealtimeContext
	{
	public:
		ScopedRealtimeContext() noexcept;
		~ScopedRealtimeContext();
		ScopedRealtimeContext(const ScopedRealtimeContext&) = delete;
		ScopedRealtimeContext& operator=(const ScopedRealtimeContext&) = delete;
	};
}
