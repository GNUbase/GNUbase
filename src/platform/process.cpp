// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/process.h"

#include "platform/rt/realtime.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
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
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

// posix_spawn_file_actions_addclosefrom_np came with glibc 2.34; elsewhere on Linux the descriptors to close are listed
// one by one.
#if defined(__linux__) && !(defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34)))
#define GNUBASE_LIST_DESCRIPTORS_TO_CLOSE 1
#include <cstdlib>
#include <dirent.h>
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

		[[noreturn]] void throwLastError(const std::string& what)
		{
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), what);
		}

		// Closes a handle when it goes out of scope.
		class OwnedHandle
		{
		public:
			explicit OwnedHandle(HANDLE handle) noexcept : m_handle(handle == INVALID_HANDLE_VALUE ? nullptr : handle)
			{
			}
			~OwnedHandle()
			{
				if (m_handle != nullptr)
					CloseHandle(m_handle);
			}
			OwnedHandle(const OwnedHandle&) = delete;
			OwnedHandle& operator=(const OwnedHandle&) = delete;
			OwnedHandle(OwnedHandle&&) = delete;
			OwnedHandle& operator=(OwnedHandle&&) = delete;

			HANDLE get() const noexcept { return m_handle; }

		private:
			HANDLE m_handle;
		};

		// The job every child process belongs to. It ends them all when its last handle, this one, closes: when
		// GNUbase ends or crashes. Null if the system refuses a job.
		HANDLE childJob() noexcept
		{
			static void* const job = []() noexcept
			{
				HANDLE created = CreateJobObjectW(nullptr, nullptr);
				if (created == nullptr)
					return created;
				JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
				limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
				if (SetInformationJobObject(created, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ==
					FALSE)
				{
					CloseHandle(created);
					return HANDLE{nullptr};
				}
				return created;
			}();
			return job;
		}

		// A thread attribute list for CreateProcess, deleted when it goes out of scope.
		class AttributeList
		{
		public:
			explicit AttributeList(DWORD count)
			{
				SIZE_T size = 0;
				InitializeProcThreadAttributeList(nullptr, count, 0, &size);
				m_storage.resize(size);
				if (InitializeProcThreadAttributeList(get(), count, 0, &size) == FALSE)
					throwLastError("Cannot set up a process's attributes");
			}
			~AttributeList() { DeleteProcThreadAttributeList(get()); }
			AttributeList(const AttributeList&) = delete;
			AttributeList& operator=(const AttributeList&) = delete;
			AttributeList(AttributeList&&) = delete;
			AttributeList& operator=(AttributeList&&) = delete;

			LPPROC_THREAD_ATTRIBUTE_LIST get() noexcept
			{
				return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_storage.data());
			}

		private:
			std::vector<std::byte> m_storage;
		};
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

		void check(int error, const std::string& what)
		{
			if (error != 0)
				throw std::system_error(error, std::generic_category(), what);
		}

		// The file actions and attributes of posix_spawn, destroyed when they go out of scope.
		struct SpawnSetup
		{
			SpawnSetup()
			{
				check(posix_spawn_file_actions_init(&actions), "posix_spawn_file_actions_init");
				const int error = posix_spawnattr_init(&attributes);
				if (error != 0)
				{
					posix_spawn_file_actions_destroy(&actions);
					check(error, "posix_spawnattr_init");
				}
			}
			~SpawnSetup()
			{
				posix_spawnattr_destroy(&attributes);
				posix_spawn_file_actions_destroy(&actions);
			}
			SpawnSetup(const SpawnSetup&) = delete;
			SpawnSetup& operator=(const SpawnSetup&) = delete;
			SpawnSetup(SpawnSetup&&) = delete;
			SpawnSetup& operator=(SpawnSetup&&) = delete;

			posix_spawn_file_actions_t actions{};
			posix_spawnattr_t attributes{};
		};

#ifdef GNUBASE_LIST_DESCRIPTORS_TO_CLOSE
		// Closes in the child every descriptor above standard error that it would inherit. One that another thread
		// closes meanwhile does no harm: glibc and musl ignore EBADF for these actions.
		void closeInheritedDescriptors(posix_spawn_file_actions_t& actions)
		{
			DIR* directory = opendir("/proc/self/fd");
			if (directory == nullptr)
				return;
			const int own = dirfd(directory);
			while (const dirent* entry = readdir(directory))
			{
				char* end = nullptr;
				const long number = std::strtol(entry->d_name, &end, 10);
				if (end == entry->d_name || *end != '\0' || number <= 2 || number == own)
					continue;
				const auto descriptor = static_cast<int>(number);
				const int flags = fcntl(descriptor, F_GETFD);
				if (flags >= 0 && (flags & FD_CLOEXEC) == 0 &&
					posix_spawn_file_actions_addclose(&actions, descriptor) != 0)
					break; // out of memory: the child inherits the rest
			}
			closedir(directory);
		}
#endif

		// Collects a killed process that did not go in time, so it does not stay a zombie.
		void reapLater(pid_t pid) noexcept
		{
			try
			{
				std::thread(
					[pid]
					{
						int status = 0;
						while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
						{
						}
					})
					.detach();
			}
			catch (...) // NOLINT(bugprone-empty-catch): without a thread the zombie stays until GNUbase ends
			{
			}
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

	void endWithParent([[maybe_unused]] uint32_t parentId) noexcept
	{
#ifdef __linux__
		if (static_cast<uint32_t>(getppid()) != parentId)
			return;
		prctl(PR_SET_PDEATHSIG, SIGKILL);
		// Had the parent ended just before, the signal would never come; ProcessWatch notices that case.
#endif
	}

	ProcessWatch::ProcessWatch(uint32_t id) : m_id(id)
	{
#ifdef _WIN32
		m_handle = OpenProcess(SYNCHRONIZE, FALSE, id);
		m_goneAtStart = m_handle == nullptr && GetLastError() == ERROR_INVALID_PARAMETER;
#else
		m_parent = static_cast<uint32_t>(getppid()) == id;
		m_goneAtStart = !processExists(id);
#endif
	}

	ProcessWatch::~ProcessWatch()
	{
		release();
	}

	ProcessWatch::ProcessWatch(ProcessWatch&& other) noexcept
		: m_id(std::exchange(other.m_id, 0)), m_handle(std::exchange(other.m_handle, nullptr)),
		  m_parent(std::exchange(other.m_parent, false)), m_goneAtStart(std::exchange(other.m_goneAtStart, false))
	{
	}

	ProcessWatch& ProcessWatch::operator=(ProcessWatch&& other) noexcept
	{
		if (this != &other)
		{
			release();
			m_id = std::exchange(other.m_id, 0);
			m_handle = std::exchange(other.m_handle, nullptr);
			m_parent = std::exchange(other.m_parent, false);
			m_goneAtStart = std::exchange(other.m_goneAtStart, false);
		}
		return *this;
	}

	bool ProcessWatch::gone() const noexcept
	{
		if (m_goneAtStart)
			return true;
		if (m_id == 0)
			return false;
#ifdef _WIN32
		return m_handle != nullptr && WaitForSingleObject(m_handle, 0) == WAIT_OBJECT_0;
#else
		// An orphan gets a new parent, so a reused process id cannot fool this.
		if (m_parent)
			return static_cast<uint32_t>(getppid()) != m_id;
		return ::kill(static_cast<pid_t>(m_id), 0) != 0 && errno == ESRCH;
#endif
	}

	void ProcessWatch::release() noexcept
	{
#ifdef _WIN32
		if (m_handle != nullptr)
			CloseHandle(m_handle);
#endif
		m_handle = nullptr;
		m_id = 0;
		m_parent = false;
		m_goneAtStart = false;
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

	ChildProcess ChildProcess::start(const std::filesystem::path& program, const std::vector<std::string>& arguments,
		const std::filesystem::path& errorOutput)
	{
		rt::assertNotRealtime("ChildProcess::start");
		ChildProcess process;
#ifdef _WIN32
		std::wstring commandLine = quote(program.wstring());
		for (const std::string& argument : arguments)
			commandLine += L" " + quote(widen(argument));

		// Standard input and output on the null device, standard error to the file or to GNUbase's own. The child
		// inherits these handles and no others.
		SECURITY_ATTRIBUTES inheritable{};
		inheritable.nLength = sizeof(inheritable);
		inheritable.bInheritHandle = TRUE;
		const OwnedHandle null(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			&inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (null.get() == nullptr)
			throwLastError("Cannot open the null device");
		HANDLE errorHandle = nullptr;
		if (!errorOutput.empty())
		{
			errorHandle = CreateFileW(errorOutput.c_str(), FILE_APPEND_DATA,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inheritable, OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (errorHandle == INVALID_HANDLE_VALUE)
				throwLastError("Cannot open " + errorOutput.u8string());
		}
		else if (HANDLE own = GetStdHandle(STD_ERROR_HANDLE); own != nullptr && own != INVALID_HANDLE_VALUE)
		{
			if (DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &errorHandle, 0, TRUE,
					DUPLICATE_SAME_ACCESS) == FALSE)
				errorHandle = nullptr;
		}
		const OwnedHandle error(errorHandle);
		std::array<HANDLE, 2> inherited{null.get(), error.get() != nullptr ? error.get() : null.get()};
		const DWORD inheritedCount = error.get() != nullptr ? 2 : 1;

		AttributeList attributes(1);
		if (UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
				static_cast<void*>(inherited.data()), inheritedCount * sizeof(HANDLE), nullptr, nullptr) == FALSE)
			throwLastError("Cannot set up the handles of " + program.u8string());
		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdInput = null.get();
		startup.StartupInfo.hStdOutput = null.get();
		startup.StartupInfo.hStdError = inherited[1];
		startup.lpAttributeList = attributes.get();
		PROCESS_INFORMATION info{};
		// Suspended until it is in the job, so that nothing it starts escapes the job. No console window: plugin hosts
		// are console programs, and GNUbase is not.
		if (CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
				&startup.StartupInfo, &info) == FALSE)
			throwLastError("Cannot start " + program.u8string());
		process.m_handle = info.hProcess;
		process.m_id = info.dwProcessId;
		const OwnedHandle thread(info.hThread);
		if (HANDLE job = childJob(); job != nullptr)
			AssignProcessToJobObject(job, info.hProcess); // if the system refuses, ~ChildProcess still ends it
		if (ResumeThread(thread.get()) == static_cast<DWORD>(-1))
			throwLastError("Cannot start " + program.u8string()); // ~ChildProcess kills it
#else
		const std::string path = program.string();
		std::vector<std::string> strings{path};
		strings.insert(strings.end(), arguments.begin(), arguments.end());
		std::vector<char*> argv;
		argv.reserve(strings.size() + 1);
		for (std::string& string : strings)
			argv.push_back(string.data());
		argv.push_back(nullptr);

		// Standard input and output on /dev/null (in headless mode stdout carries MCP), standard error to the file or
		// GNUbase's own, and no other descriptors, such as the log file's.
		SpawnSetup setup;
		check(posix_spawn_file_actions_addopen(&setup.actions, 0, "/dev/null", O_RDONLY, 0), "Cannot open /dev/null");
		check(posix_spawn_file_actions_addopen(&setup.actions, 1, "/dev/null", O_WRONLY, 0), "Cannot open /dev/null");
		const std::string errorPath = errorOutput.string();
		if (!errorOutput.empty())
			check(posix_spawn_file_actions_addopen(
					  &setup.actions, 2, errorPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644),
				"Cannot open " + errorPath);
#ifdef __APPLE__
		// The child gets only the descriptors the file actions name.
		if (errorOutput.empty())
			check(posix_spawn_file_actions_addinherit_np(&setup.actions, 2), "posix_spawn_file_actions_addinherit_np");
		check(posix_spawnattr_setflags(&setup.attributes, static_cast<short>(POSIX_SPAWN_CLOEXEC_DEFAULT)),
			"posix_spawnattr_setflags");
#elif defined(GNUBASE_LIST_DESCRIPTORS_TO_CLOSE)
		closeInheritedDescriptors(setup.actions);
#else
		check(posix_spawn_file_actions_addclosefrom_np(&setup.actions, 3), "posix_spawn_file_actions_addclosefrom_np");
#endif
		pid_t pid = 0;
		const int error =
			posix_spawn(&pid, path.c_str(), &setup.actions, &setup.attributes, argv.data(), environment());
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

	void ChildProcess::kill(std::chrono::milliseconds timeout) noexcept
	{
		rt::assertNotRealtime("ChildProcess::kill");
		if (m_id == 0 || m_exitCode)
			return;
#ifdef _WIN32
		TerminateProcess(m_handle, 1);
		m_exitCode = waitForHandle(m_handle, static_cast<DWORD>(std::max<int64_t>(timeout.count(), 0)));
		// Otherwise it ends whenever the kernel lets it: Windows leaves no zombie to collect.
		if (!m_exitCode)
			m_exitCode = -1;
#else
		const auto pid = static_cast<pid_t>(m_id);
		::kill(pid, SIGKILL);
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		for (;;)
		{
			int status = 0;
			const pid_t result = waitpid(pid, &status, WNOHANG);
			if (result == pid)
			{
				m_exitCode = exitCodeOf(status);
				return;
			}
			if (result < 0 && errno != EINTR)
			{
				m_exitCode = -1; // not our child any more
				return;
			}
			if (std::chrono::steady_clock::now() >= deadline)
				break;
			usleep(1000); // not std::this_thread::sleep_for, which may throw
		}
		reapLater(pid);
		m_exitCode = -1;
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
