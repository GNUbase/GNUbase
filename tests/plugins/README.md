# Test plugins

GNUbase's own minimal plugins for testing plugin hosting (#63). They are small, deterministic and free of any GUI, so tests can check exact output, and two of them misbehave on purpose to test that a crashing or hanging plugin never takes GNUbase down (CLAUDE.md, rule 3).

| Plugin | Id | What it does | Parameters |
|---|---|---|---|
| gain | `org.gnubase.test.gain` | Multiplies its stereo input by a gain, from the event's sample offset on, without smoothing. | Gain (-60 to +12 dB) |
| synth | `org.gnubase.test.synth` | Instrument: 16 sine voices with a 2 ms attack and a linear release of up to 2 s, its tail. Sends a note end event when a voice has faded out or was taken for a new note because all 16 were busy (CLAP only). Plays CLAP notes, MIDI note on/off and VST3 note events. | Volume (dB), Release (ms) |
| latency | `org.gnubase.test.latency` | Reports a latency and delays its input by exactly that many samples. Changing the latency while active asks the host for a restart; the new latency applies on the next activate. | Latency (0 to 48000 samples, default 64) |
| paramstress | `org.gnubase.test.paramstress` | 1024 parameters with ids that are not contiguous: continuous, integer, list and switch parameters. While "Emit automation" is on, each block changes eight parameters itself, each with gesture begin, value and gesture end, like a user turning knobs in an editor. Audio passes through. | Emit automation, Parameter 1 to 1023 |
| crash | `org.gnubase.test.crash` | Crashes with an access violation as soon as Crash is set to On, on whichever thread sets it, or when it loads a state saved with Crash on load On (see below). | Crash, Crash on load (Off, On) |
| hang | `org.gnubase.test.hang` | Stops responding forever as soon as Hang is set to On, on whichever thread sets it, or when it loads a state saved with Hang on load On. | Hang, Hang on load (Off, On) |
| stateblob | `org.gnubase.test.stateblob` | Saves "Size" bytes of pseudo-random data generated from "Seed", with a header and a checksum, and refuses to load a state whose length or checksum is wrong. | Seed, Size (0 to 16 MiB, default 64 KiB) |

Every plugin has a stereo output port; all but the synth have a stereo input port. Every plugin has parameters and state; the state of all but stateblob is the list of parameter values.

## Crash and hang on demand

The crash and hang plugins misbehave on the thread that sets Crash or Hang:

- a parameter event in a processed block: the audio thread,
- a parameter flush while the plugin is inactive: the main thread.

Setting Crash on load or Hang on load does nothing by itself, so a host can save a state with it On. Loading that state crashes or hangs the plugin on the main thread, while it loads. So a project saved with it On crashes or hangs its plugin host again when it loads, which is what tests of reloading after a crash need. Never trigger them in the test process itself: only in a plugin host process (#64, #65). Tests that go through every plugin use a list without crash and hang, so a test that sets parameters can't trigger them by accident.

## Layout

- `common/testplugin.h`, `common/testplugin.cpp`: the format-independent plugin interface (parameters, events, process, state, latency) and its defaults.
- `common/<name>.cpp`: one file per plugin, each defining `pluginDescriptor()` and `createPlugin()`.
- `clap/wrapper.cpp`: turns a test plugin into a CLAP plugin: `clap_entry`, a factory with one plugin, and the audio-ports, note-ports, params, state and latency extensions.
- `vst3/wrapper.cpp`: turns a test plugin into a VST3 plugin: the module entry points and `GetPluginFactory`, a factory with one class, and a single component that is processor and edit controller at once (see below).
- `common/Info.plist.in`: the Info.plist of the macOS bundles of both formats.

Each plugin is built from its own file plus the shared code into a plugin of its own per format, so a test can load, scan or blocklist one without the others. Another format only needs another wrapper; the plugins themselves stay the same.

## VST3

The VST3 wrapper uses only the interface headers of the VST3 SDK (`ext/vst3sdk/pluginterfaces`) and implements the interfaces itself. It refers to the SDK's namespace through the `vst` alias from `src/pluginhost/vst3/vst3ns.h` ([docs/dependencies.md](../../docs/dependencies.md#vst3-sdk)). What differs from CLAP:

- **One object.** Each plugin's component (`IComponent`, `IAudioProcessor`) is also its edit controller (`IEditController`): `getControllerClassId` returns `kNotImplemented`, and hosts query the controller from the component. The class isn't distributable (`classFlags` 0). The factory implements `IPluginFactory3`; the class id is hashed from the plugin's id, so it is stable across builds.
- **Normalized parameters.** Hosts see values from 0 to 1, mapped linearly onto each parameter's range. Stepped parameters have `stepCount` set and follow VST3's rule for discrete values: of n steps, step k is k/n, and a normalized value v is step min(n, floor(v·(n+1))). Parameters with value names are lists. Parameter changes from the host's queues are applied as steps at their sample offsets, not as ramps; the points of all queues and the note events are sorted by time, those at the same time in the order the host sent them.
- **Who sets values.** While the plugin is active, the processor owns the values: `setParamNormalized` on the controller doesn't change them then, because the host sends the same change to the processor. While inactive, `setParamNormalized` sets the value on the main thread, like CLAP's parameter flush, so it is the main-thread trigger of crash and hang.
- **Restart.** The latency plugin asks for a restart on the audio thread. VST3 only allows `restartComponent` on the main thread, so the wrapper passes it on with `kLatencyChanged` the next time the host calls `setParamNormalized`, which hosts do for every change they send to the processor. `setState` calls `restartComponent` with `kParamValuesChanged`, plus `kLatencyChanged` when the loaded state changed the latency of an active plugin.
- **Reset.** `setActive(true)` and `setProcessing(false)` reset the plugin's state, like CLAP's `reset`: no synth voice survives a restart, e.g. for a new latency or sample rate.
- **Tail.** `getTailSamples` reports the plugin's longest tail at the sample rate from `setupProcessing`: the synth's longest release, `kNoTail` for the others.
- **Output events.** Parameter values the plugin sets itself go to the output parameter changes. Gestures and note end events have no VST3 equivalent in process, so the parameter stress plugin's automation comes without gestures and the synth's voices end silently.
- **State.** `getState` and `setState` of the component and of the controller have the same signatures, so one implementation serves both; `setComponentState` has nothing left to do. `setState` reads until the stream returns no bytes or reports a short read as `kResultFalse`, as a file stream does on its last read, and keeps the bytes of that read.
- At most 8192 input events per block are used (the buffer is allocated on activation, not in process). Only 32-bit samples and stereo buses are supported.

## Building

The CMake option `GNUBASE_TEST_PLUGINS` (on by default, so on in CI) builds them, with the CLAP SDK in `ext/clap` and the VST3 SDK in `ext/vst3sdk`. With the option off, CMake needs neither SDK. The target `gnubase-test-plugins` builds them all, and `gnubase-tests` depends on it. They go into `<build>/plugins/<config>/clap/` and `<build>/plugins/<config>/vst3/`, with each platform's layout:

| Platform | CLAP | VST3 |
|---|---|---|
| Windows | `gnubase-test-gain.clap`, a DLL | `gnubase-test-gain.vst3/Contents/x86_64-win/gnubase-test-gain.vst3`, a DLL (`arm64-win` on ARM64) |
| Linux | `gnubase-test-gain.clap`, a shared library | `gnubase-test-gain.vst3/Contents/x86_64-linux/gnubase-test-gain.so` (`aarch64-linux` on ARM64) |
| macOS | `gnubase-test-gain.clap/Contents/MacOS/gnubase-test-gain` with `Contents/Info.plist`, a bundle | `gnubase-test-gain.vst3/Contents/MacOS/gnubase-test-gain` with `Contents/Info.plist`, a bundle |

The plugins are GNUbase code: they get the same warnings, sanitizers and clang-tidy checks as everything else. They export only their format's entry points: `clap_entry`, or `GetPluginFactory` with `InitDll`/`ExitDll` (Windows), `ModuleEntry`/`ModuleExit` (Linux) or `bundleEntry`/`bundleExit` (macOS). Everything else has hidden visibility; on Linux a version script and on macOS an exported symbols list, both generated by CMake, also hide what the compiler still exports, such as the standard library's template code, and on Windows only what the code marks `dllexport` is exported. A ctest test per plugin checks this with `nm` or `dumpbin` (`check-exports.cmake`).

## Tests that use them

`tests/clap_plugin_tests.cpp` and `tests/vst3_plugin_tests.cpp` load each plugin the way a host does, but into the test process, because GNUbase has no plugin host yet (#64). For CLAP: `clap_entry`, `init` with the plugin's path, the plugin factory, `create_plugin`, `init`, `activate`, `start_processing`, `process`. For VST3: the platform's module entry, `GetPluginFactory`, `createInstance`, `initialize`, `setBusArrangements`, `setupProcessing`, `setActive`, `setProcessing`, `process`. Both check that every plugin but crash and hang loads and processes a block (VST3 also every plugin's bundle layout, the factory's class infos and unique class ids), that the gain is applied from the right sample, that the synth plays notes and its voices end, that the latency is reported and applied and that changing it requests a restart, the 1024 parameters and their automation output, and a byte-for-byte state round trip through streams that move only a few bytes per call. For crash and hang they only check that their parameters exist and are off, and that a state with Crash on load or Hang on load On can be saved. The VST3 tests also cover a file stream's short last read, the host notifications after a state load, the reset on activation and when processing stops, the synth's tail, VST3's rule for stepped values and eight points on each of the parameter stress plugin's parameters in one block.

The test executable finds the plugins through `GNUBASE_TEST_PLUGINS_DIR`, set by CMake. Plugin host tests (#64 to #67) will load the same files in `gnubase-pluginhost` processes, including crash and hang.
