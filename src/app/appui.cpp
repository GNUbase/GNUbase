// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/appui.h"

#include "app/uitools.h"
#include "platform/log.h"
#include "ui/fonts.h"
#include "ui/widgets.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Lua.h>
#include <lua.h>

#include <array>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace gnubase::app
{
	namespace
	{
		// GNUbase's fonts, which documents need, then the themes.
		ui::Themes fontsAndThemes(const std::filesystem::path& resources)
		{
			ui::Fonts(resources / "fonts").load();
			return ui::Themes(resources);
		}

		struct ProjectPanel
		{
			const char* id;
			const char* title;
			const char* zone;
		};

		constexpr std::array<ProjectPanel, 6> g_projectPanels{{
			{"inspector", "Inspector", "left"},
			{"visibility", "Visibility", "left"},
			{"editor", "Editor", "lower"},
			{"mixer", "Mixer", "lower"},
			{"media", "Media", "right"},
			{"meter", "Meter", "right"},
		}};

		// The event display's commands (ui::EventDisplay::actions) and the reference's keys for them in the project
		// window (manual p. 1573), if it has any.
		struct ProjectCommand
		{
			const char* id;
			const char* description;
			const char* keys;
		};

		constexpr std::array<ProjectCommand, 36> g_projectCommands{{
			{"zoom.in", "Zooms the project window in.", "H"},
			{"zoom.out", "Zooms the project window out.", "G"},
			{"zoom.inVertically", "Makes the project window's tracks taller.", "Shift+H"},
			{"zoom.outVertically", "Makes the project window's tracks lower.", "Shift+G"},
			{"zoom.full", "Shows the whole project in the project window.", "Shift+F"},
			{"zoom.undo", "Undoes the project window's last zoom.", nullptr},
			{"zoom.redo", "Redoes the project window's last undone zoom.", nullptr},
			{"view.autoScroll", "Switches the project window's auto-scroll on or off.", "F"},
			// Editing parts and events (docs/features/parts-and-events.md); Backspace deletes too.
			{"edit.delete", "Deletes the selected events.", "Delete"},
			{"edit.mute", "Mutes the selected events.", "Shift+M"},
			{"edit.unmute", "Unmutes the selected events.", "Shift+U"},
			{"edit.lock", "Locks the selected events.", "Mod+Shift+L"},
			{"edit.unlock", "Unlocks the selected events.", "Mod+Shift+U"},
			{"edit.splitAtCursor", "Splits the selected events, or all there, at the project cursor.", "Alt+X"},
			{"edit.splitLoop", "Splits the selected events, or all there, at the locators.", nullptr},
			{"edit.glue", "Glues the selected events together.", nullptr},
			{"edit.duplicate", "Duplicates the selected events.", "Mod+D"},
			{"edit.repeat", "Opens the Repeat Events dialog for the selected events.", "Mod+K"},
			{"edit.fillLoop", "Fills the locator range with copies of the selected events.", nullptr},
			{"edit.toRealCopy", "Converts the selected shared copies to real copies.", nullptr},
			{"edit.nudgeLeft", "Moves the selected events left.", "Mod+Left"},
			{"edit.nudgeRight", "Moves the selected events right.", "Mod+Right"},
			{"edit.nudgeStartLeft", "Moves the start of the selected events left.", "Alt+Left"},
			{"edit.nudgeStartRight", "Moves the start of the selected events right.", "Alt+Shift+Right"},
			{"edit.nudgeEndLeft", "Moves the end of the selected events left.", "Alt+Shift+Left"},
			{"edit.nudgeEndRight", "Moves the end of the selected events right.", "Alt+Right"},
			{"edit.moveStartToCursor", "Moves the selected events so that they start at the project cursor.", "Mod+L"},
			{"edit.moveEndToCursor", "Moves the selected events so that they end at the project cursor.", nullptr},
			{"edit.selectAll", "Selects every event in the project window.", "Mod+A"},
			{"edit.selectNone", "Selects no events.", "Mod+Shift+A"},
			{"edit.invertSelection", "Selects the events that aren't selected, and no others.", "Mod+Alt+I"},
			{"edit.selectInLoop", "Selects the events between the locators.", nullptr},
			{"edit.selectFromStartToCursor", "Selects the events that end before the project cursor.", nullptr},
			{"edit.selectFromCursorToEnd", "Selects the events that start after the project cursor.", nullptr},
			{"edit.selectUnderCursor", "Selects the events at the project cursor.", nullptr},
			{"edit.locatorsToSelection", "Sets the locators around the selected events.", "P"},
		}};

		// The transport controls' displays that the enter.* commands edit, with the reference's keys (manual
		// pp. 311-312, 1570).
		struct EnterCommand
		{
			const char* field;
			const char* id;
			const char* description;
			const char* keys;
		};

		constexpr std::array<EnterCommand, 6> g_enterCommands{{
			{"left", "enter.leftLocator", "Edits the left locator in the transport bar.", "Shift+L"},
			{"right", "enter.rightLocator", "Edits the right locator in the transport bar.", "Shift+R"},
			{"position", "enter.position", "Edits the project cursor position in the transport bar.", "Shift+P"},
			{"length", "enter.length", "Edits the locator range duration in the transport bar.", "Shift+D"},
			{"tempo", "enter.tempo", "Edits the tempo in the transport bar.", "Shift+T"},
			{"signature", "enter.signature", "Edits the time signature in the transport bar.", "Shift+C"},
		}};

		constexpr ui::Size g_transportPanelSize{1100, 48};
		constexpr ui::Size g_timeDisplaySize{480, 120};

		std::string readText(const std::filesystem::path& file)
		{
			std::ifstream in(file, std::ios::binary);
			std::ostringstream text;
			text << in.rdbuf();
			return text.str();
		}
	}

	AppUi::AppUi(engine::Engine& engine, const std::filesystem::path& resources, const ui::WindowSpec& main,
		std::ostream& output, int msaaSamples)
		: m_renderer(m_windows, msaaSamples), m_lua(engine, scripting::Trust::Trusted, output), m_rmlui(m_windows),
		  m_themes(fontsAndThemes(resources)), m_shell(m_windows, m_renderer), m_settings(engine, m_shell, m_themes),
		  m_binding(engine, [this] { m_shell.wake(); }), m_keys(engine, m_binding), m_main(m_shell.open(main)),
		  m_docking(engine, m_binding, m_shell, resources / "ui" / "panel-window.rml", main.hidden), m_engine(engine),
		  m_resources(resources), m_hiddenWindows(main.hidden)
	{
		ui::registerWidgets(m_rmlui);
		lua_State* state = m_lua.state();
		lua_getglobal(state, "print");
		Rml::Lua::Initialise(state);   // until Rml::Shutdown
		lua_setglobal(state, "print"); // GNUbase's, instead of the plugin's, which writes into RmlUi's log
		addUiFunctions(m_lua, m_shell);
		m_shell.eachStep(
			[this]
			{
				if (const std::optional<std::string> error = m_lua.dispatchEvents())
					logging::warning("A Lua event handler of the UI failed: {}", *error);
				return m_binding.update();
			});
		m_shell.onUnusedKey([this](ui::Window& window, const ui::KeyPress& keys) { m_keys.press(window, keys); });

		// The project window's panels (reference manual pp. 54, 75-98), which their tickets fill.
		for (const ProjectPanel& panel : g_projectPanels)
			m_docking.add({panel.id, panel.title, "project", panel.zone, {}});
		m_docking.addKeyCommands(m_keys);

		m_keys.add({"window.studioSetup", "Opens the studio setup dialog for the audio and MIDI devices.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openStudioSetup(window); }});
		for (const ProjectCommand& command : g_projectCommands)
		{
			m_keys.add(
				{command.id, command.description, [this, id = std::string(command.id)](ui::Window&, Rml::Element*)
					{
						if (m_eventDisplay)
							m_eventDisplay->actions().at(id)();
					}});
			if (command.keys != nullptr)
				m_keys.bind({command.keys, command.id, engine::Json::object(), "project"});
		}
		m_keys.bind({"Backspace", "edit.delete", engine::Json::object(), "project"});

		// The transport controls' commands (docs/features/transport-bar.md); the engine's transport commands have
		// their keys in the default key map.
		m_keys.add({"window.transportPanel", "Shows or hides the transport panel.",
			[this](ui::Window&, Rml::Element*) { toggleTransportPanel(); }});
		m_keys.bind({"F2", "window.transportPanel"});
		m_keys.add({"window.timeDisplay", "Shows or hides the time display window.",
			[this](ui::Window&, Rml::Element*) { toggleTimeDisplay(); }});
		m_keys.add({"time.exchangeFormats", "Switches the primary and the secondary time format of the transport bar.",
			[this](ui::Window&, Rml::Element*)
			{
				if (m_transportBar)
					m_transportBar->exchangeFormats();
				else if (m_transportPanel.controls)
					m_transportPanel.controls->exchangeFormats();
			}});
		m_keys.bind({".", "time.exchangeFormats"});
		// Go to Previous Marker/Zero and Next Marker/Project End: to the project start or end without a marker.
		for (const bool next : {false, true})
			m_keys.add({next ? "locate.nextMarker" : "locate.previousMarker",
				next ? "Moves the project cursor to the next marker, or to the project end if there is none."
					 : "Moves the project cursor to the previous marker, or to the project start if there is none.",
				[this, next](ui::Window&, Rml::Element*)
				{
					m_binding.send(next ? "marker.locateNext" : "marker.locatePrevious", engine::Json::object(),
						[this, next](const engine::Result& result)
						{
							if (!result.ok() || result.value.at("marker").is_null())
								m_binding.send("transport.locate", {{"target", next ? "projectEnd" : "projectStart"}});
						});
				}});
		for (const EnterCommand& command : g_enterCommands)
		{
			m_keys.add({command.id, command.description,
				[this, field = std::string(command.field)](ui::Window&, Rml::Element*) { enter(field); }});
			m_keys.bind({command.keys, command.id});
		}
	}

	std::unique_ptr<ui::TransportControls> AppUi::transportControls(Rml::Element& root, ui::Window& window, bool fill)
	{
		if (fill)
			root.SetInnerRML(readText(m_resources / "ui" / "transport.rml"));
		const ui::WindowId id = window.id();
		return std::make_unique<ui::TransportControls>(m_engine, m_binding, root,
			[this, id](const std::string& command, const engine::Json& parameters)
			{
				if (ui::Window* found = m_shell.find(id))
					m_keys.run(command, parameters, *found, nullptr);
			});
	}

	void AppUi::toggle(TransportWindow& shown, const char* file, const char* title, ui::Size size)
	{
		if (ui::Window* window = shown.shown(m_shell))
		{
			shown.controls.reset();
			m_shell.close(window->id());
			shown.window = 0;
			return;
		}
		shown.controls.reset(); // its window was closed
		ui::WindowSpec spec;
		spec.title = title;
		spec.size = size;
		spec.hidden = m_hiddenWindows;
		ui::Window& window = m_shell.open(spec);
		const std::filesystem::path path = m_resources / "ui" / file;
		Rml::ElementDocument* document = window.context().LoadDocument(path.generic_u8string());
		Rml::Element* controls = document != nullptr ? document->GetElementById("transport") : nullptr;
		Rml::Element* root = controls != nullptr ? controls : document;
		if (root == nullptr)
		{
			m_shell.close(window.id());
			logging::error("Cannot load {}", path.u8string());
			return;
		}
		document->Show();
		shown.window = window.id();
		// The transport panel's #transport gets the controls; the time display window has its own.
		shown.controls = transportControls(*root, window, root == controls);
	}

	void AppUi::toggleTransportPanel()
	{
		toggle(m_transportPanel, "transport-panel.rml", "Transport", g_transportPanelSize);
	}

	void AppUi::toggleTimeDisplay()
	{
		toggle(m_timeDisplay, "time-display.rml", "Time Display", g_timeDisplaySize);
	}

	void AppUi::enter(const std::string& field)
	{
		if (m_transportBar && m_transportBar->edit(field))
			return;
		if (transportPanel() == nullptr)
			toggleTransportPanel();
		if (m_transportPanel.controls)
			m_transportPanel.controls->edit(field);
	}

	ui::EventDisplay& AppUi::showProject()
	{
		const std::filesystem::path file = m_resources / "ui" / "project.rml";
		Rml::ElementDocument* document = m_main.context().LoadDocument(file.generic_u8string());
		Rml::Element* root = document == nullptr ? nullptr : document->GetElementById("event-display");
		if (root == nullptr)
		{
			if (document != nullptr)
				document->Close();
			throw std::runtime_error("cannot load the project window from " + file.u8string());
		}
		document->Show();
		m_docking.attach("project", *document);
		m_eventDisplay = std::make_unique<ui::EventDisplay>(m_engine, m_binding, *root);
		if (Rml::Element* bar = document->GetElementById("transport-bar"))
			m_transportBar = transportControls(*bar, m_main, true);
		return *m_eventDisplay;
	}

	Rml::ElementDocument& AppUi::openStudioSetup(ui::Window& window) const
	{
		const std::filesystem::path file = m_resources / "ui" / "studio-setup.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
			throw std::runtime_error("cannot load the studio setup dialog from " + file.u8string());
		document->Show(Rml::ModalFlag::Modal);
		return *document;
	}
}
