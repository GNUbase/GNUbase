// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vector>

// Tempo detection (#158, docs/features/tempo-detection.md): where the beats of rhythmic material are, from where its
// sounds start. It allocates and takes time in proportion to the material, so it runs on the command thread, never on
// audio threads.
namespace gnubase::engine::analysis
{
	// A sound's start, in seconds, and how strongly it stands out (any positive scale).
	struct Pulse
	{
		double time = 0.0;
		double weight = 1.0;
	};

	// The beats of material length seconds long, in seconds, in order. The pulses make an onset envelope at 200 frames
	// per second; its autocorrelation, weighted towards 120 BPM, gives the beat period (40 to 240 BPM), and dynamic
	// programming the beats, which may drift from it (beat tracking). A beat near a pulse moves onto it. Empty for
	// material with fewer than two beats.
	std::vector<double> detectBeats(const std::vector<Pulse>& pulses, double length);

	// The beats factor times as fast (2: a beat between each two, 0.5 every other one), half a beat later with
	// offbeat, between the first and the last beat.
	std::vector<double> rescaleBeats(const std::vector<double>& beats, double factor, bool offbeat);

	// The beats at a steady tempo: on the straight line that fits them best.
	std::vector<double> smoothBeats(const std::vector<double>& beats);
}
