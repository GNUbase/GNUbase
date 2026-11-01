# MIDI notes and controllers

Notes and controllers in the document, and the resolution of MIDI events. How they play is in [playback.md](playback.md), how tests send MIDI into tracks and read back what instruments played in [midi-injection.md](midi-injection.md).

- Notes have a pitch, a MIDI channel, a velocity and a release (note-off) velocity, which defaults to the center, MIDI 1.0's 64 (#137); DAWproject stores it as `rel`.
- MIDI controllers have a type: `controller` (a number and a value), `pitchBend`, `channelPressure`, `polyPressure` (a key and a value), `program` (a number and an optional bank, 0 to 16383) and `sysEx` (the whole message, from 0xF0 to 0xF7). Values are normalized: 0 to 1, pitch bend centered at 0.5. In DAWproject controllers, pitch bend and both kinds of pressure are `Points` lanes; program changes and SysEx are GNUbase-only data. An instrument track's channel names the built-in instrument it plays (`channel.set` with `instrument`), until plugin slots come.
- Events are MIDI 2.0 resolution: 16-bit velocities and 32-bit controller values (`src/engine/midi/midivalues.h`). Converting to and from MIDI 1.0 follows the MIDI 2.0 min-center-max rule: scaling up repeats bits so the center and the maximum stay exact, scaling down shifts.
