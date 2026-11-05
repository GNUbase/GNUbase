# Plugin slots

Each channel with audio has up to 16 plugin slots (#282). The `plugin.*` commands change them in the document; the
audio engine makes and runs their plugins from the render snapshot.

## Plugins in the engine

`engine::plugins::SlotInstances` makes, activates, deactivates and destroys the slots' plugins as jobs on the plugin
job thread (`HostManager::run`), never on the command or audio thread. Until a plugin is active for the device's
sample rate and block size, its slot node has no processor and passes the audio on.

A plugin that a graph may still process leaves the graph first: the command thread builds and publishes a graph
without it, and only once the audio thread runs that graph (or no device runs) is the plugin deactivated, activated
again or destroyed. `SlotInstances` waits for that on a thread of its own, so the job thread goes on making other
plugins meanwhile. That thread sleeps on a semaphore (`rt::Semaphore`) and checks again only when it is woken
(`SlotInstances::wake`): by the audio thread when it starts a newer graph (a semaphore release, which never blocks),
when the device stops, when another plugin is to leave, and when the slots close. Nothing polls.

## Status and events

A slot reports its plugin's status: `loading`, `active`, `inactive`, `failed`, `unavailable` or, without an open
audio device, `noDevice`. `plugin.changed` announces a slot whenever its status changes:

- after `audio.updatePlugins`, which the engine runs when a plugin is ready, failed or asked to be restarted;
- when the device closes or opens: `audio.setDevice`, `audio.set`, `audio.reset`, `audio.reconnect`, and a new
  project sample rate that the device follows. A device that goes away turns every slot to `noDevice`; one that
  returns with the same format brings back the statuses the slots had. If one of these commands fails after it closed
  the device, every client still gets the events, as it does `audio.changed`.

`plugin.move` to another channel emits `channel.changed` for both channels (by id), `plugin.changed` with the slot's
new channel and index, then `automationLane.removed` for the lanes of its parameters, the same events in the same
order as undoing and redoing the move. Within a channel only the order changes, which `channel.changed` carries.

## Files

In a DAWproject file, a built-in plugin is a `BuiltinDevice` in its channel's `Devices`, with a `RealParameter` for
each parameter set on it. The parameter's unit becomes the DAWproject unit the format requires: dB `decibel`, Hz
`hertz`, s `seconds`, % `percent`, and `linear` for the others, such as milliseconds and plain numbers.
