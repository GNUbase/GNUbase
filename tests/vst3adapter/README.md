# VST3 adapter test plugin

`adaptertest.cpp` is a small VST3 plugin for the tests of the VST3 adapter (#68) in `tests/vst3_adapter_tests.cpp`. The shared test plugins in `tests/plugins` cover loading, audio, notes, latency, many parameters, large states, crashes and hangs; they are components that are their own controllers. This plugin covers what a VST3 host must also handle and they don't have:

- a **separate controller class**, which the adapter creates from the component's controller class id and connects to the component in both directions. The component greets the controller with a message the host allocates (`IHostApplication::createInstance`); the hidden parameter "Edit trigger" shows in its text whether the greeting arrived;
- **buses**: a stereo main input, a side-chain input that is off by default, a stereo output, and an event input and output. Mono and stereo are accepted, as long as the main input and output match;
- **parameters with units**: Gain and Mode belong to the unit "Amp"; Bypass has `kIsBypass`, Mode and Program are lists, three parameters are read only and report what the processor got (note ons, the last tuning expression, the tempo of each process call);
- a **program list** on the root unit with three programs and their pitch names, and a program change parameter;
- **MIDI mapping**: controller 7 controls Gain;
- **events**: every input event comes back on the output bus as it came in;
- **edits in the controller**: setting "Edit trigger" makes the controller edit Gain with `beginEdit`, `performEdit` and `endEdit`, as an editor would;
- **restarts**: a Latency change in the controller calls `restartComponent(kLatencyChanged)`, a program change `kParamValuesChanged`;
- **both states**: the component's state holds the values, the controller's counts the edits.

The plugin's audio is the input times twice the normalized gain, inverted or muted by Mode, plus the side-chain when it is active, delayed by the latency. `adaptertest.h` has the class id and parameter ids the tests use.

It is written against the interface headers alone (`ext/vst3sdk/pluginterfaces`) and refers to the SDK's namespace through `src/pluginhost/vst3/vst3ns.h`. CMake builds it with the shared test plugins (`GNUBASE_TEST_PLUGINS`) into `<build>/plugins/<config>/vst3adapter/gnubase-vst3-adaptertest.vst3`, with the same bundle layout as theirs, and passes the path to the tests as `GNUBASE_VST3_ADAPTERTEST`.
