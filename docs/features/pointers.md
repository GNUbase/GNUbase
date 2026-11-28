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
| A mixer channel's name, which a drag moves | `pointer` | p. 496 |
| Free Warp in the sample editor, the time warp tool in the event display | `tool-timewarp`, a clock with arrows | pp. 655, 1466 |
| The key editor keyboard's left part, which scrolls and zooms | `move`, the four-way arrow | p. 1261 |
| The key editor's note display, by the tool (`[tool]` on `.key-editor`) | `tool-draw`, `tool-line`, `tool-erase`, `tool-split` for trim and split, `tool-glue`, `tool-mute` | p. 1215 |
| The audio part editor's lanes, by the tool | `tool-erase`, `tool-split`, `tool-glue`, `tool-mute`, `tool-play` for play and scrub | pp. 693, 700-701 |
| The drum editor's note display with the drumstick | `tool-drumstick` | p. 1224 |
| A note's ends where a press resizes it: the object selection tool's without Shift or Ctrl (Cmd), the drumstick's with Show Note Length | `resize` | pp. 1213-1214, 1242 |
| Pitchbend events in the controller display, with the object selection tool | `pointer` | p. 1176 |
| The controller display, by the tool of the key, drum or in-place editor: every lane but the Articulations lane with the draw and line tools, controller events with the erase tool, articulations with the erase and mute tools and the Articulations lane's rows with the draw tool | `tool-draw`, `tool-line`, `tool-erase`, `tool-mute` | pp. 1176, 1180-1181, 1186, 1288-1289 |
| The list editor's event display with the draw tool, its rows with the erase tool | `tool-draw`, `tool-erase` | |
| The pattern editor's steps with the draw tool, or with Alt and the step selection tool | `tool-draw` | p. 924 |
| Edit Hitpoints with Alt away from a hitpoint, and on a hitpoint's line with Shift | `tool-draw`, `crosshair` | p. 640 |
| A warp marker with a modifier key in Free Warp | `tool-erase` | pp. 654, 656 |
| The value widgets (knob, fader, slider, value field), dialog title bars and dock tabs | the OS's arrow: a drag there does what the element plainly is for, turning the knob or moving the dialog or the tab, so they get no pointer of their own | |

Where the pointer depends on where the mouse is or on the modifier keys, code names what a press there does in the
element's `pointer` attribute (`ui::setPointer`), and the style sheets choose the pointer by it: the event lanes'
`.event-area`, the key editor's `.key-editor-grid`, the pattern editor and the sample editor. Their editors take the
modifier keys from mouse moves, and from the keys going down and up anywhere in the window while the mouse rests: their
document hears the keys, and the pointer is for the element under the mouse (`ui::heldModifiers` counts the key itself
before the event's modifiers say so).

The in-place editors lie in the event lanes, but the project window's tool doesn't choose their pointers: while the
mouse is over one, the event lanes' `.event-area` has no `pointer`, so where the in-place editor's own tool sets none,
over its note display with the object selection tool or a controller lane's empty area with the erase tool, the OS's
arrow shows (#2171).

An element that RmlUi drags (`drag: drag`, as splitters, the sampler's markers and the value widgets have) keeps its
pointer while the drag goes on, also when the mouse leaves it, until the button goes up.

**Tests.** `tests/pointers_ui_tests.cpp` hovers splitters, the MIDI inserts' divider, the history's separator, a text
field, a mixer channel's name, the event lanes with the time warp tool and the key editor's note display with each tool
and at a note's ends, also with the modifier keys going down and up while the mouse rests, through the automation API
and reads the pointer as MCP's `ui_windows` reports it, and checks that a splitter's drag keeps its pointer outside it
until release; `tests/sampleeditor_ui_tests.cpp` does so for the snap point and Free Warp,
`tests/hitpoints_ui_tests.cpp` for Edit Hitpoints' modifiers, also while the mouse rests,
`tests/audioparteditor_ui_tests.cpp` for the audio part editor's tools, `tests/drumeditor_ui_tests.cpp` for the
drumstick, `tests/controllerlanes_ui_tests.cpp` for pitchbend events and the tools on the lanes
(`tests/pointers_ui_tests.cpp` in the drum and in-place editors, also with the project window's draw tool chosen), `tests/listeditor_ui_tests.cpp` and
`tests/patterneditor_ui_tests.cpp` for those editors' tools and the pattern editor's Alt. `tests/svg_tests.cpp` checks
that every `cursor` name in the style sheets and documents is one of the OS's pointers or one of GNUbase's with a
picture and a hotspot.

**Not yet** (left in #1857): drag and drop's not-allowed and copy pointers; the pointing hand where the reference shows
one and GNUbase doesn't have the gesture yet: the ruler's locator range (p. 315) and the segments of the sample editor's
pitch editing (p. 674); links and clickable cells.
