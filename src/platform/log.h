// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "platform/rt/realtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>

namespace gnubase::logging
{
	enum class Level : uint8_t
	{
		Debug,
		Info,
		Warning,
		Error,
	};

	namespace detail
	{
		inline constexpr size_t g_maxArgs = 6;

		using Arg = std::variant<int64_t, uint64_t, double, std::string_view>;

		template <typename T> Arg toArg(const T& value)
		{
			if constexpr (std::is_same_v<T, bool>)
				return std::string_view(value ? "true" : "false");
			else if constexpr (std::is_enum_v<T> || (std::is_integral_v<T> && std::is_signed_v<T>))
				return static_cast<int64_t>(value);
			else if constexpr (std::is_integral_v<T>)
				return static_cast<uint64_t>(value);
			else if constexpr (std::is_floating_point_v<T>)
				return static_cast<double>(value);
			else
				return std::string_view(value);
		}

		void submit(Level level, std::string_view format, const Arg* args, size_t count);
		void assertionFailed(const char* condition, const char* file, int line);
	}

	// Logs format with each "{}" replaced by the next argument: a number, bool, enum or string.
	// On audio threads (see rt::ScopedRealtimeContext) the message and its strings are copied into a lock-free queue
	// without allocating, and the session's writer thread writes them. Other threads write right away.
	template <typename... Args> void write(Level level, std::string_view format, const Args&... args)
	{
		static_assert(sizeof...(Args) <= detail::g_maxArgs, "too many log arguments");
		const std::array<detail::Arg, sizeof...(Args)> values{detail::toArg(args)...};
		detail::submit(level, format, values.data(), values.size());
	}

	template <typename... Args> void debug(std::string_view format, const Args&... args)
	{
		write(Level::Debug, format, args...);
	}

	template <typename... Args> void info(std::string_view format, const Args&... args)
	{
		write(Level::Info, format, args...);
	}

	template <typename... Args> void warning(std::string_view format, const Args&... args)
	{
		write(Level::Warning, format, args...);
	}

	template <typename... Args> void error(std::string_view format, const Args&... args)
	{
		write(Level::Error, format, args...);
	}

	// Writes the messages that audio threads queued. The session's writer thread does this every few milliseconds.
	void flush();

	// The per-user log folder: %LOCALAPPDATA%\GNUbase\logs, ~/Library/Logs/GNUbase or ~/.local/state/gnubase/logs.
	std::filesystem::path defaultDirectory();

	// Logs to a file while it exists and records crashes there: a backtrace (macOS, Linux) or a note and a minidump
	// next to the file (Windows). The previous run's file is kept as <name>.previous.log. Without a session, lines go
	// to stderr. One session per process (app, each plugin host), created in main.
	class Session
	{
	public:
		explicit Session(const std::filesystem::path& file);
		~Session();
		Session(const Session&) = delete;
		Session& operator=(const Session&) = delete;

	private:
		std::atomic<bool> m_running{true};
		std::thread m_writer;
	};

	// Called when a GNUBASE_ASSERT fails. The default handler logs the failure; debug builds then abort.
	using AssertionHandler = void (*)(const char* condition, const char* file, int line);

	// Installs a handler and returns the previous one. Tests use this to check assertions without aborting.
	AssertionHandler setAssertionHandler(AssertionHandler handler) noexcept;
}

// Checks a condition in every build. A failure is logged, and debug builds stop there.
#define GNUBASE_ASSERT(condition)                                                                                      \
	do                                                                                                                 \
	{                                                                                                                  \
		if (!(condition))                                                                                              \
			::gnubase::logging::detail::assertionFailed(#condition, __FILE__, __LINE__);                               \
	} while (false)

// For code that must run on an audio thread. Code that must not calls rt::assertNotRealtime instead.
#define GNUBASE_ASSERT_REALTIME() GNUBASE_ASSERT(::gnubase::rt::isRealtimeContext())
