// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/sdlwindowsystem.h"

#include "platform/log.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/TextInputContext.h>
#include <RmlUi_Platform_SDL.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::runtime_error sdlError(const std::string& what)
		{
			return std::runtime_error(what + ": " + SDL_GetError());
		}

		Rect toRect(const SDL_Rect& rect)
		{
			return {rect.x, rect.y, rect.w, rect.h};
		}

		bool isInput(Uint32 type)
		{
			switch (type)
			{
			case SDL_EVENT_MOUSE_MOTION:
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
			case SDL_EVENT_MOUSE_WHEEL:
			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
			case SDL_EVENT_TEXT_INPUT:
			case SDL_EVENT_FINGER_DOWN:
			case SDL_EVENT_FINGER_UP:
			case SDL_EVENT_FINGER_MOTION:
			case SDL_EVENT_WINDOW_MOUSE_LEAVE:
			case SDL_EVENT_WINDOW_FOCUS_LOST:
				return true;
			default:
				return false;
			}
		}

		// The OS's pointers for the CSS names in g_systemCursors, in the same order.
		constexpr std::array<SDL_SystemCursor, g_systemCursors.size()> g_sdlCursors = {SDL_SYSTEM_CURSOR_CROSSHAIR,
			SDL_SYSTEM_CURSOR_EW_RESIZE, SDL_SYSTEM_CURSOR_MOVE, SDL_SYSTEM_CURSOR_NESW_RESIZE,
			SDL_SYSTEM_CURSOR_NOT_ALLOWED, SDL_SYSTEM_CURSOR_NS_RESIZE, SDL_SYSTEM_CURSOR_NWSE_RESIZE,
			SDL_SYSTEM_CURSOR_POINTER, SDL_SYSTEM_CURSOR_PROGRESS, SDL_SYSTEM_CURSOR_TEXT, SDL_SYSTEM_CURSOR_WAIT};
		static_assert(g_sdlCursors.back() != SDL_SYSTEM_CURSOR_DEFAULT, "a pointer for every name in g_systemCursors");

		// The OS's pointer for a CSS cursor name.
		std::optional<SDL_SystemCursor> systemCursor(const std::string& name)
		{
			for (size_t index = 0; index < g_systemCursors.size(); ++index)
				if (name == g_systemCursors.at(index))
					return g_sdlCursors.at(index);
			return std::nullopt;
		}

		// An SDL surface over the picture's pixels, which SDL only reads.
		SDL_Surface* surfaceOf(const CursorImage& image)
		{
			// NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): SDL's surfaces take non-const pixels
			auto* pixels = const_cast<uint8_t*>(image.pixels.data());
			SDL_Surface* surface =
				SDL_CreateSurfaceFrom(image.width, image.height, SDL_PIXELFORMAT_RGBA32, pixels, image.width * 4);
			if (surface == nullptr)
				return nullptr;
			// Each scale's own hotspot, which SDL takes for that scale's picture (else the first one's, unscaled).
			const SDL_PropertiesID properties = SDL_GetSurfaceProperties(surface);
			SDL_SetNumberProperty(properties, SDL_PROP_SURFACE_HOTSPOT_X_NUMBER, image.hotspot.x);
			SDL_SetNumberProperty(properties, SDL_PROP_SURFACE_HOTSPOT_Y_NUMBER, image.hotspot.y);
			return surface;
		}

		// A file dialog that is open: what SDL needs until the dialog calls back.
		struct DialogRequest
		{
			uint32_t eventType = 0;
			uint64_t id = 0;
			std::vector<std::string> patterns;         // the filters' extensions, which filters points into
			std::vector<SDL_DialogFileFilter> filters; // SDL keeps a pointer to them until it calls back
		};

		// What a file dialog returned, on its way to the main thread as an SDL event.
		struct DialogResult
		{
			uint64_t id = 0;
			std::vector<std::filesystem::path> files;
			std::string error;
		};

		// SDL calls this when the dialog is answered, on the main thread or another one, depending on the OS: it
		// posts the result as an event, which nextEvent turns into a FileDialogClosed.
		void SDLCALL dialogClosed(void* userdata, const char* const* files, int /*filter*/)
		{
			const std::unique_ptr<DialogRequest> request(static_cast<DialogRequest*>(userdata));
			auto result = std::make_unique<DialogResult>();
			result->id = request->id;
			if (files == nullptr)
				result->error = SDL_GetError();
			else
				for (const char* const* file = files; *file != nullptr; ++file)
					result->files.push_back(std::filesystem::u8path(*file));
			SDL_Event event{};
			event.type = request->eventType;
			event.user.data1 = result.release(); // nextEvent takes it back
			if (!SDL_PushEvent(&event))
				const std::unique_ptr<DialogResult> lost(static_cast<DialogResult*>(event.user.data1));
		}

		SDL_FileDialogType dialogType(FileDialogKind kind)
		{
			switch (kind)
			{
			case FileDialogKind::Save:
				return SDL_FILEDIALOG_SAVEFILE;
			case FileDialogKind::Folder:
				return SDL_FILEDIALOG_OPENFOLDER;
			default:
				return SDL_FILEDIALOG_OPENFILE;
			}
		}

		// The window's pixels per window coordinate: 2 on a Retina display, 1 where windows count in pixels.
		float pixelDensity(SDL_Window* window)
		{
			const float density = SDL_GetWindowPixelDensity(window);
			return density > 0.0F ? density : 1.0F;
		}

		// What an SDL event means to the shell; empty for events it doesn't care about.
		std::optional<WindowEventKind> kindOf(Uint32 type)
		{
			switch (type)
			{
			case SDL_EVENT_QUIT:
				return WindowEventKind::Quit;
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
				return WindowEventKind::CloseRequested;
			case SDL_EVENT_WINDOW_MOVED:
				return WindowEventKind::Moved;
			case SDL_EVENT_WINDOW_RESIZED:
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				return WindowEventKind::Resized;
			case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
			case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
				return WindowEventKind::ScaleChanged;
			case SDL_EVENT_WINDOW_SHOWN:
			case SDL_EVENT_WINDOW_EXPOSED:
			case SDL_EVENT_WINDOW_RESTORED:
			case SDL_EVENT_WINDOW_MAXIMIZED:
				return WindowEventKind::Exposed;
			case SDL_EVENT_DISPLAY_ADDED:
			case SDL_EVENT_DISPLAY_REMOVED:
			case SDL_EVENT_DISPLAY_MOVED:
			case SDL_EVENT_DISPLAY_ORIENTATION:
			case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
			case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
			case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
				return WindowEventKind::DisplaysChanged;
			default:
				return std::nullopt;
			}
		}
	}

	// Turns SDL's text input on for a window while one of its text fields has the focus, places the IME's candidate
	// window at the field, and hands the IME's compositions to the field through RmlUi's own SDL IME code.
	class SdlTextInput final : public Rml::TextInputHandler
	{
	public:
		explicit SdlTextInput(SDL_Window* window) : m_window(window) {}

		void OnActivate(Rml::TextInputContext* input) override
		{
			m_active = input;
			m_ime.OnActivate(input);
			Rml::Rectanglef box;
			if (input->GetBoundingBox(box))
			{
				const float density = pixelDensity(m_window);
				const SDL_Rect area{static_cast<int>(box.Left() / density), static_cast<int>(box.Top() / density),
					static_cast<int>(std::ceil(box.Width() / density)),
					static_cast<int>(std::ceil(box.Height() / density))};
				SDL_SetTextInputArea(m_window, &area, 0);
			}
			SDL_StartTextInput(m_window);
		}

		void OnDeactivate(Rml::TextInputContext* input) override
		{
			m_ime.OnDeactivate(input);
			if (input != m_active)
				return; // another field took the focus first
			m_active = nullptr;
			SDL_StopTextInput(m_window);
		}

		void OnDestroy(Rml::TextInputContext* input) override
		{
			m_ime.OnDestroy(input);
			if (input == m_active)
				m_active = nullptr;
		}

		// The IME's text while the user composes it, e.g. kana before they become kanji.
		void edit(const SDL_TextEditingEvent& event) { m_ime.HandleEdit(event); }

	private:
		SDL_Window* m_window;
		TextInputMethodEditor_SDL m_ime;
		Rml::TextInputContext* m_active = nullptr;
	};

	SdlWindowSystem::SdlWindowSystem()
	{
		// On Windows, mouse pointers with pictures for several scales show the one for the monitor's scale.
		SDL_SetHint(SDL_HINT_MOUSE_DPI_SCALE_CURSORS, "1");
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
			throw sdlError("SDL cannot start its video subsystem");
		m_dialogEvent = SDL_RegisterEvents(1);
		m_wakeEvent = SDL_RegisterEvents(1);
	}

	SdlWindowSystem::~SdlWindowSystem()
	{
		// Dialog results that nobody took.
		SDL_Event event;
		while (m_dialogEvent != 0 && SDL_PeepEvents(&event, 1, SDL_GETEVENT, m_dialogEvent, m_dialogEvent) == 1)
			const std::unique_ptr<DialogResult> unused(static_cast<DialogResult*>(event.user.data1));
		SDL_SetCursor(SDL_GetDefaultCursor());
		m_cursors.clear();
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
	}

	const char* SdlWindowSystem::driver()
	{
		const char* name = SDL_GetCurrentVideoDriver();
		return name != nullptr ? name : "";
	}

	int SdlWindowSystem::ask(const std::string& title, const std::string& message,
		const std::vector<std::string>& buttons, Severity severity)
	{
		std::vector<SDL_MessageBoxButtonData> data;
		data.reserve(buttons.size());
		for (size_t i = 0; i < buttons.size(); ++i)
			data.push_back(
				{i == 0 ? SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT : 0U,
					static_cast<int>(i), buttons[i].c_str()});
		const SDL_MessageBoxData box{
			severity == Severity::Warning ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION, nullptr, title.c_str(),
			message.c_str(), static_cast<int>(data.size()), data.data(), nullptr};
		int chosen = -1;
		return SDL_ShowMessageBox(&box, &chosen) ? chosen : -1;
	}

	std::vector<Display> SdlWindowSystem::displays() const
	{
		std::vector<Display> result;
		int count = 0;
		SDL_DisplayID* ids = SDL_GetDisplays(&count);
		if (ids == nullptr)
			return result;
		const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
		for (int i = 0; i < count; ++i)
		{
			Display display;
			display.id = ids[i];
			// SDL names a display the OS gives no name, e.g. the offscreen driver's, after its ID, which changes from
			// one start to the next: such a display has no name that a saved layout could find it by again (#163).
			const char* name = SDL_GetDisplayName(ids[i]);
			display.name = name != nullptr && std::to_string(ids[i]) != name ? name : "";
			SDL_Rect rect;
			if (SDL_GetDisplayBounds(ids[i], &rect))
				display.bounds = toRect(rect);
			display.usable = SDL_GetDisplayUsableBounds(ids[i], &rect) ? toRect(rect) : display.bounds;
			const float scale = SDL_GetDisplayContentScale(ids[i]);
			display.scale = scale > 0.0F ? scale : 1.0F;
			result.push_back(display);
		}
		SDL_free(ids);
		std::stable_partition(result.begin(), result.end(), [primary](const Display& d) { return d.id == primary; });
		return result;
	}

	WindowId SdlWindowSystem::createWindow(const WindowSpec& spec)
	{
		const Rect bounds = spec.bounds.value_or(Rect{0, 0, spec.size.width, spec.size.height});
		const SDL_PropertiesID properties = SDL_CreateProperties();
		SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING, spec.title.c_str());
		if (spec.bounds)
		{
			SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_X_NUMBER, bounds.x);
			SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_Y_NUMBER, bounds.y);
		}
		SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, bounds.width);
		SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, bounds.height);
		SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
		SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
		SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN, spec.hidden);
		SDL_Window* window = SDL_CreateWindowWithProperties(properties);
		SDL_DestroyProperties(properties);
		if (window == nullptr)
			throw sdlError("SDL cannot open a window");
		const WindowId id = SDL_GetWindowID(window);
		m_textInputs[id] = std::make_unique<SdlTextInput>(window);
		return id;
	}

	void SdlWindowSystem::destroyWindow(WindowId window)
	{
		m_inputTargets.erase(window);
		m_drops.erase(window);
		SDL_DestroyWindow(sdlWindow(window));
		m_textInputs.erase(window);
	}

	SDL_Window* SdlWindowSystem::sdlWindow(WindowId window)
	{
		return SDL_GetWindowFromID(window);
	}

	Rect SdlWindowSystem::bounds(WindowId window) const
	{
		Rect result;
		SDL_Window* sdl = sdlWindow(window);
		SDL_GetWindowPosition(sdl, &result.x, &result.y);
		SDL_GetWindowSize(sdl, &result.width, &result.height);
		return result;
	}

	void SdlWindowSystem::setBounds(WindowId window, const Rect& bounds)
	{
		SDL_Window* sdl = sdlWindow(window);
		SDL_SetWindowPosition(sdl, bounds.x, bounds.y);
		SDL_SetWindowSize(sdl, bounds.width, bounds.height);
	}

	void SdlWindowSystem::minimize(WindowId window)
	{
		if (SDL_Window* sdl = sdlWindow(window); sdl != nullptr && !SDL_MinimizeWindow(sdl))
			logging::warning("SDL cannot minimize a window: {}", SDL_GetError());
	}

	void SdlWindowSystem::restore(WindowId window)
	{
		SDL_Window* sdl = sdlWindow(window);
		if (sdl != nullptr && (SDL_GetWindowFlags(sdl) & SDL_WINDOW_MINIMIZED) != 0 && !SDL_RestoreWindow(sdl))
			logging::warning("SDL cannot restore a window: {}", SDL_GetError());
	}

	bool SdlWindowSystem::maximized(WindowId window) const
	{
		SDL_Window* sdl = sdlWindow(window);
		return sdl != nullptr && (SDL_GetWindowFlags(sdl) & SDL_WINDOW_MAXIMIZED) != 0;
	}

	void SdlWindowSystem::setMaximized(WindowId window, bool maximized)
	{
		SDL_Window* sdl = sdlWindow(window);
		if (sdl == nullptr || this->maximized(window) == maximized)
			return;
		if (!(maximized ? SDL_MaximizeWindow(sdl) : SDL_RestoreWindow(sdl)))
			logging::warning("SDL cannot maximize or restore a window: {}", SDL_GetError());
	}

	void SdlWindowSystem::raise(WindowId window)
	{
		SDL_Window* sdl = sdlWindow(window);
		if (sdl == nullptr)
			return;
		// Only a minimized one: restoring a maximized window would make it smaller.
		if ((SDL_GetWindowFlags(sdl) & SDL_WINDOW_MINIMIZED) != 0 && !SDL_RestoreWindow(sdl))
			logging::warning("SDL cannot restore a window: {}", SDL_GetError());
		SDL_RaiseWindow(sdl);
	}

	Size SdlWindowSystem::pixelSize(WindowId window) const
	{
		Size result;
		SDL_GetWindowSizeInPixels(sdlWindow(window), &result.width, &result.height);
		return result;
	}

	float SdlWindowSystem::scale(WindowId window) const
	{
		const float scale = SDL_GetWindowDisplayScale(sdlWindow(window));
		return scale > 0.0F ? scale : 1.0F;
	}

	void SdlWindowSystem::setInputTarget(WindowId window, Rml::Context* context)
	{
		if (context != nullptr)
			m_inputTargets[window] = context;
		else
			m_inputTargets.erase(window);
	}

	Rml::TextInputHandler* SdlWindowSystem::textInputHandler(WindowId window)
	{
		const auto it = m_textInputs.find(window);
		return it == m_textInputs.end() ? nullptr : it->second.get();
	}

	std::string SdlWindowSystem::clipboardText() const
	{
		char* text = SDL_GetClipboardText();
		std::string result = text != nullptr ? text : "";
		SDL_free(text);
		return result;
	}

	void SdlWindowSystem::setClipboardText(const std::string& text)
	{
		SDL_SetClipboardText(text.c_str());
	}

	void SdlWindowSystem::DestroyCursor::operator()(SDL_Cursor* cursor) const
	{
		SDL_DestroyCursor(cursor);
	}

	void SdlWindowSystem::setCursor(const Cursor& cursor)
	{
		auto found = m_cursors.find(cursor.name);
		if (found == m_cursors.end())
		{
			SDL_Cursor* made = nullptr;
			if (!cursor.images.empty())
			{
				// The picture at scale 1, with the others for HiDPI monitors, of which SDL takes the nearest to the
				// monitor's scale (Windows, macOS, Wayland; X11 shows the first).
				SDL_Surface* base = surfaceOf(cursor.images.front());
				for (size_t i = 1; base != nullptr && i < cursor.images.size(); ++i)
					if (SDL_Surface* scaled = surfaceOf(cursor.images[i]))
					{
						SDL_AddSurfaceAlternateImage(base, scaled);
						SDL_DestroySurface(scaled); // the base keeps a reference
					}
				if (base != nullptr)
				{
					const Point hotspot = cursor.images.front().hotspot;
					made = SDL_CreateColorCursor(base, hotspot.x, hotspot.y);
					SDL_DestroySurface(base);
				}
			}
			else if (const std::optional<SDL_SystemCursor> system = systemCursor(cursor.name))
			{
				made = SDL_CreateSystemCursor(*system);
			}
			if (made == nullptr && !cursor.name.empty())
				logging::debug("The mouse pointer {} is the OS's arrow: {}", cursor.name, SDL_GetError());
			found = m_cursors.emplace(cursor.name, std::unique_ptr<SDL_Cursor, DestroyCursor>(made)).first;
		}
		SDL_SetCursor(found->second != nullptr ? found->second.get() : SDL_GetDefaultCursor());
	}

	void SdlWindowSystem::showFileDialog(uint64_t id, const FileDialog& dialog)
	{
		auto request = std::make_unique<DialogRequest>();
		request->eventType = m_dialogEvent;
		request->id = id;
		request->patterns.reserve(dialog.filters.size());
		for (const FileFilter& filter : dialog.filters)
			request->patterns.push_back(filter.extensions);
		for (size_t i = 0; i < dialog.filters.size(); ++i)
			request->filters.push_back({dialog.filters[i].name.c_str(), request->patterns[i].c_str()});

		const SDL_PropertiesID properties = SDL_CreateProperties();
		if (!request->filters.empty())
		{
			SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, request->filters.data());
			SDL_SetNumberProperty(
				properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, static_cast<Sint64>(request->filters.size()));
		}
		if (SDL_Window* parent = sdlWindow(dialog.parent))
			SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, parent);
		if (!dialog.location.empty())
			SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, dialog.location.u8string().c_str());
		if (!dialog.title.empty())
			SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, dialog.title.c_str());
		SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, dialog.many);
		// The callback owns the request; it may run before this returns.
		SDL_ShowFileDialogWithProperties(dialogType(dialog.kind), dialogClosed, request.release(), properties);
		SDL_DestroyProperties(properties);
	}

	std::optional<WindowEvent> SdlWindowSystem::nextEvent(std::optional<std::chrono::milliseconds> timeout)
	{
		const Clock::time_point deadline = timeout ? Clock::now() + *timeout : Clock::time_point::max();
		for (;;)
		{
			Sint32 wait = -1;
			if (timeout)
			{
				const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
				wait = static_cast<Sint32>(std::max<std::chrono::milliseconds::rep>(left.count(), 0));
			}
			SDL_Event event;
			if (!SDL_WaitEventTimeout(&event, wait))
				return std::nullopt;
			if (std::optional<WindowEvent> translated = translate(event))
				return translated;
		}
	}

	void SdlWindowSystem::wake()
	{
		// SDL's event queue takes events from any thread.
		SDL_Event event{};
		event.type = m_wakeEvent;
		if (m_wakeEvent == 0 || !SDL_PushEvent(&event))
			logging::warning("Cannot wake the UI thread: {}", SDL_GetError());
	}

	std::optional<WindowEvent> SdlWindowSystem::translate(SDL_Event& event)
	{
		if (event.type == m_wakeEvent && m_wakeEvent != 0)
			return WindowEvent{WindowEventKind::Wake};
		if (event.type == m_dialogEvent)
		{
			const std::unique_ptr<DialogResult> result(static_cast<DialogResult*>(event.user.data1));
			WindowEvent closed{WindowEventKind::FileDialogClosed};
			closed.dialog = result->id;
			closed.files = std::move(result->files);
			closed.error = std::move(result->error);
			return closed;
		}

		SDL_Window* window = SDL_GetWindowFromEvent(&event);
		const WindowId id = window != nullptr ? SDL_GetWindowID(window) : 0;
		switch (event.type)
		{
		case SDL_EVENT_TEXT_EDITING:
			if (const auto input = m_textInputs.find(id); input != m_textInputs.end())
			{
				input->second->edit(event.edit);
				return WindowEvent{WindowEventKind::Input, id};
			}
			return std::nullopt;
		case SDL_EVENT_DROP_BEGIN:
			m_drops[id] = {};
			return std::nullopt;
		case SDL_EVENT_DROP_FILE:
		case SDL_EVENT_DROP_POSITION:
		{
			Drop& drop = m_drops[id];
			if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr)
				drop.files.push_back(std::filesystem::u8path(event.drop.data));
			if (window != nullptr)
			{
				const float density = pixelDensity(window);
				drop.position = {static_cast<int>(event.drop.x * density), static_cast<int>(event.drop.y * density)};
			}
			return std::nullopt;
		}
		case SDL_EVENT_DROP_COMPLETE:
		{
			const auto drop = m_drops.find(id);
			if (drop == m_drops.end())
				return std::nullopt;
			WindowEvent dropped{WindowEventKind::FilesDropped, id};
			dropped.position = drop->second.position;
			dropped.files = std::move(drop->second.files);
			m_drops.erase(drop);
			if (dropped.files.empty())
				return std::nullopt; // text, or a drag that left without dropping
			return dropped;
		}
		default:
			break;
		}

		if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED)
		{
			m_keyboardGained = id;
			m_keyboardGainedAt = event.window.timestamp;
		}
		if (isInput(event.type))
		{
			const auto target = m_inputTargets.find(id);
			if (target == m_inputTargets.end())
				return std::nullopt;
			// A hidden window has no mouse to lose: the OS may still say so, e.g. for UI tests' windows, which would
			// take the hover from what the tests point at.
			if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE && (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) != 0)
				return std::nullopt;
			// Alt pressed and released alone opens the menu bar (ui/widgets.h), but not when SDL presses or releases
			// it for a change of the keyboard focus. When the window loses the keyboard, e.g. to Alt+Tab, SDL releases
			// the keys still held. When it gets the keyboard, e.g. by a window manager's Alt+J, SDL presses the keys
			// held for it on Linux, right after the focus event: that Alt's press and release don't count.
			const bool alt = (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) &&
				(event.key.key == SDLK_LALT || event.key.key == SDLK_RALT);
			const bool pressedForFocus = alt && event.type == SDL_EVENT_KEY_DOWN && id == m_keyboardGained &&
				event.key.timestamp < m_keyboardGainedAt + 100'000'000;
			if (event.type != SDL_EVENT_KEY_DOWN || alt)
				m_keyboardGained = 0;
			if (alt && event.type == SDL_EVENT_KEY_DOWN)
				m_altHeldOnGaining = pressedForFocus ? id : 0;
			if (pressedForFocus)
				return std::nullopt;
			if (alt && event.type == SDL_EVENT_KEY_UP && (SDL_GetKeyboardFocus() != window || m_altHeldOnGaining == id))
			{
				m_altHeldOnGaining = 0;
				return std::nullopt;
			}
			if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT)
				m_leftHeld.insert(id);
			else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT)
				m_leftHeld.erase(id);
			// SDL doesn't release the mouse buttons with the focus, and the button may go up over another application,
			// e.g. after Alt+Tab: the context's drag ends here instead of following the mouse without a button
			// (#1191). The mouse leaves first, so that the button going up clicks nothing.
			if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST && m_leftHeld.erase(id) != 0)
			{
				target->second->ProcessMouseLeave();
				target->second->ProcessMouseButtonUp(0, RmlSDL::GetKeyModifierState());
			}
			const bool unused = RmlSDL::InputEventHandler(target->second, window, event);
			WindowEvent input{WindowEventKind::Input, id};
			if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && unused) // one key command per press
				input.key =
					KeyPress{RmlSDL::ConvertKey(static_cast<int>(event.key.key)), RmlSDL::GetKeyModifierState()};
			else if (event.type == SDL_EVENT_KEY_UP)
				input.keyUp = KeyPress{RmlSDL::ConvertKey(static_cast<int>(event.key.key)), 0};
			else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) // a key held then goes up elsewhere
				input.keyUp = KeyPress{Rml::Input::KI_UNKNOWN, 0};
			return input;
		}
		if (const std::optional<WindowEventKind> kind = kindOf(event.type))
			return WindowEvent{*kind, id};
		return std::nullopt;
	}
}
