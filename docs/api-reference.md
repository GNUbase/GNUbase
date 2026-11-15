# API references

Two references are generated from the code and are not in the repository, so that they can't conflict between branches:

- **Lua API** (`lua.md`): every engine command as a Lua function, the events, running scripts and the `gnubase.test` framework, written by `scripting::reference` (`src/scripting/reference.cpp`) from the command registry.
- **MCP tool catalogue** (`mcp-tools.md`): every MCP tool by area, its engine command, its arguments and its result, and the resources, written by `mcp::catalogue` (`src/mcp/catalogue.cpp`) from what the server lists. [mcp.md](mcp.md) is the guide that goes with it.

The references of `main` are on the web: the [Lua API](https://gnubase.github.io/GNUbase/lua.html) and the [MCP tool catalogue](https://gnubase.github.io/GNUbase/mcp-tools.html). CI publishes them with GitHub Pages after every push to `main`.

The unit tests write both files into `docs/` of the build folder, e.g. `build/<preset>/docs/`. Add the configuration you built, e.g. `-C Release` after the README's Release build, as the test presets run Debug otherwise:

```
ctest --preset <preset> -C Release -R "generated from"
```

CI's Linux job uploads them as the artifact `api-reference` on every run and fails when one is missing, so they are the Linux build's. On a pull request, its job summary shows how they differ from `main`'s, so that a reviewer sees what a change of descriptions or schemas does to them.

A branch made before they left the repository (#919) conflicts on `docs/lua.md` and `docs/mcp.md` when it merges `main`: take `main`'s side, so `docs/lua.md` stays deleted and `docs/mcp.md` stays the guide. A unit test fails while `docs/lua.md`, `docs/mcp-tools.md` or catalogue entries in `docs/mcp.md` are in the repository.

A new or changed engine command needs a description and parameter and result schemas; the generated references and the MCP tools and Lua functions follow from them. The coverage tests (`tests/coverage_tests.cpp`) fail when a command lacks a description, a parameter schema of type object, its MCP tool or its Lua function, when a command has no section in the Lua reference or a tool of `tools/list` none in the catalogue, when a reference links a relative path, which leads nowhere from the build folder, the artifact or the web, and when a reference can't be written. A command without a result keeps the empty result schema, so result schemas aren't checked.
