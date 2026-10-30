# UI automation

MCP clients, UI tests ([ui-tests.md](ui-tests.md)) and Lua ([ui-lua.md](ui-lua.md)) read GNUbase's UI back and drive it as a user would
(#58): its windows and documents, the DOM with each element's box, screenshots of windows or single elements, and mouse
and keyboard input. [mcp.md](../mcp.md#ui-tools) is the guide for agents.

- **Interface.** `ui::Automation` (`src/ui/automation.h`) does the work, on the UI thread. It reads windows, documents
  and elements as JSON and gives input to a window's RmlUi context, the way `SdlWindowSystem` delivers the user's, so
  documents can't tell it from real input. It needs only the shell, so the tests run it on the fake shell
  (`tests/uitools_tests.cpp`) as well as on real windows (`tests/window_tests.cpp`).
- **Threads.** `ui::onUiThread` runs work from another thread on the UI thread through `Shell::post` and waits for its
  result, at most 10 seconds for an MCP tool, e.g. while the UI thread loads a project. The work captures what it uses
  by value, since it may still run after the caller gave up. When the shell quits, work it hasn't run yet is dropped,
  and whoever waits for it gets an error at once.
- **MCP tools.** `app::addUiTools` adds `ui_windows`, `ui_dom`, `ui_screenshot`, `ui_wait`, `ui_move`, `ui_click`,
  `ui_drag`, `ui_wheel`, `ui_key` and `ui_text` to the MCP server; the windowed app serves them with `--mcp-http`, next
  to the engine's tools and `script_run`. Headless GNUbase has no UI and no UI tools. A screenshot comes back as MCP
  image content, a PNG: a tool returns the PNG as `Json::binary`, which `mcp::Server` sends as an image. PNG encoding
  runs on the server's thread. `app::addUiFunctions` adds the same tools to the UI's Lua as `gnubase.ui.click` and so
  on, from the same list ([ui-lua.md](ui-lua.md)).
- **Coordinates.** Everything is in the window's pixels, as in its screenshots: an element's box, the points that
  mouse tools take, the window's size. A window's dp ratio (its monitor's scale times the UI scale, #53) converts to dp.
- **The DOM.** An element has its tag, id, classes, other attributes, a form control's value, its active
  pseudo-classes (`hover`, `focus`, `checked`, `disabled`, ...), whether it's visible (not `display: none` or
  `visibility: hidden`, nor inside such an element, and not empty) and its border box. Text is a child `{"text": ...}`
  with white space collapsed; an element whose children are left out by the depth has its text instead. A selector
  matches in every document of the window, the documents themselves included, in document order.
- **Input.** A click moves the mouse to the centre of the element, or to the point, and presses and releases the
  button; a double click is two clicks, which RmlUi recognises by their timing. A drag presses at the start, moves in
  eight steps and releases at the end, so RmlUi sends `dragstart`, `drag` and `dragdrop`. Wheel turns are in lines,
  positive to the right and down. Keys are pressed and released with modifiers; they don't type text, which `ui_text`
  sends as text input, as the OS's text input does after a key press. Each input tool answers with the element under
  the mouse or with the focus, so a client sees what it hit.
- **Waiting.** `ui_wait` asks the UI thread every 20 ms whether an element that a selector matches is visible (with a
  given text), or that none is, for at most 60 seconds.
