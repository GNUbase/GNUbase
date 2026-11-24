// SPDX-License-Identifier: GPL-3.0-or-later
// The meters' commands (#111, docs/features/metering.md): meter.get reads the channels' level meters, the main
// mix's master meter and the loudness meter, which the audio thread publishes; meter.reset starts their maxima and
// the loudness measurement over, or one of them. readMeters reads them all once a frame for the UI
// (Engine::readMeters).
#include "engine/analysis/analysis.h"
#include "engine/audio/audioengine.h"
#include "engine/engine.h"

#include <algorithm>
#include <mutex>
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
					"truePeakMax", "truePeakHold"]},
				"loudness": {"type": "object", "required": ["momentary", "shortTerm", "momentaryMax",
					"shortTermMax", "integrated", "range", "seconds"]}}})");
		}
	}

	void AudioEngine::readMeters(MeterFrame& frame)
	{
		frame.levels.resize(frame.channels.size());
		{
			const std::scoped_lock lock(m_channelStatesMutex);
			for (size_t i = 0; i < frame.channels.size(); ++i)
			{
				// A channel listed twice reads once: reading takes its held peak.
				const auto first = std::find(frame.channels.begin(), frame.channels.end(), frame.channels[i]);
				if (const auto index = static_cast<size_t>(first - frame.channels.begin()); index < i)
				{
					frame.levels[i] = frame.levels[index];
					continue;
				}
				const auto state = m_channelStates.find(frame.channels[i]);
				frame.levels[i] = state != m_channelStates.end() ? state->second->meter.readFrame() : LevelReading{};
			}
		}
		frame.main = m_mainMeter.readFrame();
		frame.loudness = m_loudness.readFrame();
	}

	void AudioEngine::registerMeterCommands(Engine& engine)
	{
		engine.registerCommand(
			{"meter.get", 1,
				"The meters, as the audio thread measured them last while the device ran. channels: each channel's "
				"level where the user setting meter.position says (all channels, or those given): by default its "
				"output after its fader, panner and muting, per audio channel of the output; postFader after the fader "
				"and muting without the panner, and input after the pre section, its input gain and low and high cut, "
				"per audio channel of the channel; a MIDI channel's one value is the velocity of the loudest note that "
				"sounds, 127 as 0 dBFS. A VCA channel's level is the sum of the outputs of the channels it controls, "
				"after their faders, panners and muting whatever meter.position says, in the main mix's format. Each "
				"has peak (the largest sample of the last 100 ms) and rms (averaged over the last 300 ms) in dBFS, and "
				"their maxima peakMax and rmsMax since meter.reset, also while the transport stands; null for silence, "
				"empty lists for VCA channels without audio channels and channels that haven't played yet. main: the "
				"same for the main mix's first 8 device outputs, with rms averaged over the user setting "
				"meter.rmsResolution and 3 dB higher with meter.rmsAes17, plus the master meter's truePeak, "
				"truePeakMax and truePeakHold (4x oversampled, dBTP): truePeakHold is the largest true peak since "
				"meter.reset with \"main\" or without meters, also while the transport stands, as peakMax; truePeakMax "
				"the loudness meter's, which meter.reset with \"loudness\" or without meters clears. loudness: the "
				"main mix's loudness per EBU R 128 in LUFS: momentary (400 ms), shortTerm (3 s), their maxima, "
				"integrated (gated) and range (LU), and seconds measured; they change every 100 ms. truePeakMax, the "
				"loudness maxima, integrated, range and seconds only count while the transport "
				"plays, with blocks from where playback started, and go on across audio.reset at the same sample "
				"rate; null until there is a value. The user setting meter.resetOnStart starts the loudness meter "
				"and the main mix's maxima over when playback starts.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "properties": {
					"channels": {"type": "array", "items": {"type": "integer"}}}})"),
				meterSchema(), true},
			[this](CommandContext& context)
			{
				// The channels asked for, in that order, or every channel of the project.
				std::vector<ObjectId> wanted;
				if (const auto given = context.parameters().find("channels"); given != context.parameters().end())
				{
					wanted = given->get<std::vector<ObjectId>>();
					// The project's channels, not the meters: only a graph build drops a deleted channel's meter,
					// and while no device runs there is none.
					const std::vector<RenderChannel>& known = m_snapshots.latest().channels;
					const std::vector<RenderVca>& vcas = m_snapshots.latest().vcas;
					for (const ObjectId id : wanted)
						if (std::none_of(known.begin(), known.end(),
								[id](const RenderChannel& channel) { return channel.id == id; }) &&
							std::none_of(vcas.begin(), vcas.end(), [id](const RenderVca& vca) { return vca.id == id; }))
							throw CommandError("no channel " + std::to_string(id));
				}
				else
				{
					for (const RenderChannel& channel : m_snapshots.latest().channels)
						wanted.push_back(channel.id);
					for (const RenderVca& vca : m_snapshots.latest().vcas)
						wanted.push_back(vca.id);
				}
				Json channels = Json::array();
				for (const ObjectId id : wanted)
				{
					const auto state = m_channelStates.find(id);
					Json reading =
						levels(state != m_channelStates.end() ? state->second->meter.read() : LevelReading{});
					reading["channel"] = id;
					channels.push_back(std::move(reading));
				}
				const LoudnessReading measured = m_loudness.read();
				Json main = levels(m_mainMeter.read());
				main["truePeak"] = decibels(measured.truePeak, measured.channels);
				main["truePeakMax"] = decibels(measured.truePeakMax, measured.channels);
				main["truePeakHold"] = decibels(measured.truePeakHold, measured.channels);
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
				"loudness measurement over (integrated, range, seconds); see meter.get. meters chooses what: \"main\" "
				"only the main mix's peakMax, rmsMax and truePeakHold (the master meter's reset), \"loudness\" only "
				"the loudness meter's values, truePeakMax included (its Reset Loudness), \"mainRmsMax\" only the main "
				"mix's rmsMax (a Ctrl-click on RMS Max.), \"mainTruePeakHold\" only its truePeakHold (on Peak Max.), "
				"\"channels\" only every channel's peakMax and rmsMax (a click on a channel's peak level); left out, "
				"everything, every channel's maxima included. Announces meter.reset with the meters given.",
				Json::parse(R"({"type": "object", "additionalProperties": false,
					"properties": {"meters": {"type": "string",
						"enum": ["main", "loudness", "mainRmsMax", "mainTruePeakHold", "channels"]}}})"),
				Json::parse(R"({"type": "object"})"), false},
			[this](CommandContext& context)
			{
				const std::string meters = context.parameters().value("meters", std::string());
				const bool all = meters.empty();
				if (all || meters == "channels")
					for (const auto& [id, state] : m_channelStates)
						state->meter.reset();
				if (all || meters == "main")
					m_mainMeter.reset();
				if (meters == "mainRmsMax")
					m_mainMeter.resetRms();
				if (all || meters == "main" || meters == "mainTruePeakHold")
					m_loudness.resetTruePeakHold();
				if (all || meters == "loudness")
					m_loudness.reset();
				context.emit("meter.reset", meters.empty() ? Json::object() : Json{{"meters", meters}});
				return Json::object();
			});
	}
}
