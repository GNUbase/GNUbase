// SPDX-License-Identifier: GPL-3.0-or-later
// Track presets and track import and export (#164, reference manual pp. 199-206, 238-244): presets keep the sound of
// one or more tracks, track archives keep tracks with their events and automation. Both are DAWproject files of their
// own, so that they use the project file's serialization, and importing copies tracks from such a file, or from any
// project file, into the project.
#include "engine/document/trackpresets.h"

#include "engine/dawproject/dawproject.h"
#include "engine/document/channelsettings.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/document/trackversions.h"
#include "engine/engine.h"
#include "engine/media/backup.h"
#include "engine/midi/articulationmaps.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr const char* g_presets = "Track Presets"; // in the settings folder
		constexpr const char* g_extension = ".dawproject";

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		// What a copy of tracks takes along besides the tracks themselves.
		struct CopyOptions
		{
			bool events = true;     // events, parts, markers and track versions
			bool settings = true;   // the channel's settings and inserts
			bool automation = true; // automation lanes
		};

		// The tracks that can be imported and exported. The ones a project has only one of, and VCA faders with their
		// link groups, not yet.
		bool importable(TrackType type)
		{
			return type == TrackType::Audio || type == TrackType::Instrument || type == TrackType::Midi ||
				type == TrackType::Folder || type == TrackType::Group || type == TrackType::Effect ||
				type == TrackType::Marker || type == TrackType::Ruler;
		}

		// The tracks that presets keep the sound of (reference manual p. 238).
		bool presettable(TrackType type)
		{
			return type == TrackType::Audio || type == TrackType::Instrument || type == TrackType::Midi ||
				type == TrackType::Group || type == TrackType::Effect;
		}

		// Copies tracks of one project into another with new ids: their channels, inserts, events, automation, and the
		// clips, media files and drum maps those use. Routing within the copied tracks stays; routing to busses and
		// channels that weren't copied goes to those of the same name in the project, if it has them (reference manual
		// p. 200). The buttons (mute, solo, listen, monitor, record enable) are off in the copies.
		class TrackCopier
		{
		public:
			TrackCopier(const Project& from, Project& to, CopyOptions options)
				: m_from(from), m_to(to), m_options(options)
			{
			}

			// Copies a track with the tracks inside it into the folder parent; returns the copy, not yet among the
			// folder's tracks.
			ObjectId copy(ObjectId id, ObjectId parent)
			{
				Track track = m_from.trackById.at(id);
				require(importable(track.type),
					"cannot import or export " + std::string(trackTypeName(track.type)) + " tracks yet");
				if (track.type == TrackType::Marker)
				{
					const auto markerTracks = std::count_if(m_to.trackById.begin(), m_to.trackById.end(),
						[](const auto& entry) { return entry.second.type == TrackType::Marker; });
					require(markerTracks < g_maxMarkerTracks,
						"a project has at most " + std::to_string(g_maxMarkerTracks) + " marker tracks");
				}
				track.id = m_to.newId();
				track.parent = parent;
				track.children.clear();
				// ponytail: freeze files belong to the other project's media folder; copies come unfrozen.
				track.frozen.reset();
				if (track.drumMap != 0)
					track.drumMap = drumMap(track.drumMap);
				if (track.articulationMap != 0)
					track.articulationMap = articulationMap(track.articulationMap);
				if (!m_options.events)
				{
					track.versions.clear();
					track.activeVersion = 1;
				}
				for (TrackVersion& version : track.versions)
				{
					version = copyVersion(m_to, version, track.id);
					for (AudioEvent& event : version.audioEvents)
						event.clip = clip(event.clip);
					for (MidiPart& part : version.midiParts)
						unshare(part);
				}
				if (track.channel != 0)
					track.channel = copyChannel(track.channel, track.id);
				m_tracks.push_back(track.id);
				if (m_options.events)
					copyEvents(id, track.id);
				if (m_options.automation)
					copyAutomation(id, track.id);
				for (const ObjectId child : m_from.trackById.at(id).children)
					track.children.push_back(copy(child, track.id));
				const ObjectId result = track.id;
				m_to.trackById[result] = std::move(track);
				return result;
			}

			// Routes the copies once all tracks are copied.
			void connect()
			{
				for (const auto& [source, copy] : m_channels)
				{
					const Channel& original = m_from.channels.at(source);
					Channel& channel = m_to.channels.at(copy);
					channel.input = bus(original.input, BusDirection::Input);
					channel.output = destination(original.output, false);
					if (channel.output == 0 && original.output != 0 && channel.format != ChannelFormat::None)
						channel.output = mainMix(m_to);
					for (Send& send : channel.sends)
						send.destination = destination(send.destination, true);
				}
			}

			// The copies' events: the media files, clips and drum maps first, then the tracks from top to bottom with
			// their channels, then what they hold.
			void announce(CommandContext& context) const
			{
				for (const auto& [name, data] : m_resources)
					context.emit(name, data);
				for (const ObjectId id : m_tracks)
				{
					const Track& track = m_to.trackById.at(id);
					context.emit("track.added",
						{{"track", toJson(track)},
							{"channel", track.channel != 0 ? toJson(m_to.channels.at(track.channel)) : Json(nullptr)}});
				}
				for (const auto& [name, data] : m_contents)
					context.emit(name, data);
			}

		private:
			ObjectId copyChannel(ObjectId source, ObjectId track)
			{
				const Channel& original = m_from.channels.at(source);
				Channel channel = original;
				channel.slots.clear();
				if (!m_options.settings)
				{
					channel = Channel();
					channel.format = original.format;
					channel.instrument = original.instrument;
					channel.instrumentParameters = original.instrumentParameters;
					channel.midiInput = original.midiInput;
				}
				// Routed in connect.
				channel.input = original.input;
				channel.output = original.output;
				channel.id = m_to.newId();
				channel.track = track;
				channel.sample = clip(original.sample); // a sampler's (#180), with the instrument
				// The fader top may be lower here (as project.set brings volumes down to it).
				channel.volume = std::min(channel.volume, m_to.maxVolume);
				for (Send& send : channel.sends)
					send.level = std::min(send.level, m_to.maxVolume);
				channel.monitor = false;
				channel.recordEnable = false;
				channel.mute = false;
				channel.solo = false;
				channel.listen = false;
				if (m_options.settings)
					for (const ObjectId id : original.slots)
					{
						Slot slot = m_from.slots.at(id);
						slot.id = m_to.newId();
						slot.channel = channel.id;
						m_slots[id] = slot.id;
						channel.slots.push_back(slot.id);
						m_to.slots[slot.id] = std::move(slot);
					}
				m_channels[source] = channel.id;
				m_to.channels[channel.id] = std::move(channel);
				return m_channels.at(source);
			}

			void copyEvents(ObjectId source, ObjectId track)
			{
				std::map<ObjectId, ObjectId> parts;
				for (const auto& [id, original] : m_from.audioParts)
					if (original.track == source)
					{
						AudioPart part = original;
						part.id = m_to.newId();
						part.track = track;
						parts[id] = part.id;
						m_contents.emplace_back("audioPart.added", Json{{"part", toJson(part)}});
						m_to.audioParts[part.id] = std::move(part);
					}
				// New ids in stack order keep the order the events are stacked in (#99).
				std::vector<AudioEvent> events;
				for (const auto& [id, event] : m_from.audioEvents)
					if (event.track == source)
						events.push_back(event);
				std::stable_sort(events.begin(), events.end(),
					[](const AudioEvent& a, const AudioEvent& b) { return frontOf(a) < frontOf(b); });
				std::map<ObjectId, ObjectId> ids;
				for (const AudioEvent& event : events)
					ids[event.id] = m_to.newId();
				for (AudioEvent& event : events)
				{
					event.id = ids.at(event.id);
					event.track = track;
					event.front = 0;
					const auto from = ids.find(event.crossfade.from);
					event.crossfade.from = from == ids.end() ? 0 : from->second;
					const auto part = parts.find(event.part);
					event.part = part == parts.end() ? 0 : part->second;
					event.clip = clip(event.clip);
					m_contents.emplace_back("audioEvent.added", Json{{"event", toJson(event)}});
					m_to.audioEvents[event.id] = event;
				}
				for (const auto& [id, original] : m_from.midiParts)
					if (original.track == source)
					{
						MidiPart part = original;
						part.id = m_to.newId();
						part.track = track;
						unshare(part);
						m_contents.emplace_back("midiPart.added", Json{{"part", toJson(part)}});
						m_to.midiParts[part.id] = std::move(part);
					}
				for (const auto& [id, original] : m_from.markers)
					if (original.track == source)
					{
						Marker marker = original;
						marker.id = m_to.newId();
						marker.track = track;
						m_contents.emplace_back("marker.added", Json{{"marker", toJson(marker)}});
						m_to.markers[marker.id] = std::move(marker);
					}
			}

			void copyAutomation(ObjectId source, ObjectId track)
			{
				for (const auto& [id, original] : m_from.automation)
				{
					if (original.track != source)
						continue;
					AutomationLane lane = original;
					if (lane.target.type == AutomationTargetType::Plugin)
					{
						const auto slot = m_slots.find(lane.target.slot);
						if (slot == m_slots.end()) // the plugin stayed behind without the settings
							continue;
						lane.target.slot = slot->second;
					}
					lane.id = m_to.newId();
					lane.track = track;
					std::map<ObjectId, AutomationPoint> points;
					for (const auto& [pointId, point] : lane.points)
					{
						AutomationPoint renumbered = point;
						renumbered.id = m_to.newId();
						if (lane.target.type == AutomationTargetType::Volume)
							renumbered.value = std::min(renumbered.value, m_to.maxVolume);
						points[renumbered.id] = renumbered;
					}
					lane.points = std::move(points);
					m_contents.emplace_back("automationLane.added", Json{{"lane", toJson(lane)}});
					m_to.automation[lane.id] = std::move(lane);
				}
			}

			// ponytail: shared copies of MIDI parts come as parts of their own; keeping them shared needs their notes'
			// ids mapped alike.
			void unshare(MidiPart& part)
			{
				part.shared = 0;
				renumber(m_to, part);
			}

			// The copy of a clip, with its media file: a file the project has already is used as it is.
			ObjectId clip(ObjectId id)
			{
				if (const auto found = m_clips.find(id); found != m_clips.end())
					return found->second;
				const auto original = m_from.clips.find(id);
				if (original == m_from.clips.end())
					return 0;
				Clip clip = original->second;
				clip.id = m_to.newId();
				clip.folder = 0;
				clip.media = media(clip.media);
				for (ClipRegion& region : clip.regions)
					region.id = m_to.newId();
				m_clips[id] = clip.id;
				m_resources.emplace_back("clip.added", Json{{"clip", toJson(clip)}});
				m_to.clips[clip.id] = std::move(clip);
				return m_clips.at(id);
			}

			ObjectId media(ObjectId id)
			{
				if (const auto found = m_media.find(id); found != m_media.end())
					return found->second;
				MediaFile file = m_from.media.at(id);
				for (const auto& [mine, other] : m_to.media)
					if (other.path == file.path)
						return m_media[id] = mine;
				file.id = m_to.newId();
				m_media[id] = file.id;
				m_resources.emplace_back("media.added", Json{{"media", toJson(file)}});
				m_to.media[file.id] = std::move(file);
				return m_media.at(id);
			}

			// The project's drum map with the same name and sounds, or else a copy.
			ObjectId drumMap(ObjectId id)
			{
				const auto original = m_from.drumMaps.find(id);
				if (original == m_from.drumMaps.end())
					return 0;
				const DrumMap& map = original->second;
				for (const auto& [mine, other] : m_to.drumMaps)
					if (other.name == map.name && other.sounds == map.sounds && other.order == map.order)
						return mine;
				DrumMap copy = map;
				copy.id = m_to.newId();
				const ObjectId result = copy.id;
				m_resources.emplace_back("drumMap.added", Json{{"map", toJson(copy)}});
				m_to.drumMaps[result] = std::move(copy);
				return result;
			}

			// The project's articulation map that is the same but for its id, or else a copy (#175).
			ObjectId articulationMap(ObjectId id)
			{
				const auto original = m_from.articulationMaps.find(id);
				if (original == m_from.articulationMaps.end())
					return 0;
				ArticulationMap copy = original->second;
				for (const auto& [mine, other] : m_to.articulationMaps)
				{
					copy.id = mine;
					if (other == copy)
						return mine;
				}
				copy.id = m_to.newId();
				const ObjectId result = copy.id;
				m_resources.emplace_back("articulationMap.added", Json{{"map", toJson(copy)}});
				m_to.articulationMaps[result] = std::move(copy);
				return result;
			}

			// The project's bus with the name of the other project's bus, or 0.
			ObjectId bus(ObjectId id, BusDirection direction) const
			{
				const auto original = std::find_if(
					m_from.busses.begin(), m_from.busses.end(), [id](const Bus& bus) { return bus.id == id; });
				if (original == m_from.busses.end())
					return 0;
				const auto mine = std::find_if(m_to.busses.begin(), m_to.busses.end(),
					[&](const Bus& bus) { return bus.name == original->name && bus.direction == direction; });
				return mine == m_to.busses.end() ? 0 : mine->id;
			}

			// Where a copy plays or sends to: the copy of a copied channel, the bus of the same name (outputs only), or
			// the channel of the project's track with the same name and type as the destination's track.
			ObjectId destination(ObjectId id, bool send) const
			{
				if (id == 0)
					return 0;
				if (const auto copy = m_channels.find(id); copy != m_channels.end())
					return copy->second;
				const auto channel = m_from.channels.find(id);
				if (channel == m_from.channels.end())
					return send ? 0 : bus(id, BusDirection::Output);
				const Track& track = m_from.trackById.at(channel->second.track);
				for (const auto& [mine, other] : m_to.trackById)
					if (other.name == track.name && other.type == track.type && other.channel != 0)
						return other.channel;
				return 0;
			}

			const Project& m_from;
			Project& m_to;
			CopyOptions m_options;
			std::vector<ObjectId> m_tracks; // the copies, from top to bottom
			std::map<ObjectId, ObjectId> m_channels;
			std::map<ObjectId, ObjectId> m_slots;
			std::map<ObjectId, ObjectId> m_clips;
			std::map<ObjectId, ObjectId> m_media;
			std::vector<std::pair<std::string, Json>> m_resources;
			std::vector<std::pair<std::string, Json>> m_contents;
		};

		// Copies tracks of from, each listed once, into the folder parent of to at index, in from's order: a listed
		// folder with the tracks inside it, marker tracks at the top level. Emits their events if context isn't null.
		// Returns the copies of the listed tracks that aren't inside a listed folder.
		std::vector<ObjectId> copyTracks(const Project& from, Project& to, const std::vector<ObjectId>& tracks,
			CopyOptions options, ObjectId parent, size_t index, CommandContext* context)
		{
			require(!tracks.empty(), "no tracks given");
			for (const ObjectId id : tracks)
				require(from.trackById.count(id) != 0, "no track " + std::to_string(id));
			const auto listed = [&tracks](ObjectId id)
			{ return std::find(tracks.begin(), tracks.end(), id) != tracks.end(); };
			TrackCopier copier(from, to, options);
			std::vector<ObjectId> copies;
			for (const ObjectId id : from.trackOrder())
			{
				bool inside = false;
				for (ObjectId folder = from.trackById.at(id).parent; folder != 0 && !inside;
					folder = from.trackById.at(folder).parent)
					inside = listed(folder);
				if (!listed(id) || inside)
					continue;
				const bool top = from.trackById.at(id).type == TrackType::Marker;
				const ObjectId copy = copier.copy(id, top ? 0 : parent);
				std::vector<ObjectId>& list = top || parent == 0 ? to.tracks : to.trackById.at(parent).children;
				const size_t at = top ? list.size() : std::min(index++, list.size());
				list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), copy);
				copies.push_back(copy);
			}
			copier.connect();
			if (context == nullptr)
				return copies;
			copier.announce(*context);
			context->emit("track.reordered", {{"tracks", to.trackOrder()}});
			if (to.activeMarkerTrack == 0) // as a new marker track is
				for (const ObjectId copy : copies)
					if (to.trackById.at(copy).type == TrackType::Marker)
					{
						to.activeMarkerTrack = copy;
						context->emit("marker.trackActivated", {{"track", copy}});
						break;
					}
			return copies;
		}

		// A file with the given tracks of the project as a project of its own, for track presets and archives: the
		// project's sample rate, tempo, time signatures, fader top and busses, so that the tracks' positions, volumes
		// and routing mean the same there.
		Project tracksFile(const Project& project, const std::vector<ObjectId>& tracks, CopyOptions options)
		{
			Project file;
			file.name = project.name;
			file.sampleRate = project.sampleRate;
			file.tempo = project.tempo;
			file.tempoTrackActive = project.tempoTrackActive;
			file.otherTempo = project.otherTempo;
			file.signatures = project.signatures;
			file.maxVolume = project.maxVolume;
			file.busses = project.busses;
			file.nextId = project.nextId;
			copyTracks(project, file, tracks, options, 0, 0, nullptr);
			return file;
		}

		void save(const Project& project, const fs::path& file, bool embedMedia)
		{
			try
			{
				if (file.has_parent_path())
					fs::create_directories(file.parent_path());
				DawProjectSaveOptions options;
				options.embedMedia = embedMedia;
				saveDawProject(project, file.u8string(), options);
			}
			catch (const std::exception& error)
			{
				throw CommandError(error.what());
			}
		}

		// A project or track archive. Media files embedded in it are extracted next to it, as project.load does.
		Project load(const fs::path& file)
		{
			std::error_code error;
			require(fs::is_regular_file(file, error), "there is no file " + file.u8string());
			try
			{
				return loadDawProject(
					file.u8string(), (file.parent_path() / fs::u8path(file.stem().u8string() + " media")).u8string());
			}
			catch (const std::exception& problem)
			{
				throw CommandError(problem.what());
			}
		}

		fs::path presetFile(const Engine& engine, const std::string& name)
		{
			require(!engine.settingsDirectory().empty(), "there is no settings folder to keep track presets in");
			return engine.settingsDirectory() / g_presets / fs::u8path(name + g_extension);
		}

		Project loadPreset(const Engine& engine, const std::string& name)
		{
			const std::string problem = pathNameProblem(name);
			require(problem.empty(), problem);
			const fs::path file = presetFile(engine, name);
			std::error_code error;
			require(fs::is_regular_file(file, error), "there is no track preset " + name);
			return load(file);
		}

		// The folder and index below a track (the track parameter), or the end of the track list.
		std::pair<ObjectId, size_t> below(const Project& project, const Json& parameters)
		{
			if (!parameters.contains("track"))
				return {0, project.tracks.size()};
			const ObjectId id = parameters.at("track").get<ObjectId>();
			const auto track = project.trackById.find(id);
			require(track != project.trackById.end(), "no track " + std::to_string(id));
			const ObjectId parent = track->second.parent;
			const std::vector<ObjectId>& list = parent == 0 ? project.tracks : project.trackById.at(parent).children;
			return {parent, static_cast<size_t>(std::find(list.begin(), list.end(), id) - list.begin()) + 1};
		}

		std::vector<ObjectId> ids(const Json& list)
		{
			return list.get<std::vector<ObjectId>>();
		}

		// Gives a track the sound of a preset's track (reference manual pp. 238-240): its channel's sections, fader
		// and pan, inserts, MIDI settings and instrument, and for MIDI and instrument tracks its color. Not its name,
		// routing, sends, buttons or notes.
		void applyPreset(const ProjectModel& model, Project& project, ObjectId id, const Project& preset,
			ObjectId source, CommandContext& context)
		{
			Track& track = project.trackById.at(id);
			const Track& original = preset.trackById.at(source);
			if (track.type == TrackType::Midi || track.type == TrackType::Instrument)
			{
				track.color = original.color;
				context.emit("track.changed", {{"track", toJson(track)}});
			}
			Channel& channel = project.channels.at(track.channel);
			const Channel& sound = preset.channels.at(original.channel);
			channel.sections = sound.sections;
			channel.volume = std::min(sound.volume, project.maxVolume);
			channel.pan = sound.pan;
			channel.panner = sound.panner;
			channel.panLeft = sound.panLeft;
			channel.panRight = sound.panRight;
			channel.panBypass = sound.panBypass;
			channel.midiVolume = sound.midiVolume;
			channel.midiPan = sound.midiPan;
			channel.program = sound.program;
			channel.bank = sound.bank;
			channel.device = sound.device;
			channel.modifiers = sound.modifiers;
			channel.midiInserts = sound.midiInserts;
			channel.midiSends = sound.midiSends;
			if (channel.instrument != sound.instrument)
				channel.sample = 0; // only the sampler plays one (#180), as with channel.set
			channel.instrument = sound.instrument;
			channel.instrumentParameters = sound.instrumentParameters;
			if (channel.format != ChannelFormat::None)
			{
				std::vector<Slot> slots;
				slots.reserve(sound.slots.size());
				for (const ObjectId slot : sound.slots)
					slots.push_back(preset.slots.at(slot));
				replaceSlots(project, context, channel.id, slots);
			}
			context.emit("channel.changed", {{"channel", toJson(channel)}});
			for (const ObjectId slot : channel.slots)
				context.emit("plugin.added", {{"slot", model.describeSlot(project, slot)}});
		}
	}

	void registerTrackPresetCommands(ProjectModel& model, Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json idsSchema = {{"type", "array"}, {"items", idSchema}, {"minItems", 1}};
		const Json stringSchema = {{"type", "string"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json fileResult = objectSchema({{"file", stringSchema}}, {"file"});
		const Json tracksResult = objectSchema({{"tracks", {{"type", "array"}, {"items", idSchema}}}}, {"tracks"});

		engine.registerCommand(
			{"track.savePreset", 1,
				"Saves the sound of tracks as the track preset name (Save Track Preset, reference manual p. 239), "
				"replacing a preset of that name: of one track, or of several as a multi-track preset (p. 241). "
				"Audio, instrument, MIDI, group and effect tracks have one: their channel's pre section, equalizer, "
				"strip, fader, pan and inserts, their MIDI modifiers, inserts, sends, volume, pan, program and device, "
				"the instrument with its parameters, and the color. Presets are DAWproject files in the folder Track "
				"Presets in the user settings' folder. Changes nothing in the project. Returns the preset's file.",
				objectSchema({{"tracks", idsSchema}, {"name", stringSchema}}, {"tracks", "name"}), fileResult, false},
			[&model, &engine](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const std::string name = parameters.at("name").get<std::string>();
				const std::string problem = fileNameProblem(name);
				require(problem.empty(), problem);
				const Project& project = model.project();
				const std::vector<ObjectId> tracks = ids(parameters.at("tracks"));
				for (const ObjectId id : tracks)
				{
					const auto track = project.trackById.find(id);
					require(track != project.trackById.end(), "no track " + std::to_string(id));
					require(presettable(track->second.type),
						std::string(trackTypeName(track->second.type)) +
							" tracks have no track presets: audio, instrument, MIDI, group and effect tracks have");
				}
				const fs::path file = presetFile(engine, name);
				save(tracksFile(project, tracks, {false, true, false}), file, false);
				return Json{{"file", file.u8string()}};
			});
		engine.registerCommand(
			{"track.presets", 1,
				"The track presets (track.savePreset), sorted by name: each a name, its file and the types of its "
				"tracks from top to bottom, which the tracks it goes to must have.",
				objectSchema({}),
				objectSchema({{"presets",
								 {{"type", "array"},
									 {"items",
										 objectSchema({{"name", stringSchema}, {"file", stringSchema},
														  {"tracks", {{"type", "array"}, {"items", stringSchema}}}},
											 {"name", "file", "tracks"})}}}},
					{"presets"}),
				true},
			[&engine](CommandContext&)
			{
				std::vector<fs::path> files;
				std::error_code error;
				if (!engine.settingsDirectory().empty())
					for (const fs::directory_entry& entry :
						fs::directory_iterator(engine.settingsDirectory() / g_presets, error))
						if (entry.path().extension() == g_extension && entry.is_regular_file(error))
							files.push_back(entry.path());
				std::sort(files.begin(), files.end());
				Json presets = Json::array();
				for (const fs::path& file : files)
				{
					Json types = Json::array();
					try
					{
						const Project preset = load(file);
						for (const ObjectId id : preset.trackOrder())
							types.push_back(trackTypeName(preset.trackById.at(id).type));
					}
					catch (const std::exception&)
					{
						continue; // not a preset GNUbase can read
					}
					presets.push_back(
						{{"name", file.stem().u8string()}, {"file", file.u8string()}, {"tracks", std::move(types)}});
				}
				return Json{{"presets", std::move(presets)}};
			});
		model.addEdit(engine,
			{"track.loadPreset", 1,
				"Gives tracks the sound of a track preset (Load Track Preset, reference manual pp. 239-243), in one "
				"undo step: what track.savePreset keeps, not their names, routing, sends or buttons. Their inserts are "
				"replaced by copies of the preset's, the automation of the plugins they had goes with them. The "
				"tracks must be as many as the preset's, and of the same types in the same order. Emits "
				"track.changed, channel.changed, plugin.removed and plugin.added.",
				objectSchema({{"tracks", idsSchema}, {"name", stringSchema}}, {"tracks", "name"}), tracksResult, false},
			"Load track preset",
			[&model, &engine](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const std::string name = parameters.at("name").get<std::string>();
				const Project preset = loadPreset(engine, name);
				const std::vector<ObjectId> sources = preset.trackOrder();
				const std::vector<ObjectId> tracks = ids(parameters.at("tracks"));
				require(tracks.size() == sources.size(),
					"the track preset " + name + " is for " + std::to_string(sources.size()) +
						(sources.size() == 1 ? " track" : " tracks") + ", not " + std::to_string(tracks.size()));
				for (size_t i = 0; i < tracks.size(); ++i)
				{
					const auto track = project.trackById.find(tracks[i]);
					require(track != project.trackById.end(), "no track " + std::to_string(tracks[i]));
					const TrackType type = preset.trackById.at(sources[i]).type;
					require(track->second.type == type,
						"track " + std::to_string(tracks[i]) + " is not of the type " + trackTypeName(type) +
							" that the preset has there: presets go to tracks of their types, in their order");
				}
				for (size_t i = 0; i < tracks.size(); ++i)
					applyPreset(model, project, tracks[i], preset, sources[i], context);
				return Json{{"tracks", tracks}};
			});
		model.addEdit(engine,
			{"track.addFromPreset", 1,
				"Adds the tracks of a track preset with its sound (Use Track Preset, reference manual p. 199), below "
				"track in its folder or else at the bottom of the track list. Sends to group and effect channels go "
				"to the project's tracks with the same name, outputs to its busses with the same name or else the "
				"main output. Emits track.added and track.reordered. Returns the new tracks.",
				objectSchema({{"name", stringSchema}, {"track", idSchema}}, {"name"}), tracksResult, false},
			"Add tracks from preset",
			[&engine](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project preset = loadPreset(engine, parameters.at("name").get<std::string>());
				const auto [parent, index] = below(project, parameters);
				return Json{{"tracks", copyTracks(preset, project, preset.tracks, {}, parent, index, &context)}};
			});

		engine.registerCommand(
			{"track.export", 1,
				"Exports tracks as a track archive (Export Selected Tracks, reference manual pp. 205-206): a "
				"DAWproject file with the tracks, a folder with the tracks inside it, their channels, events and "
				"automation, which track.import imports into other projects and other applications open as a "
				"project. The project's tempo, time signatures and busses come along so that positions and routing "
				"mean the same. copyMedia (default true) copies the media files into the archive, otherwise it refers "
				"to them where they are. Marker and ruler tracks can be exported; tempo, signature, arranger, "
				"transpose, chord and VCA tracks not yet. Changes nothing in the project. Returns the file.",
				objectSchema(
					{{"tracks", idsSchema}, {"path", stringSchema}, {"copyMedia", booleanSchema}}, {"tracks", "path"}),
				fileResult, false},
			[&model](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const fs::path file = fs::u8path(parameters.at("path").get<std::string>());
				save(tracksFile(model.project(), ids(parameters.at("tracks")), {}), file,
					parameters.value("copyMedia", true));
				return Json{{"file", file.u8string()}};
			});
		engine.registerCommand(
			{"track.importSources", 1,
				"The tracks of a DAWproject file that track.import can import from: a track archive (track.export) or "
				"a project. Each with its id in the file, name, type and parent (the folder it is in, or 0), from top "
				"to bottom.",
				objectSchema({{"path", stringSchema}}, {"path"}),
				objectSchema({{"tracks",
								 {{"type", "array"},
									 {"items",
										 objectSchema({{"id", idSchema}, {"name", stringSchema}, {"type", stringSchema},
														  {"parent", idSchema}},
											 {"id", "name", "type", "parent"})}}}},
					{"tracks"}),
				true},
			[](CommandContext& context)
			{
				const Project file = load(fs::u8path(context.parameters().at("path").get<std::string>()));
				Json tracks = Json::array();
				for (const ObjectId id : file.trackOrder())
				{
					const Track& track = file.trackById.at(id);
					tracks.push_back({{"id", id}, {"name", track.name}, {"type", trackTypeName(track.type)},
						{"parent", track.parent}});
				}
				return Json{{"tracks", std::move(tracks)}};
			});
		model.addEdit(engine,
			{"track.import", 1,
				"Imports tracks from a track archive (track.export) or a project file as new tracks (Import Tracks "
				"from Project and Track Archive, reference manual pp. 200-205), in one undo step: tracks (ids in the "
				"file, see track.importSources; default: every track it can import), a folder with the tracks inside "
				"it, below track in its folder or else at the bottom of the track list; marker tracks go to the top "
				"level. events (default true) brings their events, parts, markers and track versions at their "
				"positions, settings (default true) their channel settings and inserts, automation (default true) "
				"their automation. The buttons (mute, solo, listen, monitor, record enable) are off. Routing between "
				"the imported tracks stays; sends and outputs to other channels go to the project's tracks with the "
				"same name and type, outputs to busses to its busses with the same name or else the main output. "
				"Media files embedded in the file are extracted next to it, as project.load does. Emits track.added "
				"and the events of what the tracks hold. Returns the new tracks.",
				objectSchema({{"path", stringSchema}, {"tracks", idsSchema}, {"track", idSchema},
								 {"events", booleanSchema}, {"settings", booleanSchema}, {"automation", booleanSchema}},
					{"path"}),
				tracksResult, false},
			"Import tracks",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Project file = load(fs::u8path(parameters.at("path").get<std::string>()));
				if (file.sampleRate != project.sampleRate) // linear positions are in samples: keep their time
				{
					const Json none = Json::object();
					CommandContext ignored(none);
					changeSampleRate(file, project.sampleRate, ignored);
				}
				std::vector<ObjectId> tracks;
				if (parameters.contains("tracks"))
					tracks = ids(parameters.at("tracks"));
				else
					for (const ObjectId id : file.tracks)
						if (importable(file.trackById.at(id).type))
							tracks.push_back(id);
				const CopyOptions options{parameters.value("events", true), parameters.value("settings", true),
					parameters.value("automation", true)};
				const auto [parent, index] = below(project, parameters);
				return Json{{"tracks", copyTracks(file, project, tracks, options, parent, index, &context)}};
			});
	}
}
