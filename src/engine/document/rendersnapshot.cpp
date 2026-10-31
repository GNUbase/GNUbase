// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/rendersnapshot.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <tuple>
#include <utility>
#include <vector>

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
			const auto index = static_cast<int32_t>(channel.midiParts.size());
			channel.midiParts.push_back({toSamples(project, start), toSamples(project, end)});
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
						controller.number, controller.value, controller.channel, controller.data, index});
		}

		// Mute, solo and disabled tracks: with a channel soloed, only soloed channels, those they play into and those
		// that play into them are heard (feeds: the channels each channel plays into, by index).
		void markSilent(const Project& project, const std::vector<std::vector<size_t>>& feeds, RenderSnapshot& snapshot)
		{
			const size_t count = snapshot.channels.size();
			std::vector<bool> heard(count, true);
			std::vector<size_t> soloed;
			for (size_t i = 0; i < count; ++i)
				if (project.channels.at(snapshot.channels[i].id).solo)
					soloed.push_back(i);
			if (!soloed.empty())
			{
				std::vector<std::vector<size_t>> fedBy(count);
				for (size_t i = 0; i < count; ++i)
					for (const size_t to : feeds[i])
						fedBy[to].push_back(i);
				heard.assign(count, false);
				for (const auto* links : {&feeds, &std::as_const(fedBy)})
				{
					std::vector<bool> reached(count, false);
					std::vector<size_t> pending = soloed;
					while (!pending.empty())
					{
						const size_t i = pending.back();
						pending.pop_back();
						if (reached[i])
							continue;
						reached[i] = true;
						heard[i] = true;
						pending.insert(pending.end(), (*links)[i].begin(), (*links)[i].end());
					}
				}
			}
			for (size_t i = 0; i < count; ++i)
			{
				RenderChannel& channel = snapshot.channels[i];
				channel.silent =
					!heard[i] || project.channels.at(channel.id).mute || !project.trackById.at(channel.track).enabled;
			}
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
			snapshot->busses.push_back({bus.id, bus.direction, bus.format, bus.ports, -1, bus.parentChannels});
		}
		for (size_t i = 0; i < project.busses.size(); ++i)
			if (project.busses[i].parent != 0)
				snapshot->busses[i].parent = busIndex.at(project.busses[i].parent);
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
			render.monitor = channel.monitor;
			render.recordEnable = channel.recordEnable;
			render.autoFades = track.autoFades.value_or(project.autoFades);
			if (const auto input = busIndex.find(channel.input); input != busIndex.end())
				render.input = input->second;
			snapshot->channels.push_back(std::move(render));
		}

		// Outputs, once every channel has its index.
		std::vector<std::vector<size_t>> feeds(snapshot->channels.size()); // the channels each one plays into
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
				feeds[static_cast<size_t>(channelIndex.at(render.id))].push_back(static_cast<size_t>(channel->second));
			}
		}
		markSilent(project, feeds, *snapshot);

		const auto plays = [&project](ObjectId track) { return project.trackById.at(track).enabled; };
		for (const auto& [id, event] : project.audioEvents)
		{
			if (!plays(event.track))
				continue;
			const Clip& clip = project.clips.at(event.clip);
			snapshot->channels[trackChannel.at(event.track)].audioEvents.push_back(
				{id, toSamples(project, event.position), mediaIndex.at(clip.media), event.offset, event.length,
					static_cast<float>(std::pow(10.0, event.volume / 20.0)), event.mute, event.fadeIn, event.fadeOut,
					clip.fadeIn, clip.fadeOut, crossfadeApplies(project, event) ? event.crossfade : Crossfade()});
		}
		for (const auto& [id, part] : project.midiParts)
		{
			RenderChannel& channel = snapshot->channels[static_cast<size_t>(trackChannel.at(part.track))];
			// A silent MIDI track's notes would still reach its instrument.
			if (!part.mute && plays(part.track) && (!channel.silent || channel.type != TrackType::Midi))
				compilePart(project, part, channel);
		}

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
			// The parts by start; their controller events keep pointing at them.
			std::vector<size_t> byStart(channel.midiParts.size());
			std::iota(byStart.begin(), byStart.end(), size_t{0});
			std::stable_sort(byStart.begin(), byStart.end(),
				[&channel](size_t a, size_t b) { return channel.midiParts[a].start < channel.midiParts[b].start; });
			std::vector<int32_t> moved(byStart.size());
			std::vector<RenderPart> parts;
			parts.reserve(byStart.size());
			for (size_t i = 0; i < byStart.size(); ++i)
			{
				moved[byStart[i]] = static_cast<int32_t>(i);
				parts.push_back(channel.midiParts[byStart[i]]);
			}
			channel.midiParts = std::move(parts);
			for (RenderController& controller : channel.controllers)
				if (controller.part >= 0)
					controller.part = moved[static_cast<size_t>(controller.part)];
		}
		return snapshot;
	}
}
