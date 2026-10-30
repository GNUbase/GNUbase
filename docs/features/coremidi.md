# CoreMIDI

macOS MIDI ports through CoreMIDI (`src/platform/midi/coremidi.cpp`, backend `coremidi`, #41). The port model and the
other backends are in [architecture.md](../architecture.md#midi-ports).

- **Ports.** CoreMIDI's sources are inputs and its destinations outputs: devices, the IAC driver's buses and the virtual
  sources and destinations of other applications. A port is listed by its display name ("IAC Driver Bus 1"), and its id
  is the endpoint's CoreMIDI unique id, which stays the same across runs. A port's `midi2` is set when the endpoint
  speaks MIDI 2.0 (CoreMIDI's protocol property).
- **Universal MIDI Packets.** Every supported macOS (11 and later) has CoreMIDI's UMP interface, so ports use it only:
  inputs and outputs speak the MIDI 1.0 protocol, and CoreMIDI translates to and from MIDI 2.0 endpoints. Channel
  messages are UMP type 2, system common and real-time messages type 1; SysEx isn't carried yet. MIDI 2.0 resolution
  inside the engine comes later.
- **Time.** CoreMIDI stamps input with mach absolute time; the backend turns it into host time. The two clocks drift
  apart by the time the Mac slept, so each conversion reads both clocks. Outputs stamp what is sent for later with its
  mach time and leave the scheduling to CoreMIDI.
- **One client.** The process has one CoreMIDI client, since CoreMIDI advises against disposing clients. It starts with
  the first listing or port, so an engine that never uses MIDI leaves CoreMIDI alone. Its notifications arrive on the
  run loop of a thread of its own; each setup change makes every CoreMIDI backend report changed ports.
- **Tests** (`tests/mididevice_tests.cpp`, macOS only): the port contract runs against a loopback of virtual ports the
  test creates itself (a virtual destination that passes everything on to a virtual source of the same name), checks
  that the ports are noticed coming and going, and also loops through "IAC Driver Bus 1" when the IAC driver is
  enabled (Audio MIDI Setup). A virtual MIDI 2.0 source checks the `midi2` flag and the translation to MIDI 1.0. The
  hidden `[hardware]` contract test covers CoreMIDI too, e.g. with `GNUBASE_TEST_MIDI_LOOPBACK="IAC Driver Bus 1"`.
