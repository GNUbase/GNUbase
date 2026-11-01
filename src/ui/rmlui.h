// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Plugin.h>
#include <RmlUi/Core/SystemInterface.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::ui
{
	class WindowSystem;

	// The RmlUi event that the element under the mouse gets when files are dropped on a window. It bubbles up, and
	// its parameters are "files" (the files' UTF-8 paths, separated by newlines), "count" (how many), and "mouse_x"
	// and "mouse_y" (where they were dropped, in the window's pixels).
	inline constexpr const char* g_fileDropEvent = "filedrop";

	// RmlUi's clock, log, clipboard and paths: elapsed time since start, messages into GNUbase's log, the window
	// system's clipboard, which text fields copy to and paste from, and links in documents resolved as file paths.
	class SystemInterface final : public Rml::SystemInterface
	{
	public:
		explicit SystemInterface(WindowSystem& windows) : m_windows(windows) {}

		double GetElapsedTime() override;
		bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;
		void SetClipboardText(const Rml::String& text) override;
		void GetClipboardText(Rml::String& text) override;
		// A link (a style sheet, an image, a template) relative to the document that has it, or an absolute path.
		void JoinPath(Rml::String& translatedPath, const Rml::String& documentPath, const Rml::String& path) override;

	private:
		WindowSystem& m_windows;
		std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
	};

	// RmlUi's files: documents, style sheets, fonts and images. RmlUi names them in UTF-8, and they are read whole
	// through std::filesystem, so paths with any characters work on every OS; give RmlUi a path as
	// path.generic_u8string(), with / between folders. Remembers which files RmlUi read and when they last changed,
	// so that documents can be loaded again when their files change (ui/livereload.h).
	class FileInterface final : public Rml::FileInterface
	{
	public:
		Rml::FileHandle Open(const Rml::String& path) override;
		void Close(Rml::FileHandle file) override;
		size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
		bool Seek(Rml::FileHandle file, long offset, int origin) override;
		size_t Tell(Rml::FileHandle file) override;
		size_t Length(Rml::FileHandle file) override;
		bool LoadFile(const Rml::String& path, Rml::String& outData) override;

		// Whether RmlUi read the file (a path as RmlUi names it, or any path to the same file).
		bool hasRead(const std::filesystem::path& file) const;

		// The files RmlUi read that changed on disk since then, each reported once per change. Files that are
		// missing, e.g. while an editor saves them, are reported once they are back.
		std::vector<std::filesystem::path> changedFiles();

	private:
		struct OpenFile
		{
			std::string data;
			size_t position = 0;
		};

		// Reads the whole file, and remembers when it last changed. False if it can't be read.
		bool read(const Rml::String& path, std::string& data);

		std::map<Rml::FileHandle, OpenFile> m_open;
		Rml::FileHandle m_nextHandle = 1;
		std::map<std::filesystem::path, std::filesystem::file_time_type> m_read; // by normal path
	};

	// RmlUi, set up for GNUbase while the object lives: Rml::Initialise and Rml::Shutdown, with FreeType for fonts.
	// One at a time. Windows' contexts (Shell) and the renderer's render interface must outlive it, since
	// Rml::Shutdown releases their resources; the window system must outlive it too. Throws std::runtime_error if
	// RmlUi can't start.
	class RmlUi
	{
	public:
		explicit RmlUi(WindowSystem& windows);
		~RmlUi();
		RmlUi(const RmlUi&) = delete;
		RmlUi& operator=(const RmlUi&) = delete;
		RmlUi(RmlUi&&) = delete;
		RmlUi& operator=(RmlUi&&) = delete;

		// Loads a TrueType or OpenType font for every window while RmlUi runs. Documents pick it by its family name,
		// weight and style; a fallback font supplies the characters that the chosen font lacks. False if it can't be
		// loaded.
		static bool loadFont(const std::filesystem::path& file, bool fallback = false);

		// Makes the element type T, derived from Rml::Element and constructed from its tag, the one that documents
		// get for the tag. Register elements before loading the documents that use them.
		template <typename T> void registerElement(const std::string& tag)
		{
			registerElement(tag, std::make_unique<Rml::ElementInstancerGeneric<T>>());
		}

		// Same, for elements that need more than their tag to be made.
		void registerElement(const std::string& tag, std::unique_ptr<Rml::ElementInstancer> instancer);

		// Registers an RmlUi plugin, e.g. one that hooks every document as it loads, until RmlUi shuts down.
		void registerPlugin(std::unique_ptr<Rml::Plugin> plugin);

		FileInterface& files() { return m_files; }

	private:
		SystemInterface m_system;
		FileInterface m_files;
		std::vector<std::unique_ptr<Rml::ElementInstancer>> m_instancers; // RmlUi uses them until Rml::Shutdown
		std::vector<std::unique_ptr<Rml::Plugin>> m_plugins;              // the same
	};
}
