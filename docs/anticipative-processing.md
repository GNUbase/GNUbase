# Anticipative processing

Design note for #23. Status: decided, not implemented yet. It follows the reference manual, pp. 1587–1592.

## Goal

Most of a project plays from the timeline: audio events, MIDI parts and the instruments, inserts and busses that process them. Nothing a musician plays live reaches these nodes, so they can be computed ahead of the audio device, in larger blocks, on threads that may be preempted. Only the nodes that carry live input must run in the device callback. As a result:

- fewer dropouts at small device buffers, because short spikes in the prefetched part are absorbed by its lead;
- more tracks and plugins at a given buffer size, because large blocks cost less per sample, especially across the plugin IPC;
- the device latency for live playing stays as low as the hardware allows.

The cost is the **prefetch latency**: a change to a prefetched node, such as a fader move, is heard that much later. Timeline material isn't affected: it plays at its position, and the prefetch latency is not part of the signal path.

## Two paths through one graph

The graph (#15) gets a **live** flag per node. The command thread computes it when it describes the graph:

1. A node is a **live source** when it receives live input:
   - an input bus feeding a channel that may monitor its input (monitor or record enable on, [features/latency-and-monitoring.md](features/latency-and-monitoring.md)) or records (#96);
   - an instrument or MIDI channel that monitors or is record-enabled (#97), and so receives MIDI from a device;
   - a plugin slot with prefetching turned off (a per-plugin setting, for plugins that misbehave with large or early blocks);
   - a plugin slot whose editor is open, or with automation in a write mode (write, touch, latch; #120). Edits in a plugin's editor reach the plugin as it processes; on a prefetched slot that is up to *L* ahead of what the user hears, so knob moves would lag by *L* and touch automation would be recorded *L* early ([plugin-api.md](plugin-api.md#parameters-and-automation));
   - external effects and instruments, which route through hardware.
2. Every node that receives audio or events from a live node is live too, down to the output busses. For example, a group bus that sums a monitored track is live; the other tracks feeding it stay prefetched.
3. Everything else is prefetched. With prefetching turned off, every node is live, which is today's behavior.

`CompiledGraph` splits its processing order into two schedules:

- The **prefetch schedule** holds the prefetched nodes in the same deterministic order. It runs on the prefetch threads (see "Threads") in **prefetch blocks** of a fixed size *P*.
- The **live schedule** holds the live nodes. It runs in the device callback in device blocks of size *B*, on the audio thread and the workers of #212.

Every connection from a prefetched node to a live node becomes a **prefetch link**, a single-producer, single-consumer ring:

- An audio link holds the node's channels for *L + P* frames.
- An event link holds timestamped events, with their offsets converted to stream frames.

The live schedule reads a link like an input: it mixes the link's frames for the current device block, or slices its events into the block. An output bus that only prefetched nodes reach becomes a plain link: the live side copies it to the device.

Each processor belongs to exactly one schedule at a time. No processor is ever processed from two threads in one cycle.

## Timing

The prefetch side runs ahead by the **prefetch latency** *L*, a whole number of prefetch blocks. A setting chooses the level: low, normal or high, for example 2, 4 or 8 blocks of *P* = 512 frames at 48 kHz, scaled with the sample rate. Higher levels are more stable, but they add latency and use more memory, just like the reference's levels.

Both sides count **stream frames**, the frames the device has played since it started. Every link entry is stamped with the stream frame it is meant for. The prefetch side processes stream frames [*T + L*, *T + L + P*) while the device plays *T*.

### Transport

The prefetch side has its own copy of the transport (`Transport`, #16), fed from the same request queue:

- **Cycle jumps** are known in advance. The prefetch side computes them itself, so loop points stay sample-accurate.
- **Play and locate** go to the prefetch side first. It ends its sounding notes (#19) at its head, starts again at the new position and fills *L*. The live side applies the request once the link holds the frames for the new position, and drops what it hasn't played of the old position.
  - This uses the same wait as disk cueing: `transport.play` and `transport.locate` already wait up to two seconds for the disk streams, and now also for the prefetch fill. *L* usually fills in a few milliseconds, because the prefetch threads run flat out.
- **Stop** must be heard at once, but the prefetch side has already processed up to *L* frames past the stop as playing, and processor state can't be rewound. So:
  1. The live side fades the links out over 5 ms from the stop frame and drops the rest.
  2. The prefetch side applies the stop at its head. Its processors keep running with the transport stopped, so reverb tails go on from there.
  3. The live side fades the links back in at that head.

  The tail of a prefetched node is therefore cut for up to *L*. At normal levels that is tens of milliseconds, and it only happens on stop.

### Parameter and document changes

- **Parameter changes and automation** for prefetched nodes are applied at the prefetch head: a fader moved now is heard after *L*. Automation (#120) reads the timeline, so it is exact at its position.
- **A new graph** (after a command) is swapped in on both sides at block boundaries. The prefetch side applies it at its head, so an edit is heard after *L*, without flushing and without a glitch.

### Moving a node between the paths

When a node switches from prefetched to live, for example because the user turns on monitoring:

1. The prefetch side stops processing the node at its head *H*. Its state is now exactly at *H*.
2. The live side keeps reading the node's link until it reaches *H*, then processes the node itself from *H* on.

Nothing is lost and nothing is computed twice. The switch completes within *L*.

When a node switches from live to prefetched, the prefetch side would need the node's state *L* frames ahead of where the live side has it, which nobody can compute in time. The node therefore stays live until the next stop or locate, when the links are refilled anyway. Monitoring turns off without a glitch; the processing load drops a little later.

## Determinism

Rendering stays bit-identical (#212). The output depends on the project, the input, *B*, *P* and *L*:

- Prefetch blocks start on a fixed grid of stream frames from playback start. The prefetch schedule always processes the same blocks with the same events, whatever the timing of the threads.
- Offline rendering (`audio.render`) runs both schedules on the calling thread: it fills the links to *L*, then alternates one live block with the prefetch blocks that keep the links full. Offline and real time therefore produce the same samples, as long as the prefetch side keeps up.
- When the prefetch side falls behind, the live side finds a link empty for its block. It plays silence for that link and counts a **prefetch dropout**. It never processes a prefetched node itself, because that node's state belongs to the other side. Prefetch dropouts and the prefetch load go to the performance data (#24, #45), next to the real-time load, as in the reference's performance monitor.

## Threads

The prefetch schedule runs on its own pool of **prefetch threads**, one per core minus one. Their real-time priority is lower than the audio thread's and the workers' (MMCSS "Audio" with a lower priority on Windows; on macOS, a lower-priority thread outside the device workgroup; on Linux, a lower SCHED_FIFO priority).

The scheduler, not cooperation, makes the live path win. A prefetch task can take as long as one large block, and the live path must never wait for one to finish. Prefetch threads follow every real-time rule: no allocations, locks or blocking calls. Within its schedule the pool uses the task graph of #212, with the same dependency counters and fixed summing order.

## Plugin IPC

- A plugin slot is prepared with a maximum block size of max(*B*, *P*). It keeps that size when it moves between the paths, so moving never calls into the plugin's setup.
- The GNUbase plugin API (#61) has a **process mode**: real time, prefetch or offline. The format adapters map it to their formats; VST3, for example, has a prefetch processing mode. Some plugins, such as samplers that stream from disk, need it to behave correctly. Every format sets the mode outside processing, so it is a main-thread call while the plugin is inactive, and changing it is a restart. A slot that moves from the prefetch path to the live path keeps its prefetch mode until the next stop, when it is restarted with the real-time mode; it never waits for the restart. Every block also says what it is, for information ([plugin-api.md](plugin-api.md#process)).
- Opening a plugin's editor, or putting its automation into a write mode, makes the slot live (see "Two paths through one graph"). It moves at once; closing the editor moves it back at the next stop or locate, like every live-to-prefetched switch.
- Large blocks amortize the cost per block of the IPC wake-up and the round trip (#62, #76). Several host processes compute prefetch blocks in parallel.
- The watchdog (#65) gives prefetch blocks a deadline in proportion to their length and their lead, not the device block's.
- A plugin's own latency is compensated as on the live path (#71). Prefetching doesn't change a node's path latency.

## Latency, meters and taps

- `audio.get` reports `prefetchLatency` in frames. Channels with a parameter control or editor should be able to show whether they are live (`graph.get`: `live` per node).
- The live path never sees the prefetch latency, so live input stays aligned with prefetched material by stream frame. I/O latency compensation for recording (#44) is unchanged.
- **Meters and capture taps** of prefetched nodes are computed early. They go through the links with their stream frame, and the live side publishes them when it plays that frame, so meters match what is heard (#111).

## Engine API

- `audio.setDevice` / `audio.get`: `prefetch` (on or off) and `prefetchLevel` (low, normal or high). Turning it on or off rebuilds the graph and applies at the next stop.
- `graph.get`: `live` per node, and the reason (input, monitored, record-enabled, plugin setting, editor open, automation writing, downstream).
- Plugin slots: `prefetch` (true by default), with the plugin slot milestone.

## Implementation steps

Each step comes with tests first, as always.

1. **Graph:** the live flag, the two schedules and the prefetch links, offline only. Tests:
   - A project without live nodes renders identically for every device block size *B* at a fixed *P*.
   - A project with a monitored input renders identically with prefetching on and off, except for the documented parameter-change latency.
   - The position probe and the note probe check sample positions across the links.
2. **Real time:** the prefetch threads and the transport handshake for play, locate, stop and cycle. Tests:
   - Real time matches offline.
   - The RT checker stays green on both sides.
   - Prefetch dropouts are counted when the prefetch threads are artificially stalled.
3. **Switching:** moving nodes to the live path while playing, and back at stop.
4. **Plugins:** the process mode in the plugin API, the per-plugin setting, and watchdog deadlines (with M05).

## Alternatives considered

- **One schedule, bigger device buffers.** This is simple, but live playing gets the large latency, which is what the feature exists to avoid.
- **Prefetching cooperatively on the #212 workers.** This needs no extra threads, but a long prefetch task blocks a worker the live path needs. Preemption by priority is simpler and more robust.
- **Rewinding prefetched processors on stop.** Plugins can't snapshot and restore their state in real time, so a short fade and a cut tail are the honest option.
- **Prefetching live nodes with lookahead on their input.** Impossible: live input doesn't exist before it is played.
