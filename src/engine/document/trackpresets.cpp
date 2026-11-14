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
#include "engine/time/tempomap.h"
#include "engine/time/timecode.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <map>
#include <optional>
#include <set>
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

		// The version IDs an import gives the file's: a new one for each ID of the file, shared by the versions that
		// shared it there, so that they keep playing together (reference manual p. 233) and none of them falls into a
		// version group the project has of its own.
		class VersionIds
		{
		public:
			explicit VersionIds(const Project& project) : m_next(newVersionId(project)) {}

			int of(int id)
			{
				if (const auto found = m_ids.find(id); found != m_ids.end())
					return found->second;
				return m_ids.emplace(id, m_next++).first->second;
			}

		private:
			std::map<int, int> m_ids;
			int m_next;
		};

		// Copies tracks of one project into another with new ids: their channels, inserts, events, automation, and the
		// clips, media files and drum maps those use; with the events their patterns, which their pattern events play.
		// Routing within the copied tracks stays; routing to busses and channels that weren't copied goes to those of
		// the same name in the project, if it has them (reference manual p. 200). The buttons (mute, solo, listen,
		// monitor, record enable) are off in the copies, and their automation reads, as the reference doesn't import
		// these states. replacedMarkers marker tracks of to are about to be replaced by copies, so these copies don't
		// count towards the limit of marker tracks. versionIds gives the copies' track versions their ids in to;
		// without it they keep the ones they have in from, as an export writes a project of its own.
		class TrackCopier
		{
		public:
			TrackCopier(const Project& from, Project& to, CopyOptions options, std::ptrdiff_t replacedMarkers,
				VersionIds* versionIds)
				: m_from(from), m_to(to), m_options(options), m_firstCopy(to.nextId),
				  m_replacedMarkers(replacedMarkers), m_versionIds(versionIds)
			{
			}

			// Copies a track with the tracks inside it into the folder parent; returns the copy, not yet among the
			// folder's tracks. Tracks inside it that can't be copied stay behind (skipped).
			ObjectId copy(ObjectId id, ObjectId parent)
			{
				Track track = m_from.trackById.at(id);
				if (track.type == TrackType::Marker)
				{
					const auto markerTracks = std::count_if(m_to.trackById.begin(), m_to.trackById.end(),
						[](const auto& entry) { return entry.second.type == TrackType::Marker; });
					require(markerTracks - m_replacedMarkers < g_maxMarkerTracks,
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
				else
				{
					copyPatterns(id, track.id);
					if (m_versionIds != nullptr && !track.versions.empty())
					{
						for (TrackVersion& version : track.versions)
							version.id = m_versionIds->of(version.id);
						track.activeVersion = m_versionIds->of(track.activeVersion);
					}
				}
				for (TrackVersion& version : track.versions)
				{
					version = copyVersion(m_to, version, track.id);
					for (AudioEvent& event : version.audioEvents)
						event.clip = clip(event.clip);
					for (MidiPart& part : version.midiParts)
						copyPart(part);
				}
				if (track.channel != 0)
					track.channel = copyChannel(track.channel, track.id);
				m_copies[id] = track.id;
				if (m_options.events)
					copyEvents(id, track.id);
				if (m_options.automation)
					copyAutomation(id, track.id);
				for (const ObjectId child : m_from.trackById.at(id).children)
				{
					if (importable(m_from.trackById.at(child).type))
						track.children.push_back(copy(child, track.id));
					else
						m_skipped.push_back(child);
				}
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
					for (midi::EffectSlot& slot : channel.midiSends.slots)
						slot.output = destination(slot.output, true);
				}
			}

			// The tracks inside copied folders that stayed behind, from top to bottom.
			const std::vector<ObjectId>& skipped() const { return m_skipped; }

			// The copy of each track copied, by its id in from.
			const std::map<ObjectId, ObjectId>& copies() const { return m_copies; }

			// The media files that were added, not the project's that the copies share.
			const std::vector<ObjectId>& addedMedia() const { return m_addedMedia; }

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
				m_to.clips[clip.id] = std::move(clip);
				return m_clips.at(id);
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
				channel.pads = original.pads;           // a drum machine's (#182)
				for (auto& [pad, drum] : channel.pads)
					drum.sample = clip(drum.sample);
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
						m_to.audioParts[part.id] = std::move(part);
					}
				// New ids in stack order keep the order the events are stacked in (#99).
				std::vector<AudioEvent> events;
				for (const auto& [id, event] : m_from.audioEvents)
					if (event.track == source)
						events.push_back(event);
				std::stable_sort(events.begin(), events.end(),
					[](const AudioEvent& a, const AudioEvent& b) { return stackKey(a) < stackKey(b); });
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
					m_to.audioEvents[event.id] = event;
				}
				for (const auto& [id, original] : m_from.midiParts)
					if (original.track == source)
					{
						MidiPart part = original;
						part.id = m_to.newId();
						part.track = track;
						copyPart(part);
						m_to.midiParts[part.id] = std::move(part);
					}
				for (const auto& [id, original] : m_from.markers)
					if (original.track == source)
					{
						Marker marker = original;
						marker.id = m_to.newId();
						marker.track = track;
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
					lane.read = true; // as a new lane's: the read state isn't imported (reference manual p. 200)
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
					m_to.automation[lane.id] = std::move(lane);
				}
			}

			// The track's patterns with new ids (#1347).
			void copyPatterns(ObjectId source, ObjectId track)
			{
				for (const auto& [id, original] : m_from.patterns)
					if (original.track == source)
					{
						Pattern pattern = original;
						pattern.id = m_to.newId();
						pattern.track = track;
						pattern.copiedFrom = 0;
						m_patterns[id] = pattern.id;
						m_to.patterns[pattern.id] = std::move(pattern);
					}
			}

			// ponytail: shared copies of MIDI parts come as parts of their own; keeping them shared needs their notes'
			// ids mapped alike. A pattern event plays the copy of its pattern; one of a pattern that wasn't copied
			// (another track's) becomes an ordinary part.
			void copyPart(MidiPart& part)
			{
				part.shared = 0;
				const auto pattern = m_patterns.find(part.pattern);
				part.pattern = pattern == m_patterns.end() ? 0 : pattern->second;
				renumber(m_to, part);
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
				m_addedMedia.push_back(file.id);
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
			// the channel of the project's track with the same name and type as the destination's track: one it had
			// before the copy, not another copy, which could route the copies in a loop.
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
					if (mine < m_firstCopy && other.name == track.name && other.type == track.type &&
						other.channel != 0)
						return other.channel;
				return 0;
			}

			const Project& m_from;
			Project& m_to;
			CopyOptions m_options;
			ObjectId m_firstCopy; // the copies' ids start here
			std::ptrdiff_t m_replacedMarkers;
			VersionIds* m_versionIds;              // the copies' track version ids, or null to keep the ones of from
			std::map<ObjectId, ObjectId> m_copies; // by the track's id in from
			std::vector<ObjectId> m_skipped;
			std::map<ObjectId, ObjectId> m_channels;
			std::map<ObjectId, ObjectId> m_slots;
			std::map<ObjectId, ObjectId> m_clips;
			std::map<ObjectId, ObjectId> m_patterns;
			std::map<ObjectId, ObjectId> m_media;
			std::vector<ObjectId> m_addedMedia;
		};

		struct Copied
		{
			std::vector<ObjectId> tracks;        // the copies of the listed tracks that aren't inside a listed folder
			std::vector<ObjectId> skipped;       // the tracks of from inside listed folders that can't be copied
			std::map<ObjectId, ObjectId> copies; // the copy of each track copied, by its id in from
			std::vector<ObjectId> media;         // the media files added
		};

		// Copies tracks of from, each listed once, into the folder parent of to at index, in from's order: a listed
		// folder with the tracks inside it that can be copied, marker tracks at the top level. Listed tracks that can't
		// be copied are refused, also inside a listed folder. Emits nothing: the commands announce what changed.
		// replacedMarkers and versionIds: see TrackCopier.
		Copied copyTracks(const Project& from, Project& to, const std::vector<ObjectId>& tracks, CopyOptions options,
			ObjectId parent, size_t index, std::ptrdiff_t replacedMarkers = 0, VersionIds* versionIds = nullptr)
		{
			require(!tracks.empty(), "no tracks given");
			for (const ObjectId id : tracks)
			{
				const auto track = from.trackById.find(id);
				require(track != from.trackById.end(), "no track " + std::to_string(id));
				require(importable(track->second.type),
					"cannot import or export " + std::string(trackTypeName(track->second.type)) + " tracks yet");
			}
			const auto listed = [&tracks](ObjectId id)
			{ return std::find(tracks.begin(), tracks.end(), id) != tracks.end(); };
			TrackCopier copier(from, to, options, replacedMarkers, versionIds);
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
			if (to.activeMarkerTrack == 0) // as a new marker track is
				for (const ObjectId copy : copies)
					if (to.trackById.at(copy).type == TrackType::Marker)
					{
						to.activeMarkerTrack = copy;
						break;
					}
			return {copies, copier.skipped(), copier.copies(), copier.addedMedia()};
		}

		// The tracks as their ids, names and types, for the tracks an import or export leaves out, from top to bottom.
		Json describe(const Project& project, const std::vector<ObjectId>& tracks)
		{
			Json result = Json::array();
			for (const ObjectId id : project.trackOrder())
				if (std::find(tracks.begin(), tracks.end(), id) != tracks.end())
				{
					const Track& track = project.trackById.at(id);
					result.push_back({{"id", id}, {"name", track.name}, {"type", trackTypeName(track.type)}});
				}
			return result;
		}

		// The sample rates of the media files each track's audio events play, by the track's id, each once from the
		// lowest, also the events of the versions that aren't active: what Convert Sample Rate has to convert for that
		// track (reference manual p. 205). A media file no event plays is nobody's.
		std::map<ObjectId, std::set<double>> mediaSampleRatesByTrack(const Project& file)
		{
			const auto add = [&file](
								 std::map<ObjectId, std::set<double>>& rates, ObjectId track, const AudioEvent& event)
			{
				const auto clip = file.clips.find(event.clip);
				if (clip == file.clips.end())
					return;
				const auto media = file.media.find(clip->second.media);
				if (media != file.media.end())
					rates[track].insert(media->second.sampleRate);
			};
			std::map<ObjectId, std::set<double>> rates;
			for (const auto& [id, event] : file.audioEvents)
				add(rates, event.track, event);
			for (const auto& [id, track] : file.trackById)
				for (const TrackVersion& version : track.versions)
					for (const AudioEvent& event : version.audioEvents)
						add(rates, id, event);
			return rates;
		}

		// A file with the given tracks of the project as a project of its own, for track presets and archives: the
		// project's sample rate, tempo, time signatures, fader top and busses, so that the tracks' positions, volumes
		// and routing mean the same there. The tracks inside the folders that can't go along are skipped.
		Project tracksFile(
			const Project& project, const std::vector<ObjectId>& tracks, CopyOptions options, Json* skipped = nullptr)
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
			const Copied copied = copyTracks(project, file, tracks, options, 0, 0);
			if (skipped != nullptr)
				*skipped = describe(project, copied.skipped);
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

		// A project or track archive. Media files embedded in it are extracted next to it, as project.load does, unless
		// only what it holds is listed (extract false).
		Project load(const fs::path& file, bool extract = true)
		{
			std::error_code error;
			require(fs::is_regular_file(file, error), "there is no file " + file.u8string());
			try
			{
				return loadDawProject(file.u8string(),
					(file.parent_path() / fs::u8path(file.stem().u8string() + " media")).u8string(), extract);
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

		// Keeps the items for which keep is true, in their order.
		template <typename Key, typename T, typename Keep> void keepIf(std::map<Key, T>& items, Keep keep)
		{
			for (auto item = items.begin(); item != items.end();)
				item = keep(item->second) ? std::next(item) : items.erase(item);
		}

		template <typename T, typename Keep> void keepIf(std::vector<T>& items, Keep keep)
		{
			std::vector<T> kept;
			for (T& item : items)
				if (keep(item))
					kept.push_back(std::move(item));
			items = std::move(kept);
		}

		// Where track.import puts what the file's tracks hold (Import Position and Range, reference manual p. 204):
		// only what starts within range, if given: samples at the project's sample rate from the file's start, as the
		// file is at the project's rate by then; an audio event in a part goes with its part. That moves by offset
		// samples of the project, musical positions by that time in its tempo. Refuses what would start before the
		// project start. First leaves out what the import doesn't bring: what isn't on the tracks of the file that it
		// copies (copied), and the events or automation that options leave out.
		void place(Project& file, const Project& project, time::Samples offset,
			const std::optional<std::pair<time::Samples, time::Samples>>& range, const std::set<ObjectId>& copied,
			CopyOptions options)
		{
			const auto brought = [&copied](const auto& item) { return copied.count(item.track) != 0; };
			const auto withEvents = [&](const auto& item) { return options.events && brought(item); };
			keepIf(file.audioParts, withEvents);
			keepIf(file.audioEvents, withEvents);
			keepIf(file.midiParts, withEvents);
			keepIf(file.markers, withEvents);
			keepIf(file.automation, [&](const AutomationLane& lane) { return options.automation && brought(lane); });
			for (auto& [id, track] : file.trackById)
				if (!options.events || copied.count(id) == 0)
					track.versions.clear();
			const auto move = [&](time::Position& position)
			{
				if (offset == 0)
					return;
				const time::Samples at = time::toSamples(position, project.tempo, project.sampleRate) + offset;
				require(at >= 0,
					"the tracks' events would start before the project start: import them at their relative position "
					"or at the cursor");
				position = time::rebase({time::TimeBase::Linear, at}, position.base, project.tempo, project.sampleRate);
			};
			// ponytail: what starts in the range comes whole, not cut at its ends.
			const auto keep = [&](time::Position& position)
			{
				const time::Samples at = time::toSamples(position, file.tempo, file.sampleRate);
				if (range && (at < range->first || at >= range->second))
					return false;
				move(position);
				return true;
			};
			const auto events = [&](auto& audioParts, auto& audioEvents, auto& midiParts)
			{
				std::set<ObjectId> parts; // the audio parts that stay
				keepIf(audioParts,
					[&](AudioPart& part)
					{
						const bool kept = keep(part.position);
						if (kept)
							parts.insert(part.id);
						return kept;
					});
				keepIf(audioEvents,
					[&](AudioEvent& event)
					{
						if (event.part == 0)
							return keep(event.position);
						if (parts.count(event.part) == 0)
							return false;
						move(event.position);
						return true;
					});
				keepIf(midiParts, [&](MidiPart& part) { return keep(part.position); });
			};
			events(file.audioParts, file.audioEvents, file.midiParts);
			for (auto& [id, track] : file.trackById)
				for (TrackVersion& version : track.versions)
					events(version.audioParts, version.audioEvents, version.midiParts);
			keepIf(file.markers,
				[&](Marker& marker)
				{
					if (!keep(marker.position))
						return false;
					move(marker.end);
					return true;
				});
			for (auto& [id, lane] : file.automation)
				keepIf(lane.points, [&](AutomationPoint& point) { return keep(point.position); });
		}

		// The end of what the tracks hold, in samples from the project start: their events, parts, markers and
		// automation points.
		time::Samples contentEnd(const Project& project, const std::set<ObjectId>& tracks)
		{
			const auto at = [&project](const time::Position& position)
			{ return time::toSamples(position, project.tempo, project.sampleRate); };
			time::Samples end = 0;
			for (const auto& [id, event] : project.audioEvents)
			{
				if (tracks.count(event.track) == 0)
					continue;
				const auto clip = project.clips.find(event.clip);
				const bool played = clip != project.clips.end() && project.media.count(clip->second.media) != 0;
				end = std::max(end, played ? eventSpan(project, event).end : at(event.position));
			}
			for (const auto& [id, part] : project.audioParts)
				if (tracks.count(part.track) != 0)
					end = std::max(end, at({part.position.base, part.position.value + part.length}));
			for (const auto& [id, part] : project.midiParts)
				if (tracks.count(part.track) != 0)
					end = std::max(end, at({part.position.base, part.position.value + part.length}));
			for (const auto& [id, marker] : project.markers)
				if (tracks.count(marker.track) != 0)
					end = std::max({end, at(marker.position), at(marker.end)});
			for (const auto& [id, lane] : project.automation)
				if (tracks.count(lane.track) != 0)
					for (const auto& [pointId, point] : lane.points)
						end = std::max(end, at(point.position));
			return end;
		}

		// What an import onto an existing track does with its events and parts (reference manual p. 203).
		enum class Existing : uint8_t
		{
			Replace, // Replace All Events and Parts
			Add,     // Add Events and Parts
			Version, // Create a New Track Version
		};

		// What played or sent into the channel from goes into the channel to.
		void reroute(Project& project, ObjectId from, ObjectId to)
		{
			for (auto& [id, channel] : project.channels)
			{
				if (channel.output == from)
					channel.output = to;
				for (Send& send : channel.sends)
					if (send.destination == from)
						send.destination = to;
			}
		}

		// A group, effect, folder, marker or ruler track imported onto the project's track replaces it, with the
		// tracks inside it (reference manual p. 200): the copy takes its place, and what played or sent into its
		// channel goes into the copy's.
		void replaceTrack(ProjectModel& model, Project& project, ObjectId copy, ObjectId id, CommandContext& inner)
		{
			Track& moved = project.trackById.at(copy);
			std::vector<ObjectId>& from =
				moved.parent == 0 ? project.tracks : project.trackById.at(moved.parent).children;
			from.erase(std::find(from.begin(), from.end(), copy));
			const Track& track = project.trackById.at(id);
			std::vector<ObjectId>& into =
				track.parent == 0 ? project.tracks : project.trackById.at(track.parent).children;
			into.insert(std::find(into.begin(), into.end(), id), copy);
			moved.parent = track.parent;
			if (track.channel != 0 && moved.channel != 0)
				reroute(project, track.channel, moved.channel);
			if (project.activeMarkerTrack == id) // the copy has the marker track's data now
				project.activeMarkerTrack = copy;
			model.runEdit("track.remove", project, {{"track", id}}, inner);
		}

		// An audio, instrument or MIDI track imported onto the project's track (reference manual pp. 200, 203-204): the
		// track gets the copy's events and parts as existing says (for version, the caller gave it a new version
		// already), and the copy's other versions after its own; with settings its channel settings, inserts and drum
		// and articulation maps, but its own buttons; with automation its automation instead of the track's, without
		// it none once events or settings come. What played or sent into the copy's channel goes into the track's, and
		// the copy goes. The copy's versions have the IDs they get here already (VersionIds), so that versions of
		// several tracks that shared an ID in the file keep sharing one.
		void mergeTrack(ProjectModel& model, Project& project, ObjectId copy, ObjectId id, CopyOptions options,
			Existing existing, CommandContext& inner)
		{
			// Positions are in the track's time base.
			if (project.trackById.at(copy).timeBase != project.trackById.at(id).timeBase)
				rebaseTrack(project, project.trackById.at(copy), project.trackById.at(id).timeBase, inner);
			if (options.events)
			{
				const auto mine = [id](const auto& item) { return item.track != id; };
				if (existing == Existing::Replace)
				{
					keepIf(project.audioEvents, mine);
					keepIf(project.audioParts, mine);
					keepIf(project.midiParts, mine);
				}
				const auto onto = [copy, id](auto& items)
				{
					for (auto& item : items)
						if (item.second.track == copy)
							item.second.track = id;
				};
				onto(project.audioEvents);
				onto(project.audioParts);
				onto(project.midiParts);
				onto(project.patterns); // which the copy's pattern events play, in its other versions too (#1347)
				Track& source = project.trackById.at(copy);
				Track& track = project.trackById.at(id);
				if (!source.versions.empty() && track.versions.empty())
					track.versions = versionsOf(track);
				for (TrackVersion& version : source.versions)
				{
					if (version.id == source.activeVersion)
						continue; // its events and parts went onto the track
					if (std::any_of(track.versions.begin(), track.versions.end(),
							[&version](const TrackVersion& other) { return other.name == version.name; }))
						version.name = newVersionName(track);
					for (AudioEvent& event : version.audioEvents)
						event.track = id;
					for (AudioPart& part : version.audioParts)
						part.track = id;
					for (MidiPart& part : version.midiParts)
						part.track = id;
					track.versions.push_back(std::move(version));
				}
				source.versions.clear();
			}
			const Track& source = project.trackById.at(copy);
			Track& track = project.trackById.at(id);
			if (options.settings && source.channel != 0 && track.channel != 0)
			{
				const Channel& old = project.channels.at(track.channel);
				Channel channel = project.channels.at(source.channel);
				for (const ObjectId slot : old.slots) // the track's plugins go, with their automation
				{
					project.slots.erase(slot);
					keepIf(project.automation, [slot](const AutomationLane& lane)
						{ return lane.target.type != AutomationTargetType::Plugin || lane.target.slot != slot; });
				}
				channel.id = old.id;
				channel.track = id;
				channel.mute = old.mute;
				channel.solo = old.solo;
				channel.listen = old.listen;
				channel.monitor = old.monitor;
				channel.recordEnable = old.recordEnable;
				for (const ObjectId slot : channel.slots)
					project.slots.at(slot).channel = channel.id;
				project.channels.at(source.channel).slots.clear();
				project.channels[channel.id] = std::move(channel);
				track.drumMap = source.drumMap;
				track.articulationMap = source.articulationMap;
			}
			// Without automation, importing events or settings resets the track's (the note on Automation, p. 204).
			if (options.automation || options.events || options.settings)
				keepIf(project.automation, [id](const AutomationLane& lane) { return lane.track != id; });
			if (options.automation)
				for (auto& [laneId, lane] : project.automation)
					if (lane.track == copy)
						lane.track = id;
			if (source.channel != 0 && track.channel != 0)
				reroute(project, source.channel, track.channel);
			model.runEdit("track.remove", project, {{"track", copy}}, inner);
		}

		// Gives a track the sound of a preset's track (reference manual pp. 238-240): its channel's sections, fader
		// and pan, inserts, MIDI settings and instrument, and for MIDI and instrument tracks its color. Not its name,
		// routing, sends, buttons or notes.
		void applyPreset(const ProjectModel& model, Project& project, ObjectId id, const Project& preset,
			ObjectId source, TrackCopier& copier, CommandContext& context)
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
			channel.midiChannel = sound.midiChannel;
			channel.device = sound.device;
			channel.modifiers = sound.modifiers;
			const Channel before = channel;
			channel.midiInserts = sound.midiInserts;
			channel.midiSends = sound.midiSends;
			removeMidiEffectLanes(project, context, before, channel);
			for (midi::EffectSlot& slot : channel.midiSends.slots)
				slot.output = 0; // the preset's tracks aren't this project's
			if (channel.instrument != sound.instrument)
			{
				channel.sample = 0; // only the sampler plays one (#180), as with channel.set
				channel.pads.clear();
			}
			channel.instrument = sound.instrument;
			channel.instrumentParameters = sound.instrumentParameters;
			if (sound.instrument == "drumMachine") // the pads with their names and samples, as copies take them (#1361)
			{
				channel.pads = sound.pads;
				for (auto& [pad, drum] : channel.pads)
					drum.sample = copier.clip(drum.sample);
			}
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
		const Json numberSchema = {{"type", "number"}};
		const Json samplesSchema = {{"type", "integer"}}; // a position in samples
		const Json fileResult = objectSchema({{"file", stringSchema}}, {"file"});
		const Json tracksResult = objectSchema({{"tracks", {{"type", "array"}, {"items", idSchema}}}}, {"tracks"});
		const Json skippedSchema = {{"type", "array"},
			{"items",
				objectSchema(
					{{"id", idSchema}, {"name", stringSchema}, {"type", stringSchema}}, {"id", "name", "type"})}};

		engine.registerCommand(
			{"track.savePreset", 1,
				"Saves the sound of tracks as the track preset name (Save Track Preset, reference manual p. 239), "
				"replacing a preset of that name: of one track, or of several as a multi-track preset (p. 241), "
				"which keeps them in the order of the track list, whatever the order of tracks. "
				"Audio, instrument, MIDI, group and effect tracks have one: their channel's pre section, equalizer, "
				"strip, fader, pan and inserts, their MIDI modifiers, inserts, sends, volume, pan, program, bank and "
				"device, the MIDI channel (channel.set midiChannel; a MIDI track's preset keeps both its channel and "
				"its patch, of which the reference manual's keeps one, p. 238), the instrument with its parameters "
				"and a drum machine's pads, and the color. Presets are DAWproject files in the folder Track "
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
						const Project preset = load(file, false);
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
				"tracks must be as many as the preset's, and of the same types in the same order: the first of tracks "
				"gets the sound of the preset's top track, and so on. Faders above the project's fader top come down "
				"to it. A drum machine's pads come with their names and samples, whose clips come into the pool. "
				"Emits track.changed, channel.changed, plugin.removed, plugin.added, media.added and clip.added.",
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
				// The clips the preset's drum pads play come into the pool, their media files unless the project has
				// them.
				const ObjectId firstNew = project.nextId;
				TrackCopier copier(preset, project, {}, 0, nullptr);
				for (size_t i = 0; i < tracks.size(); ++i)
					applyPreset(model, project, tracks[i], preset, sources[i], copier, context);
				for (const ObjectId media : copier.addedMedia())
					context.emit("media.added", {{"media", toJson(project.media.at(media))}});
				for (auto clip = project.clips.lower_bound(firstNew); clip != project.clips.end(); ++clip)
					context.emit("clip.added", {{"clip", toJson(clip->second)}});
				return Json{{"tracks", tracks}};
			});
		model.addEdit(engine,
			{"track.addFromPreset", 1,
				"Adds the tracks of a track preset with its sound (Use Track Preset, reference manual p. 199), below "
				"track in its folder or else at the bottom of the track list. Sends to group and effect channels go "
				"to the project's tracks with the same name, outputs to its busses with the same name or else the "
				"main output. Faders and send levels above the project's fader top come down to it. Emits track.added "
				"and track.reordered. Returns the new tracks.",
				objectSchema({{"name", stringSchema}, {"track", idSchema}}, {"name"}), tracksResult, false},
			"Add tracks from preset",
			[&model, &engine](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project preset = loadPreset(engine, parameters.at("name").get<std::string>());
				const auto [parent, index] = below(project, parameters);
				const Project before = project;
				const Copied copied = copyTracks(preset, project, preset.tracks, {}, parent, index);
				model.announceChange(before, project, context);
				return Json{{"tracks", copied.tracks}};
			});

		engine.registerCommand(
			{"track.export", 1,
				"Exports tracks as a track archive (Export Selected Tracks, reference manual pp. 205-206): a "
				"DAWproject file with the tracks, a folder with the tracks inside it, their channels, events, patterns "
				"and automation, which track.import imports into other projects and other applications open as a "
				"project. "
				"The project's sample rate, tempo, time signatures, fader top and busses come along so that positions, "
				"volumes and routing mean the same. copyMedia (default true) copies the media files into the archive, "
				"otherwise it refers to them where they are. Marker and ruler tracks can be exported; tempo, "
				"signature, arranger, transpose, chord and VCA tracks not yet: listed, also inside a listed folder, "
				"they are refused, otherwise they stay behind. Changes nothing in the project. Returns the file and "
				"the tracks that stayed behind (skipped).",
				objectSchema(
					{{"tracks", idsSchema}, {"path", stringSchema}, {"copyMedia", booleanSchema}}, {"tracks", "path"}),
				objectSchema({{"file", stringSchema}, {"skipped", skippedSchema}}, {"file", "skipped"}), false},
			[&model](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const fs::path file = fs::u8path(parameters.at("path").get<std::string>());
				Json skipped;
				save(tracksFile(model.project(), ids(parameters.at("tracks")), {}, &skipped), file,
					parameters.value("copyMedia", true));
				return Json{{"file", file.u8string()}, {"skipped", std::move(skipped)}};
			});
		engine.registerCommand(
			{"track.importSources", 1,
				"The tracks of a DAWproject file that track.import can import from: a track archive (track.export) or "
				"a project. Each with its id in the file, name, type, parent (the folder it is in, or 0), whether "
				"track.import can import it (importable), and the sample rates of the media files its events play, "
				"each once from the lowest (mediaSampleRates), as media.conform converts those at another rate than "
				"the project's (Convert Sample Rate); from top to bottom. And the file's project settings as "
				"project.get has them, to compare them with the project's (Project Settings, reference manual "
				"p. 205): name, sampleRate, frameRate and startTime. Extracts no media files.",
				objectSchema({{"path", stringSchema}}, {"path"}),
				objectSchema(
					{{"tracks",
						 {{"type", "array"},
							 {"items",
								 objectSchema({{"id", idSchema}, {"name", stringSchema}, {"type", stringSchema},
												  {"parent", idSchema}, {"importable", booleanSchema},
												  {"mediaSampleRates", {{"type", "array"}, {"items", numberSchema}}}},
									 {"id", "name", "type", "parent", "importable", "mediaSampleRates"})}}},
						{"project",
							objectSchema({{"name", stringSchema}, {"sampleRate", numberSchema},
											 {"frameRate", stringSchema}, {"startTime", numberSchema}},
								{"name", "sampleRate", "frameRate", "startTime"})}},
					{"tracks", "project"}),
				true},
			[](CommandContext& context)
			{
				const Project file = load(fs::u8path(context.parameters().at("path").get<std::string>()), false);
				const std::map<ObjectId, std::set<double>> rates = mediaSampleRatesByTrack(file);
				Json tracks = Json::array();
				for (const ObjectId id : file.trackOrder())
				{
					const Track& track = file.trackById.at(id);
					std::vector<double> each;
					if (const auto found = rates.find(id); found != rates.end())
						each.assign(found->second.begin(), found->second.end());
					tracks.push_back({{"id", id}, {"name", track.name}, {"type", trackTypeName(track.type)},
						{"parent", track.parent}, {"importable", importable(track.type)}, {"mediaSampleRates", each}});
				}
				return Json{{"tracks", std::move(tracks)},
					{"project",
						{{"name", file.name}, {"sampleRate", file.sampleRate},
							{"frameRate", time::frameRateName(file.setup.frameRate)},
							{"startTime", file.setup.startTime}}}};
			});
		model.addEdit(engine,
			{"track.import", 1,
				"Imports tracks from a track archive (track.export) or a project file (Import Tracks from Project and "
				"Track Archive, reference manual pp. 200-205), in one undo step: tracks (ids in the file, see "
				"track.importSources; default: every top-level track it can import), a folder with the tracks inside "
				"it that can be imported, in the file's order. They become new tracks below track in its folder or "
				"else at the bottom of the track list (Import Position for New Tracks); marker tracks go to the top "
				"level. destinations import some of tracks onto the project's tracks of their type instead "
				"(Destination Track, p. 203), each source onto one track, not one inside a listed folder: a group, "
				"effect, folder, marker or ruler track replaces the track, with the tracks inside it, which can't be "
				"destinations themselves, and takes what played or sent into it (a marker track replaced makes room "
				"for its copy within the limit of marker tracks); new tracks below a track inside it go below it "
				"instead; an audio, instrument or MIDI track gives the track its events and parts as existing says (On "
				"Import on Existing Tracks: replace replaces the track's, add adds them, version, the default, puts "
				"them into a new version, which the tracks imported onto get together), with its other versions after "
				"the track's, its settings and its automation, and the track keeps its name and buttons, all in the "
				"track's time base. The version IDs of the file become new ones, shared by the versions that shared "
				"one there, on new tracks too; the new version of a track imported onto gets the ID its source's "
				"active version has, so that the tracks imported onto get one ID when their sources played versions "
				"that shared one, and a version with the name of one of the track's gets a name of its own. events "
				"(default true) brings their events, parts, patterns, markers and track versions, settings (default "
				"true) "
				"their channel settings and inserts, automation (default true) their automation, replacing the "
				"track's; without automation, a track that events or settings are imported onto loses its automation "
				"(p. 204). position (Import Position) puts the events where they were in the file's timecode "
				"(absolute, the default: moved by the difference of the project start times), as far from the project "
				"start as from the file's (relative), or as far from the cursor (cursor); refused if they would start "
				"before the project start. range {start, end} imports only what starts from start up to end, in "
				"samples at the project's sample rate from the file's start (Range). "
				"The project's length grows to the end of what is imported, up to 24 hours (p. 204). The buttons "
				"(mute, solo, listen, monitor, record enable) of new tracks are off and automation reads, as these "
				"states aren't imported (p. 200). Routing between the imported tracks stays; sends and outputs to "
				"other channels go to the tracks the project had with the same name and type, outputs to busses to its "
				"busses with the same name or else the main output. Linear positions from a file at another sample "
				"rate are rescaled to keep their time, as project.set does; faders, send levels and volume automation "
				"above the project's fader top come down to it. Media files embedded in the file are extracted next to "
				"it, as project.load does; media.prepareArchive and media.conform with the media it returns copy and "
				"convert them into the project folder (Media Files, p. 205). Tempo, signature, arranger, transpose, "
				"chord and VCA tracks can't be imported yet: listed, also inside a listed folder, they are refused, "
				"otherwise they stay behind. Emits the events of what changed, as redoing it would. Returns the new "
				"tracks, also those that replace tracks, the tracks of the file that stayed behind (skipped), from top "
				"to bottom, and the media files it added (media).",
				objectSchema(
					{{"path", stringSchema}, {"tracks", idsSchema}, {"track", idSchema}, {"events", booleanSchema},
						{"settings", booleanSchema}, {"automation", booleanSchema},
						{"destinations",
							{{"type", "array"},
								{"items",
									objectSchema({{"source", idSchema}, {"track", idSchema}}, {"source", "track"})}}},
						{"existing", {{"type", "string"}, {"enum", {"replace", "add", "version"}}}},
						{"position", {{"type", "string"}, {"enum", {"absolute", "relative", "cursor"}}}},
						{"range", objectSchema({{"start", samplesSchema}, {"end", samplesSchema}}, {"start", "end"})}},
					{"path"}),
				objectSchema({{"tracks", {{"type", "array"}, {"items", idSchema}}}, {"skipped", skippedSchema},
								 {"media", {{"type", "array"}, {"items", idSchema}}}},
					{"tracks", "skipped", "media"}),
				false},
			"Import tracks",
			[&model](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				std::optional<std::pair<time::Samples, time::Samples>> range;
				if (parameters.contains("range"))
				{
					range.emplace(parameters.at("range").at("start").get<time::Samples>(),
						parameters.at("range").at("end").get<time::Samples>());
					require(range->first < range->second, "range must end after its start");
				}
				Project file = load(fs::u8path(parameters.at("path").get<std::string>()));
				const Json none = Json::object();
				CommandContext inner(none); // for the parts of the import, which announces what changed at the end
				if (file.sampleRate != project.sampleRate) // linear positions are in samples: keep their time
					changeSampleRate(file, project.sampleRate, inner);
				const std::string position = parameters.value("position", "absolute");
				time::Samples offset = 0;
				if (position == "absolute")
					offset = std::llround((file.setup.startTime - project.setup.startTime) * project.sampleRate);
				else if (position == "cursor")
					offset = context.call("transport.get").at("position").get<time::Samples>();
				std::vector<ObjectId> tracks;
				std::vector<ObjectId> skipped;
				if (parameters.contains("tracks"))
					tracks = ids(parameters.at("tracks"));
				else
					for (const ObjectId id : file.tracks)
						(importable(file.trackById.at(id).type) ? tracks : skipped).push_back(id);
				const CopyOptions options{parameters.value("events", true), parameters.value("settings", true),
					parameters.value("automation", true)};
				std::set<ObjectId> brought; // the tracks of the file that are copied, as copyTracks copies them
				for (const ObjectId id : file.trackOrder())
				{
					const Track& track = file.trackById.at(id);
					if (std::find(tracks.begin(), tracks.end(), id) != tracks.end() ||
						(brought.count(track.parent) != 0 && importable(track.type)))
						brought.insert(id);
				}
				place(file, project, offset, range, brought, options);
				// A track replaced goes with the tracks inside it: none of them may be a destination, whatever the
				// order, and the new tracks go below the outermost one replaced instead of into it. A marker track
				// replaced makes room for the one that replaces it (p. 201).
				const auto merges = [](TrackType type)
				{ return type == TrackType::Audio || type == TrackType::Instrument || type == TrackType::Midi; };
				const Json destinations = parameters.value("destinations", Json::array());
				std::set<ObjectId> replaced;
				std::ptrdiff_t replacedMarkers = 0;
				for (const Json& entry : destinations)
				{
					const auto track = project.trackById.find(entry.at("track").get<ObjectId>());
					if (track != project.trackById.end() && !merges(track->second.type) &&
						replaced.insert(track->first).second && track->second.type == TrackType::Marker)
						++replacedMarkers;
				}
				const auto replacing = [&project, &replaced](ObjectId id) // the outermost replaced track id is in, or 0
				{
					ObjectId outer = 0;
					for (; id != 0; id = project.trackById.at(id).parent)
						if (replaced.count(id) != 0)
							outer = id;
					return outer;
				};
				const auto named = [&project](ObjectId id)
				{ return "track " + std::to_string(id) + " (" + project.trackById.at(id).name + ")"; };
				for (const Json& entry : destinations)
				{
					const auto track = project.trackById.find(entry.at("track").get<ObjectId>());
					if (track == project.trackById.end())
						continue; // refused below
					if (const ObjectId outer = replacing(track->second.parent); outer != 0)
						throw CommandError(
							named(track->first) + " is inside a track that another one replaces: " + named(outer));
				}
				auto [parent, index] = below(project, parameters);
				if (const ObjectId folder = replacing(parent); folder != 0)
				{
					parent = project.trackById.at(folder).parent;
					const std::vector<ObjectId>& list =
						parent == 0 ? project.tracks : project.trackById.at(parent).children;
					index = static_cast<size_t>(std::find(list.begin(), list.end(), folder) - list.begin()) + 1;
				}
				const Project before = project;
				VersionIds versionIds(project);
				Copied copied = copyTracks(file, project, tracks, options, parent, index, replacedMarkers, &versionIds);
				// The project's end moves after what the import brings (the note on Import Position, p. 204).
				std::set<ObjectId> copies;
				for (const auto& [source, copy] : copied.copies)
					copies.insert(copy);
				const double end = static_cast<double>(contentEnd(project, copies)) / project.sampleRate;
				if (end > project.setup.length)
					project.setup.length = std::min(end, g_maxProjectLength);
				Existing existing = Existing::Version;
				if (parameters.value("existing", "version") == "replace")
					existing = Existing::Replace;
				else if (parameters.value("existing", "version") == "add")
					existing = Existing::Add;
				struct Destination
				{
					ObjectId copy = 0;
					ObjectId track = 0;
					bool merge = false;
					int version = 0; // the ID the track's new version gets: its source's active version's here
				};
				const bool newVersions = options.events && existing == Existing::Version;
				std::vector<Destination> onto;
				std::vector<ObjectId> merged;
				std::set<ObjectId> sources;
				for (const Json& entry : destinations)
				{
					const auto source = entry.at("source").get<ObjectId>();
					const auto id = entry.at("track").get<ObjectId>();
					require(std::find(tracks.begin(), tracks.end(), source) != tracks.end(),
						"track " + std::to_string(source) + " of the file isn't among the tracks imported");
					const auto track = before.trackById.find(id);
					require(track != before.trackById.end(), "no track " + std::to_string(id));
					const TrackType type = file.trackById.at(source).type;
					require(track->second.type == type,
						"track " + std::to_string(id) + " is not a " + trackTypeName(type) +
							" track: tracks are imported onto tracks of their type");
					const auto twice = [id](const Destination& other) { return other.track == id; };
					require(std::none_of(onto.begin(), onto.end(), twice),
						"track " + std::to_string(id) + " is a destination twice");
					require(sources.insert(source).second,
						"track " + std::to_string(source) + " of the file is imported onto two tracks");
					const ObjectId copy = copied.copies.at(source);
					require(std::find(copied.tracks.begin(), copied.tracks.end(), copy) != copied.tracks.end(),
						"track " + std::to_string(source) +
							" of the file is inside a folder that is imported: it comes along with the folder");
					const bool merge = merges(type);
					onto.push_back({copy, id, merge,
						merge && newVersions ? versionIds.of(file.trackById.at(source).activeVersion) : 0});
					if (merge)
						merged.push_back(id);
				}
				// The tracks imported onto get their new versions together, each with the ID its source's active
				// version has here, so that the versions that shared an ID in the file share one (p. 233): one ID when
				// the tracks played versions that shared one there.
				if (newVersions && !merged.empty())
				{
					model.runEdit("trackVersion.add", project, {{"tracks", merged}}, inner);
					for (const Destination& destination : onto)
						if (destination.merge)
						{
							Track& track = project.trackById.at(destination.track);
							track.versions.back().id = destination.version; // the new version, which is active
							track.activeVersion = destination.version;
						}
				}
				for (const Destination& destination : onto)
				{
					if (destination.merge)
					{
						copied.tracks.erase(std::find(copied.tracks.begin(), copied.tracks.end(), destination.copy));
						mergeTrack(model, project, destination.copy, destination.track, options, existing, inner);
					}
					else
					{
						replaceTrack(model, project, destination.copy, destination.track, inner);
					}
				}
				model.announceChange(before, project, context);
				skipped.insert(skipped.end(), copied.skipped.begin(), copied.skipped.end());
				return Json{{"tracks", std::move(copied.tracks)}, {"skipped", describe(file, skipped)},
					{"media", std::move(copied.media)}};
			});
	}
}
