// SPDX-License-Identifier: GPL-3.0-or-later
// RT checker: replaces the global operator new and delete so that allocations in real-time code are reported.
// Linked only into test and debug executables (gnubase-rtcheck). MSVC debug builds also hook the C runtime's heap, so
// malloc, realloc, free and _aligned_malloc in third-party code are reported too.
// ponytail: other builds see C++ allocations only, not malloc, locks or system calls in third-party code.
// RealtimeSanitizer (Clang 20+, -fsanitize=realtime, Linux and macOS) catches all of those if we need it.
#include "platform/rt/realtime.h"

#include <cstdlib>
#include <new>

#ifdef _WIN32
#include <malloc.h>
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#define GNUBASE_RTCHECK_CRT_HEAP
#endif

namespace
{
	// Set while operator new or delete calls the C runtime: they report themselves, so the heap hook skips them.
	thread_local bool g_inOperator = false;

	class InOperator
	{
	public:
		InOperator() noexcept { g_inOperator = true; }
		~InOperator() { g_inOperator = false; }
		InOperator(const InOperator&) = delete;
		InOperator& operator=(const InOperator&) = delete;
	};

#ifdef GNUBASE_RTCHECK_CRT_HEAP
	int crtHeapHook(int type, void* /*data*/, size_t /*size*/, int blockType, long /*request*/,
		const unsigned char* /*file*/, int /*line*/)
	{
		// The C runtime's own blocks are left alone, as its documentation asks of allocation hooks.
		if (blockType != _CRT_BLOCK && !g_inOperator && gnubase::rt::isRealtimeContext())
			gnubase::rt::reportViolation(
				type == _HOOK_FREE ? "memory deallocation in real-time code" : "memory allocation in real-time code");
		return 1; // go on with the allocation
	}

	const struct InstallCrtHeapHook
	{
		InstallCrtHeapHook() noexcept { _CrtSetAllocHook(&crtHeapHook); }
	} g_installCrtHeapHook;
#endif

	void checkAllocation() noexcept
	{
		if (gnubase::rt::isRealtimeContext())
			gnubase::rt::reportViolation("memory allocation in real-time code");
	}

	void checkDeallocation(const void* memory) noexcept
	{
		if (memory != nullptr && gnubase::rt::isRealtimeContext())
			gnubase::rt::reportViolation("memory deallocation in real-time code");
	}

	void* allocate(std::size_t size)
	{
		checkAllocation();
		const InOperator inOperator;
		if (void* memory = std::malloc(size != 0 ? size : 1))
			return memory;
		throw std::bad_alloc();
	}

	void* allocateAligned(std::size_t size, std::align_val_t alignment)
	{
		checkAllocation();
		const auto align = static_cast<std::size_t>(alignment);
		size = size != 0 ? size : 1;
		const InOperator inOperator;
#ifdef _WIN32
		void* memory = _aligned_malloc(size, align);
#else
		void* memory = nullptr;
		if (posix_memalign(&memory, align < sizeof(void*) ? sizeof(void*) : align, size) != 0)
			memory = nullptr;
#endif
		if (memory == nullptr)
			throw std::bad_alloc();
		return memory;
	}

	void deallocate(void* memory) noexcept
	{
		checkDeallocation(memory);
		const InOperator inOperator;
		std::free(memory);
	}

	void deallocateAligned(void* memory) noexcept
	{
		checkDeallocation(memory);
		const InOperator inOperator;
#ifdef _WIN32
		_aligned_free(memory);
#else
		std::free(memory);
#endif
	}
}

// The standard library declares these with reserved parameter names, so the names can never match.
// NOLINTBEGIN(readability-inconsistent-declaration-parameter-name)
void* operator new(std::size_t size)
{
	return allocate(size);
}

void* operator new[](std::size_t size)
{
	return allocate(size);
}

void* operator new(std::size_t size, const std::nothrow_t& /*unused*/) noexcept
{
	try
	{
		return allocate(size);
	}
	catch (const std::bad_alloc&)
	{
		return nullptr;
	}
}

void* operator new[](std::size_t size, const std::nothrow_t& /*unused*/) noexcept
{
	return operator new(size, std::nothrow);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
	return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
	return allocateAligned(size, alignment);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*unused*/) noexcept
{
	try
	{
		return allocateAligned(size, alignment);
	}
	catch (const std::bad_alloc&)
	{
		return nullptr;
	}
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*unused*/) noexcept
{
	return operator new(size, alignment, std::nothrow);
}

void operator delete(void* memory) noexcept
{
	deallocate(memory);
}

void operator delete[](void* memory) noexcept
{
	deallocate(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept
{
	deallocate(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept
{
	deallocate(memory);
}

void operator delete(void* memory, const std::nothrow_t& /*unused*/) noexcept
{
	deallocate(memory);
}

void operator delete[](void* memory, const std::nothrow_t& /*unused*/) noexcept
{
	deallocate(memory);
}

void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept
{
	deallocateAligned(memory);
}

void operator delete[](void* memory, std::align_val_t /*alignment*/) noexcept
{
	deallocateAligned(memory);
}

void operator delete(void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
	deallocateAligned(memory);
}

void operator delete[](void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
	deallocateAligned(memory);
}

void operator delete(void* memory, std::align_val_t /*alignment*/, const std::nothrow_t& /*unused*/) noexcept
{
	deallocateAligned(memory);
}

void operator delete[](void* memory, std::align_val_t /*alignment*/, const std::nothrow_t& /*unused*/) noexcept
{
	deallocateAligned(memory);
}
// NOLINTEND(readability-inconsistent-declaration-parameter-name)
