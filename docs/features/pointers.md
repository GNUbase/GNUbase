# Mouse pointers by context

The mouse pointer shows what a drag or click does where it is (#1857). Style sheets choose it with RmlUi's `cursor`
property, by an element's class, state or attribute; code never sets a pointer. The pointers themselves, GNUbase's
own in `resources/cursors` and the OS's by their CSS names, are in [svg.md](svg.md); the event display's pointers per
tool in [tools.md](tools.md).

| Where | Pointer | Reference manual |
| --- | --- | --- |
| Splitters in a row (zones, track list) | `ew-resize`, the OS's left-right double arrow | |
| Splitters in a column (lower zone), the MIDI inserts' divider, the history's separator | `ns-resize` | |
| Text fields (`input.text`, `input.password`, `textarea`) | `text`, the I-beam | |
| Event edges, sampler zone edges and markers, sample editor regions and hitpoints | `resize` | pp. 347, 640 |
| The sample editor's snap point | `pointer`, a hand | p. 628 |
| Free Warp in the sample editor, the time warp tool in the event display | `tool-timewarp`, a clock with arrows | pp. 655, 1466 |
| The key editor keyboard's left part, which scrolls and zooms | `move`, the four-way arrow | p. 1261 |
| The key editor's note display, by the tool (`[tool]` on `.key-editor`) | `tool-draw`, `tool-line`, `tool-erase`, `tool-split` for trim and split, `tool-glue`, `tool-mute` | p. 1215 |
| The audio part editor's lanes, by the tool | `tool-erase`, `tool-split`, `tool-glue`, `tool-mute`, `tool-play` for play and scrub | p. 604 |

An element that RmlUi drags (`drag: drag`, as splitters, the sampler's markers and the value widgets have) keeps its
pointer while the drag goes on, also when the mouse leaves it, until the button goes up.

**Tests.** `tests/pointers_ui_tests.cpp` hovers splitters, a text field, the event lanes with the time warp tool and
the key editor's note display with each tool through the automation API and reads the pointer as MCP's `ui_windows`
reports it, and checks that a splitter's drag keeps its pointer outside it until release;
`tests/sampleeditor_ui_tests.cpp` does so for the snap point and Free Warp, `tests/audioparteditor_ui_tests.cpp` for
the audio part editor's tools. `tests/svg_tests.cpp` checks that every `cursor` name in the style sheets and documents is one of the OS's
pointers or one of GNUbase's with a picture and a hotspot.

**Not yet** (left in #1857): drag and drop's not-allowed and copy pointers; pointers that follow what a press does
where the mouse is in the editors, as the event display's do (note edges, modifiers such as the sample editor's Alt
click that inserts a hitpoint or the eraser of p. 654), and the pitchbend events' pointing hand (p. 1176); the value
widgets, dialog title bars and dock tabs; links and clickable cells.
