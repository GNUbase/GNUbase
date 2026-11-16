<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Schema validation

The shared engine validator checks the constraints listed in `src/engine/schema.h`. Commands check
`minProperties` themselves so they can explain which change is needed. Pattern matching is bounded and regex
compilation or matching errors become validation errors, including when settings are loaded.

Numeric bounds reject NaN and infinities. Infinities report the bound on their side; if the schema only bounds the other
side, validation requires a finite number. Unbounded numbers retain their existing behavior. Bounds compare as doubles;
schemas do not place integer bounds near the precision limit of a double. The generated Lua reference and MCP catalogue
show string lengths, array sizes and inclusive and exclusive bounds.

Tests guard the engine, settings and MCP UI tool schemas against unknown keywords, invalid keyword types and
patterns without a safe length limit. Synthetic bad schemas test the guards themselves. Command regressions cover
empty mixdown and render-in-place names, an empty paste-settings channel list after copying, and nonpositive scrub
durations, including calls from Lua. The mixdown dialog asks for a name before submitting.

Project files with ids outside Lua's positive signed integer range are refused on load; valid large ids remain
exact in Lua and editor names (project-files.md). Allocation reserves the last signed integer for the next-id counter.

Lengths count UTF-8 code points, not bytes. As before, stray continuation bytes from Lua count as no characters;
a byte limit also bounds pattern inputs. Settings values rejected by a schema use the setting's default as a whole,
and modifying that setting subsequently writes the default-based value. This retains the store's existing policy.

Checks: `engine_tests.cpp` covers exact infinity error messages for all four bounds and one-sided ranges.
`tracklayout_tests.cpp` checks the command's height-bound messages. Other coverage is in `uitools_tests.cpp`,
`scripting_tests.cpp`, `inplaceeditor_tests.cpp`, and the mixdown, render-in-place, channel-settings and playback command
tests.
