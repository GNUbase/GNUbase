// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/serialize.h"

#include "engine/processors/builtin.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr struct
		{
			TrackType type;
			const char* name;
		} g_trackTypeNames[] = {
			{TrackType::Audio, "audio"},
			{TrackType::Instrument, "instrument"},
			{TrackType::Midi, "midi"},
			{TrackType::Folder, "folder"},
			{TrackType::Group, "group"},
			{TrackType::Effect, "effect"},
			{TrackType::Marker, "marker"},
			{TrackType::Ruler, "ruler"},
			{TrackType::Tempo, "tempo"},
			{TrackType::Signature, "signature"},
		};

		constexpr struct
		{
			ControllerType type;
			const char* name;
		} g_controllerTypeNames[] = {
			{ControllerType::Control, "controller"},
			{ControllerType::PitchBend, "pitchBend"},
			{ControllerType::ChannelPressure, "channelPressure"},
			{ControllerType::PolyPressure, "polyPressure"},
			{ControllerType::Program, "program"},
			{ControllerType::SysEx, "sysEx"},
		};

		constexpr struct
		{
			FadeShape shape;
			const char* name;
		} g_fadeShapeNames[] = {
			{FadeShape::Linear, "linear"},
			{FadeShape::Exponential, "exponential"},
			{FadeShape::Logarithmic, "logarithmic"},
			{FadeShape::SCurve, "sCurve"},
			{FadeShape::EqualPower, "equalPower"},
		};

		constexpr struct
		{
			AutomationTargetType type;
			const char* name;
		} g_automationTargetNames[] = {
			{AutomationTargetType::Volume, "volume"},
			{AutomationTargetType::Pan, "pan"},
			{AutomationTargetType::PanLeft, "panLeft"},
			{AutomationTargetType::PanRight, "panRight"},
			{AutomationTargetType::Instrument, "instrument"},
			{AutomationTargetType::Plugin, "plugin"},
			{AutomationTargetType::Mute, "mute"},
			{AutomationTargetType::Controller, "controller"},
			{AutomationTargetType::PitchBend, "pitchBend"},
			{AutomationTargetType::ChannelPressure, "channelPressure"},
		};

		constexpr struct
		{
			AutomationShape shape;
			const char* name;
		} g_automationShapeNames[] = {
			{AutomationShape::Step, "step"},
			{AutomationShape::Linear, "linear"},
			{AutomationShape::Curve, "curve"},
		};

		constexpr struct
		{
			CrossfadeMode mode;
			const char* name;
		} g_crossfadeModeNames[] = {
			{CrossfadeMode::EqualGain, "equalGain"},
			{CrossfadeMode::EqualPower, "equalPower"},
			{CrossfadeMode::Custom, "custom"},
		};

		FadeShape shapeFrom(const Json& json)
		{
			const auto shape = fadeShapeFromName(json.get<std::string>());
			if (!shape)
				throw std::invalid_argument("unknown fade shape " + json.dump());
			return *shape;
		}

		CrossfadeMode modeFrom(const Json& json)
		{
			const auto mode = crossfadeModeFromName(json.get<std::string>());
			if (!mode)
				throw std::invalid_argument("unknown crossfade mode " + json.dump());
			return *mode;
		}

		Crossfade crossfadeFrom(const Json& json)
		{
			Crossfade crossfade;
			if (json.is_null())
				return crossfade;
			crossfade.from = json.at("from").get<ObjectId>();
			crossfade.mode = modeFrom(json.at("mode"));
			crossfade.shape = shapeFrom(json.at("shape"));
			crossfade.outShape = shapeFrom(json.at("outShape"));
			crossfade.length = json.at("length").get<time::Samples>();
			crossfade.outLength = json.at("outLength").get<time::Samples>();
			return crossfade;
		}

		Json colorJson(const std::string& color)
		{
			return color.empty() ? Json(nullptr) : Json(color);
		}

		std::string colorFrom(const Json& json)
		{
			return json.is_null() ? std::string() : json.get<std::string>();
		}

		// An event's or part's color: files from before #94 have none, and one that isn't #rrggbb follows the track.
		std::string eventColorFrom(const Json& item)
		{
			const Json color = item.value("color", Json());
			return color.is_string() && isColor(color.get<std::string>()) ? color.get<std::string>() : std::string();
		}

		Json timingJson(const NoteTiming& timing)
		{
			return {{"position", timing.position}, {"length", timing.length},
				{"velocity", timing.velocity / g_maxVelocity}};
		}

		uint16_t velocityFrom(const Json& json)
		{
			return static_cast<uint16_t>(std::lround(json.get<double>() * g_maxVelocity));
		}

		NoteTiming timingFrom(const Json& json)
		{
			return {json.at("position").get<time::Ticks>(), json.at("length").get<time::Ticks>(),
				velocityFrom(json.at("velocity"))};
		}

		constexpr const char* g_base64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

		// Standard base64 with padding, as a slot's state is stored in the project.
		std::string toBase64(const std::vector<uint8_t>& bytes)
		{
			std::string text;
			text.reserve((bytes.size() + 2) / 3 * 4);
			for (size_t i = 0; i < bytes.size(); i += 3)
			{
				const size_t left = bytes.size() - i;
				const uint32_t triple = (uint32_t{bytes[i]} << 16) | (left > 1 ? uint32_t{bytes[i + 1]} << 8 : 0) |
					(left > 2 ? uint32_t{bytes[i + 2]} : 0);
				text += g_base64[(triple >> 18) & 63];
				text += g_base64[(triple >> 12) & 63];
				text += left > 1 ? g_base64[(triple >> 6) & 63] : '=';
				text += left > 2 ? g_base64[triple & 63] : '=';
			}
			return text;
		}

		// The inverse of toBase64; nothing for text that isn't base64 with padding.
		std::optional<std::vector<uint8_t>> fromBase64(const std::string& text)
		{
			if (text.size() % 4 != 0)
				return std::nullopt;
			const auto value = [](char c) -> int
			{
				const char* found = c == '\0' ? nullptr : std::strchr(g_base64, c);
				return found == nullptr ? -1 : static_cast<int>(found - g_base64);
			};
			std::vector<uint8_t> bytes;
			bytes.reserve(text.size() / 4 * 3);
			for (size_t i = 0; i < text.size(); i += 4)
			{
				const bool last = i + 4 == text.size();
				const int padding = last ? (text[i + 3] == '=' ? 1 : 0) + (text[i + 2] == '=' ? 1 : 0) : 0;
				if (padding == 1 && text[i + 3] != '=')
					return std::nullopt;
				uint32_t triple = 0;
				for (size_t j = 0; j < 4; ++j)
				{
					const int digit = j >= 4 - static_cast<size_t>(padding) ? 0 : value(text[i + j]);
					if (digit < 0)
						return std::nullopt;
					triple = (triple << 6) | static_cast<uint32_t>(digit);
				}
				// The bits after the last byte are zero, so that a state has one text.
				if ((padding == 2 && (triple & 0xFFFFU) != 0) || (padding == 1 && (triple & 0xFFU) != 0))
					return std::nullopt;
				bytes.push_back(static_cast<uint8_t>(triple >> 16));
				if (padding < 2)
					bytes.push_back(static_cast<uint8_t>(triple >> 8));
				if (padding < 1)
					bytes.push_back(static_cast<uint8_t>(triple));
			}
			return bytes;
		}

		template <typename Map> Json toJsonArray(const Map& objects)
		{
			Json array = Json::array();
			for (const auto& entry : objects)
				array.push_back(toJson(entry.second));
			return array;
		}
	}

	const char* trackTypeName(TrackType type)
	{
		for (const auto& entry : g_trackTypeNames)
			if (entry.type == type)
				return entry.name;
		return "audio"; // not reached: every type is in the table
	}

	std::optional<TrackType> trackTypeFromName(const std::string& name)
	{
		for (const auto& entry : g_trackTypeNames)
			if (name == entry.name)
				return entry.type;
		return std::nullopt;
	}

	const char* controllerTypeName(ControllerType type)
	{
		for (const auto& entry : g_controllerTypeNames)
			if (entry.type == type)
				return entry.name;
		return "controller"; // not reached: every type is in the table
	}

	std::optional<ControllerType> controllerTypeFromName(const std::string& name)
	{
		for (const auto& entry : g_controllerTypeNames)
			if (name == entry.name)
				return entry.type;
		return std::nullopt;
	}

	const char* automationTargetTypeName(AutomationTargetType type)
	{
		for (const auto& entry : g_automationTargetNames)
			if (entry.type == type)
				return entry.name;
		return "volume"; // not reached: every type is in the table
	}

	std::optional<AutomationTargetType> automationTargetTypeFromName(const std::string& name)
	{
		for (const auto& entry : g_automationTargetNames)
			if (name == entry.name)
				return entry.type;
		return std::nullopt;
	}

	bool isParameterId(const Json& json)
	{
		if (json.is_number_unsigned())
			return json.get<uint64_t>() <= UINT32_MAX;
		return json.is_number_integer() && json.get<int64_t>() >= 0 && json.get<int64_t>() <= UINT32_MAX;
	}

	const char* automationShapeName(AutomationShape shape)
	{
		for (const auto& entry : g_automationShapeNames)
			if (entry.shape == shape)
				return entry.name;
		return "linear"; // not reached: every shape is in the table
	}

	std::optional<AutomationShape> automationShapeFromName(const std::string& name)
	{
		for (const auto& entry : g_automationShapeNames)
			if (name == entry.name)
				return entry.shape;
		return std::nullopt;
	}

	const char* timeBaseName(time::TimeBase base)
	{
		return base == time::TimeBase::Musical ? "musical" : "linear";
	}

	time::TimeBase timeBaseFromName(const std::string& name)
	{
		return name == "linear" ? time::TimeBase::Linear : time::TimeBase::Musical;
	}

	const char* directionName(BusDirection direction)
	{
		return direction == BusDirection::Input ? "input" : "output";
	}

	std::vector<int> parentChannelsFromJson(const Json& json)
	{
		std::vector<int> channels;
		for (const int64_t channel : json.get<std::vector<int64_t>>())
			channels.push_back(
				channel >= 0 && channel <= std::numeric_limits<int>::max() ? static_cast<int>(channel) : -1);
		return channels;
	}

	const char* fadeShapeName(FadeShape shape)
	{
		for (const auto& entry : g_fadeShapeNames)
			if (entry.shape == shape)
				return entry.name;
		return "linear"; // not reached: every shape is in the table
	}

	std::optional<FadeShape> fadeShapeFromName(const std::string& name)
	{
		for (const auto& entry : g_fadeShapeNames)
			if (name == entry.name)
				return entry.shape;
		return std::nullopt;
	}

	const char* crossfadeModeName(CrossfadeMode mode)
	{
		for (const auto& entry : g_crossfadeModeNames)
			if (entry.mode == mode)
				return entry.name;
		return "equalGain"; // not reached: every mode is in the table
	}

	std::optional<CrossfadeMode> crossfadeModeFromName(const std::string& name)
	{
		for (const auto& entry : g_crossfadeModeNames)
			if (name == entry.name)
				return entry.mode;
		return std::nullopt;
	}

	std::vector<std::string> fadeShapeNames()
	{
		std::vector<std::string> names;
		for (const auto& entry : g_fadeShapeNames)
			names.emplace_back(entry.name);
		return names;
	}

	std::vector<std::string> crossfadeModeNames()
	{
		std::vector<std::string> names;
		for (const auto& entry : g_crossfadeModeNames)
			names.emplace_back(entry.name);
		return names;
	}

	Json toJson(const Fade& fade)
	{
		return {{"length", fade.length}, {"shape", fadeShapeName(fade.shape)}};
	}

	Json toJson(const Crossfade& crossfade)
	{
		if (crossfade.from == 0)
			return nullptr;
		return {{"from", crossfade.from}, {"mode", crossfadeModeName(crossfade.mode)},
			{"shape", fadeShapeName(crossfade.shape)}, {"outShape", fadeShapeName(crossfade.outShape)},
			{"length", crossfade.length}, {"outLength", crossfade.outLength}};
	}

	Json toJson(const AutoFades& settings)
	{
		return {{"fadeIn", settings.fadeIn}, {"fadeOut", settings.fadeOut}, {"crossfades", settings.crossfades},
			{"length", settings.length}, {"fadeInShape", fadeShapeName(settings.fadeInShape)},
			{"fadeOutShape", fadeShapeName(settings.fadeOutShape)},
			{"crossfadeMode", crossfadeModeName(settings.crossfadeMode)},
			{"crossfadeShape", fadeShapeName(settings.crossfadeShape)}};
	}

	Json toJson(const std::vector<VolumePoint>& curve)
	{
		Json json = Json::array();
		for (const VolumePoint& point : curve)
			json.push_back({{"position", point.position}, {"volume", point.volume}, {"curve", point.curve}});
		return json;
	}

	std::vector<VolumePoint> volumeCurveFromJson(const Json& json)
	{
		std::vector<VolumePoint> curve;
		for (const Json& point : json)
			curve.push_back(
				{point.at("position").get<time::Samples>(), point.value("volume", 0.0), point.value("curve", 0.0)});
		return curve;
	}

	Fade fadeFromJson(const Json& json, const Fade& fallback)
	{
		Fade fade = fallback;
		if (json.contains("length"))
			fade.length = json.at("length").get<time::Samples>();
		if (json.contains("shape"))
			fade.shape = shapeFrom(json.at("shape"));
		return fade;
	}

	Json toJson(const ProjectSetup& setup)
	{
		return {{"recordFileType", setup.recordFileType}, {"recordBitDepth", setup.recordBitDepth},
			{"frameRate", time::frameRateName(setup.frameRate)}, {"startTime", setup.startTime},
			{"length", setup.length}, {"barOffset", setup.barOffset}, {"timeFormat", setup.timeFormat}};
	}

	ProjectSetup projectSetupFromJson(const Json& json, const ProjectSetup& fallback)
	{
		ProjectSetup setup = fallback;
		const auto oneOf = [&json](const char* key, const std::string& current, const auto& names)
		{
			std::string name = json.value(key, current);
			if (std::find(names.begin(), names.end(), name) == names.end())
				throw std::invalid_argument(std::string("unknown ") + key + " " + name);
			return name;
		};
		setup.recordFileType = oneOf("recordFileType", setup.recordFileType, g_recordFileTypes);
		setup.recordBitDepth = oneOf("recordBitDepth", setup.recordBitDepth, g_recordBitDepths);
		setup.timeFormat = oneOf("timeFormat", setup.timeFormat, g_rulerTimeFormats);
		if (json.contains("frameRate"))
		{
			const std::string name = json.at("frameRate").get<std::string>();
			const std::optional<time::FrameRate> rate = time::frameRateFromName(name);
			if (!rate)
				throw std::invalid_argument("unknown frameRate " + name);
			setup.frameRate = *rate;
		}
		setup.startTime = json.value("startTime", setup.startTime);
		setup.length = json.value("length", setup.length);
		setup.barOffset = json.value("barOffset", setup.barOffset);

		// What recording can write (AudioFileWriter): AIFF in integers, FLAC in 16 and 24 bits, 64-bit float in WAV.
		const std::string& type = setup.recordFileType;
		const std::string& depth = setup.recordBitDepth;
		const bool wav = type == "wav" || type == "broadcastWav";
		if (type == "aiff" && depth.rfind("int", 0) != 0)
			throw std::invalid_argument("AIFF files record 16, 24 or 32 bit, not " + depth);
		if (type == "flac" && depth != "int16" && depth != "int24")
			throw std::invalid_argument("FLAC files record 16 or 24 bit, not " + depth);
		if (!wav && depth == "float64")
			throw std::invalid_argument("only Wave files record 64 bit float");
		if (!std::isfinite(setup.startTime) || setup.startTime < 0.0 || setup.startTime >= 86400.0)
			throw std::invalid_argument("startTime must be from 0 to less than 24 hours (86400 seconds)");
		if (!std::isfinite(setup.length) || setup.length <= 0.0 || setup.length > 86400.0)
			throw std::invalid_argument("length must be more than 0 and at most 24 hours (86400 seconds)");
		if (setup.barOffset < -9999 || setup.barOffset > 9999)
			throw std::invalid_argument("barOffset must be from -9999 to 9999");
		return setup;
	}

	AutoFades autoFadesFromJson(const Json& json, const AutoFades& fallback)
	{
		AutoFades settings = fallback;
		settings.fadeIn = json.value("fadeIn", settings.fadeIn);
		settings.fadeOut = json.value("fadeOut", settings.fadeOut);
		settings.crossfades = json.value("crossfades", settings.crossfades);
		settings.length = json.value("length", settings.length);
		if (json.contains("fadeInShape"))
			settings.fadeInShape = shapeFrom(json.at("fadeInShape"));
		if (json.contains("fadeOutShape"))
			settings.fadeOutShape = shapeFrom(json.at("fadeOutShape"));
		if (json.contains("crossfadeMode"))
			settings.crossfadeMode = modeFrom(json.at("crossfadeMode"));
		if (json.contains("crossfadeShape"))
			settings.crossfadeShape = shapeFrom(json.at("crossfadeShape"));
		return settings;
	}

	const char* markerTypeName(MarkerType type)
	{
		return type == MarkerType::Cycle ? "cycle" : "position";
	}

	std::optional<MarkerType> markerTypeFromName(const std::string& name)
	{
		if (name == "position")
			return MarkerType::Position;
		if (name == "cycle")
			return MarkerType::Cycle;
		return std::nullopt;
	}

	Json toJson(const Bus& bus)
	{
		return {{"id", bus.id}, {"name", bus.name}, {"direction", directionName(bus.direction)},
			{"format", formatName(bus.format)}, {"channels", channelCount(bus.format)}, {"ports", bus.ports},
			{"parent", bus.parent}, {"parentChannels", bus.parentChannels}, {"mainMix", bus.mainMix}};
	}

	Json toJson(const Track& track)
	{
		Json json = {{"id", track.id}, {"type", trackTypeName(track.type)}, {"name", track.name},
			{"color", colorJson(track.color)}, {"timeBase", timeBaseName(track.timeBase)}, {"parent", track.parent},
			{"channel", track.channel}, {"autoFades", track.autoFades ? toJson(*track.autoFades) : Json(nullptr)},
			{"enabled", track.enabled}, {"picture", track.picture}};
		if (track.type == TrackType::Ruler)
			json["timeFormat"] = track.timeFormat;
		return json;
	}

	Json toJson(const Channel& channel)
	{
		Json parameters = Json::array();
		for (const auto& [parameter, value] : channel.instrumentParameters)
			parameters.push_back({{"parameter", parameter}, {"value", value}});
		Json json = {{"id", channel.id}, {"track", channel.track}, {"format", formatName(channel.format)},
			{"channels", channelCount(channel.format)}, {"input", channel.input}, {"output", channel.output},
			{"instrument", channel.instrument}, {"instrumentParameters", std::move(parameters)},
			{"volume", channel.volume}, {"pan", channel.pan},
			{"panner", channel.panner == StereoPanner::Combined ? "combined" : "balance"}, {"panLeft", channel.panLeft},
			{"panRight", channel.panRight}, {"panBypass", channel.panBypass},
			{"midiVolume", channel.midiVolume ? Json(*channel.midiVolume) : Json(nullptr)},
			{"midiPan", channel.midiPan ? Json(*channel.midiPan) : Json(nullptr)}, {"monitor", channel.monitor},
			{"recordEnable", channel.recordEnable}, {"midiInput", channel.midiInput}, {"mute", channel.mute},
			{"solo", channel.solo}, {"soloDefeat", channel.soloDefeat}, {"listen", channel.listen},
			{"slots", channel.slots}, {"notes", channel.notes}};
		json.update(toJson(channel.sections));
		return json;
	}

	Json toJson(const Slot& slot)
	{
		Json parameters = Json::array();
		for (const auto& [parameter, value] : slot.parameters)
			parameters.push_back({{"parameter", parameter}, {"value", value}});
		Json json = {{"id", slot.id}, {"channel", slot.channel}, {"plugin", slot.plugin}, {"name", slot.name},
			{"bypass", slot.bypass}, {"active", slot.active}, {"parameters", std::move(parameters)}};
		if (slot.state)
			json["state"] = toBase64(*slot.state);
		return json;
	}

	Json toJson(const MediaFile& media)
	{
		return {{"id", media.id}, {"path", media.path}, {"sampleRate", media.sampleRate}, {"channels", media.channels},
			{"length", media.length}};
	}

	Json toJson(const Clip& clip)
	{
		return {{"id", clip.id}, {"name", clip.name}, {"media", clip.media}, {"fadeIn", toJson(clip.fadeIn)},
			{"fadeOut", toJson(clip.fadeOut)}, {"folder", clip.folder}};
	}

	Json toJson(const PoolFolder& folder)
	{
		return {{"id", folder.id}, {"name", folder.name}, {"parent", folder.parent}};
	}

	Json toJson(const AudioEvent& event)
	{
		return {{"id", event.id}, {"track", event.track}, {"clip", event.clip}, {"position", event.position.value},
			{"offset", event.offset}, {"length", event.length}, {"volume", event.volume}, {"mute", event.mute},
			{"lock", event.lock}, {"fadeIn", toJson(event.fadeIn)}, {"fadeOut", toJson(event.fadeOut)},
			{"crossfade", toJson(event.crossfade)}, {"snapPoint", event.snapPoint ? Json(*event.snapPoint) : Json()},
			{"color", colorJson(event.color)}, {"part", event.part}, {"volumeCurve", toJson(event.volumeCurve)}};
	}

	Json toJson(const AudioPart& part)
	{
		return {{"id", part.id}, {"track", part.track}, {"name", part.name}, {"position", part.position.value},
			{"length", part.length}, {"mute", part.mute}, {"lock", part.lock}, {"color", colorJson(part.color)}};
	}

	Json toJson(const Note& note)
	{
		Json json = {{"id", note.id}, {"position", note.position}, {"length", note.length}, {"pitch", note.pitch},
			{"velocity", note.velocity / g_maxVelocity}, {"releaseVelocity", note.releaseVelocity / g_maxVelocity},
			{"channel", note.channel}};
		if (note.origin)
			json["origin"] = {
				{"original", timingJson(note.origin->original)}, {"quantized", timingJson(note.origin->quantized)}};
		return json;
	}

	Json toJson(const Controller& controller)
	{
		Json json = {
			{"id", controller.id}, {"position", controller.position}, {"type", controllerTypeName(controller.type)}};
		switch (controller.type)
		{
		case ControllerType::Control:
		case ControllerType::PolyPressure:
			json["number"] = controller.number;
			json["value"] = controller.value / g_maxControllerValue;
			break;
		case ControllerType::PitchBend:
		case ControllerType::ChannelPressure:
			json["value"] = controller.value / g_maxControllerValue;
			break;
		case ControllerType::Program:
			json["number"] = controller.number;
			if (controller.value != g_noBank)
				json["bank"] = controller.value;
			break;
		case ControllerType::SysEx:
			json["data"] = controller.data;
			return json;
		}
		json["channel"] = controller.channel;
		return json;
	}

	Controller controllerFromJson(const Json& json)
	{
		Controller controller;
		controller.type =
			controllerTypeFromName(json.value("type", std::string("controller"))).value_or(ControllerType::Control);
		// Only what the type uses; the rest stays zero, as toJson leaves it out.
		const ControllerType type = controller.type;
		if (type == ControllerType::Control || type == ControllerType::PolyPressure || type == ControllerType::Program)
			controller.number = json.value("number", uint8_t{0});
		if (type != ControllerType::SysEx)
			controller.channel = json.value("channel", uint8_t{0});
		if (type == ControllerType::Program)
			controller.value = json.value("bank", g_noBank);
		else if (type != ControllerType::SysEx && json.contains("value"))
			controller.value =
				static_cast<uint32_t>(std::llround(json.at("value").get<double>() * g_maxControllerValue));
		if (type == ControllerType::SysEx)
			controller.data = json.at("data").get<std::vector<uint8_t>>();
		return controller;
	}

	Json toJson(const MidiPart& part)
	{
		return {{"id", part.id}, {"track", part.track}, {"name", part.name}, {"position", part.position.value},
			{"length", part.length}, {"mute", part.mute}, {"lock", part.lock}, {"shared", part.shared},
			{"color", colorJson(part.color)}, {"notes", toJsonArray(part.notes)},
			{"controllers", toJsonArray(part.controllers)}};
	}

	Json toJson(const Marker& marker)
	{
		return {{"id", marker.id}, {"track", marker.track}, {"type", markerTypeName(marker.type)},
			{"number", marker.number}, {"position", marker.position.value},
			{"end", marker.type == MarkerType::Cycle ? Json(marker.end.value) : Json(nullptr)}, {"name", marker.name},
			{"color", colorJson(marker.color)}};
	}

	Json toJson(const AutomationTarget& target)
	{
		Json json = {{"type", automationTargetTypeName(target.type)}};
		switch (target.type)
		{
		case AutomationTargetType::Volume:
		case AutomationTargetType::Pan:
		case AutomationTargetType::PanLeft:
		case AutomationTargetType::PanRight:
		case AutomationTargetType::Mute:
			break;
		case AutomationTargetType::Instrument:
			json["parameter"] = target.parameter;
			break;
		case AutomationTargetType::Plugin:
			json["slot"] = target.slot;
			json["parameter"] = target.parameter;
			break;
		case AutomationTargetType::Controller:
			json["number"] = target.number;
			json["channel"] = target.channel;
			break;
		case AutomationTargetType::PitchBend:
		case AutomationTargetType::ChannelPressure:
			json["channel"] = target.channel;
			break;
		}
		return json;
	}

	Json toJson(const AutomationPoint& point)
	{
		Json json = {{"id", point.id}, {"position", point.position.value}, {"value", point.value},
			{"shape", automationShapeName(point.shape)}};
		if (point.shape == AutomationShape::Curve)
			json["curve"] = point.curve;
		return json;
	}

	Json toJson(const AutomationLane& lane)
	{
		std::vector<const AutomationPoint*> points;
		points.reserve(lane.points.size());
		for (const auto& [id, point] : lane.points)
			points.push_back(&point);
		std::stable_sort(points.begin(), points.end(),
			[](const AutomationPoint* a, const AutomationPoint* b) { return a->position.value < b->position.value; });
		Json list = Json::array();
		for (const AutomationPoint* point : points)
			list.push_back(toJson(*point));
		return {{"id", lane.id}, {"track", lane.track}, {"target", toJson(lane.target)}, {"read", lane.read},
			{"points", std::move(list)}};
	}

	Json toJson(const std::vector<ProjectColor>& colors)
	{
		Json json = Json::array();
		for (const ProjectColor& color : colors)
			json.push_back({{"name", color.name}, {"color", color.color}});
		return json;
	}

	std::vector<ProjectColor> colorsFromJson(const Json& json)
	{
		std::vector<ProjectColor> colors;
		for (const Json& item : json)
		{
			ProjectColor color{item.at("name").get<std::string>(), item.at("color").get<std::string>()};
			if (!isColor(color.color))
				throw std::invalid_argument("color " + color.color + " isn't #rrggbb");
			colors.push_back(std::move(color));
		}
		if (colors.empty())
			throw std::invalid_argument("a color set has at least one color");
		return colors;
	}

	Json toJson(const MixerView& view)
	{
		return {{"hidden", view.hidden}, {"hiddenTypes", view.hiddenTypes}};
	}

	namespace
	{
		Json mixersJson(const std::map<std::string, MixerView>& mixers)
		{
			Json json = Json::object();
			for (const auto& [name, view] : mixers)
				json[name] = toJson(view);
			return json;
		}
	}

	Json toJson(const time::TempoMap& tempo)
	{
		Json points = Json::array();
		for (const time::TempoPoint& point : tempo.points())
			points.push_back({{"position", point.position}, {"bpm", point.bpm}, {"ramp", point.ramp}});
		return points;
	}

	time::TempoMap tempoFromJson(const Json& points)
	{
		std::vector<time::TempoPoint> tempo;
		for (const Json& point : points)
			tempo.push_back(
				{point.at("position").get<time::Ticks>(), point.at("bpm").get<double>(), point.at("ramp").get<bool>()});
		return time::TempoMap(tempo);
	}

	Json toJson(const Project& project)
	{
		Json busses = Json::array();
		for (const Bus& bus : project.busses)
			busses.push_back(toJson(bus));
		Json tracks = Json::array();
		for (const ObjectId id : project.trackOrder())
			tracks.push_back(toJson(project.trackById.at(id)));
		Json signatures = Json::array();
		for (const time::SignatureChange& change : project.signatures.changes())
			signatures.push_back({{"bar", change.bar}, {"numerator", change.signature.numerator},
				{"denominator", change.signature.denominator}});
		Json json = {{"name", project.name}, {"sampleRate", project.sampleRate}, {"tempo", toJson(project.tempo)},
			{"tempoTrackActive", project.tempoTrackActive}, {"tempoTrack", toJson(project.tempoTrack())},
			{"fixedTempo", project.fixedTempo()}, {"signatures", signatures}, {"autoFades", toJson(project.autoFades)},
			{"panLaw", panLawName(project.panLaw)}, {"maxVolume", project.maxVolume},
			{"colors", toJson(project.colors)}, {"busses", busses}, {"tracks", tracks},
			{"channels", toJsonArray(project.channels)}, {"slots", toJsonArray(project.slots)},
			{"media", toJsonArray(project.media)}, {"clips", toJsonArray(project.clips)},
			{"poolFolders", toJsonArray(project.poolFolders)}, {"audioEvents", toJsonArray(project.audioEvents)},
			{"audioParts", toJsonArray(project.audioParts)}, {"midiParts", toJsonArray(project.midiParts)},
			{"markers", toJsonArray(project.markers)}, {"automation", toJsonArray(project.automation)},
			{"quantize", midi::toJson(project.quantize)}, {"autoQuantize", project.autoQuantize},
			{"activeMarkerTrack", project.activeMarkerTrack}, {"snap", toJson(project.snap)},
			{"mixers", mixersJson(project.mixers)}};
		json.update(toJson(project.setup));
		return json;
	}

	namespace
	{
		bool numberTaken(const Project& project, const Marker& marker, int number)
		{
			return std::any_of(project.markers.begin(), project.markers.end(),
				[&marker, number](const auto& entry)
				{
					const Marker& other = entry.second;
					return other.id != marker.id && other.track == marker.track && other.type == marker.type &&
						other.number == number;
				});
		}

		// Markers without a track go onto a new marker track at the top, which becomes the active one if none is.
		// Markers without a number get the first free ones on their track, in timeline order. Cycle markers shorter
		// than a sample get longer.
		void placeOldMarkers(Project& project)
		{
			const bool trackless = std::any_of(project.markers.begin(), project.markers.end(),
				[](const auto& entry) { return entry.second.track == 0; });
			if (trackless)
			{
				Track track;
				track.id = project.newId();
				track.type = TrackType::Marker;
				track.name = "Markers";
				project.trackById[track.id] = track;
				project.tracks.insert(project.tracks.begin(), track.id);
				if (project.activeMarkerTrack == 0)
					project.activeMarkerTrack = track.id;
				for (auto& [id, marker] : project.markers)
					if (marker.track == 0)
						marker.track = track.id;
			}
			std::vector<Marker*> unnumbered;
			for (auto& [id, marker] : project.markers)
			{
				keepMinimumLength(project, marker);
				if (marker.number == 0)
					unnumbered.push_back(&marker);
			}
			std::stable_sort(unnumbered.begin(), unnumbered.end(),
				[](const Marker* a, const Marker* b) { return a->position.value < b->position.value; });
			for (Marker* marker : unnumbered)
			{
				int number = 1;
				while (numberTaken(project, *marker, number))
					++number;
				marker->number = number;
			}
		}

		std::string numberText(double value)
		{
			std::ostringstream text;
			text << value;
			return text.str();
		}

		// A number from a file: finite and from minimum to maximum.
		double checkedNumber(const Json& value, double minimum, double maximum, const std::string& name)
		{
			const auto number = value.get<double>();
			if (!std::isfinite(number) || number < minimum || number > maximum)
				throw std::invalid_argument(
					name + " must be a number from " + numberText(minimum) + " to " + numberText(maximum));
			return number;
		}

		// A bus's or channel's format; files from before formats have only a channel count.
		ChannelFormat formatFromJson(const Json& json)
		{
			if (json.contains("format"))
			{
				const std::string name = json.at("format").get<std::string>();
				if (const std::optional<ChannelFormat> format = formatFromName(name))
					return *format;
				throw std::invalid_argument("unknown format " + name);
			}
			const int channels = json.at("channels").get<int>();
			const ChannelFormat format = defaultFormat(channels);
			if (format == ChannelFormat::None && channels != 0)
				throw std::invalid_argument("no format has " + std::to_string(channels) + " channels");
			return format;
		}

		// The values of a channel's instrument parameters from a file. Like parameter.set, values beyond a parameter's
		// range stop at its ends and stepped ones round, since a newer build may have widened the range; values of
		// parameters the instrument doesn't have, e.g. from a newer build, are left out.
		std::map<uint32_t, double> instrumentParametersFromJson(
			const Json& json, const std::string& instrument, const std::string& name)
		{
			if (!json.is_array())
				throw std::invalid_argument(name + " instrumentParameters must be a list");
			std::map<uint32_t, double> values;
			const processors::BuiltinInfo* info = processors::findBuiltin(instrument);
			for (const Json& item : json)
			{
				if (!item.is_object() || !item.contains("parameter") || !isParameterId(item.at("parameter")) ||
					!item.contains("value") || !item.at("value").is_number())
					throw std::invalid_argument(name + " has an instrument parameter that isn't one: " + item.dump());
				const auto id = item.at("parameter").get<uint32_t>();
				const processors::ParameterInfo* parameter =
					info != nullptr ? processors::findParameter(*info, id) : nullptr;
				if (parameter == nullptr)
					continue;
				const std::string valueName = name + " parameter " + std::to_string(id);
				double value = item.at("value").get<double>();
				if (!std::isfinite(value))
					throw std::invalid_argument(valueName + " must be a finite number");
				value = std::clamp(value, parameter->minimum, parameter->maximum);
				if (parameter->stepped)
					value = std::round(value);
				if (!values.emplace(id, value).second)
					throw std::invalid_argument(valueName + " is there twice");
			}
			return values;
		}

		// The values of a slot's parameters from a file. A built-in plugin's are checked like instrument parameters;
		// the values of other plugins, which this build may not have, are kept as long as they are numbers.
		std::map<uint32_t, double> slotParametersFromJson(const Json& json, const Slot& slot, const std::string& name)
		{
			// A built-in plugin this build has: values as for an instrument. Others, e.g. one of a newer build, keep
			// theirs.
			if (const processors::BuiltinInfo* builtin = processors::findBuiltinPlugin(slot.plugin))
				return instrumentParametersFromJson(json, builtin->id, name);
			if (!json.is_array())
				throw std::invalid_argument(name + " parameters must be a list");
			std::map<uint32_t, double> values;
			for (const Json& item : json)
			{
				if (!item.is_object() || !item.contains("parameter") || !isParameterId(item.at("parameter")) ||
					!item.contains("value") || !item.at("value").is_number())
					throw std::invalid_argument(name + " has a parameter that isn't one: " + item.dump());
				const auto id = item.at("parameter").get<uint32_t>();
				const std::string valueName = name + " parameter " + std::to_string(id);
				const auto value = item.at("value").get<double>();
				if (!std::isfinite(value))
					throw std::invalid_argument(valueName + " must be a finite number");
				if (!values.emplace(id, value).second)
					throw std::invalid_argument(valueName + " is there twice");
			}
			return values;
		}

		Slot slotFromJson(const Json& item)
		{
			Slot slot;
			slot.id = item.at("id").get<ObjectId>();
			const std::string name = "slot " + std::to_string(slot.id);
			slot.channel = item.at("channel").get<ObjectId>();
			slot.plugin = item.at("plugin").get<std::string>();
			const size_t colon = slot.plugin.find(':');
			if (colon == std::string::npos || colon == 0 || colon + 1 == slot.plugin.size())
				throw std::invalid_argument(name + " has a plugin id that isn't \"<format>:<id>\": " + slot.plugin);
			slot.name = item.value("name", std::string());
			slot.bypass = item.value("bypass", false);
			slot.active = item.value("active", true);
			const processors::BuiltinInfo* builtin = processors::findBuiltinPlugin(slot.plugin);
			if (builtin != nullptr && builtin->instrument)
				throw std::invalid_argument(name + ": " + slot.plugin + " is an instrument, not an effect");
			slot.parameters = slotParametersFromJson(item.value("parameters", Json::array()), slot, name);
			if (item.contains("state"))
			{
				std::optional<std::vector<uint8_t>> state = fromBase64(item.at("state").get<std::string>());
				if (!state.has_value())
					throw std::invalid_argument(name + " state isn't base64");
				slot.state = std::make_shared<const std::vector<uint8_t>>(std::move(*state));
			}
			return slot;
		}

		// An optional whole number from a file, 0 if it is missing.
		int64_t checkedInteger(const Json& json, const char* key, int64_t maximum, const std::string& name)
		{
			const auto value = json.value(key, int64_t{0});
			if (value < 0 || value > maximum)
				throw std::invalid_argument(name + "." + key + " must be from 0 to " + std::to_string(maximum));
			return value;
		}

		// An automation lane from a file, checked as far as the automation commands check what they get without
		// the instrument (whose parameters may have changed): the track it fits, the target's numbers and its
		// points. NaN or a curve that overflows exp() would otherwise reach the audio.
		AutomationLane automationLaneFromJson(const Project& project, const Json& item)
		{
			AutomationLane lane;
			lane.id = item.at("id").get<ObjectId>();
			lane.track = item.at("track").get<ObjectId>();
			lane.read = item.at("read").get<bool>();
			const std::string name = "automation lane " + std::to_string(lane.id);
			const auto found = project.trackById.find(lane.track);
			if (found == project.trackById.end() || project.channels.count(found->second.channel) == 0)
				throw std::invalid_argument(name + " is on no track with a channel");
			const Track& track = found->second;

			const Json& target = item.at("target");
			const std::optional<AutomationTargetType> type =
				automationTargetTypeFromName(target.at("type").get<std::string>());
			if (!type.has_value())
				throw std::invalid_argument(name + " has an unknown target type " + target.at("type").dump());
			lane.target.type = *type;
			lane.target.parameter =
				static_cast<uint32_t>(checkedInteger(target, "parameter", UINT32_MAX, name + " target"));
			lane.target.number = static_cast<uint8_t>(checkedInteger(target, "number", 127, name + " target"));
			lane.target.channel = static_cast<uint8_t>(checkedInteger(target, "channel", 15, name + " target"));
			lane.target.slot = target.value("slot", ObjectId{0});
			const bool midi = track.type == TrackType::Midi || track.type == TrackType::Instrument;
			bool fits = midi;
			double minimum = 0.0;
			double maximum = 1.0;
			switch (lane.target.type)
			{
			case AutomationTargetType::Volume:
			case AutomationTargetType::Pan:
			case AutomationTargetType::PanLeft:
			case AutomationTargetType::PanRight:
				fits = project.channels.at(track.channel).format != ChannelFormat::None;
				minimum = lane.target.type == AutomationTargetType::Volume ? g_minChannelVolume : -1.0;
				maximum = lane.target.type == AutomationTargetType::Volume ? project.maxVolume : 1.0;
				break;
			case AutomationTargetType::Mute:
				fits = project.channels.at(track.channel).format != ChannelFormat::None;
				maximum = 1.0;
				break;
			case AutomationTargetType::Plugin:
			{
				// A slot of the track's channel; the plugin's range applies when it plays, like an instrument's.
				const auto slot = project.slots.find(lane.target.slot);
				fits = slot != project.slots.end() && slot->second.channel == track.channel;
				minimum = std::numeric_limits<double>::lowest();
				maximum = std::numeric_limits<double>::max();
				break;
			}
			case AutomationTargetType::Instrument:
				// The instrument's range applies when it plays; the instrument may have changed since.
				fits = track.type == TrackType::Instrument;
				minimum = std::numeric_limits<double>::lowest();
				maximum = std::numeric_limits<double>::max();
				break;
			case AutomationTargetType::Controller:
			case AutomationTargetType::PitchBend:
			case AutomationTargetType::ChannelPressure:
				break;
			}
			if (!fits)
				throw std::invalid_argument(name + " has a target that track " + std::to_string(track.id) + " lacks");

			for (const Json& p : item.at("points"))
			{
				AutomationPoint point;
				point.id = p.at("id").get<ObjectId>();
				const std::string pointName = name + " point " + std::to_string(point.id);
				point.position = {track.timeBase, p.at("position").get<int64_t>()};
				if (point.position.value < 0)
					throw std::invalid_argument(pointName + " has a negative position");
				point.value = checkedNumber(p.at("value"), minimum, maximum, pointName + " value");
				const std::optional<AutomationShape> shape = automationShapeFromName(p.at("shape").get<std::string>());
				if (!shape.has_value())
					throw std::invalid_argument(pointName + " has an unknown shape " + p.at("shape").dump());
				point.shape = *shape;
				if (point.shape == AutomationShape::Curve)
					point.curve = checkedNumber(p.value("curve", Json(0.0)), -1.0, 1.0, pointName + " curve");
				lane.points[point.id] = point;
			}
			return lane;
		}
	}

	namespace
	{
		const std::array<const char*, 5> g_eqBandTypes = {"bell", "lowShelf", "highShelf", "lowCut", "highCut"};

		Json toJson(const CutFilter& filter)
		{
			return {{"on", filter.on}, {"frequency", filter.frequency}, {"slope", filter.slope}};
		}

		// The object at key, or an empty one.
		Json section(const Json& json, const char* key, const std::string& name)
		{
			if (!json.contains(key))
				return Json::object();
			if (!json.at(key).is_object())
				throw std::invalid_argument(name + " must be an object");
			return json.at(key);
		}

		void read(const Json& json, const char* key, bool& value, const std::string& name)
		{
			if (!json.contains(key))
				return;
			if (!json.at(key).is_boolean())
				throw std::invalid_argument(name + "." + key + " must be true or false");
			value = json.at(key).get<bool>();
		}

		void read(
			const Json& json, const char* key, double& value, double minimum, double maximum, const std::string& name)
		{
			if (!json.contains(key))
				return;
			if (!json.at(key).is_number())
				throw std::invalid_argument(name + "." + key + " must be a number");
			value = checkedNumber(json.at(key), minimum, maximum, name + "." + key);
		}

		void read(const Json& json, const char* key, CutFilter& filter, double minimum, const std::string& name)
		{
			const std::string what = name + "." + key;
			const Json given = section(json, key, what);
			read(given, "on", filter.on, what);
			read(given, "frequency", filter.frequency, minimum, 20000.0, what);
			if (given.contains("slope"))
			{
				const Json& slope = given.at("slope");
				const std::array<int64_t, 5> slopes = {6, 12, 24, 36, 48};
				if (!slope.is_number_integer() ||
					std::find(slopes.begin(), slopes.end(), slope.get<int64_t>()) == slopes.end())
					throw std::invalid_argument(what + ".slope must be 6, 12, 24, 36 or 48");
				filter.slope = slope.get<int>();
			}
		}

		void read(const Json& json, int number, EqBand& band)
		{
			const std::string name = "eq band " + std::to_string(number);
			read(json, "on", band.on, name);
			read(json, "frequency", band.frequency, 20.0, 20000.0, name);
			read(json, "gain", band.gain, -24.0, 24.0, name);
			read(json, "q", band.q, 0.1, 12.0, name);
			if (!json.contains("type"))
				return;
			// Bands 2 and 3 are always bells; band 1 can be a low shelf or cut, band 4 a high one.
			const Json& type = json.at("type");
			const auto position = static_cast<size_t>(std::find(g_eqBandTypes.begin(), g_eqBandTypes.end(),
														  type.is_string() ? type.get<std::string>() : std::string()) -
				g_eqBandTypes.begin());
			const auto index = static_cast<EqBandType>(position);
			const bool fits = position < g_eqBandTypes.size() &&
				(index == EqBandType::Bell ||
					(number == 1 && (index == EqBandType::LowShelf || index == EqBandType::LowCut)) ||
					(number == 4 && (index == EqBandType::HighShelf || index == EqBandType::HighCut)));
			if (!fits)
				throw std::invalid_argument(name + " can't be of type " + type.dump());
			band.type = index;
		}
	}

	Json toJson(const ChannelSections& sections)
	{
		Json bands = Json::array();
		for (size_t i = 0; i < sections.eq.size(); ++i)
		{
			const EqBand& band = sections.eq[i];
			bands.push_back({{"band", i + 1}, {"on", band.on}, {"type", g_eqBandTypes[static_cast<size_t>(band.type)]},
				{"frequency", band.frequency}, {"gain", band.gain}, {"q", band.q}});
		}
		const StripGate& gate = sections.gate;
		const StripCompressor& compressor = sections.compressor;
		const StripLimiter& limiter = sections.limiter;
		return {{"pre",
					{{"gain", sections.gain}, {"phase", sections.phase}, {"lowCut", toJson(sections.lowCut)},
						{"highCut", toJson(sections.highCut)}}},
			{"eq", {{"bypass", sections.eqBypass}, {"bands", std::move(bands)}}},
			{"strip",
				{{"gate",
					 {{"on", gate.on}, {"threshold", gate.threshold}, {"range", gate.range}, {"attack", gate.attack},
						 {"release", gate.release}}},
					{"compressor",
						{{"on", compressor.on}, {"threshold", compressor.threshold}, {"ratio", compressor.ratio},
							{"attack", compressor.attack}, {"release", compressor.release},
							{"makeup", compressor.makeup}}},
					{"limiter",
						{{"on", limiter.on}, {"threshold", limiter.threshold}, {"release", limiter.release}}}}}};
	}

	ChannelSections sectionsFromJson(const Json& json, const ChannelSections& fallback)
	{
		ChannelSections sections = fallback;
		const Json pre = section(json, "pre", "pre");
		read(pre, "gain", sections.gain, -24.0, 24.0, "pre");
		read(pre, "phase", sections.phase, "pre");
		read(pre, "lowCut", sections.lowCut, 20.0, "pre");
		read(pre, "highCut", sections.highCut, 50.0, "pre");

		const Json eq = section(json, "eq", "eq");
		read(eq, "bypass", sections.eqBypass, "eq");
		if (eq.contains("bands"))
		{
			if (!eq.at("bands").is_array())
				throw std::invalid_argument("eq.bands must be an array");
			for (const Json& band : eq.at("bands"))
			{
				const Json number = band.is_object() ? band.value("band", Json()) : Json();
				if (!number.is_number_integer() || number.get<int64_t>() < 1 || number.get<int64_t>() > 4)
					throw std::invalid_argument("each of eq.bands must have a band from 1 to 4");
				read(band, number.get<int>(), sections.eq[number.get<size_t>() - 1]);
			}
		}

		const Json strip = section(json, "strip", "strip");
		const Json gate = section(strip, "gate", "strip.gate");
		StripGate& g = sections.gate;
		read(gate, "on", g.on, "strip.gate");
		read(gate, "threshold", g.threshold, -80.0, 0.0, "strip.gate");
		read(gate, "range", g.range, -80.0, 0.0, "strip.gate");
		read(gate, "attack", g.attack, 0.01, 100.0, "strip.gate");
		read(gate, "release", g.release, 1.0, 4000.0, "strip.gate");
		const Json compressor = section(strip, "compressor", "strip.compressor");
		StripCompressor& c = sections.compressor;
		read(compressor, "on", c.on, "strip.compressor");
		read(compressor, "threshold", c.threshold, -60.0, 0.0, "strip.compressor");
		read(compressor, "ratio", c.ratio, 1.0, 20.0, "strip.compressor");
		read(compressor, "attack", c.attack, 0.1, 100.0, "strip.compressor");
		read(compressor, "release", c.release, 10.0, 2000.0, "strip.compressor");
		read(compressor, "makeup", c.makeup, 0.0, 24.0, "strip.compressor");
		const Json limiter = section(strip, "limiter", "strip.limiter");
		StripLimiter& l = sections.limiter;
		read(limiter, "on", l.on, "strip.limiter");
		read(limiter, "threshold", l.threshold, -30.0, 0.0, "strip.limiter");
		read(limiter, "release", l.release, 1.0, 1000.0, "strip.limiter");
		return sections;
	}

	Project projectFromJson(const Json& json)
	{
		Project project;
		project.name = json.at("name").get<std::string>();
		project.sampleRate = json.at("sampleRate").get<double>();
		project.tempo = tempoFromJson(json.at("tempo"));
		// Files from before tempo modes (#155) follow their tempo changes, if they have any.
		project.tempoTrackActive = json.value("tempoTrackActive", project.tempo.points().size() > 1);
		if (project.tempoTrackActive)
			project.otherTempo = time::TempoMap(json.value("fixedTempo", project.tempo.points().front().bpm));
		else
			project.otherTempo = json.contains("tempoTrack") ? tempoFromJson(json.at("tempoTrack")) : project.tempo;
		std::vector<time::SignatureChange> signatures;
		for (const Json& change : json.at("signatures"))
			signatures.push_back({change.at("bar").get<int64_t>(),
				{change.at("numerator").get<int>(), change.at("denominator").get<int>()}});
		project.signatures = time::SignatureMap(signatures);
		// Files from before fades have none of them.
		project.autoFades = autoFadesFromJson(json.value("autoFades", Json::object()));
		// Files from before pan laws and maximum volumes have the defaults.
		const std::string panLaw = json.value("panLaw", panLawName(PanLaw::Minus3));
		if (const std::optional<PanLaw> law = panLawFromName(panLaw))
			project.panLaw = *law;
		else
			throw std::invalid_argument("unknown pan law " + panLaw);
		project.maxVolume = json.value("maxVolume", 6.0);
		if (project.maxVolume != 6.0 && project.maxVolume != 12.0)
			throw std::invalid_argument("maxVolume must be 6 or 12");
		if (json.contains("colors")) // files from before #94 have the factory colors
			project.colors = colorsFromJson(json.at("colors"));
		// Files from before the project setup have its defaults.
		project.setup = projectSetupFromJson(json);

		// Files from before ports have only the first four.
		for (const Json& item : json.at("busses"))
			project.busses.push_back({item.at("id").get<ObjectId>(), item.at("name").get<std::string>(),
				item.at("direction") == "input" ? BusDirection::Input : BusDirection::Output, formatFromJson(item),
				item.value("ports", std::vector<std::string>()), item.value("parent", ObjectId{0}),
				parentChannelsFromJson(item.value("parentChannels", Json::array())), item.value("mainMix", false)});
		for (const Bus& bus : project.busses)
			if (const std::string problem = busProblem(project, bus); !problem.empty())
				throw std::invalid_argument("bus " + std::to_string(bus.id) + ": " + problem);
		if (std::count_if(project.busses.begin(), project.busses.end(), [](const Bus& bus) { return bus.mainMix; }) > 1)
			throw std::invalid_argument("more than one bus is the main mix");
		keepMainMix(project);
		// Tracks come from top to bottom, folders before what they contain.
		for (const Json& item : json.at("tracks"))
		{
			Track track;
			track.id = item.at("id").get<ObjectId>();
			const auto type = trackTypeFromName(item.at("type").get<std::string>());
			if (!type)
				throw std::invalid_argument("unknown track type " + item.at("type").dump());
			track.type = *type;
			track.name = item.at("name").get<std::string>();
			track.color = colorFrom(item.at("color"));
			track.timeBase = timeBaseFromName(item.at("timeBase").get<std::string>());
			track.parent = item.at("parent").get<ObjectId>();
			track.channel = item.at("channel").get<ObjectId>();
			// Files from before disabled tracks have every track enabled; only some types can be disabled.
			track.enabled = !canDisable(track.type) || item.value("enabled", true);
			track.picture = item.value("picture", std::string()); // files from before #106 have none
			if (const Json autoFades = item.value("autoFades", Json()); !autoFades.is_null())
				track.autoFades = autoFadesFromJson(autoFades);
			if (track.type == TrackType::Ruler)
			{
				track.timeFormat = item.at("timeFormat").get<std::string>();
				if (std::find(g_rulerTimeFormats.begin(), g_rulerTimeFormats.end(), track.timeFormat) ==
					g_rulerTimeFormats.end())
					throw std::invalid_argument("unknown ruler time format " + track.timeFormat);
			}
			(track.parent == 0 ? project.tracks : project.trackById.at(track.parent).children).push_back(track.id);
			project.trackById[track.id] = track;
		}
		for (const Json& item : json.at("channels"))
		{
			const auto id = item.at("id").get<ObjectId>();
			const std::string name = "channel " + std::to_string(id);
			// Files from before instruments and channel volume have none.
			Channel channel{id, item.at("track").get<ObjectId>(), formatFromJson(item),
				item.at("input").get<ObjectId>(), item.at("output").get<ObjectId>(),
				item.value("instrument", std::string()),
				checkedNumber(item.value("volume", Json(0.0)), g_minChannelVolume, project.maxVolume, name + " volume"),
				checkedNumber(item.value("pan", Json(0.0)), -1.0, 1.0, name + " pan")};
			// Files from before the combined panner and pan bypass have the defaults.
			channel.panner =
				item.value("panner", "balance") == "combined" ? StereoPanner::Combined : StereoPanner::Balance;
			channel.panLeft = checkedNumber(item.value("panLeft", Json(-1.0)), -1.0, 1.0, name + " panLeft");
			channel.panRight = checkedNumber(item.value("panRight", Json(1.0)), -1.0, 1.0, name + " panRight");
			channel.panBypass = item.value("panBypass", false);
			for (const auto& [key, value] :
				{std::pair{"midiVolume", &channel.midiVolume}, std::pair{"midiPan", &channel.midiPan}})
				if (const Json stored = item.value(key, Json()); !stored.is_null())
					*value = static_cast<int>(checkedNumber(stored, 0.0, 127.0, name + " " + key));
			// An instrument this build doesn't have, e.g. in a file from a newer one, or one on a track that doesn't
			// play instruments: the channel plays none.
			const auto track = project.trackById.find(channel.track);
			if (!processors::isInstrument(channel.instrument) || track == project.trackById.end() ||
				track->second.type != TrackType::Instrument)
				channel.instrument.clear();
			// Files from before instrument parameters have none.
			channel.instrumentParameters = instrumentParametersFromJson(
				item.value("instrumentParameters", Json::array()), channel.instrument, name);
			// Audio, MIDI and instrument tracks have the buttons; files from before them have them off. MIDI input
			// other than none is all MIDI inputs, also in files from before it.
			const TrackType type = track != project.trackById.end() ? track->second.type : TrackType::Folder;
			const bool midi = type == TrackType::Midi || type == TrackType::Instrument;
			const bool buttons = type == TrackType::Audio || midi;
			channel.monitor = buttons && item.value("monitor", false);
			channel.recordEnable = buttons && item.value("recordEnable", false);
			channel.midiInput = midi && item.value("midiInput", std::string()) == "none" ? "none" : "all";
			channel.mute = item.value("mute", false); // files from before mute and solo have neither
			channel.solo = item.value("solo", false);
			channel.soloDefeat = item.value("soloDefeat", false); // files from before #109 have neither
			channel.listen = channel.format != ChannelFormat::None && item.value("listen", false);
			channel.slots = item.value("slots", std::vector<ObjectId>()); // files from before #282 have none
			channel.notes = item.value("notes", std::string());           // files from before #89 have none
			channel.sections = sectionsFromJson(item);                    // files from before #107 have them off
			project.channels[id] = std::move(channel);
		}
		// Every slot is in the list of its channel, which has audio, once, and no channel lists more than it may.
		for (const Json& item : json.value("slots", Json::array()))
		{
			Slot slot = slotFromJson(item);
			const auto channel = project.channels.find(slot.channel);
			if (channel == project.channels.end() || channel->second.format == ChannelFormat::None)
				throw std::invalid_argument("slot " + std::to_string(slot.id) + " is on no channel with audio");
			const ObjectId id = slot.id;
			if (!project.slots.emplace(id, std::move(slot)).second)
				throw std::invalid_argument("slot " + std::to_string(id) + " is there twice");
		}
		size_t listed = 0;
		for (const auto& [id, channel] : project.channels)
		{
			const std::string name = "channel " + std::to_string(id);
			if (channel.slots.size() > g_maxSlots)
				throw std::invalid_argument(name + " has more than " + std::to_string(g_maxSlots) + " slots");
			for (auto slot = channel.slots.begin(); slot != channel.slots.end(); ++slot)
			{
				const auto found = project.slots.find(*slot);
				if (found == project.slots.end() || found->second.channel != id ||
					std::find(channel.slots.begin(), slot, *slot) != slot)
					throw std::invalid_argument(name + " lists slot " + std::to_string(*slot) + " that isn't its own");
			}
			listed += channel.slots.size();
		}
		if (listed != project.slots.size())
			throw std::invalid_argument("a slot is in no channel's list");
		for (const Json& item : json.at("media"))
		{
			const auto id = item.at("id").get<ObjectId>();
			project.media[id] = {id, item.at("path").get<std::string>(), item.at("sampleRate").get<double>(),
				item.at("channels").get<int>(), item.at("length").get<time::Samples>()};
		}
		for (const Json& item : json.at("clips"))
		{
			const auto id = item.at("id").get<ObjectId>();
			project.clips[id] = {id, item.at("name").get<std::string>(), item.at("media").get<ObjectId>(),
				fadeFromJson(item.value("fadeIn", Json::object())), fadeFromJson(item.value("fadeOut", Json::object())),
				item.value("folder", ObjectId{0})};
		}
		// Files from before the pool's folders have none. Folders inside folders that aren't there go to the top, and
		// of each loop of folders inside each other the first one; clips in folders that aren't there go to the
		// Audio folder.
		for (const Json& item : json.value("poolFolders", Json::array()))
		{
			const auto id = item.at("id").get<ObjectId>();
			project.poolFolders[id] = {id, item.at("name").get<std::string>(), item.value("parent", ObjectId{0})};
		}
		for (auto& [id, folder] : project.poolFolders)
			if (project.poolFolders.count(folder.parent) == 0)
				folder.parent = 0;
		// Every parent is there now, so a folder is in a loop if its parents lead back to it. One that is only inside
		// a loop keeps its place once the loop is broken.
		const auto inLoop = [&project](ObjectId id)
		{
			ObjectId folder = project.poolFolders.at(id).parent;
			for (size_t steps = 0; folder != 0 && folder != id && steps < project.poolFolders.size(); ++steps)
				folder = project.poolFolders.at(folder).parent;
			return folder == id;
		};
		for (auto& [id, folder] : project.poolFolders)
			if (inLoop(id))
				folder.parent = 0;
		for (auto& [id, clip] : project.clips)
			if (project.poolFolders.count(clip.folder) == 0)
				clip.folder = 0;
		const auto base = [&project](ObjectId track) { return project.trackById.at(track).timeBase; };
		for (const Json& item : json.at("audioEvents"))
		{
			AudioEvent event;
			event.id = item.at("id").get<ObjectId>();
			event.track = item.at("track").get<ObjectId>();
			event.position = {base(event.track), item.at("position").get<int64_t>()};
			event.clip = item.at("clip").get<ObjectId>();
			event.offset = item.at("offset").get<time::Samples>();
			event.length = item.at("length").get<time::Samples>();
			event.volume = item.value("volume", 0.0); // files from before event volume and mute
			event.mute = item.value("mute", false);
			event.lock = item.value("lock", false); // files from before #85 have neither lock nor shared copies
			event.fadeIn = fadeFromJson(item.value("fadeIn", Json::object()));
			event.fadeOut = fadeFromJson(item.value("fadeOut", Json::object()));
			event.crossfade = crossfadeFrom(item.value("crossfade", Json()));
			if (const Json snapPoint = item.value("snapPoint", Json()); !snapPoint.is_null()) // files from before #87
				event.snapPoint = snapPoint.get<time::Samples>();
			event.color = eventColorFrom(item);
			event.volumeCurve = volumeCurveFromJson(item.value("volumeCurve", Json::array())); // files from before #139
			event.part = item.value("part", ObjectId{0}); // files from before #471 have no audio parts
			project.audioEvents[event.id] = event;
		}
		for (const Json& item : json.value("audioParts", Json::array()))
		{
			AudioPart part;
			part.id = item.at("id").get<ObjectId>();
			part.track = item.at("track").get<ObjectId>();
			part.name = item.at("name").get<std::string>();
			part.position = {base(part.track), item.at("position").get<int64_t>()};
			part.length = item.at("length").get<int64_t>();
			part.mute = item.value("mute", false);
			part.lock = item.value("lock", false);
			part.color = eventColorFrom(item);
			project.audioParts[part.id] = part;
		}
		// An event in a part that isn't there, or on another track, is an event of its own.
		for (auto& [id, event] : project.audioEvents)
			if (const auto part = project.audioParts.find(event.part);
				part == project.audioParts.end() || part->second.track != event.track)
				event.part = 0;
		for (const Json& item : json.at("midiParts"))
		{
			MidiPart part;
			part.id = item.at("id").get<ObjectId>();
			part.track = item.at("track").get<ObjectId>();
			part.name = item.at("name").get<std::string>();
			part.position = {base(part.track), item.at("position").get<int64_t>()};
			part.length = item.at("length").get<int64_t>();
			part.mute = item.value("mute", false);
			part.lock = item.value("lock", false);
			part.shared = item.value("shared", ObjectId{0});
			part.color = eventColorFrom(item);
			for (const Json& n : item.at("notes"))
			{
				Note note;
				note.id = n.at("id").get<ObjectId>();
				note.position = n.at("position").get<time::Ticks>();
				note.length = n.at("length").get<time::Ticks>();
				note.pitch = n.at("pitch").get<uint8_t>();
				note.velocity = velocityFrom(n.at("velocity"));
				note.channel = n.at("channel").get<uint8_t>();
				if (n.contains("releaseVelocity")) // projects saved before #137 don't have it
					note.releaseVelocity =
						static_cast<uint16_t>(std::lround(n.at("releaseVelocity").get<double>() * g_maxVelocity));
				if (n.contains("origin"))
					note.origin = QuantizeOrigin{
						timingFrom(n.at("origin").at("original")), timingFrom(n.at("origin").at("quantized"))};
				part.notes[note.id] = note;
			}
			for (const Json& c : item.at("controllers"))
			{
				Controller controller = controllerFromJson(c);
				controller.id = c.at("id").get<ObjectId>();
				controller.position = c.at("position").get<time::Ticks>();
				part.controllers[controller.id] = std::move(controller);
			}
			project.midiParts[part.id] = std::move(part);
		}
		// Files from before marker tracks have markers without a track, type and number: position markers in ticks.
		for (const Json& item : json.at("markers"))
		{
			Marker marker;
			marker.id = item.at("id").get<ObjectId>();
			marker.track = item.value("track", ObjectId{0});
			const std::string typeName = item.value("type", std::string("position"));
			const auto type = markerTypeFromName(typeName);
			if (!type)
				throw std::invalid_argument("unknown marker type " + typeName);
			marker.type = *type;
			marker.number = item.value("number", 0); // 0: numbered below
			const time::TimeBase timeBase = marker.track == 0 ? time::TimeBase::Musical : base(marker.track);
			marker.position = {timeBase, item.at("position").get<int64_t>()};
			marker.end = marker.type == MarkerType::Cycle ? time::Position{timeBase, item.at("end").get<int64_t>()}
														  : marker.position;
			marker.name = item.at("name").get<std::string>();
			marker.color = colorFrom(item.at("color"));
			project.markers[marker.id] = marker;
		}
		project.activeMarkerTrack = json.value("activeMarkerTrack", ObjectId{0});
		for (const Json& item : json.value("automation", Json::array()))
		{
			AutomationLane lane = automationLaneFromJson(project, item);
			const ObjectId id = lane.id;
			project.automation[id] = std::move(lane);
		}
		// Files from before quantizing have neither.
		if (json.contains("quantize"))
			project.quantize = midi::quantizeSettingsFromJson(json.at("quantize"));
		project.autoQuantize = json.value("autoQuantize", false);
		project.snap = snapSettingsFrom(json.value("snap", Json::object()), SnapSettings(), true); // before #87: none
		const Json mixers = json.value("mixers", Json::object()); // files from before #106 have none
		for (const auto& [name, view] : mixers.items())
			project.mixers[name] = {
				view.value("hidden", std::vector<ObjectId>()), view.value("hiddenTypes", std::vector<std::string>())};

		// Without a counter, ids continue after the highest one.
		ObjectId highest = 0;
		const auto see = [&highest](ObjectId id) { highest = std::max(highest, id); };
		for (const Bus& bus : project.busses)
			see(bus.id);
		for (const auto& [id, track] : project.trackById)
			see(id);
		for (const auto& [id, channel] : project.channels)
			see(id);
		for (const auto& [id, slot] : project.slots)
			see(id);
		for (const auto& [id, media] : project.media)
			see(id);
		for (const auto& [id, clip] : project.clips)
			see(id);
		for (const auto& [id, folder] : project.poolFolders)
			see(id);
		for (const auto& [id, event] : project.audioEvents)
			see(id);
		for (const auto& [id, part] : project.audioParts)
			see(id);

		for (const auto& [id, part] : project.midiParts)
		{
			see(id);
			for (const auto& [noteId, note] : part.notes)
				see(noteId);
			for (const auto& [controllerId, controller] : part.controllers)
				see(controllerId);
		}
		for (const auto& [id, marker] : project.markers)
			see(id);
		for (const auto& [id, lane] : project.automation)
		{
			see(id);
			for (const auto& [pointId, point] : lane.points)
				see(pointId);
		}
		project.nextId = std::max(json.value("nextId", ObjectId{0}), highest + 1);
		placeOldMarkers(project);
		return project;
	}
}
