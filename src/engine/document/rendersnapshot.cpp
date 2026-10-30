// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/rendersnapshot.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace gnubase::engine
{
	namespace
	{
		time::Samples toSamples(const Project& project, time::Position position)
		{
			return time::toSamples(position, project.tempo, project.sampleRate);
		}

		time::Samples toSamples(const Project& project, time::Ticks tick)
		{
			return project.tempo.toSamples(tick, project.sampleRate);
		}

		// Adds a part's notes and controllers, cut at the part's end.
		void compilePart(const Project& project, const MidiPart& part, RenderChannel& channel)
		{
			const time::Ticks start = time::toTicks(part.position, project.tempo, project.sampleRate);
			const time::Position endPosition{part.position.base, part.position.value + part.length};
			const time::Ticks end = time::toTicks(endPosition, project.tempo, project.sampleRate);
			for (const auto& [id, note] : part.notes)
			{
				if (note.position >= end - start)
					continue;
				const time::Ticks noteEnd = std::min(start + note.position + note.length, end);
				channel.notes.push_back({toSamples(project, start + note.position), toSamples(project, noteEnd),
					note.pitch, note.velocity, note.channel});
			}
			for (const auto& [id, controller] : part.controllers)
				if (controller.position < end - start)
					channel.controllers.push_back({toSamples(project, start + controller.position), controller.type,
						controller.number, controller.value, controller.channel, controller.data});
		}
	}

	double volumeGain(double decibels)
	{
		return decibels <= g_minChannelVolume ? 0.0 : std::pow(10.0, decibels / 20.0);
	}

	std::unique_ptr<const RenderSnapshot> compile(const Project& project, uint64_t revision)
	{
		auto snapshot = std::make_unique<RenderSnapshot>();
		snapshot->revision = revision;
		snapshot->sampleRate = project.sampleRate;
		snapshot->tempo = project.tempo;
		snapshot->panLaw = project.panLaw;
		snapshot->maxVolume = project.maxVolume;

		std::map<ObjectId, int> busIndex;
		for (const Bus& bus : project.busses)
		{
			busIndex[bus.id] = static_cast<int>(snapshot->busses.size());
			snapshot->busses.push_back({bus.id, bus.direction, bus.format});
		}
		std::map<ObjectId, int> mediaIndex;
		for (const auto& [id, media] : project.media)
		{
			mediaIndex[id] = static_cast<int>(snapshot->media.size());
			snapshot->media.push_back({id, media.path, media.sampleRate, media.channels, media.length});
		}

		std::map<ObjectId, int> channelIndex;
		std::map<ObjectId, int> trackChannel;
		for (const ObjectId trackId : project.trackOrder())
		{
			const Track& track = project.trackById.at(trackId);
			if (track.channel == 0)
				continue;
			const Channel& channel = project.channels.at(track.channel);
			channelIndex[channel.id] = static_cast<int>(snapshot->channels.size());
			trackChannel[trackId] = channelIndex[channel.id];
			RenderChannel render;
			render.id = channel.id;
			render.track = trackId;
			render.type = track.type;
			render.format = channel.format;
			render.instrument = channel.instrument;
			render.instrumentParameters = channel.instrumentParameters;
			render.volume = volumeGain(channel.volume);
			render.pan = channel.pan;
			render.stereoPanner = channel.panner;
			render.panLeft = channel.panLeft;
			render.panRight = channel.panRight;
			render.panBypass = channel.panBypass;
			render.midiVolume = channel.midiVolume;
			render.midiPan = channel.midiPan;
			render.autoFades = track.autoFades.value_or(project.autoFades);
			if (const auto input = busIndex.find(channel.input); input != busIndex.end())
				render.input = input->second;
			snapshot->channels.push_back(std::move(render));
		}

		// Outputs, once every channel has its index.
		for (RenderChannel& render : snapshot->channels)
		{
			const ObjectId output = project.channels.at(render.id).output;
			if (const auto bus = busIndex.find(output); bus != busIndex.end())
			{
				render.output = {RenderTarget::Kind::Bus, bus->second};
				render.panner = pannerFor(render.format, snapshot->busses[static_cast<size_t>(bus->second)].format);
			}
			else if (const auto channel = channelIndex.find(output); channel != channelIndex.end())
			{
				render.output = {RenderTarget::Kind::Channel, channel->second};
				render.panner =
					pannerFor(render.format, snapshot->channels[static_cast<size_t>(channel->second)].format);
			}
		}

		for (const auto& [id, event] : project.audioEvents)
		{
			const Clip& clip = project.clips.at(event.clip);
			snapshot->channels[trackChannel.at(event.track)].audioEvents.push_back(
				{id, toSamples(project, event.position), mediaIndex.at(clip.media), event.offset, event.length,
					static_cast<float>(std::pow(10.0, event.volume / 20.0)), event.mute, event.fadeIn, event.fadeOut,
					clip.fadeIn, clip.fadeOut, crossfadeApplies(project, event) ? event.crossfade : Crossfade()});
		}
		for (const auto& [id, part] : project.midiParts)
			compilePart(project, part, snapshot->channels[trackChannel.at(part.track)]);

		for (const auto& [id, lane] : project.automation)
		{
			const auto channel = trackChannel.find(lane.track);
			if (!lane.read || lane.points.empty() || channel == trackChannel.end())
				continue;
			RenderAutomationLane render;
			render.target = lane.target;
			for (const auto& [pointId, point] : lane.points)
				render.points.push_back({toSamples(project, point.position),
					lane.target.type == AutomationTargetType::Volume ? volumeGain(point.value) : point.value,
					point.shape, point.curve});
			std::stable_sort(render.points.begin(), render.points.end(),
				[](const RenderAutomationPoint& a, const RenderAutomationPoint& b) { return a.position < b.position; });
			snapshot->channels[static_cast<size_t>(channel->second)].automation.push_back(std::move(render));
		}

		for (RenderChannel& channel : snapshot->channels)
		{
			std::stable_sort(channel.audioEvents.begin(), channel.audioEvents.end(),
				[](const RenderAudioEvent& a, const RenderAudioEvent& b) { return a.start < b.start; });
			std::stable_sort(channel.notes.begin(), channel.notes.end(), [](const RenderNote& a, const RenderNote& b)
				{ return std::tie(a.start, a.pitch) < std::tie(b.start, b.pitch); });
			std::stable_sort(channel.controllers.begin(), channel.controllers.end(),
				[](const RenderController& a, const RenderController& b) { return a.position < b.position; });
		}
		return snapshot;
	}
}
