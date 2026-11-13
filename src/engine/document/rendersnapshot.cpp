// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/rendersnapshot.h"

#include "engine/audio/automation.h"
#include "engine/audio/fades.h"
#include "engine/document/chordpads.h"
#include "engine/document/chordtrack.h"
#include "engine/document/stripparameters.h"
#include "engine/document/warp.h"
#include "engine/dsp/dsp.h"
#include "engine/midi/articulationmaps.h"
#include "engine/midi/controllerlanes.h"
#include "engine/midi/drummaps.h"
#include "engine/midi/midivalues.h"
#include "engine/midi/patterns.h"
#include "engine/processors/drummachine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string>
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

		// The channel a note plays on: a sound slot's (#175), else its drum sound's (#129), else its own.
		uint8_t playedChannel(const Note& note, const DrumSound* sound, const SoundSlot* slot)
		{
			if (slot != nullptr && slot->channel >= 0)
				return static_cast<uint8_t>(slot->channel);
			if (sound != nullptr && sound->channel >= 0)
				return static_cast<uint8_t>(sound->channel);
			return note.channel;
		}

		// The output events of a part's sound slots (#175, reference manual p. 1270), in the order of the notes'
		// starts: where the slot changes, the off events of the one before, on the channel its notes played on, and
		// the on events of the new one, on the channel of the note. Key switches play for a sample, ending where the
		// note starts; a jump after them sends them again (MidiSequencer::chaseNotes).
		void addOutputEvents(const Project& project, const MidiPart& part, time::Ticks start, time::Ticks end,
			int32_t index, const ArticulationMap& map, const std::map<ObjectId, size_t>& slots, const DrumMap* drums,
			RenderChannel& channel)
		{
			// Notes that don't play (their probability, a muted drum sound) select no slot.
			std::vector<const Note*> notes;
			for (const auto& [id, note] : part.notes)
				if (note.position < end - start && slots.count(id) != 0 && midi::notePlays(note, part.id) &&
					(drums == nullptr || !drums->sounds.at(note.pitch).mute))
					notes.push_back(&note);
			std::stable_sort(
				notes.begin(), notes.end(), [](const Note* a, const Note* b) { return a->position < b->position; });
			const SoundSlot* previous = nullptr;
			uint8_t previousChannel = 0;
			for (const Note* note : notes)
			{
				const SoundSlot& slot = map.slots.at(slots.at(note->id));
				if (&slot == previous)
					continue;
				const time::Samples at = toSamples(project, start + std::max<time::Ticks>(note->position, 0));
				const auto send = [&](const SoundSlot& from, bool off, uint8_t on)
				{
					for (const OutputEvent& output : from.outputs)
					{
						if (output.off != off)
							continue;
						if (output.status == OutputStatus::Note)
							channel.notes.push_back({std::max<time::Samples>(at - 1, 0), std::max<time::Samples>(at, 1),
								output.data1, midi::velocityFromMidi1(output.data2), on, true});
						else if (output.status == OutputStatus::Controller)
							channel.controllers.push_back({at, ControllerType::Control, output.data1,
								midi::controllerFromMidi1(output.data2), on, {}, index});
						else
							channel.controllers.push_back(
								{at, ControllerType::Program, output.data1, g_noBank, on, {}, index});
					}
				};
				if (previous != nullptr)
					send(*previous, true, previousChannel);
				const uint8_t played = playedChannel(*note, nullptr, &slot);
				send(slot, false, played);
				previous = &slot;
				previousChannel = played;
			}
		}

		// Adds a part's notes and controllers, cut at the part's end, the notes as the chord track maps them if the
		// track follows it (#173), then as the channel's MIDI modifiers, the part's transpose and velocity and global
		// transposition (#168) at their start play them. follower is the part's track's.
		void compilePart(const Project& project, ObjectId partId, const MidiPart& part, const ChordFollower& follower,
			RenderChannel& channel)
		{
			const time::Ticks start = time::toTicks(part.position, project.tempo, project.sampleRate);
			const time::Position endPosition{part.position.base, part.position.value + part.length};
			const time::Ticks end = time::toTicks(endPosition, project.tempo, project.sampleRate);
			const auto index = static_cast<int32_t>(channel.midiParts.size());
			channel.midiParts.push_back({toSamples(project, start), toSamples(project, end)});
			// The part's transpose and velocity add to the track's modifiers, also while those are bypassed.
			midi::Modifiers modifiers = channel.modifiers.bypass ? midi::Modifiers() : channel.modifiers;
			modifiers.transpose += part.transpose;
			modifiers.velocityShift += part.velocity;
			// The track's drum map plays each note's sound: its O-note, on its channel unless that is Any, and not
			// at all if it is muted (#129). The modifiers then apply to the O-note.
			const DrumMap* map = midi::drumMapOf(project, part.track);
			// The track's articulation map plays each note's sound slot (#175): the slot's output events where the
			// slot changes, and the slot's MIDI modifiers.
			const ArticulationMap* articulations = midi::articulationMapOf(project, part.track);
			const std::map<ObjectId, size_t> slots =
				articulations != nullptr ? midi::soundSlots(*articulations, part) : std::map<ObjectId, size_t>();
			const auto slotOf = [&](ObjectId note) -> const SoundSlot*
			{
				const auto found = slots.find(note);
				return articulations != nullptr && found != slots.end() ? &articulations->slots.at(found->second)
																		: nullptr;
			};
			if (articulations != nullptr && !slots.empty())
				addOutputEvents(project, part, start, end, index, *articulations, slots, map, channel);
			for (const auto& [id, note] : part.notes)
			{
				// Notes the part hides beyond its end don't play, wherever a random position would move them.
				if (note.position >= end - start || !midi::notePlays(note, part.id))
					continue;
				const DrumSound* sound = map != nullptr ? &map->sounds.at(note.pitch) : nullptr;
				if (sound != nullptr && sound->mute)
					continue;
				const SoundSlot* slot = slotOf(id);
				const uint64_t seed = midi::noteSeed(partId, id);
				uint8_t pitch = sound != nullptr ? sound->oNote : note.pitch;
				if (sound == nullptr && follower.active())
					pitch = static_cast<uint8_t>(follower.pitch(pitch, start + note.position));
				uint16_t velocity = midi::playedVelocity(note, part.id);
				if (slot != nullptr)
				{
					pitch = static_cast<uint8_t>(std::clamp(pitch + slot->transpose, 0, 127));
					velocity = static_cast<uint16_t>(std::clamp<int64_t>(
						(int64_t{velocity} * slot->velocity) / 100, 1, std::numeric_limits<uint16_t>::max()));
				}
				midi::Modifiers transposed = modifiers;
				transposed.transpose += globalTranspose(project, part, toSamples(project, start + note.position));
				if (!midi::modifyNote(transposed, seed, pitch, velocity))
					continue;
				// A note moved before its part's start, or the project's, starts there, with its length: the part plays
				// only what lies in it.
				const time::Ticks moved =
					note.position + midi::randomOffset(modifiers, midi::RandomTarget::Position, seed);
				const time::Ticks position = std::max({moved, time::Ticks{0}, -start});
				if (position >= end - start) // moved out of the part
					continue;
				const time::Ticks slotLength =
					slot != nullptr ? std::max<time::Ticks>((note.length * slot->length) / 100, 1) : note.length;
				const time::Ticks length = note.length > 0 ? midi::modifyLength(modifiers, seed, slotLength) : 0;
				const time::Ticks noteEnd = std::min(start + position + length, end);
				channel.notes.push_back({toSamples(project, start + position), toSamples(project, noteEnd), pitch,
					velocity, playedChannel(note, sound, slot), false});
			}
			for (const auto& [id, controller] : part.controllers)
				if (controller.position < end - start && controller.type != ControllerType::Articulation)
					channel.controllers.push_back({toSamples(project, start + controller.position), controller.type,
						controller.number, controller.value, controller.channel, controller.data, index});
		}

		// An audio part's event plays only within the part (#471): cut at its edges, where its fades are cut short. A
		// muted part's events, and those it hides, don't play. Returns whether the event plays.
		// ponytail: a fade cut by the part restarts from its beginning over what is left of it; a part that cuts its
		// events this way doesn't keep their crossfades.
		bool inPart(const Project& project, const AudioPart& part, double ratio, RenderAudioEvent& event)
		{
			if (part.mute)
				return false;
			const time::Samples start = toSamples(project, part.position);
			const time::Samples end =
				toSamples(project, time::Position{part.position.base, part.position.value + part.length});
			const auto frames = [ratio](time::Samples samples)
			{ return static_cast<time::Samples>(std::llround(static_cast<double>(samples) / ratio)); };
			if (const time::Samples cut = frames(start - event.start); cut > 0)
			{
				event.start = start;
				event.offset += cut;
				event.length -= cut;
				event.fadeIn.length = std::max<time::Samples>(0, event.fadeIn.length - cut);
				event.crossfade = Crossfade();
			}
			const time::Samples eventEnd =
				event.start + static_cast<time::Samples>(std::llround(static_cast<double>(event.length) * ratio));
			if (const time::Samples cut = frames(eventEnd - end); cut > 0)
			{
				event.length -= cut;
				event.fadeOut.length = std::max<time::Samples>(0, event.fadeOut.length - cut);
			}
			return event.length > 0;
		}

		// An event of a warped clip plays a media file of its own (RenderMedia::warp), whose frames start at the
		// event's start: its clip fades and its volume curve where its audio plays.
		// ponytail: its own fades keep their length in clip frames, whatever the stretch there.
		void warpEvent(
			const Project& project, const AudioEvent& event, RenderSnapshot& snapshot, RenderAudioEvent& render)
		{
			const Clip& clip = project.clips.at(event.clip);
			const MediaFile& file = project.media.at(clip.media);
			RenderMedia media{event.id, file.path, file.sampleRate, file.channels, 0, warpMap(project, event),
				dsp::stretchPresetFromString(clip.warp.preset).value_or(dsp::StretchPreset::Quality)};
			media.length = std::llround(media.warp.back().target);
			const auto moved = [&media](time::Samples frame)
			{
				return static_cast<time::Samples>(
					std::llround(dsp::warpTarget(media.warp, static_cast<double>(frame))));
			};
			for (GainPoint& point : render.volumeCurve)
				point.position = moved(point.position);
			const WarpedClipEdges edges = warpedClipEdges(clip, file.length, media.warp);
			render.clipStart = edges.start;
			render.clipEnd = edges.end;
			render.clipFadeIn.length = edges.fadeInEnd - edges.start;
			render.clipFadeOut.length = edges.end - edges.fadeOutStart;
			render.media = static_cast<int>(snapshot.media.size());
			render.offset = 0;
			render.length = media.length;
			snapshot.media.push_back(std::move(media));
		}

		// What still plays on a frozen track: the fader, pan and mute, and the strip's sections after the plugin slots;
		// its freeze file has the rest.
		bool afterFreeze(const AutomationTarget& target)
		{
			switch (target.type)
			{
			case AutomationTargetType::Strip:
				return !isPreSection(target.parameter);
			case AutomationTargetType::Volume:
			case AutomationTargetType::Pan:
			case AutomationTargetType::PanLeft:
			case AutomationTargetType::PanRight:
			case AutomationTargetType::Mute:
				return true;
			default:
				return false;
			}
		}

		// A frozen track's channel plays its freeze file from the project's start, as an audio track's event, without
		// the pre section and plugin slots that it was rendered through (RenderPoint::Inserts). The media file and the
		// event take the ids of the channel and the track, which nothing else has.
		void freeze(const Track& track, const Freeze& frozen, RenderSnapshot& snapshot, RenderChannel& channel)
		{
			channel.type = TrackType::Audio;
			channel.instrument.clear();
			channel.instrumentParameters.clear();
			channel.samples.clear();
			channel.midiThru = false;
			channel.padThru = false;
			channel.autoFades = AutoFades();
			channel.sections.gain = 0.0;
			channel.sections.phase = false;
			channel.sections.lowCut.on = false;
			channel.sections.highCut.on = false;
			if (!track.enabled)
				return;
			RenderAudioEvent event;
			event.id = track.id;
			event.media = static_cast<int>(snapshot.media.size());
			event.length = frozen.length;
			channel.audioEvents.push_back(std::move(event));
			snapshot.media.push_back({channel.id, frozen.path, frozen.sampleRate, frozen.channels, frozen.length, {},
				dsp::StretchPreset::Quality});
		}

		// Mute, solo and disabled tracks: with a channel soloed, only soloed channels, those they play into and those
		// that play into them are heard, and channels with solo defeat with those they play into (feeds: the channels
		// each channel plays or sends into, by index). Pre-fader sends don't follow the mute button, so only a channel
		// that isn't heard or is on a disabled track silences them.
		void markSilent(const Project& project, const std::vector<std::vector<size_t>>& feeds, RenderSnapshot& snapshot)
		{
			const size_t count = snapshot.channels.size();
			std::vector<bool> heard(count, true);
			std::vector<size_t> soloed;
			std::vector<size_t> defeated;
			for (size_t i = 0; i < count; ++i)
			{
				const Channel& channel = project.channels.at(snapshot.channels[i].id);
				if (channel.solo)
					soloed.push_back(i);
				else if (channel.soloDefeat)
					defeated.push_back(i);
			}
			if (!soloed.empty())
			{
				std::vector<std::vector<size_t>> fedBy(count);
				for (size_t i = 0; i < count; ++i)
					for (const size_t to : feeds[i])
						fedBy[to].push_back(i);
				heard.assign(count, false);
				const auto reach = [&heard](const std::vector<std::vector<size_t>>& links, std::vector<size_t> pending)
				{
					std::vector<bool> reached(links.size(), false);
					while (!pending.empty())
					{
						const size_t i = pending.back();
						pending.pop_back();
						if (reached[i])
							continue;
						reached[i] = true;
						heard[i] = true;
						pending.insert(pending.end(), links[i].begin(), links[i].end());
					}
				};
				reach(feeds, soloed);
				reach(fedBy, soloed);
				reach(feeds, defeated);
			}
			// Mute automation that plays overrides the mute button (ChannelFader::Mute).
			std::set<ObjectId> automated;
			for (const auto& [id, lane] : project.automation)
				if (lane.target.type == AutomationTargetType::Mute && lane.read && !lane.points.empty())
					automated.insert(lane.track);
			for (size_t i = 0; i < count; ++i)
			{
				RenderChannel& channel = snapshot.channels[i];
				const bool muted = project.channels.at(channel.id).mute && automated.count(channel.track) == 0;
				const bool quiet = !heard[i] || !project.trackById.at(channel.track).enabled;
				channel.silent = quiet || muted;
				channel.quiet = quiet;
				channel.muted = muted;
				for (RenderSend& send : channel.sends)
					if (quiet && send.preFader)
						send.gain = 0.0f;
			}
		}
	}

	namespace
	{
		using Points = std::vector<RenderAutomationPoint>;

		// How far apart the points are where two volume curves that move at once are multiplied: 256 frames, about
		// 5 ms at 48 kHz.
		// ponytail: a long ramp of a VCA fader over a channel's long ramp makes a point every 256 frames (ten minutes:
		// about 110 000); a player that multiplies the curves on the audio thread would make none.
		constexpr int64_t g_productInterval = 4 * static_cast<int64_t>(g_automationInterval);

		// The value a curve arrives at a position with, before its points there (a jump) take over.
		double approached(const Points& points, int64_t at)
		{
			const auto first = std::lower_bound(points.begin(), points.end(), at,
				[](const RenderAutomationPoint& point, int64_t position) { return point.position < position; });
			if (first == points.end() || first->position != at)
				return automationValue(points, at);
			if (first == points.begin() || std::prev(first)->shape != AutomationShape::Step)
				return first->value;
			return std::prev(first)->value;
		}

		// The product of volume curves (linear gains) and a factor, as one curve: where only one of them moves, its
		// segment scaled (a curve segment only if it moves over the whole stretch); where several move at once,
		// straight pieces between their products every g_productInterval frames.
		Points multiplied(const std::vector<const Points*>& curves, double factor)
		{
			std::vector<int64_t> positions;
			for (const Points* curve : curves)
				for (const RenderAutomationPoint& point : *curve)
					positions.push_back(point.position);
			std::sort(positions.begin(), positions.end());
			positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
			const auto product = [&curves, factor](int64_t at, bool before)
			{
				double value = factor;
				for (const Points* curve : curves)
					value *= before ? approached(*curve, at) : automationValue(*curve, at);
				return value;
			};
			Points result;
			for (size_t i = 0; i < positions.size(); ++i)
			{
				const int64_t at = positions[i];
				const double left = product(at, true);
				const double right = product(at, false);
				result.push_back({at, left, AutomationShape::Step, 0.0});
				if (right != left)
					result.push_back({at, right, AutomationShape::Step, 0.0});
				if (i + 1 == positions.size())
					break;
				const int64_t next = positions[i + 1];
				int moving = 0;
				const RenderAutomationPoint* from = nullptr;
				const RenderAutomationPoint* to = nullptr;
				for (const Points* curve : curves)
				{
					const auto after = std::upper_bound(curve->begin(), curve->end(), at,
						[](int64_t position, const RenderAutomationPoint& point) { return position < point.position; });
					if (after == curve->begin() || after == curve->end())
						continue;
					const RenderAutomationPoint& start = *std::prev(after);
					if (start.shape == AutomationShape::Step || start.value == after->value)
						continue;
					++moving;
					from = &start;
					to = &*after;
				}
				if (moving == 0)
					continue;
				RenderAutomationPoint& segment = result.back();
				if (moving == 1 &&
					(from->shape == AutomationShape::Linear || (from->position == at && to->position == next)))
				{
					segment.shape = from->shape;
					segment.curve = from->curve;
					continue;
				}
				segment.shape = AutomationShape::Linear;
				for (int64_t piece = at + g_productInterval; piece < next; piece += g_productInterval)
					result.push_back({piece, product(piece, false), AutomationShape::Linear, 0.0});
			}
			return result;
		}

		// VCA faders (docs/features/link-groups.md): each channel's volume, static and automated, times the volume of
		// every VCA fader that controls it. vcaCurves: the VCA faders' volume automation that plays, by channel.
		void applyVcaFaders(
			const Project& project, const std::map<ObjectId, Points>& vcaCurves, RenderSnapshot& snapshot)
		{
			for (RenderChannel& channel : snapshot.channels)
			{
				const std::vector<ObjectId> chain = vcaChain(project, channel.id);
				if (chain.empty() || channel.format == ChannelFormat::None)
					continue;
				double factor = 1.0;
				std::vector<const Points*> curves;
				for (const ObjectId vca : chain)
				{
					const auto curve = vcaCurves.find(vca);
					if (curve != vcaCurves.end())
						curves.push_back(&curve->second);
					else
						factor *= volumeGain(project.channels.at(vca).volume);
				}
				const auto own = std::find_if(channel.automation.begin(), channel.automation.end(),
					[](const RenderAutomationLane& lane) { return lane.target.type == AutomationTargetType::Volume; });
				if (curves.empty())
				{
					channel.volume *= factor;
					if (own != channel.automation.end())
						for (RenderAutomationPoint& point : own->points)
							point.value *= factor;
					continue;
				}
				if (own != channel.automation.end())
				{
					// The product has no gaps: the channel's own gaps play its static volume.
					for (RenderAutomationPoint& point : own->points)
						if (point.gap)
						{
							point.value = channel.volume;
							point.gap = false;
						}
					curves.push_back(&own->points);
					own->points = multiplied(curves, factor);
					continue;
				}
				RenderAutomationLane lane;
				lane.target.type = AutomationTargetType::Volume;
				lane.points = multiplied(curves, factor * channel.volume);
				channel.automation.push_back(std::move(lane));
			}
		}
	}

	double volumeGain(double decibels)
	{
		return decibels <= g_minChannelVolume ? 0.0 : std::pow(10.0, decibels / 20.0);
	}

	std::unique_ptr<const RenderSnapshot> compile(
		const Project& project, uint64_t revision, const RenderSnapshot* previous, uint64_t opened)
	{
		auto snapshot = std::make_unique<RenderSnapshot>();
		snapshot->revision = revision;
		snapshot->opened = opened;
		snapshot->sampleRate = project.sampleRate;
		snapshot->tempo = project.tempo;
		snapshot->signatures = project.signatures;
		ArrangerPlan plan = arrangerPlan(project);
		if (previous != nullptr && *previous->arranger == plan)
			snapshot->arranger = previous->arranger;
		else
			snapshot->arranger = std::make_shared<const ArrangerPlan>(std::move(plan));
		snapshot->metronome = project.metronome;
		std::vector<ObjectId>& outputs = snapshot->metronome.outputs;
		outputs.erase(std::remove_if(outputs.begin(), outputs.end(),
						  [&project](ObjectId id)
						  {
							  return std::none_of(project.busses.begin(), project.busses.end(), [id](const Bus& bus)
								  { return bus.id == id && bus.direction == BusDirection::Output; });
						  }),
			outputs.end());
		if (outputs.empty() && mainMix(project) != 0)
			outputs.push_back(mainMix(project)); // the default output, also when the chosen busses are gone
		snapshot->chordPadKeys = chordPadKeys(project.chordPads);
		if (project.chordPads.snapPlayback)
			snapshot->chordPadKeys.snapTicks = project.quantize.grid.ticks();
		snapshot->panLaw = project.panLaw;
		snapshot->maxVolume = project.maxVolume;
		snapshot->recordBitDepth = project.setup.recordBitDepth;

		std::map<ObjectId, int> busIndex;
		for (const Bus& bus : project.busses)
		{
			busIndex[bus.id] = static_cast<int>(snapshot->busses.size());
			snapshot->busses.push_back(
				{bus.id, bus.direction, bus.format, bus.ports, -1, bus.parentChannels, bus.mainMix});
		}
		for (size_t i = 0; i < project.busses.size(); ++i)
			if (project.busses[i].parent != 0)
				snapshot->busses[i].parent = busIndex.at(project.busses[i].parent);
		std::map<ObjectId, int> mediaIndex;
		for (const auto& [id, media] : project.media)
		{
			mediaIndex[id] = static_cast<int>(snapshot->media.size());
			snapshot->media.push_back(
				{id, media.path, media.sampleRate, media.channels, media.length, {}, dsp::StretchPreset::Quality});
		}
		for (const auto& [id, clip] : project.clips)
			snapshot->clips.push_back({id, clip.media, clip.fadeIn, clip.fadeOut});

		std::map<ObjectId, int> channelIndex;
		std::map<ObjectId, int> trackChannel;
		for (const ObjectId trackId : project.trackOrder())
		{
			const Track& track = project.trackById.at(trackId);
			if (track.channel == 0 || track.type == TrackType::Vca) // a VCA fader has no audio
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
			const auto path = [&project](ObjectId id)
			{
				const auto clip = project.clips.find(id);
				return clip != project.clips.end() ? project.media.at(clip->second.media).path : std::string();
			};
			if (channel.instrument == "sampler")
			{
				render.samples = {path(channel.sample)};
			}
			else if (channel.instrument == "drumMachine")
			{
				render.samples.resize(processors::g_drumPads);
				for (const auto& [pad, drum] : channel.pads)
					render.samples.at(static_cast<size_t>(pad)) = path(drum.sample);
			}
			for (const ObjectId id : channel.slots)
			{
				const Slot& slot = project.slots.at(id);
				if (!track.frozen)
					render.slots.push_back(
						{slot.id, slot.plugin, slot.bypass, slot.active, slot.parameters, slot.state});
			}
			render.volume = volumeGain(channel.volume);
			render.pan = channel.pan;
			render.stereoPanner = channel.panner;
			render.panLeft = channel.panLeft;
			render.panRight = channel.panRight;
			render.panBypass = channel.panBypass;
			render.sections = channel.sections;
			render.midiVolume = channel.midiVolume;
			render.midiPan = channel.midiPan;
			render.program = channel.program;
			render.bank = channel.bank;
			render.midiChannel = static_cast<uint8_t>(channel.midiChannel.value_or(0));
			render.modifiers = channel.modifiers;
			if (const ArticulationMap* articulations = midi::articulationMapOf(project, track.id))
				render.articulations = *articulations;
			render.midiInserts = channel.midiInserts;
			render.midiSends = channel.midiSends;
			if (channel.inputTransformer.mode != midi::InputTransformerMode::Off)
				for (const midi::InputTransformerModule& module :
					channel.inputTransformer.mode == midi::InputTransformerMode::Project
						? project.inputTransformer
						: channel.inputTransformer.modules)
					if (module.on)
						render.inputTransformer.push_back(module.rules);
			render.monitor = channel.monitor;
			render.recordEnable = channel.recordEnable;
			render.listen = channel.listen;
			render.midiThru = (track.type == TrackType::Midi || track.type == TrackType::Instrument) &&
				channel.midiInput == "all" && (channel.monitor || channel.recordEnable);
			render.padThru = (track.type == TrackType::Midi || track.type == TrackType::Instrument) &&
				channel.midiInput == "chordPads" && (channel.monitor || channel.recordEnable);
			for (size_t player = 0; player < project.chordPads.players.size(); ++player)
				if (project.chordPads.players[player].track == track.id && track.id != 0)
					render.padPlayer = static_cast<int>(player);
			if (const DrumMap* map = midi::drumMapOf(project, track.id))
				for (const uint8_t pitch : midi::inputPitches(*map))
				{
					const DrumSound& sound = map->sounds.at(pitch);
					render.drumInput.push_back({sound.oNote, static_cast<int8_t>(sound.channel), sound.mute});
				}
			render.autoFades = track.autoFades.value_or(project.autoFades);
			if (const auto input = busIndex.find(channel.input); input != busIndex.end())
				render.input = input->second;
			if (track.frozen)
				freeze(track, *track.frozen, *snapshot, render);
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
			// A MIDI send to the instrument the track plays anyway plays to its own output.
			int own = render.output.kind == RenderTarget::Kind::Channel ? render.output.index : -1;
			if (render.type == TrackType::Instrument)
				own = channelIndex.at(render.id);
			const bool notes = render.type == TrackType::Midi || render.type == TrackType::Instrument; // not frozen
			for (size_t s = 0; s < render.midiSends.slots.size() && notes; ++s)
				if (const auto target = channelIndex.find(render.midiSends.slots[s].output);
					target != channelIndex.end() && target->second != own &&
					snapshot->channels[static_cast<size_t>(target->second)].type == TrackType::Instrument &&
					!midiSendLoops(project, render.id, target->first))
					render.midiSendOutputs[s] = target->second;
			const std::array<Send, g_maxSends>& sends = project.channels.at(render.id).sends;
			for (size_t s = 0; s < sends.size() && render.format != ChannelFormat::None; ++s)
			{
				const auto destination = channelIndex.find(sends[s].destination);
				if (destination == channelIndex.end())
					continue;
				render.sends.push_back({static_cast<int>(s), destination->second,
					sends[s].on ? static_cast<float>(volumeGain(sends[s].level)) : 0.0f, sends[s].preFader});
				feeds[static_cast<size_t>(channelIndex.at(render.id))].push_back(
					static_cast<size_t>(destination->second));
			}
		}
		markSilent(project, feeds, *snapshot);

		// A frozen track's events and parts don't play: its freeze file does.
		const auto plays = [&project](ObjectId track)
		{
			const Track& found = project.trackById.at(track);
			return found.enabled && !found.frozen;
		};
		for (const auto& [id, event] : project.audioEvents)
		{
			if (!plays(event.track))
				continue;
			const Clip& clip = project.clips.at(event.clip);
			RenderAudioEvent render{id, toSamples(project, event.position), mediaIndex.at(clip.media), event.offset,
				event.length, static_cast<float>(std::pow(10.0, event.volume / 20.0)), event.mute, event.fadeIn,
				event.fadeOut, clip.fadeIn, clip.fadeOut,
				crossfadeApplies(project, event) ? event.crossfade : Crossfade(), gainPoints(event.volumeCurve),
				frontOf(event), 0, 0};
			if (warped(clip))
				warpEvent(project, event, *snapshot, render);
			if (const auto part = project.audioParts.find(event.part); part != project.audioParts.end() &&
				!inPart(project, part->second, project.sampleRate / project.media.at(clip.media).sampleRate, render))
				continue;
			snapshot->channels[trackChannel.at(event.track)].audioEvents.push_back(std::move(render));
		}
		// MIDI sends before the modifiers (p. 1106) get the parts' notes without them, where the modifiers play.
		const auto preNotes = [](const RenderChannel& channel)
		{
			const midi::EffectRack& sends = channel.midiSends;
			return sends.pre && !sends.bypass && !channel.modifiers.bypass &&
				!(channel.modifiers == midi::Modifiers()) &&
				std::any_of(sends.slots.begin(), sends.slots.end(),
					[](const midi::EffectSlot& slot) { return slot.on && !slot.effect.empty(); });
		};
		// What each track's notes play as it follows the chord track, made once per track, not once per part.
		std::map<ObjectId, ChordFollower> followers;
		const auto follower = [&](ObjectId track) -> const ChordFollower&
		{ return followers.try_emplace(track, project, track).first->second; };
		for (const auto& [id, part] : project.midiParts)
		{
			RenderChannel& channel = snapshot->channels[static_cast<size_t>(trackChannel.at(part.track))];
			// A soloed-out or disabled MIDI track's notes would still reach its instrument. A muted one's play into its
			// strip, whose mute drops them and keeps the latest controller values for unmuting (ChannelStrip).
			if (part.mute || !plays(part.track) || (channel.quiet && channel.type == TrackType::Midi))
				continue;
			const bool isPattern = project.patterns.count(part.pattern) != 0;
			MidiPart played;
			if (isPattern)
			{
				// A pattern event (#181) plays its pattern's notes instead of its own.
				// ponytail: pattern events that overlap on a track all play, not only the topmost (p. 935).
				played = part;
				played.notes = midi::playedNotes(project, part);
			}
			const MidiPart& compiled = isPattern ? played : part;
			compilePart(project, id, compiled, follower(part.track), channel);
			if (!preNotes(channel))
				continue;
			RenderChannel unmodified;
			compilePart(project, id, compiled, follower(part.track), unmodified);
			channel.preNotes.insert(channel.preNotes.end(), unmodified.notes.begin(), unmodified.notes.end());
		}

		std::map<ObjectId, Points> vcaCurves; // by the VCA fader's channel
		for (const auto& [id, lane] : project.automation)
		{
			const auto channel = trackChannel.find(lane.track);
			const Track& track = project.trackById.at(lane.track);
			const bool vca = track.type == TrackType::Vca && lane.target.type == AutomationTargetType::Volume;
			if (!lane.read || lane.points.empty() || (channel == trackChannel.end() && !vca) ||
				(track.frozen && !afterFreeze(lane.target)))
				continue;
			std::vector<const AutomationPoint*> sorted;
			sorted.reserve(lane.points.size());
			for (const auto& [pointId, point] : lane.points)
				sorted.push_back(&point);
			std::stable_sort(sorted.begin(), sorted.end(), [](const AutomationPoint* a, const AutomationPoint* b)
				{ return a->position.value < b->position.value; });
			// Gaps (reference manual p. 881) are steps to the static value; a mute lane's is the mute button. A VCA
			// fader's curve is multiplied into other channels' curves here, so its gaps get its static volume now.
			const Channel& owner = project.channels.at(track.channel);
			double gapValue = lane.target.type == AutomationTargetType::Mute && owner.mute ? 1.0 : 0.0;
			if (vca)
				gapValue = volumeGain(owner.volume);
			const auto gapAt = [gapValue, vca](time::Samples position)
			{ return RenderAutomationPoint{position, gapValue, AutomationShape::Step, 0.0, !vca}; };
			RenderAutomationLane played;
			played.target = lane.target;
			for (const AutomationPoint* point : sorted)
			{
				const time::Samples position = toSamples(project, point->position);
				if (project.virginTerritory && played.points.empty())
					played.points.push_back(gapAt(position));
				// The fader's volume and the pre gain glide in a straight line of their linear gain.
				double value = point->value;
				if (lane.target.type == AutomationTargetType::Volume)
					value = volumeGain(point->value);
				else if (lane.target == AutomationTarget{AutomationTargetType::Strip, StripParameter::PreGain, 0, 0, 0})
					value = dsp::dbToGain(point->value);
				played.points.push_back({position, value, point->shape, point->curve});
				if (point->terminator || (project.virginTerritory && point == sorted.back()))
					played.points.push_back(gapAt(position));
			}
			if (vca)
				vcaCurves[track.channel] = std::move(played.points);
			else
				snapshot->channels[static_cast<size_t>(channel->second)].automation.push_back(std::move(played));
		}
		applyVcaFaders(project, vcaCurves, *snapshot);

		renderChordTrack(project, *snapshot);
		for (RenderChannel& channel : snapshot->channels)
		{
			std::stable_sort(channel.audioEvents.begin(), channel.audioEvents.end(),
				[](const RenderAudioEvent& a, const RenderAudioEvent& b) { return a.start < b.start; });
			// At one sample key switches come first (#175): at the project's start one can't come a sample before.
			for (std::vector<RenderNote>* notes : {&channel.notes, &channel.preNotes})
				std::stable_sort(notes->begin(), notes->end(),
					[](const RenderNote& a, const RenderNote& b)
					{
						return std::make_tuple(a.start, !a.keySwitch, a.pitch) <
							std::make_tuple(b.start, !b.keySwitch, b.pitch);
					});
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
