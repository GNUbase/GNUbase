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
#include <optional>
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

		// The event display's commands (ui::EventDisplay::actions) and the reference's keys for them in the project
		// window (manual p. 1573), if it has any.
		struct ProjectCommand
		{
			const char* id;
			const char* description;
			const char* keys;
		};

		constexpr std::array<ProjectCommand, 8> g_projectCommands{{
			{"zoom.in", "Zooms the project window in.", "H"},
			{"zoom.out", "Zooms the project window out.", "G"},
			{"zoom.inVertically", "Makes the project window's tracks taller.", "Shift+H"},
			{"zoom.outVertically", "Makes the project window's tracks lower.", "Shift+G"},
			{"zoom.full", "Shows the whole project in the project window.", "Shift+F"},
			{"zoom.undo", "Undoes the project window's last zoom.", nullptr},
			{"zoom.redo", "Redoes the project window's last undone zoom.", nullptr},
			{"view.autoScroll", "Switches the project window's auto-scroll on or off.", "F"},
		}};
	}

	AppUi::AppUi(engine::Engine& engine, const std::filesystem::path& resources, const ui::WindowSpec& main,
		std::ostream& output, int msaaSamples)
		: m_renderer(m_windows, msaaSamples), m_lua(engine, scripting::Trust::Trusted, output), m_rmlui(m_windows),
		  m_themes(fontsAndThemes(resources)), m_shell(m_windows, m_renderer), m_settings(engine, m_shell, m_themes),
		  m_binding(engine, [this] { m_shell.wake(); }), m_keys(engine, m_binding), m_main(m_shell.open(main)),
		  m_engine(engine), m_resources(resources), m_hidden(main.hidden)
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
		m_keys.add({"window.markers", "Opens or closes the markers window.",
			// Later in the step: the key may be the markers window's own, which closing it would destroy.
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_shell.post([this] { toggleMarkers(); }); }});
		m_keys.bind({"Mod+M", "window.markers"});
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
		m_markerLines.reset();
		m_eventDisplay = std::make_unique<ui::EventDisplay>(m_engine, m_binding, *root);
		m_markerLines = std::make_unique<ui::MarkerLines>(*m_eventDisplay, m_binding);
		return *m_eventDisplay;
	}

	ui::MarkersWindow* AppUi::toggleMarkers()
	{
		const bool open = m_markers != nullptr && m_shell.find(m_markersWindow) != nullptr;
		m_markers.reset();
		if (open)
		{
			m_shell.close(m_markersWindow);
			return nullptr;
		}
		ui::WindowSpec spec;
		spec.title = "Markers";
		spec.size = {720, 360};
		spec.hidden = m_hidden;
		const ui::Window& window = m_shell.open(spec);
		m_markersWindow = window.id();
		const std::filesystem::path file = m_resources / "ui" / "markers.rml";
		Rml::ElementDocument* document = window.context().LoadDocument(file.generic_u8string());
		if (document == nullptr)
		{
			m_shell.close(m_markersWindow);
			throw std::runtime_error("cannot load the markers window from " + file.u8string());
		}
		document->Show();
		m_markers = std::make_unique<ui::MarkersWindow>(m_engine, m_binding, *document);
		return m_markers.get();
	}
}
