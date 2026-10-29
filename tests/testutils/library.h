// SPDX-License-Identifier: GPL-3.0-or-later
// A shared library loaded into the test process, for the plugin loader tests (clap_plugin_tests.cpp,
// vst3_plugin_tests.cpp).
#pragma once

#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gnubase::testutils
{
	class Library
	{
	public:
		explicit Library(const std::filesystem::path& path)
		{
#ifdef _WIN32
			m_handle = LoadLibraryW(path.c_str());
#else
			m_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
		}
		~Library()
		{
			if (m_handle == nullptr)
				return;
#ifdef _WIN32
			FreeLibrary(static_cast<HMODULE>(m_handle));
#else
			dlclose(m_handle);
#endif
		}
		Library(const Library&) = delete;
		Library& operator=(const Library&) = delete;
		Library(Library&&) = delete;
		Library& operator=(Library&&) = delete;

		bool loaded() const { return m_handle != nullptr; }
		void* handle() const { return m_handle; }

		void* symbol(const char* name) const
		{
#ifdef _WIN32
			return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(m_handle), name));
#else
			return dlsym(m_handle, name);
#endif
		}

		// A function the library exports, or nullptr.
		template <typename Function> Function function(const char* name) const
		{
			return reinterpret_cast<Function>(symbol(name));
		}

	private:
		void* m_handle = nullptr;
	};
}
