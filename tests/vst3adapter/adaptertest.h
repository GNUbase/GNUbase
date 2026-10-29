// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

// The VST3 plugin that tests the VST3 adapter (README.md), and what the tests know about it. It is written against the
// VST3 interfaces alone, so it exercises what a VST3 host has to do: a component and a separate controller that talk
// through messages, buses, normalized parameters with units, a program list, MIDI mapping, events, note expressions,
// output parameters and events, latency, tail and both states.
namespace gnubase::tests::vst3adapter
{
	// The component's class id, as the adapter writes it in plugin ids: "vst3:<class id>".
	inline constexpr const char* g_classId = "6E7A2B114C3D4E5F8A9B0C1D2E3F4051";

	enum Param : uint8_t
	{
		Gain = 0,         // linear gain 2 * normalized, applied from the sample of each change; unit "Amp"
		Bypass = 1,       // the plugin's bypass: the input, delayed by the latency
		Mode = 2,         // list: Clean, Invert, Mute (VST3's rule for steps); unit "Amp"
		Latency = 3,      // latency 1000 * normalized samples, applied at activation; a change restarts the component
		Program = 4,      // program change of the root unit's program list: Init (gain 0.5), Loud (1.0), Quiet (0.25).
						  // Its default, 0.7, lies between two steps: Quiet by VST3's rule.
		NoteCount = 5,    // read only: note ons received / 128, reported at the note on's sample
		LastTuning = 6,   // read only: the last tuning note expression received (normalized)
		Tempo = 7,        // read only: the tempo / 1000 at the start of every process call
		EditTrigger = 8,  // hidden: setting it to 1 makes the controller edit Gain to 0.25 as an editor would, with a
						  // gesture, and mark its state dirty. Its text says whether the controller got the component's
						  // message.
		Layout = 9,       // hidden: 1 asks for mono main buses. The controller restarts the component (kIoChanged), and
						  // the component switches its main input and output when it is deactivated.
		AudioEdit = 10,   // hidden: 1 makes the component edit Gain to 0.75 with a gesture and ask for a values rescan
						  // through the controller's component handler, on the audio thread, as many plugins do
		ProcessMode = 11, // read only, at the start of every process call: (mode of setupProcessing + 1) / 4 if the
						  // process call has the same mode, otherwise 0
		Odd = 12,         // read only: its title fills all 128 characters, without a terminator, and starts with an
						  // unpaired UTF-16 surrogate
		ParamCount = 13,
	};

	inline constexpr uint32_t g_maxLatency = 1000;
	inline constexpr uint32_t g_tail = 4800;
	inline constexpr uint8_t g_gainController = 7; // MIDI CC 7 controls Gain
}
