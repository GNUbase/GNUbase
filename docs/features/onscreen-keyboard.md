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
  a C to the E two octaves up, each labelled with the computer key that plays it. A key lights (`.pressed`) while it
  sounds.
- **Computer keys.** The lower row plays the lower octave, Z S X D C V G B H N J M for C to B, and comma the next C;
  the upper row plays from that C up, Q 2 W 3 E R 5 T 6 Y 7 U I 9 O 0 P for C to E. Several keys play at once. A key
  plays from its press to its release, and a held key's OS repeats play nothing more. Keys follow the keyboard layout,
  as key commands do ([key-commands.md](key-commands.md)).
- **Mouse.** Pressing the left button on a key plays it until the button goes up, or until the mouse leaves the window,
  where the button going up sends no event.
- **Octave and velocity.** The Octave slider moves the keyboard by octaves, seven in all: its lowest key is C0 to C6,
  C3 (middle C, note 60) by default. The Velocity slider sets the notes' velocity, 1 to 127, 100 by default. A note
  ends at the pitch it started at, also when the octave changes while it sounds. Both stay while the window is closed,
  until GNUbase quits.
- **Keys taken.** While the window is open, it takes the keys that no element used before the key commands, in every
  window: the keys of the two rows play, and the other keys run nothing, except those of the reference's exceptions,
  which keep their key commands: Mod+S (Save), Num* (Record), Space (Start/Stop), Num1 (to the left locator), Delete
  and Backspace (Delete), Num/ (Cycle), F2 (the transport panel) and Alt+K (closing the keyboard). Text fields keep
  the keys typed into them. Closing the window gives the key commands their keys back.
- **Notes end.** Notes that sound end when their key or button goes up, when GNUbase loses the keyboard focus, and
  when the window closes.

## Not yet

The computer keyboard display mode and the button that switches to it (the window shows the piano keyboard, which
both the mouse and the computer keys play); pitch bend and modulation by dragging on a key, and their indicators; the
octave and velocity as user settings that last across restarts; dragging the mouse across keys to play them in turn.

## Checks

`tests/onscreenkeyboard_tests.cpp`: `midi.keyboard` reaches an instrument track only while it monitors or is
record-enabled and set to all MIDI inputs, a record-enabled track records it, and it takes only MIDI 1.0 channel
messages. In the app's UI (`test::UiApp`, [ui-tests.md](ui-tests.md)), Alt+K opens the window; with the transport
recording onto a record-enabled instrument track, Z held down and released and a mouse button held on a key after
the octave and velocity changed record two notes at those pitches and velocities, and a key held with the mouse ends
when the button goes up out of the window; Num/ keeps its key command, Num0
runs none; and Alt+K closes the window, which keeps its octave for the next time.
