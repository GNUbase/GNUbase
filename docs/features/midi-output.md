# MIDI output

A MIDI track can play to a hardware MIDI output port instead of an instrument track (#2173, reference manual p. 349).

- **Routing.** `channel.set` `midiPort` names the port by its key in the user setting `midi.portSettings`:
  `backend/out/id`, with the backend and id that `midi.ports` lists (`dummy/out/Synth`, `winmm/out/USB MIDI`). Empty,
  the default, plays to no port. A MIDI channel plays to an instrument (`output`) or to a port, not both: setting one
  clears the other, and giving both in one call is refused. Only MIDI channels have it. `project.get` shows it with the
  channel, the project file keeps it in GNUbase's entry, and undo takes it back. A port that isn't connected keeps the
  routing and plays nothing.
- **The events** are the ones the track's channel sends to an instrument: its parts' and its input's, after its MIDI
  modifiers, inserts and mute, on the track's MIDI channel (`midiChannel`, [mixer.md](mixer.md)), with the note-offs it
  sends where playback stops or jumps, and its program, bank, MIDI volume and pan. They become MIDI 1.0 messages as
  `midi.capture` shows them (`MidiTap::toMidi1`).
- **Timing.** After the graph processed a block, the audio thread queues each port channel's events, lock-free and
  without allocating, in the order of the channels (`MidiPorts::send` in `src/engine/audio/midiports.h`): each at the
  host time of the block's start plus its frame offset and the device's output latency, so it sounds with the audio of
  its frame. A sender thread, woken by a semaphore release that never blocks, passes them on to the port
  (`midi::Output::send`), whose backend sends each at its time. The times come from the frame offsets alone, so they are
  the same for any number of processing threads. What the queue (4096 messages) can't take is dropped.
- **Ports** open the first time a routing names them, which lists the MIDI ports if no midi command has yet, and stay
  open until the engine closes.

Tests: `tests/midioutput_tests.cpp` plays MIDI tracks to a dummy loopback port through the dummy device, offline, and
checks the messages' times and MIDI channel, that one and four processing threads give the same messages at the same
times, and the routing's rules.

Not yet:
- MIDI devices with an output and a MIDI channel of their own, which a track plays to (p. 349), the inspector's and the
  track list's port choice, and Panic and All Notes Off for the ports.
- MIDI sends and the metronome's MIDI click to a port.
- A port that is plugged in after the routing plays once the routing changes again; an output that went away and came
  back isn't opened again.
