// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/log.h"

#include "platform/crashhandler.h"
#include "platform/rt/mpmcqueue.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace gnubase::logging
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// A string copied into Message::text.
		struct StoredText
		{
			uint16_t offset = 0;
			uint16_t length = 0;
		};

		// A message from an audio thread. It holds copies of the format and the string arguments, so the caller's
		// strings may change or go away before the writer thread formats it.
		struct Message
		{
			Level level = Level::Info;
			Clock::time_point time;
			StoredText format;
			size_t argCount = 0;
			std::array<std::variant<int64_t, uint64_t, double, StoredText>, detail::g_maxArgs> args{};
			std::array<char, 256> text{};
		};

		const Clock::time_point g_start = Clock::now();
		// NOLINTNEXTLINE(bugprone-throwing-static-initialization): allocated before main, never on audio threads
		rt::MpmcQueue<Message> g_queue(1024);
		std::atomic<uint64_t> g_dropped{0};
		std::atomic<bool> g_sessionRunning{false};
		std::mutex g_mutex; // guards g_file and keeps lines whole
		std::FILE* g_file = nullptr;

		// Copies text into the message, cut off when the message is full.
		StoredText store(Message& message, size_t& used, std::string_view text)
		{
			const std::string_view part = text.substr(0, message.text.size() - used);
			std::copy(part.begin(), part.end(), message.text.data() + used);
			const StoredText stored{static_cast<uint16_t>(used), static_cast<uint16_t>(part.size())};
			used += part.size();
			return stored;
		}

		std::string_view view(const Message& message, StoredText stored)
		{
			return {message.text.data() + stored.offset, stored.length};
		}

		void append(std::string& text, int64_t value)
		{
			text += std::to_string(value);
		}

		void append(std::string& text, uint64_t value)
		{
			text += std::to_string(value);
		}

		void append(std::string& text, double value)
		{
			std::ostringstream stream;
			stream.imbue(std::locale::classic());
			stream << value;
			text += stream.str();
		}

		void append(std::string& text, std::string_view value)
		{
			text += value;
		}

		std::string formatMessage(std::string_view format, const detail::Arg* args, size_t count)
		{
			std::string text;
			size_t next = 0;
			for (size_t i = 0; i < format.size(); ++i)
			{
				if (next < count && format.compare(i, 2, "{}") == 0)
				{
					std::visit([&text](auto value) { append(text, value); }, args[next++]);
					++i;
				}
				else
				{
					text += format[i];
				}
			}
			return text;
		}

		const char* levelName(Level level)
		{
			switch (level)
			{
			case Level::Debug:
				return "debug";
			case Level::Info:
				return "info";
			case Level::Warning:
				return "warning";
			case Level::Error:
				return "error";
			}
			return "";
		}

		// The caller holds g_mutex.
		void writeLine(Level level, Clock::time_point time, const std::string& text)
		{
			const double seconds = std::chrono::duration<double>(time - g_start).count();
			std::fprintf(
				g_file != nullptr ? g_file : stderr, "%10.3f %-7s %s\n", seconds, levelName(level), text.c_str());
		}

		std::string localTime()
		{
			const std::time_t now = std::time(nullptr);
			std::tm local{};
#ifdef _WIN32
			localtime_s(&local, &now);
#else
			localtime_r(&now, &local);
#endif
			std::ostringstream text;
			text << std::put_time(&local, "%Y-%m-%d %H:%M:%S");
			return text.str();
		}

		void defaultAssertionHandler(const char* condition, const char* file, int line)
		{
			error("Assertion failed: {} ({}:{})", condition, file, line);
#ifndef NDEBUG
			// Debug builds stop at the failure. On audio threads the message above is only queued, so print it too.
			std::fprintf(stderr, "Assertion failed: %s (%s:%d)\n", condition, file, line);
			std::abort();
#endif
		}

		std::atomic<AssertionHandler> g_assertionHandler{&defaultAssertionHandler};
	}

	namespace detail
	{
		void submit(Level level, std::string_view format, const Arg* args, size_t count)
		{
			if (!rt::isRealtimeContext())
			{
				const std::string text = formatMessage(format, args, count);
				const std::scoped_lock lock(g_mutex);
				writeLine(level, Clock::now(), text);
				return;
			}

			Message message;
			message.level = level;
			message.time = Clock::now();
			size_t used = 0;
			message.format = store(message, used, format);
			message.argCount = count;
			for (size_t i = 0; i < count; ++i)
			{
				std::visit(
					[&](auto value)
					{
						if constexpr (std::is_same_v<decltype(value), std::string_view>)
							message.args[i] = store(message, used, value);
						else
							message.args[i] = value;
					},
					args[i]);
			}
			if (!g_queue.tryPush(message))
				g_dropped.fetch_add(1, std::memory_order_relaxed);
		}

		void assertionFailed(const char* condition, const char* file, int line)
		{
			g_assertionHandler.load()(condition, file, line);
		}
	}

	void flush()
	{
		rt::assertNotRealtime("logging::flush");
		const std::scoped_lock lock(g_mutex);
		Message message;
		while (g_queue.tryPop(message))
		{
			std::array<detail::Arg, detail::g_maxArgs> args{};
			for (size_t i = 0; i < message.argCount; ++i)
			{
				std::visit(
					[&](auto value)
					{
						if constexpr (std::is_same_v<decltype(value), StoredText>)
							args[i] = view(message, value);
						else
							args[i] = value;
					},
					message.args[i]);
			}
			writeLine(message.level, message.time,
				formatMessage(view(message, message.format), args.data(), message.argCount));
		}
		if (const uint64_t dropped = g_dropped.exchange(0); dropped > 0)
			writeLine(Level::Warning, Clock::now(),
				"Dropped " + std::to_string(dropped) + " log messages from audio threads");
	}

	std::filesystem::path defaultDirectory()
	{
#ifdef _WIN32
		if (const wchar_t* appData = _wgetenv(L"LOCALAPPDATA"); appData != nullptr)
			return std::filesystem::path(appData) / "GNUbase" / "logs";
#elif defined(__APPLE__)
		if (const char* home = std::getenv("HOME"); home != nullptr)
			return std::filesystem::path(home) / "Library" / "Logs" / "GNUbase";
#else
		if (const char* state = std::getenv("XDG_STATE_HOME"); state != nullptr && *state != '\0')
			return std::filesystem::path(state) / "gnubase" / "logs";
		if (const char* home = std::getenv("HOME"); home != nullptr)
			return std::filesystem::path(home) / ".local" / "state" / "gnubase" / "logs";
#endif
		return std::filesystem::temp_directory_path() / "gnubase" / "logs";
	}

	Session::Session(const std::filesystem::path& file)
	{
		rt::assertNotRealtime("logging::Session");
		if (g_sessionRunning.exchange(true))
			throw std::logic_error("a log session is already running");
		{
			const std::scoped_lock lock(g_mutex);
			std::error_code ignored;
			std::filesystem::create_directories(file.parent_path(), ignored);
			std::filesystem::path previous = file;
			previous.replace_extension(".previous" + file.extension().string());
			std::filesystem::rename(file, previous, ignored); // keeps the last run's log, if there is one
#ifdef _WIN32
			g_file = _wfopen(file.c_str(), L"w");
#else
			g_file = std::fopen(file.c_str(), "w");
#endif
			// Unbuffered, so every line is in the file even if the process crashes right after writing it.
			if (g_file != nullptr)
				std::setvbuf(g_file, nullptr, _IONBF, 0);
		}
		if (g_file == nullptr)
			warning("Cannot write the log file {}, logging to stderr", file.string());
		info("Log started {}", localTime());
		detail::installCrashHandler(g_file, file);
		m_writer = std::thread(
			[this]
			{
				while (m_running)
				{
					flush();
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
				flush(); // what audio threads logged until the session ended
			});
	}

	Session::~Session()
	{
		m_running = false;
		m_writer.join();
		detail::uninstallCrashHandler();
		const std::scoped_lock lock(g_mutex);
		if (g_file != nullptr)
			std::fclose(g_file);
		g_file = nullptr;
		g_sessionRunning = false;
	}

	AssertionHandler setAssertionHandler(AssertionHandler handler) noexcept
	{
		return g_assertionHandler.exchange(handler);
	}
}
