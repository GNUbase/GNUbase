# On-screen keyboard

Playing and recording MIDI notes with the computer keyboard and the mouse (#103, reference manual pp. 332-334), for
when no MIDI keyboard is at hand.

- **A MIDI input.** The engine command `midi.keyboard` (MCP `midi_keyboard`, Lua `gnubase.midi.keyboard`) plays MIDI
  1.0 channel messages from the on-screen keyboard. It is one of all MIDI inputs: the messages arrive when the command
  runs, as a MIDI port's do, through the same input queue (`MidiPorts::play`), so the audio device places them in its
  next block. MIDI and instrument tracks set to all MIDI inputs play them while they monitor or are record-enabled
  (MIDI thru), and the record-enabled ones record them with everything MIDI recording does
  ([midi-recording.md](midi-recording.md)). The keyboard isn't a port: `midi.ports` doesn't list it and `midi.monitor`
  doesn't show its messages. Unlike `midi.inject` ([midi-injection.md](midi-injection.md)), which plays into one
  track at a frame position for tests, it plays now, into every track that listens to all inputs.
- **The window.** Studio > On-Screen Keyboard, or Alt+K, opens the on-screen keyboard in a window of its own
  (`resources/ui/onscreen-keyboard.rml`, `ui::OnScreenKeyboard` in `src/ui/onscreenkeyboard.h`, opened by the UI
  command `window.onScreenKeyboard` in `app::AppUi`), and closes it again. It shows a piano keyboard of 29 keys, from
  a C to the E two octaves up, each labelled with the computer key that plays it, as the keyboard layout has it at
  that key's place (p. 333): Z on a US layout, Y on a German one (`Shell::keyLabel`, from SDL). When the layout
  changes while the window is open, the keys show the new one's (`Shell::onKeymapChanged`, from SDL's keymap change).
  A key lights (`.pressed`) while it sounds.
- **Computer keys.** The keys play by their places, whatever the keyboard layout, so that the two rows form a piano
  on every layout (#691). On a US keyboard the lower row plays the lower octave, Z S X D C V G B H N J M for C to B,
  and comma the next C; the upper row plays from that C up, Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P for C to E. So on a
  German layout the key labelled Y plays the lower C, and on a French one A plays the upper C. A key press carries its
  physical key (`KeyPress::position`, from SDL's scancode, for every key) besides the layout's key, which key commands
  follow ([key-commands.md](key-commands.md)); keys from `ui::Automation`, MCP and Lua have no place and count as typed
  on a US keyboard. Several keys play at once. A key plays from its press to its release, and a held key's OS repeats
  play nothing more.
- **Mouse.** Pressing the left button on a key plays it until the button goes up, or until the mouse leaves the window,
  where the button going up sends no event.
- **Octave and velocity.** The Octave slider moves the keyboard by octaves, seven in all: its lowest key is C0 to C6,
  C3 (middle C, note 60) by default. The Velocity slider sets the notes' velocity, 1 to 127, 100 by default. A note
  ends at the pitch it started at, also when the octave changes while it sounds. Both are the user setting
  `ui.onScreenKeyboard`, `{"octave": 3, "velocity": 100}` by default, which the window reads as it opens and sets
  once a change ends, when the mouse button or a key goes up or the window closes, not for each step of a drag; so
  they last across restarts (#691).
- **Keys taken.** While the window is open, it takes the keys that no element used before the key commands, in every
  window: the keys of the two rows play, and the other keys run nothing, except the keys of the reference's exceptions,
  commands that keep their keys (#691): Save (Mod+S by default), Start/Stop Record (Num*), Start/Stop Playback (Space),
  Jump to Left Locator (Num1), Delete (Delete, and Backspace in the project window, the sample editor and the key
  editor's note display), Cycle on/off (Num/), Show/Hide Transport Panel (F2) and Show/Hide On-Screen Keyboard (Alt+K).
  The window asks the key commands what a key would run (`KeyCommands::bound`), so a command the user gave another key
  keeps that key, and its old key runs nothing. Text fields keep the keys typed into them. Closing the window gives the
  key commands their keys back.
- **Notes end.** Notes that sound end when their key or button goes up, when GNUbase loses the keyboard focus, and
  when the window closes.

## Not yet

The computer keyboard display mode and the button that switches to it (the window shows the piano keyboard, which
both the mouse and the computer keys play); pitch bend and modulation by dragging on a key, and their indicators;
dragging the mouse across keys to play them in turn.

## Checks

`tests/onscreenkeyboard_tests.cpp`: `midi.keyboard` reaches an instrument track only while it monitors or is
record-enabled and set to all MIDI inputs, a record-enabled track records it, and it takes only MIDI 1.0 channel
messages. In the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)), Alt+K opens the window; with the transport
recording onto a record-enabled instrument track, Z held down and released and a mouse button held on a key after
the octave and velocity changed record two notes at those pitches and velocities, and a key held with the mouse ends
when the button goes up out of the window; Num/ keeps its key command, and with Cycle on Alt+Y in the user's key
commands Alt+Y runs it and Num/ runs nothing; Num0 runs none; and Alt+K closes the window, which keeps its octave for
the next time. Dragging the octave and velocity sliders through `ui::Automation` sets them, a value between two
steps rounds to one, and a drag of less than half a step per mouse move still moves the octave: sliders, knobs and
faders drag from their unrounded value, so a document that rounds each change doesn't hold the drag back (#691). A
German layout's Y at a US keyboard's Z plays the lowest key and not the upper row's Y, a key at the 2's place that the
layout's key codes don't name plays the upper C sharp, an M from the window system at a US keyboard's semicolon plays
nothing while an M without a place plays B, and the lowest key's label is what the window system says the layout has
at Z's place, also again after a keymap change. Changed octave and velocity are saved when the mouse button goes up,
not before, and kept across a restart.
