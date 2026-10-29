// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/processor.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::engine
{
	class Engine;
}

// GNUbase's built-in processors: they run inside the engine, unlike third-party plugins. The mixing effects (EQ,
// gate, compressor, limiter, delay, reverb, dither; effects.h), and test processors (generators, gain and pan, a
// simple synth), so MIDI-to-audio paths can be tested before plugin hosting. builtinplugin.h offers them all through
// the plugin API.
namespace gnubase::engine::processors
{
	struct ParameterInfo
	{
		uint32_t id = 0; // what parameter events use
		std::string name;
		double minimum = 0.0;
		double maximum = 1.0;
		double defaultValue = 0.0;
		std::string unit;                 // e.g. "Hz", "dB", "s"; empty for plain numbers
		bool stepped = false;             // whole numbers only
		std::vector<std::string> options; // the names of a stepped parameter's values, from the minimum on
	};

	struct BuiltinInfo
	{
		std::string id; // what createBuiltin takes, e.g. "sine"
		std::string name;
		std::string description;
		bool instrument = false; // plays notes; has no audio input
		std::vector<ParameterInfo> parameters;
	};

	// Every built-in processor, sorted by id.
	const std::vector<BuiltinInfo>& builtins();

	// A new instance of a built-in processor with its parameters at their defaults. Throws std::invalid_argument for
	// an unknown id.
	std::shared_ptr<graph::Processor> createBuiltin(const std::string& id);

	// Whether id is a built-in instrument.
	bool isInstrument(const std::string& id);

	// Registers processor.list, which returns builtins().
	void registerCommands(Engine& engine);

	// Splits a block at its events: calls handle(event) at each event's offset and render(begin, end) for the frames
	// in between, so parameter changes and notes take effect at their exact sample.
	template <typename Handle, typename Render>
	void splitAtEvents(const graph::ProcessContext& context, Handle&& handle, Render&& render)
	{
		uint32_t position = 0;
		for (const graph::ProcessEvent& event : context.eventsIn)
		{
			const uint32_t offset = std::min(event.offset, context.frames);
			if (offset > position)
			{
				render(position, offset);
				position = offset;
			}
			handle(event);
		}
		if (position < context.frames)
			render(position, context.frames);
	}
}
