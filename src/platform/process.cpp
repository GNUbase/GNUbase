// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/process.h"

#include "platform/rt/realtime.h"

#include <cerrno>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif
#endif

namespace gnubase::platform
{
	namespace
	{
#ifdef _WIN32
		std::wstring widen(const std::string& text)
		{
			if (text.empty())
				return {};
			const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
			std::wstring result(static_cast<size_t>(length), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
			return result;
		}

		// Quotes an argument so that the C runtime's command line parsing (CommandLineToArgvW) gives it back unchanged.
		std::wstring quote(const std::wstring& argument)
		{
			if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
				return argument;
			std::wstring result = L"\"";
			size_t backslashes = 0;
			for (const wchar_t c : argument)
			{
				if (c == L'\\')
				{
					++backslashes;
					continue;
				}
				// Backslashes are literal unless a quote follows: then each is doubled and the quote escaped.
				result.append(c == L'"' ? (backslashes * 2) + 1 : backslashes, L'\\');
				result.push_back(c);
				backslashes = 0;
			}
			result.append(backslashes * 2, L'\\');
			result.push_back(L'"');
			return result;
		}

		std::optional<int> waitForHandle(void* handle, DWORD milliseconds)
		{
			if (WaitForSingleObject(handle, milliseconds) != WAIT_OBJECT_0)
				return std::nullopt;
			DWORD code = 0;
			GetExitCodeProcess(handle, &code);
			return static_cast<int>(code);
		}
#else
		char** environment()
		{
#ifdef __APPLE__
			return *_NSGetEnviron();
#else
			return environ; // declared by <unistd.h> with _GNU_SOURCE, which g++ and clang++ define
#endif
		}

		int exitCodeOf(int status)
		{
			if (WIFEXITED(status) != 0)
				return WEXITSTATUS(status);
			if (WIFSIGNALED(status) != 0)
				return 128 + WTERMSIG(status);
			return -1;
		}
#endif
	}

	uint32_t currentProcessId() noexcept
	{
#ifdef _WIN32
		return GetCurrentProcessId();
#else
		return static_cast<uint32_t>(getpid());
#endif
	}

	bool processExists(uint32_t id) noexcept
	{
#ifdef _WIN32
		HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, id);
		if (process == nullptr)
			return GetLastError() == ERROR_ACCESS_DENIED; // it exists, but belongs to someone else
		const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
		CloseHandle(process);
		return running;
#else
		return ::kill(static_cast<pid_t>(id), 0) == 0 || errno == EPERM;
#endif
	}

	std::filesystem::path currentExecutablePath()
	{
#ifdef _WIN32
		std::wstring path(MAX_PATH, L'\0');
		for (;;)
		{
			const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
			if (length == 0)
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "GetModuleFileName");
			if (length < path.size())
			{
				path.resize(length);
				return path;
			}
			path.resize(path.size() * 2);
		}
#elif defined(__APPLE__)
		uint32_t size = 0;
		_NSGetExecutablePath(nullptr, &size);
		std::string path(size, '\0');
		if (_NSGetExecutablePath(path.data(), &size) != 0)
			throw std::system_error(ENAMETOOLONG, std::generic_category(), "_NSGetExecutablePath");
		path.resize(std::char_traits<char>::length(path.c_str()));
		return std::filesystem::canonical(path);
#else
		return std::filesystem::read_symlink("/proc/self/exe");
#endif
	}

	ChildProcess::~ChildProcess()
	{
		release();
	}

	ChildProcess::ChildProcess(ChildProcess&& other) noexcept
		: m_id(std::exchange(other.m_id, 0)), m_handle(std::exchange(other.m_handle, nullptr)),
		  m_exitCode(std::exchange(other.m_exitCode, std::nullopt))
	{
	}

	ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept
	{
		if (this != &other)
		{
			release();
			m_id = std::exchange(other.m_id, 0);
			m_handle = std::exchange(other.m_handle, nullptr);
			m_exitCode = std::exchange(other.m_exitCode, std::nullopt);
		}
		return *this;
	}

	ChildProcess ChildProcess::start(const std::filesystem::path& program, const std::vector<std::string>& arguments)
	{
		rt::assertNotRealtime("ChildProcess::start");
		ChildProcess process;
#ifdef _WIN32
		std::wstring commandLine = quote(program.wstring());
		for (const std::string& argument : arguments)
			commandLine += L" " + quote(widen(argument));
		STARTUPINFOW startup{};
		startup.cb = sizeof(startup);
		PROCESS_INFORMATION info{};
		if (CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
				&info) == FALSE)
			throw std::system_error(
				static_cast<int>(GetLastError()), std::system_category(), "Cannot start " + program.u8string());
		CloseHandle(info.hThread);
		process.m_handle = info.hProcess;
		process.m_id = info.dwProcessId;
#else
		const std::string path = program.string();
		std::vector<std::string> strings{path};
		strings.insert(strings.end(), arguments.begin(), arguments.end());
		std::vector<char*> argv;
		argv.reserve(strings.size() + 1);
		for (std::string& string : strings)
			argv.push_back(string.data());
		argv.push_back(nullptr);
		pid_t pid = 0;
		const int error = posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv.data(), environment());
		if (error != 0)
			throw std::system_error(error, std::generic_category(), "Cannot start " + path);
		process.m_id = static_cast<uint32_t>(pid);
#endif
		return process;
	}

	bool ChildProcess::running()
	{
		return m_id != 0 && !wait(std::chrono::milliseconds(0)).has_value();
	}

	std::optional<int> ChildProcess::wait(std::chrono::milliseconds timeout)
	{
		rt::assertNotRealtime("ChildProcess::wait");
		if (m_exitCode || m_id == 0)
			return m_exitCode;
#ifdef _WIN32
		m_exitCode = waitForHandle(m_handle, static_cast<DWORD>(timeout.count()));
#else
		// POSIX has no waitpid with a timeout; polling is fine outside the audio path.
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for (;;)
		{
			int status = 0;
			const pid_t result = waitpid(static_cast<pid_t>(m_id), &status, WNOHANG);
			if (result == static_cast<pid_t>(m_id))
			{
				m_exitCode = exitCodeOf(status);
				break;
			}
			if (result < 0 && errno != EINTR)
			{
				m_exitCode = -1; // not our child any more
				break;
			}
			if (std::chrono::steady_clock::now() >= deadline)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
#endif
		return m_exitCode;
	}

	void ChildProcess::kill() noexcept
	{
		rt::assertNotRealtime("ChildProcess::kill");
		if (m_id == 0 || m_exitCode)
			return;
#ifdef _WIN32
		TerminateProcess(m_handle, 1);
		m_exitCode = waitForHandle(m_handle, INFINITE);
#else
		::kill(static_cast<pid_t>(m_id), SIGKILL);
		int status = 0;
		while (waitpid(static_cast<pid_t>(m_id), &status, 0) < 0 && errno == EINTR)
		{
		}
		m_exitCode = exitCodeOf(status);
#endif
	}

	void ChildProcess::release() noexcept
	{
		kill();
#ifdef _WIN32
		if (m_handle != nullptr)
			CloseHandle(m_handle);
#endif
		m_handle = nullptr;
		m_id = 0;
		m_exitCode.reset();
	}
}
