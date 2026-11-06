# API references

Two references are generated from the code and are not in the repository, so that they can't conflict between branches:

- **Lua API** (`lua.md`): every engine command as a Lua function, the events, running scripts and the `gnubase.test` framework, written by `scripting::reference` (`src/scripting/reference.cpp`) from the command registry.
- **MCP tool catalogue** (`mcp-tools.md`): every MCP tool by area, its engine command, its arguments and its result, and the resources, written by `mcp::catalogue` (`src/mcp/catalogue.cpp`) from what the server lists. [mcp.md](mcp.md) is the guide that goes with it.

The unit tests write both files into `docs/` of the build folder, e.g. `build/<preset>/docs/`:

```
ctest --preset <preset> -R "generated from"
```

CI uploads them from the Linux job as the artifact `api-reference` on every run, so the latest ones of `main` are in its run on GitHub.

A new or changed engine command needs a description and parameter and result schemas; the generated references and the MCP tools and Lua functions follow from them, and the coverage tests fail when a command lacks one.
