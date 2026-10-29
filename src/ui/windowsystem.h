// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/displays.h"

#include <RmlUi/Core/Header.h>
#include <RmlUi/Core/Types.h>

#include <RmlUi/Core/TextInputHandler.h> // needs the two above

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// What the shell needs from the operating system's windowing: monitors, top-level windows and their events. SDL3
// provides it in the app (SdlWindowSystem); tests use a fake one with any monitors they like.
namespace gnubase::ui
{
	using WindowId = uint32_t;

	struct WindowSpec
	{
		std::string title = "GNUbase";
		Size size{1280, 800};       // in desktop coordinates; used if bounds is empty
		std::optional<Rect> bounds; // where to open it, e.g. from a workspace (#163); centred on the primary otherwise
		bool hidden = false;        // tests and offscreen screenshots
	};

	enum class WindowEventKind : uint8_t
	{
		CloseRequested,   // the user wants to close the window
		Moved,            // its position changed
		Resized,          // its size in pixels changed
		ScaleChanged,     // its display scale changed, e.g. it moved to a monitor with another scale
		Exposed,          // it needs to be drawn again
		Input,            // mouse, keyboard or touch input went to its RmlUi context
		DisplaysChanged,  // a monitor was connected, disconnected, moved or changed its scale
		Quit,             // the application should quit, e.g. Cmd+Q, the last window closed by the OS, or SIGTERM
		FilesDropped,     // files were dragged from elsewhere and dropped on the window
		FileDialogClosed, // a file dialog was answered, cancelled or failed
	};

	struct WindowEvent
	{
		WindowEvent(WindowEventKind kind, WindowId window = 0) : kind(kind), window(window) {}

		WindowEventKind kind = WindowEventKind::Exposed;
		WindowId window = 0;                      // zero for DisplaysChanged, Quit and FileDialogClosed
		Point position;                           // FilesDropped: where, in the window's pixels
		std::vector<std::filesystem::path> files; // FilesDropped: the files; FileDialogClosed: what the user chose
		uint64_t dialog = 0;                      // FileDialogClosed: the id showFileDialog was given
		std::string error;                        // FileDialogClosed: why the dialog failed; empty if it didn't
	};

	enum class FileDialogKind : uint8_t
	{
		Open,   // choose existing files
		Save,   // choose a file name to save to
		Folder, // choose a folder
	};

	struct FileFilter
	{
		std::string name;       // e.g. "Audio files"
		std::string extensions; // e.g. "wav;aif;aiff;flac", or "*" for all files
	};

	struct FileDialog
	{
		FileDialogKind kind = FileDialogKind::Open;
		std::string title;               // empty for the OS's own
		std::vector<FileFilter> filters; // Open and Save; the first is selected
		std::filesystem::path location;  // the folder to start in, or for Save the file name to suggest
		bool many = false;               // Open: the user may choose several files
		WindowId parent = 0;             // the window the dialog belongs to, if the OS supports that
	};

	class WindowSystem
	{
	public:
		virtual ~WindowSystem() = default;

		// The connected monitors, the primary one first.
		virtual std::vector<Display> displays() const = 0;

		// Opens a window where spec says. Throws std::runtime_error if it can't.
		virtual WindowId createWindow(const WindowSpec& spec) = 0;
		virtual void destroyWindow(WindowId window) = 0;

		virtual Rect bounds(WindowId window) const = 0;
		virtual void setBounds(WindowId window, const Rect& bounds) = 0;

		// The size of what the window shows, in pixels.
		virtual Size pixelSize(WindowId window) const = 0;

		// How much bigger the window's content should be than at 96 dpi: its monitor's scale, in pixels, so 2 on a
		// Retina display whether the OS counts the window's size in points or pixels.
		virtual float scale(WindowId window) const = 0;

		// The RmlUi context that the window's mouse, keyboard and touch input goes to; null for none.
		virtual void setInputTarget(WindowId window, Rml::Context* context) = 0;

		// The handler that the window's RmlUi context tells when a text field gets or loses focus: it turns the OS's
		// text input, and its input method editor (IME) for languages such as Japanese, on and off for the window.
		// Valid until the window is destroyed.
		virtual Rml::TextInputHandler* textInputHandler(WindowId window) = 0;

		// The system clipboard, as text.
		virtual std::string clipboardText() const = 0;
		virtual void setClipboardText(const std::string& text) = 0;

		// Shows the OS's file dialog without waiting for it. What the user chooses, or why the dialog failed, comes
		// later as a FileDialogClosed event with the given id.
		virtual void showFileDialog(uint64_t id, const FileDialog& dialog) = 0;

		// The next event, waiting up to timeout for one; without timeout, until one arrives. Input events are
		// delivered to the window's input target before they are reported.
		virtual std::optional<WindowEvent> nextEvent(std::optional<std::chrono::milliseconds> timeout) = 0;
	};
}
