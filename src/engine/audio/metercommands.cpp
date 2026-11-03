// SPDX-License-Identifier: GPL-3.0-or-later
// The meters' commands (#111, docs/features/metering.md): meter.get reads the channels' level meters, the main
// mix's master meter and the loudness meter, which the audio thread publishes; meter.reset starts their maxima and
// the loudness measurement over.
#include "engine/analysis/analysis.h"
#include "engine/audio/audioengine.h"
#include "engine/engine.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		Json decibels(double level)
		{
			const std::optional<double> db = analysis::decibels(level);
			return db ? Json(*db) : Json(nullptr);
		}

		// The values of a meter's channels in dBFS, null for silence.
		Json decibels(const std::array<float, g_maxMeterChannels>& levels, int channels)
		{
			Json result = Json::array();
			for (size_t c = 0; c < static_cast<size_t>(channels); ++c)
				result.push_back(decibels(levels[c]));
			return result;
		}

		Json loudness(const std::optional<double>& value)
		{
			return value ? Json(*value) : Json(nullptr);
		}

		Json levels(const LevelReading& reading)
		{
			return {{"peak", decibels(reading.peak, reading.channels)},
				{"rms", decibels(reading.rms, reading.channels)},
				{"peakMax", decibels(reading.peakMax, reading.channels)},
				{"rmsMax", decibels(reading.rmsMax, reading.channels)}};
		}

		Json meterSchema()
		{
			return Json::parse(R"({"type": "object", "required": ["channels", "main", "loudness"], "properties": {
				"channels": {"type": "array", "items": {"type": "object",
					"required": ["channel", "peak", "rms", "peakMax", "rmsMax"]}},
				"main": {"type": "object", "required": ["peak", "rms", "peakMax", "rmsMax", "truePeak",
					"truePeakMax"]},
				"loudness": {"type": "object", "required": ["momentary", "shortTerm", "momentaryMax",
					"shortTermMax", "integrated", "range", "seconds"]}}})");
		}
	}

	void AudioEngine::registerMeterCommands(Engine& engine)
	{
		engine.registerCommand(
			{"meter.get", 1,
				"The meters, as the audio thread measured them last; it publishes every 100 ms while the device "
				"runs. channels: each channel's output after its fader, panner and muting (all channels, or those "
				"given), per audio channel: peak (the largest sample of the last 100 ms) and rms (averaged over 300 "
				"ms) in dBFS, and their maxima peakMax and rmsMax since meter.reset; null for silence, empty lists "
				"for MIDI channels and channels that haven't played yet. main: the same for the main mix's device "
				"outputs, plus the master meter's truePeak and truePeakMax (4x oversampled, dBTP). loudness: the "
				"main mix's loudness per EBU R 128 in LUFS: momentary (400 ms), shortTerm (3 s), their maxima, "
				"integrated (gated) and range (LU), and seconds measured. The maxima, integrated, range and "
				"seconds only count while the transport plays; null until there is a value. The user setting "
				"meter.resetOnStart starts the loudness meter and the main mix's maxima over when playback starts.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
					"channels": {"type": "array", "items": {"type": "integer"}}}})"),
				meterSchema(), true},
			[this](CommandContext& context)
			{
				std::vector<ObjectId> wanted;
				if (const auto channels = context.parameters().find("channels"); channels != context.parameters().end())
					wanted = channels->get<std::vector<ObjectId>>();
				Json channels = Json::array();
				for (const auto& [id, state] : m_channelStates)
				{
					if (!wanted.empty() && std::find(wanted.begin(), wanted.end(), id) == wanted.end())
						continue;
					Json reading = levels(state->meter.read());
					reading["channel"] = id;
					channels.push_back(std::move(reading));
				}
				for (const ObjectId id : wanted)
					if (m_channelStates.count(id) == 0)
						throw CommandError("no channel " + std::to_string(id) + " plays");
				const LoudnessReading measured = m_loudness.read();
				Json main = levels(m_mainMeter.read());
				main["truePeak"] = decibels(measured.truePeak, measured.channels);
				main["truePeakMax"] = decibels(measured.truePeakMax, measured.channels);
				return Json{{"channels", std::move(channels)}, {"main", std::move(main)},
					{"loudness",
						{{"momentary", loudness(measured.momentary)}, {"shortTerm", loudness(measured.shortTerm)},
							{"momentaryMax", loudness(measured.momentaryMax)},
							{"shortTermMax", loudness(measured.shortTermMax)},
							{"integrated", loudness(measured.integrated)}, {"range", loudness(measured.range)},
							{"seconds", measured.seconds}}}};
			});
		engine.registerCommand(
			{"meter.reset", 1,
				"Resets the meters' maxima (peakMax, rmsMax, truePeakMax, momentaryMax, shortTermMax) and starts the "
				"loudness measurement over (integrated, range, seconds); see meter.get. Announces meter.reset.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {}})"),
				Json::parse(R"({"type": "object"})"), false},
			[this](CommandContext& context)
			{
				for (const auto& [id, state] : m_channelStates)
					state->meter.reset();
				m_mainMeter.reset();
				m_loudness.reset();
				context.emit("meter.reset", Json::object());
				return Json::object();
			});
	}
}
