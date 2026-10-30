// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/rmlui.h"

#include "platform/log.h"
#include "ui/windowsystem.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Log.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		// The file's absolute path without "." and "..", so that every way RmlUi names a file finds the same entry.
		std::filesystem::path normal(const std::filesystem::path& file)
		{
			std::error_code error;
			const std::filesystem::path absolute = std::filesystem::absolute(file, error);
			return (error ? file : absolute).lexically_normal();
		}
	}

	double SystemInterface::GetElapsedTime()
	{
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
	}

	bool SystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message)
	{
		switch (type)
		{
		case Rml::Log::LT_ERROR:
		case Rml::Log::LT_ASSERT:
			logging::error("RmlUi: {}", message);
			break;
		case Rml::Log::LT_WARNING:
			logging::warning("RmlUi: {}", message);
			break;
		case Rml::Log::LT_INFO:
		case Rml::Log::LT_ALWAYS:
			logging::info("RmlUi: {}", message);
			break;
		default:
			logging::debug("RmlUi: {}", message);
			break;
		}
		return true; // never break into the debugger
	}

	void SystemInterface::SetClipboardText(const Rml::String& text)
	{
		m_windows.setClipboardText(text);
	}

	void SystemInterface::GetClipboardText(Rml::String& text)
	{
		text = m_windows.clipboardText();
	}

	void SystemInterface::JoinPath(
		Rml::String& translatedPath, const Rml::String& documentPath, const Rml::String& path)
	{
		const std::filesystem::path link = std::filesystem::u8path(path);
		const std::filesystem::path joined =
			link.is_absolute() ? link : std::filesystem::u8path(documentPath).parent_path() / link;
		translatedPath = joined.lexically_normal().generic_u8string();
	}

	Rml::FileHandle FileInterface::Open(const Rml::String& path)
	{
		OpenFile file;
		if (!read(path, file.data))
			return 0;
		const Rml::FileHandle handle = m_nextHandle++;
		m_open.emplace(handle, std::move(file));
		return handle;
	}

	void FileInterface::Close(Rml::FileHandle file)
	{
		m_open.erase(file);
	}

	size_t FileInterface::Read(void* buffer, size_t size, Rml::FileHandle file)
	{
		const auto it = m_open.find(file);
		if (it == m_open.end())
			return 0;
		OpenFile& open = it->second;
		const size_t count = std::min(size, open.data.size() - open.position);
		std::memcpy(buffer, open.data.data() + open.position, count);
		open.position += count;
		return count;
	}

	bool FileInterface::Seek(Rml::FileHandle file, long offset, int origin)
	{
		const auto it = m_open.find(file);
		if (it == m_open.end())
			return false;
		OpenFile& open = it->second;
		long long position = offset;
		if (origin == SEEK_CUR)
			position += static_cast<long long>(open.position);
		else if (origin == SEEK_END)
			position += static_cast<long long>(open.data.size());
		else if (origin != SEEK_SET)
			return false;
		if (position < 0 || position > static_cast<long long>(open.data.size()))
			return false;
		open.position = static_cast<size_t>(position);
		return true;
	}

	size_t FileInterface::Tell(Rml::FileHandle file)
	{
		const auto it = m_open.find(file);
		return it == m_open.end() ? 0 : it->second.position;
	}

	size_t FileInterface::Length(Rml::FileHandle file)
	{
		const auto it = m_open.find(file);
		return it == m_open.end() ? 0 : it->second.data.size();
	}

	bool FileInterface::LoadFile(const Rml::String& path, Rml::String& outData)
	{
		return read(path, outData);
	}

	bool FileInterface::read(const Rml::String& path, std::string& data)
	{
		const std::filesystem::path file = normal(std::filesystem::u8path(path));
		std::error_code error;
		if (!std::filesystem::is_regular_file(file, error))
			return false;
		// The time first: a change while the file is read shows up as a later change.
		const std::filesystem::file_time_type changed = std::filesystem::last_write_time(file, error);
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return false;
		data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
		if (in.bad())
			return false;
		if (!error)
			m_read.insert_or_assign(file, changed);
		return true;
	}

	bool FileInterface::hasRead(const std::filesystem::path& file) const
	{
		return m_read.count(normal(file)) != 0;
	}

	std::vector<std::filesystem::path> FileInterface::changedFiles()
	{
		std::vector<std::filesystem::path> changed;
		for (auto& [file, time] : m_read)
		{
			std::error_code error;
			const std::filesystem::file_time_type now = std::filesystem::last_write_time(file, error);
			if (error || now == time)
				continue; // unchanged, or missing for now
			time = now;
			changed.push_back(file);
		}
		return changed;
	}

	RmlUi::RmlUi(WindowSystem& windows) : m_system(windows)
	{
		Rml::SetSystemInterface(&m_system);
		Rml::SetFileInterface(&m_files);
		if (!Rml::Initialise())
			throw std::runtime_error("RmlUi cannot start");
		Rml::RegisterEventType(g_fileDropEvent, true, true, Rml::DefaultActionPhase::None);
	}

	RmlUi::~RmlUi()
	{
		Rml::Shutdown();
	}

	bool RmlUi::loadFont(const std::filesystem::path& file, bool fallback)
	{
		return Rml::LoadFontFace(file.generic_u8string(), fallback);
	}

	void RmlUi::registerElement(const std::string& tag, std::unique_ptr<Rml::ElementInstancer> instancer)
	{
		Rml::Factory::RegisterElementInstancer(tag, instancer.get());
		m_instancers.push_back(std::move(instancer));
	}

	void RmlUi::registerPlugin(std::unique_ptr<Rml::Plugin> plugin)
	{
		Rml::RegisterPlugin(plugin.get());
		m_plugins.push_back(std::move(plugin));
	}
}
