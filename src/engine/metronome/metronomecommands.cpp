// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/metronome/metronomecommands.h"

#include "engine/document/projectmodel.h"
#include "engine/metronome/metronome.h"
#include "engine/metronome/metronomesettings.h"

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace gnubase::engine::metronome
{
	namespace
	{
		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		// What the settings refer to must be there: output busses, an instrument channel and readable sample files.
		void check(const Project& project, const MetronomeSettings& settings, const MetronomeSettings& before)
		{
			for (const ObjectId id : settings.outputs)
			{
				bool found = false;
				for (const Bus& bus : project.busses)
					found = found || (bus.id == id && bus.direction == BusDirection::Output);
				require(found, "no output bus " + std::to_string(id));
			}
			if (settings.destination != 0)
			{
				const auto channel = project.channels.find(settings.destination);
				require(channel != project.channels.end() &&
						project.trackById.at(channel->second.track).type == TrackType::Instrument,
					"the MIDI click's destination " + std::to_string(settings.destination) +
						" is not an instrument channel");
			}
			for (size_t accent = 0; accent < settings.samples.size(); ++accent)
			{
				const std::string& path = settings.samples[accent].path;
				if (path.empty() || path == before.samples[accent].path)
					continue;
				try
				{
					SoundLibrary::check(path);
				}
				catch (const std::exception& error)
				{
					throw CommandError(
						"can't read the sample of accent " + std::to_string(accent + 1) + ": " + error.what());
				}
			}
		}

		Json changed(const Project& project, CommandContext& context)
		{
			Json result = toJson(project.metronome);
			context.emit("metronome.changed", result);
			return result;
		}
	}

	void registerMetronomeCommands(ProjectModel& model, Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json settingsResult = {{"type", "object"},
			{"properties",
				{{"click", {{"type", "boolean"}}}, {"whilePlaying", {{"type", "boolean"}}},
					{"whileRecording", {{"type", "boolean"}}}, {"audio", {{"type", "object"}}},
					{"midi", {{"type", "object"}}}, {"countIn", {{"type", "object"}}},
					{"patterns", {{"type", "array"}}}}},
			{"required", {"click", "whilePlaying", "whileRecording", "audio", "midi", "countIn", "patterns"}}};

		engine.registerCommand(
			{"metronome.get", 1, "The project's metronome settings and click patterns (see metronome.set).",
				Json{{"type", "object"}, {"additionalProperties", false}}, settingsResult, true},
			[&model](CommandContext&) { return toJson(model.project().metronome); });
		model.addEdit(engine,
			{"metronome.set", 1,
				"Changes the given metronome settings and keeps the rest. click: the metronome clicks; whilePlaying, "
				"whileRecording: when it does (recording comes with #96 and #97). audio: the audio click; enabled, "
				"outputs (the output busses it plays through; empty: the first output bus), level (dB, -144 to 6, on "
				"top of each accent's), sounds (beeps, or samples: sample files where an accent has one) and per "
				"accent "
				"1 to 4 by index, beeps [{frequency (Hz, 20-20000), level}] and samples [{path (an audio file, up to "
				"10 s are played, or null), level}]. midi: the MIDI click; enabled, destination (an instrument "
				"channel, or null), channel (1-16) and per accent notes [{note (0-127), velocity (0-1)}]. countIn: "
				"enabled (starts that count in do: transport.play with countIn, and recording), bars (1-16) and "
				"signature: start (the signature and tempo at the start position), project (the bars before the start "
				"as the project has them) or custom (customSignature {numerator, denominator} at the tempo at the "
				"start). patterns replaces every click pattern (see metronome.setPattern). Accent 1 is the sound of a "
				"bar's first beat. Settings are not undo steps; they are saved with the project.",
				settingsSchema(), settingsResult, false},
			"Change metronome settings",
			[](Project& project, CommandContext& context)
			{
				MetronomeSettings settings = project.metronome;
				apply(settings, context.parameters(), true);
				check(project, settings, project.metronome);
				project.metronome = settings;
				return changed(project, context);
			});
		model.addEdit(engine,
			{"metronome.setPattern", 1,
				"Sets the click pattern of a time signature: accents, the bar's clicks, evenly spread over it, each "
				"with an accent from 1 (the first beat's sound) to 4, or 0 for a click that stays silent; up to 64 "
				"clicks, e.g. [1, 3, 3, 2, 3, 3] for triplet eighths in 2/4. Without accents, or with an empty array, "
				"the signature goes back to its default pattern: a click per beat, the first with accent 1, the others "
				"with accent 2. The patterns apply wherever a signature holds, during playback and count-ins.",
				Json{{"type", "object"}, {"additionalProperties", false},
					{"properties",
						{{"numerator", integer}, {"denominator", integer},
							{"accents", {{"type", "array"}, {"items", integer}}}}},
					{"required", {"numerator", "denominator"}}},
				settingsResult, false},
			"Change metronome settings",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto numerator = parameters.at("numerator").get<int64_t>();
				const auto denominator = parameters.at("denominator").get<int64_t>();
				require(numerator >= 1 && numerator <= g_maxClicks && denominator >= 1 && denominator <= 16 &&
						(denominator & (denominator - 1)) == 0,
					"a time signature has a numerator from 1 to " + std::to_string(g_maxClicks) +
						" and a denominator of 1, 2, 4, 8 or 16");
				const time::TimeSignature signature{static_cast<int>(numerator), static_cast<int>(denominator)};
				setPattern(project.metronome, signature, parameters.value("accents", std::vector<int>()));
				return changed(project, context);
			});
	}
}
