# UI tests

UI tests check GNUbase's UI as the app shows it (#59). They run in the test process, on the UI thread, with the same
UI setup as the app, and drive it with the interface behind MCP's UI tools ([ui-automation.md](ui-automation.md)).

- **The app's UI.** `app::AppUi` (`src/app/appui.h`) is what `gnubase` shows: SDL3 windows drawn through SDL_GPU,
  RmlUi with GNUbase's fonts and themes, the shell with the user settings and the engine binding, and the main window.
  `runWindowed` runs it with the event loop and MCP; tests build the same object, so they test what users see.
- **The harness.** `test::UiApp` (`tests/uiapp.h`) runs `app::AppUi` with the engine on the dummy device, user
  settings in a temporary folder, and a hidden main window drawn with 4x MSAA, which every GPU has with the same sample
  positions. A test:
  - loads documents into the main window (`show`);
  - runs engine commands (`call`), e.g. `settings.set`, and steps the shell as the app's event loop does until the UI
    has followed (`waitUntil`);
  - clicks, types and reads the DOM through `ui()`, the `ui::Automation` that MCP's UI tools use. What the mouse is
    over stays while the shell steps: a hidden window ignores the OS's word that the mouse left it;
  - lays the UI out at a fixed dp ratio, whatever the monitor's scale (`setDpRatio`), and compares screenshots with
    golden images in `tests/golden/ui/` (`screenshot`, `checkGolden`). Each channel of a pixel may differ by 8 of 255,
    and one pixel in 500 by more, for GPUs that round an edge or a blend differently. `GNUBASE_UPDATE_GOLDEN=1`
    accepts a new look; look at the images before committing them.
- **Where they run.** UI tests are tagged `[ui]`. On Linux they use SDL's offscreen video driver and Mesa's software
  Vulkan driver, so they need no display; CI runs them on Linux and Windows, and on Apple Silicon in
  `.github/workflows/ui-macos.yml`. Where SDL_GPU has no device, such as CI's macOS Intel runner, they are skipped,
  unless `GNUBASE_REQUIRE_GPU=1` says the machine must have one.
- **In Lua.** UI tests can be written in Lua, in `tests/lua/ui/`, and run in the same harness: see
  [ui-lua.md](ui-lua.md).
- **Tests:** `tests/uiapp_tests.cpp`.
