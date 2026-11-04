// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/metronome/metronomecommands.h"

#include "engine/document/projectmodel.h"
#include "engine/metronome/metronome.h"
#include "engine/metronome/metronomesettings.h"

#include <algorithm>
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

		// What a change refers to must be there: output busses, an instrument channel and readable sample files.
		// References the change leaves as they were may be gone, removed since; they play nothing.
		void check(const Project& project, const MetronomeSettings& settings, const MetronomeSettings& before)
		{
			for (const ObjectId id : settings.outputs)
			{
				if (std::find(before.outputs.begin(), before.outputs.end(), id) != before.outputs.end())
					continue;
				bool found = false;
				for (const Bus& bus : project.busses)
					found = found || (bus.id == id && bus.direction == BusDirection::Output);
				require(found, "no output bus " + std::to_string(id));
			}
			if (settings.destination != 0 && settings.destination != before.destination)
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
				{{"whilePlaying", {{"type", "boolean"}}}, {"whileRecording", {{"type", "boolean"}}},
					{"audio", {{"type", "object"}}}, {"midi", {{"type", "object"}}}, {"countIn", {{"type", "object"}}},
					{"patterns", {{"type", "array"}}}}},
			{"required", {"whilePlaying", "whileRecording", "audio", "midi", "countIn", "patterns"}}};

		engine.registerCommand(
			{"metronome.get", 1, "The project's metronome settings and click patterns (see metronome.set).",
				Json{{"type", "object"}, {"additionalProperties", false}}, settingsResult, true},
			[&model](CommandContext&) { return toJson(model.project().metronome); });
		model.addEdit(engine,
			{"metronome.set", 1,
				"Changes the given metronome settings and keeps the rest. The metronome switch, transport.set "
				"metronome, turns the click on; whilePlaying, whileRecording: when it sounds then. audio: the audio "
				"click; enabled, "
				"outputs (the output busses it plays through; empty: the main mix), level (dB, -144 to 6, on "
				"top of each accent's), sounds (beeps, or samples: sample files where an accent has one) and per "
				"accent "
				"1 to 4 by index, beeps [{frequency (Hz, 20-20000), level}] and samples [{path (an audio file, up to "
				"10 s are played, or null), level}]. midi: the MIDI click; enabled, destination (an instrument "
				"channel, or null), channel (1-16) and per accent notes [{note (0-127), velocity (0-1)}]. countIn: "
				"enabled (transport.record from stop counts in first, whether the click is on or not), bars (1-16) "
				"and signature: start (the signature and tempo where recording starts), project (the bars before "
				"where playback starts, as the project has them) or custom (customSignature {numerator, "
				"denominator} at the tempo where recording starts). patterns replaces every click pattern (see "
				"metronome.setPattern). Accent 1 is the sound of a "
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
				"with accent 2. The patterns apply wherever a signature holds, during playback and count-ins. "
				"Signatures with a numerator up to 64 have patterns; longer bars click on every beat.",
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
				setPattern(project.metronome, signature, parameters.value("accents", Json::array()));
				return changed(project, context);
			});
	}
}
