# Test plugins

GNUbase's own minimal plugins for testing plugin hosting (#63). They are small, deterministic and free of any GUI, so tests can check exact output, and two of them misbehave on purpose to test that a crashing or hanging plugin never takes GNUbase down (CLAUDE.md, rule 3).

| Plugin | Id | What it does | Parameters |
|---|---|---|---|
| gain | `org.gnubase.test.gain` | Multiplies its stereo input by a gain, from the event's sample offset on, without smoothing. | Gain (-60 to +12 dB) |
| synth | `org.gnubase.test.synth` | Instrument: 16 sine voices with a 2 ms attack and a linear release. Sends a note end event when a voice has faded out. Plays CLAP notes and MIDI note on/off. | Volume (dB), Release (ms) |
| latency | `org.gnubase.test.latency` | Reports a latency and delays its input by exactly that many samples. Changing the latency while active asks the host for a restart; the new latency applies on the next activate. | Latency (0 to 48000 samples, default 64) |
| paramstress | `org.gnubase.test.paramstress` | 1024 parameters with ids that are not contiguous: continuous, integer, list and switch parameters. While "Emit automation" is on, each block changes eight parameters itself, each with gesture begin, value and gesture end, like a user turning knobs in an editor. Audio passes through. | Emit automation, Parameter 1 to 1023 |
| crash | `org.gnubase.test.crash` | Crashes with an access violation as soon as Crash is set to On, on whichever thread sets it (see below). | Crash (Off, On) |
| hang | `org.gnubase.test.hang` | Stops responding forever as soon as Hang is set to On, on whichever thread sets it. | Hang (Off, On) |
| stateblob | `org.gnubase.test.stateblob` | Saves "Size" bytes of pseudo-random data generated from "Seed", with a header and a checksum, and refuses to load a state whose length or checksum is wrong. | Seed, Size (0 to 16 MiB, default 64 KiB) |

Every plugin has a stereo output port; all but the synth have a stereo input port. Every plugin has parameters and state; the state of all but stateblob is the list of parameter values.

## Crash and hang on demand

The crash and hang plugins misbehave on the thread that sets their parameter:

- a parameter event in a processed block: the audio thread,
- a parameter flush while the plugin is inactive: the main thread,
- loading a state saved with the parameter On: the main thread, while loading the state.

So a project that was saved with the parameter On crashes or hangs its plugin host again when it loads, which is what tests of reloading after a crash need. Never trigger them in the test process itself: only in a plugin host process, once there is one (#64, #65).

## Layout

- `common/testplugin.h`, `common/testplugin.cpp`: the format-independent plugin interface (parameters, events, process, state, latency) and its defaults.
- `common/<name>.cpp`: one file per plugin, each defining `pluginDescriptor()` and `createPlugin()`.
- `clap/wrapper.cpp`: turns a test plugin into a CLAP plugin: `clap_entry`, a factory with one plugin, and the audio-ports, note-ports, params, state and latency extensions. `clap/Info.plist.in` is the macOS bundle's Info.plist.

Each plugin is built from its own file plus the shared code into a plugin of its own, so a test can load, scan or blocklist one without the others. A plugin for another format, such as VST3, only needs a wrapper next to `clap/`; the plugins themselves stay the same.

## Building

The CMake option `GNUBASE_TEST_PLUGINS` (on by default, so on in CI) builds them, with the CLAP SDK in `ext/clap`. The target `gnubase-test-plugins` builds them all, and `gnubase-tests` depends on it. They go into `<build>/plugins/<config>/clap/`, with each platform's layout:

| Platform | Layout |
|---|---|
| Windows | `gnubase-test-gain.clap`, a DLL |
| Linux | `gnubase-test-gain.clap`, a shared library |
| macOS | `gnubase-test-gain.clap/Contents/MacOS/gnubase-test-gain` with `Contents/Info.plist`, a bundle |

The plugins are GNUbase code: they get the same warnings, sanitizers and clang-tidy checks as everything else. They export only `clap_entry`.

## Tests that use them

`tests/clap_plugin_tests.cpp` loads each plugin the way a host does, but into the test process: `clap_entry`, `init` with the plugin's path, the plugin factory, `create_plugin`, `init`, `activate`, `start_processing`, `process`. It checks that every plugin loads and processes a block, that the gain is applied from the right sample, that the synth plays notes and ends its voices, that the latency is reported to the host when the plugin is activated, applied, and that changing it requests a restart, that loading a state has the host rescan the values, the 1024 parameters and their automation output, and a byte-for-byte state round trip through streams that move only a few bytes per call. For crash and hang it only checks that their parameters exist and are off.

The test executable finds the plugins through `GNUBASE_TEST_PLUGINS_DIR`, set by CMake.

The plugin host tests (`tests/pluginhost_tests.cpp`) run the same plugins in plugin host processes, including hang, through a format of their own instead of plugin files: `gnubase-test-plugins-static` links every plugin into one library, with each plugin's `pluginDescriptor()` and `createPlugin()` renamed after the plugin (`gainPluginDescriptor()`), and `tests/pluginhost/testformat.h` adapts them to the GNUbase plugin API as the format `test` (`test:gain`, `test:hang`, ...). The tests' host, `gnubase-testpluginhost`, is the shipped host's code plus that format; the tests also create the plugins in the test process through it, to compare the hosted output with the in-process one. Loading the CLAP files in a host process comes with the CLAP adapter (#67).
