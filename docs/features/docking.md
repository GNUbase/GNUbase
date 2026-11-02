# Docking

Windows built from zones, tab groups, splitters and strips, and panels that dock in a zone or open in a window of
their own (#56), and setup panes that show, hide and reorder sections (#372) (reference manual pp. 54–100, 439–440,
1575–1578). The code is `ui::Docking` (`src/ui/docking.h`); `app::AppUi` makes the project window and the mixer windows
([mixer-window.md](mixer-window.md)) docking windows, and the editors will be the next ones.

## Structure

A document becomes a docking window with `Docking::attach(name, document)`. Its elements say what they are by
attributes, and the shared style sheet (`resources/styles/base.rcss`, class `.docking` on the body) lays them out:
strips above and below a row of zones, where the main zone takes what the others leave.

| Attribute | What it is |
|---|---|
| `zone="main"`, `"left"`, `"right"`, `"lower"` | A zone. Every zone but the main one gets a `<tabset>` with its panels' tabs. `size` is its default size in dp, `default="hidden"` hides it by default. Its size is its width, or its height in a flex column. |
| `splitter for="left"` | The splitter that sizes that zone; `resize="next"` for a zone after it. It hides with its zone. |
| `strip="infoLine"` | A strip; `default="hidden"` hides it by default. |
| `toggle="left"` | A toggle button or menu item that shows or hides that zone or strip. |
| `sections="channel"` | A container of the set of sections `channel`, such as each of the mixer's channels. Its children with `section="eq"` are the sections, with a `label` for the setup pane and `default="hidden"` to hide one by default. |
| `setup="channel"` | A setup pane's list of that set's sections, which the docking system fills. |

The project window (`resources/ui/project.rml`) has the menu bar ([main-menu.md](main-menu.md)), the toolbar, the info
line, the left zone, the main zone with the event display, the lower and right zones, the status line and the transport
bar, which come in that order in the reference (pp. 54–56). Its toolbar has Show/Hide Left, Lower and Right Zone and the
Set up Window Layout menu, which shows or hides the menu bar (the reference's Show Menu Bar, p. 55), the zones and the
info line, overview line, status line and transport bar (p. 65). The overview line is
part of the event display: while a strip is hidden, the body has the class `no-<strip>`, which the style sheet uses to
hide it. The transport bar has the [transport controls](transport-bar.md). The [status line](status-line.md) shows the
record format and a lost audio device. The info line is empty.

States are classes for the themes: zones, their splitters and strips get `.hidden`, and the zone with the keyboard
focus gets `.focused`, whose `.zone-frame` (an element the docking system adds over each zone) the theme draws as a
frame (p. 99). Code only sets the zones' sizes, which come from data.

## Panels and tab groups

A panel (`ui::Panel`) has an id, a title, the docking window and the zone it docks in, and a function that fills the
element it shows in, wherever that is; what the function returns lives as long as the panel shows there. `add`
registers panels; the project window has Inspector and Visibility on the left, Editor and Mixer in the lower zone, and
Media and Meter on the right (pp. 75–98), empty until their tickets fill them, the Pool in the lower zone
([pool.md](pool.md), `window.pool`, Mod+P) and the edit history on the right ([edit-history.md](edit-history.md)).
`open` shows a panel as its zone's active tab, with the zone shown. `showing` says whether a panel shows: in a window
of its own, or as its zone's active tab while the zone shows; what its function returned stays while it doesn't, so a
panel such as the pool can wait with refreshing until it shows again.

- **Tabs.** A zone's tab group shows its panels' tabs in the user's order. A tab dragged onto another takes its place.
  A right click on the tabs opens the zone's tab menu: Open in Separate Window for the tab clicked (or the active one),
  then an item per panel to show or hide its tab (p. 84). The tab shown last is the active tab the next time.
- **Windows of their own.** A panel in a window of its own leaves its zone; the window (`resources/ui/panel-window.rml`)
  has its title and a Dock button. Dock, or closing the window, puts it back. The window opens where it was last,
  moved onto a connected monitor if its monitor is gone, and renders at its monitor's scale like every window
  (architecture.md, "UI shell"). Its bounds are read twice a second and kept when they changed.
- **Contents.** Rebuilding a tab group, e.g. after a tab moved, keeps what the panels that stay in it show. A panel
  that moves to a window, or whose tab is hidden, gets its element made again where it shows next.

## Setup panes and sections

A setup pane shows and hides the parts of a docking window and reorders its sections, as the mixer's Set up Window
Layout pane does (pp. 439–440: the channel overview, the meter bridge, the left and right zones, and the channel
sections such as routing, inserts, EQ, sends and the fader). The pane is a `<menu class="setup-pane">` that a button
opens; its items are toggle buttons, so it stays open while they are clicked. Toggle buttons with a `toggle` attribute
show and hide zones and strips as elsewhere, and an element with `setup="channel"` gets a toggle button per section of
the set `channel` (`setup-section="eq"`, with the section's label), in the user's order.

- **Show and hide.** A section's button shows or hides that section in every container of its set; hidden sections
  get the state class `.hidden`.
- **Reorder.** A button dragged onto another moves its section to that one's place, in the pane and in every container.
  The sections stay where they were among the container's other children, such as a channel's name above them.
- **Defaults.** The set's first container in the document says which sections there are, their default order and which
  are hidden by default; the other containers are expected to have the same sections.
- **Containers that come later.** Containers added or rebuilt after `attach`, such as the mixer's channels as tracks
  come and go, go through `Docking::arrangeSections(element)` once they are in the document: the element and the
  containers and setup panes in it get the user's order and hidden sections at once. A set that has no container yet,
  such as on a project without channels, is learned from the first one that comes this way, which fills its setup
  panes too. Containers in a docked panel's content are arranged, and their set learned, when the panel shows. The code
  that adds containers asks for this, rather than the docking system looking for new ones: RmlUi reports no changes to
  the document, and searching every document in each frame would cost time on a large mixer.

The mixer's channels are such containers ([mixer-window.md](mixer-window.md)); `tests/data/ui/sections.rml` is a test
window with sections, and `tests/data/ui/sections-later.rml` one whose channels only come after `attach`.

## Splitters

Dragging a zone's splitter sizes the zone, between its `min-width` or `min-height` and what leaves the other side its
minimum: zones have at least 120 by 80 dp, the column with the main zone 200 dp wide (base.rcss). The size is saved when
the drag ends. A double click on a splitter collapses its zone; showing the zone again restores its size.

## The layout and the engine

The whole layout is the user setting `ui.layout` ([settings.md](settings.md)), so it survives a restart and any client
changes it: MCP and Lua with `settings.set`, as the UI does through its engine binding. The UI changes its layout at
once and saves it; on `settings.changed` from another client it applies the new layout. While its own changes are on
their way to the engine it ignores the echoes, so a quick second change doesn't flicker back.

```json
{
  "windows": {
    "project": {
      "zones": {"lower": {"visible": true, "size": 280, "tabs": ["mixer", "editor"], "hidden": [], "active": "mixer"}},
      "strips": {"overviewLine": false, "infoLine": true}
    },
    "mixer1": {
      "sections": {"channel": {"order": ["fader", "routing", "inserts"], "hidden": ["inserts"]}}
    }
  },
  "panels": {"mixer": {"floating": true, "bounds": {"x": 200, "y": 150, "width": 800, "height": 500}}}
}
```

The setting has only what the user changed; what it leaves out has its default from the document's attributes and the
panels' order. A set's `order` puts the sections it names first, the others follow in the document's order; once a set
has `hidden`, it lists every hidden section, so the defaults no longer count. Panels, zones and sections it names that
don't exist are ignored, so a layout from a newer GNUbase still loads.
Workspaces (#163) will store such layouts by name, with the main windows' bounds.

## Key commands

The reference's defaults (p. 1572), bound everywhere; they act on the docking window the key went to:

| Keys | Command | Reference |
|---|---|---|
| Mod+Alt+L, Alt+I | `window.leftZone` | Show/Hide Left Zone |
| Mod+Alt+R | `window.rightZone` | Show/Hide Right Zone |
| Mod+Alt+E, Mod+Alt+B | `window.lowerZone` | Show/Hide Lower Zone |
| Mod+Alt+T | `window.transportBar` | Show/Hide Transport Zone |
| Mod+I | `window.infoLine` | Show/Hide Info Line |
| Alt+O | `window.overviewLine` | Show/Hide Overview |
| Mod+Alt+Left, Mod+Alt+Right | `window.previousTab`, `window.nextTab` | Show Previous/Next Tab, in the zone with the focus |

`window.menuBar`, `window.toolbar` and `window.statusLine` have no keys. Each zone is a focus zone (`focuszone` `left`, `right`, `lower`,
and `project` for the main zone), so key commands can depend on it ([key-commands.md](key-commands.md)). The document
is the focus zone `project` too, so the project window's keys, such as zooming, work from the toolbar and from a side
zone that doesn't bind them itself.

**Moving the focus between zones** (#379, p. 100). A click into a zone gives it the keyboard focus. Tab activates the
next shown zone of the docking window and Shift+Tab the previous one, around the ends, in the reference's order: the
project zone, the Channel zone once it exists, then the left, lower and right zones. From outside the zones, such as
the toolbar, Tab goes on as from the project zone. The docking system takes the key from the document's keydown before
RmlUi does and focuses the zone's element, so the zone gets `.focused` and its keys
([key-commands.md](key-commands.md)) work there; a control in the zone, such as a list view, takes its own keys once
it is clicked. Tab isn't a key command and can't be bound to another key. In a text field or a select box Tab moves
between controls as RmlUi does (#60), an open menu keeps it, and so do dialogs, which are modal documents of their own,
and a panel in a window of its own, whose document has no zones. Ctrl+Tab, Alt+Tab and Cmd+Tab aren't taken.

## Not yet

Sections in a panel that shows in a window of its own keep the document's order; the Channel tab (p. 73) with the
track inspector; tabs at the bottom of the lower zone, as in the reference; dragging a tab out of its zone to open it
in a window; the editor in the lower zone taking the focus when it opens (p. 100), once editors open there; and live
reload keeps the panels' contents only when the app opens again.

## Checks

`tests/docking_tests.cpp` drives the project window in the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)):
every zone and strip shown and hidden with the toolbar buttons, the key commands, the Set up Window Layout menu and
`ui.layout` from the engine, and reset to the defaults; splitters dragged in a row and a column, for zones before and
after them, held at the zones' minimums, and a double click that collapses a zone whose size comes back; tabs dragged
into a new order, hidden through the tab menu, stepped through with the keys in the zone with the focus, which gets
the focus frame, and the active tab remembered; whether a panel shows, behind another tab, in a hidden zone or in a
window of its own; Tab and Shift+Tab through the shown zones in the reference's order, around the ends and past
hidden zones, with a zone's keys working in the zone they reach, and Tab in a text field, Ctrl+Tab and Tab in an
open menu left alone; a panel moved into a window of its own through the tab menu, with its
contents made there and gone from the zone, docked with the Dock button and by closing its window, which reopens where
it was; and the complete layout, with zone sizes, tab orders, hidden and active tabs, strips and a panel's window,
back exactly after a restart. On a test window with sections, the setup pane shows and hides sections in every
channel and stays open, reorders them by drag and drop between the channel's other elements, and shows and hides a
strip and a zone; `ui.layout` from the engine reorders and hides them too, ignores names that aren't sections, is
refused in the wrong shape and resets to the defaults; and the order and visibility come back after a restart. A
channel added or rebuilt after `attach` takes the user's order and hidden sections at once and follows later changes,
and a window that starts without channels fills its setup pane from the first one that comes, with the user's layout,
or from a channel in a docked panel's content; a setup pane added later gets its buttons in the user's order.
