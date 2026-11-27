// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI editors' common framework (#126, reference manual pp. 1132-1156): which parts the lower zone's editor and
// the editor window show, the part active for editing, the part editing mode, Keep Editor Contents and the global
// tracks shown above the notes; and the key editor's (#127, pp. 1189-1220): the selected notes and step and MIDI input,
// which editor.input applies; and whether an editor shows the key editor or the list editor (#130, pp. 1243-1258),
// whose selection also has controller events; and the audio part editor's (#146, pp. 692-702), whose parts are audio
// parts and whose selection is audio events. The commands keep the views on the command thread, outside the project,
// so they are no undo steps and don't make the project modified; editor.input's notes are.
#include "engine/document/editors.h"

#include "engine/document/drumtrack.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/document/trackvisibility.h"
#include "engine/engine.h"
#include "engine/midi/controllerlanes.h"
#include "engine/midi/drummaps.h"
#include "engine/midi/quantizesettings.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		// The in-place editors' names (#131): inPlace:<track>, one per MIDI or instrument track.
		constexpr const char* g_inPlace = "inPlace:";
		// The part editing modes (p. 1143): what selecting and editing reach.
		constexpr std::array<const char*, 3> g_editingModes = {"allParts", "activePart", "activeTrack"};
		// What an editor shows its parts in: MIDI parts the key editor (#127), the list editor (#130), the drum
		// editor (#129) or the pattern editor (#181), audio parts the audio part editor (#146).
		constexpr std::array<const char*, 5> g_contents = {"key", "list", "drum", "pattern", "audioPart"};

		struct EditorView
		{
			bool open = false;
			uint64_t project = 0; // the project it belongs to (ProjectModel::openedRevision)
			std::vector<ObjectId> parts;
			// Whether editor.changed last announced it open, and its parts: parts keeps removed ones, and an in-place
			// editor stays open with its removed track, as they come back with an undo.
			bool announcedOpen = false;
			std::vector<ObjectId> announced;
			ObjectId activePart = 0;
			std::string editingMode = "allParts";
			bool keepContents = false;
			std::vector<ObjectId> globalTracks;
			// The key editor (#127): the selected notes, each a part and a note of it, and step and MIDI input.
			std::vector<std::pair<ObjectId, ObjectId>> selection;
			bool stepInput = false;
			bool midiInput = false;
			time::Ticks stepPosition = 0; // the step input cursor, project ticks
			bool moveInsert = false;
			bool recordPitch = true;
			bool recordVelocity = true;
			bool recordReleaseVelocity = false;
			bool keepNotes = false;
			double insertVelocity = 100.0 / 127.0;
			std::string lengthQuantize =
				"link"; // a note value, link: the quantize grid's, or drumMap: the sound's snap
			std::string content = "key";
			// The drum editor's (#882): Pitch Visibility On/Off and its options Reverse Drum Sound List and, with
			// visibleSounds withEvents, Show Drum Sounds with Events (#1493, pp. 1223-1224, 1234), Show Note Length
			// On/Off (p. 1227) and Solo Instrument (#1493, p. 1223).
			bool pitchVisibility = false;
			bool reverseSounds = false;
			std::string visibleSounds = "all";
			bool soloInstrument = false;
			bool showNoteLength = false;
			std::string gridType = "quantize"; // the drum editor's grid (p. 1227): quantize, zoom (Adapt to Zoom), or
											   // drumMap: the sounds' snap
			// The controller lanes (#128): each a type (midi::g_laneTypes) and a controller number or pitch.
			std::vector<std::pair<std::string, int>> lanes = {{"velocity", 0}};
			bool showLanes = true;
			// The Articulations lane's (#1580, pp. 1287-1288): Show/Hide Sound Slots Lane, and the groups of the lane's
			// map collapsed into a row each, by index.
			bool soundSlotsLane = true;
			std::set<int> collapsedGroups;
			bool autoSelectControllers = false;
			bool acousticFeedback = false;                                  // Acoustic Feedback (pp. 1192, 1259)
			std::vector<std::pair<ObjectId, ObjectId>> controllerSelection; // each a part and a controller event of it
			// The attributes selected of selected notes on the Articulations lane (#1688, p. 1289), by part and note;
			// a selected note without them has all its attributes selected.
			std::map<std::pair<ObjectId, ObjectId>, std::vector<std::string>> selectedArticulations;
			// The audio part editor (#146): the selected audio events of its parts.
			std::vector<ObjectId> eventSelection;
			// Per-note expression in the key editor (#1512): Show Note Expression Data, Double-Click Opens Note
			// Expression Editor and the expression editor's parameter (pp. 1303, 1308-1309).
			bool showExpression = false;
			bool expressionDoubleClick = false;
			std::string expressionType = "tuning";
		};

		using Views = std::map<std::string, EditorView>;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		// The global tracks of the reference manual, p. 1144, that GNUbase has.
		bool isGlobal(const Project& project, ObjectId id)
		{
			const auto found = project.trackById.find(id);
			if (found == project.trackById.end())
				return false;
			switch (found->second.type)
			{
			case TrackType::Tempo:
			case TrackType::Signature:
			case TrackType::Chord:
			case TrackType::Ruler:
			case TrackType::Arranger:
			case TrackType::Marker:
			case TrackType::Transpose:
				return true;
			default:
				return false;
			}
		}

		bool isPart(const Project& project, ObjectId id)
		{
			return project.midiParts.count(id) != 0 || project.audioParts.count(id) != 0;
		}

		// A MIDI part's or an audio part's track and position.
		std::pair<ObjectId, time::Position> placeOf(const Project& project, ObjectId part)
		{
			if (const auto audio = project.audioParts.find(part); audio != project.audioParts.end())
				return {audio->second.track, audio->second.position};
			const MidiPart& midi = project.midiParts.at(part);
			return {midi.track, midi.position};
		}

		bool showsAudio(const EditorView& view)
		{
			return view.content == "audioPart";
		}

		// The parts that still exist, once each, in the track list's order and on each track by position.
		std::vector<ObjectId> ordered(const Project& project, std::vector<ObjectId> parts)
		{
			std::map<ObjectId, size_t> index;
			const std::vector<ObjectId> order = project.trackOrder();
			for (size_t i = 0; i < order.size(); ++i)
				index[order[i]] = i;
			parts.erase(
				std::remove_if(parts.begin(), parts.end(), [&project](ObjectId id) { return !isPart(project, id); }),
				parts.end());
			const auto key = [&](ObjectId id)
			{
				const auto [track, position] = placeOf(project, id);
				return std::make_tuple(index[track], position.value, id);
			};
			std::sort(parts.begin(), parts.end(), [&](ObjectId a, ObjectId b) { return key(a) < key(b); });
			parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
			return parts;
		}

		ObjectId trackOf(const Project& project, ObjectId part)
		{
			return part == 0 ? 0 : placeOf(project, part).first;
		}

		// The parts that selecting and editing reach in the editing mode (p. 1197).
		std::vector<ObjectId> editableParts(const Project& project, const EditorView& view)
		{
			const ObjectId activeTrack = trackOf(project, view.activePart);
			std::vector<ObjectId> editable;
			for (const ObjectId part : view.parts)
				if (view.editingMode == "allParts" || (view.editingMode == "activePart" && part == view.activePart) ||
					(view.editingMode == "activeTrack" && trackOf(project, part) == activeTrack))
					editable.push_back(part);
			return editable;
		}

		// Leaves only the selected notes and controller events that still exist in the editable parts.
		template <typename Event> void dropUneditable(const Project& project, const std::vector<ObjectId>& editable,
			std::vector<std::pair<ObjectId, ObjectId>>& selection, std::map<ObjectId, Event> MidiPart::* events)
		{
			selection.erase(
				std::remove_if(selection.begin(), selection.end(),
					[&](const std::pair<ObjectId, ObjectId>& selected)
					{
						const auto part = project.midiParts.find(selected.first);
						return std::find(editable.begin(), editable.end(), selected.first) == editable.end() ||
							part == project.midiParts.end() || (part->second.*events).count(selected.second) == 0;
					}),
				selection.end());
		}

		// The audio part an audio event is in, if the view can edit it; 0 for none.
		ObjectId editablePartOf(const Project& project, const std::vector<ObjectId>& editable, ObjectId event)
		{
			const auto found = project.audioEvents.find(event);
			return found != project.audioEvents.end() &&
					std::find(editable.begin(), editable.end(), found->second.part) != editable.end()
				? found->second.part
				: 0;
		}

		Json soundsInUse(const Project& project, ObjectId track);

		void dropUneditable(const Project& project, EditorView& view)
		{
			const std::vector<ObjectId> editable = editableParts(project, view);
			dropUneditable(project, editable, view.selection, &MidiPart::notes);
			// Show Drum Sounds in Use by Instrument hides the notes of the other sounds (p. 1234), which leave the
			// selection: neither the editor's edits nor Solo Instrument take them.
			if (view.content == "drum" && view.pitchVisibility && view.visibleSounds == "inUse")
				if (const Json inUse = soundsInUse(project, trackOf(project, view.activePart));
					inUse.is_array() && !inUse.empty())
					view.selection.erase(
						std::remove_if(view.selection.begin(), view.selection.end(),
							[&](const std::pair<ObjectId, ObjectId>& selected)
							{
								const int pitch = project.midiParts.at(selected.first).notes.at(selected.second).pitch;
								return std::find(inUse.begin(), inUse.end(), Json(pitch)) == inUse.end();
							}),
						view.selection.end());
			dropUneditable(project, editable, view.controllerSelection, &MidiPart::controllers);
			// The attributes selected keep only those the note still has (#1762): a note whose attributes selected all
			// went, as Delete on the lane removes them, is no longer selected.
			for (auto each = view.selectedArticulations.begin(); each != view.selectedArticulations.end();)
			{
				const auto selected = std::find(view.selection.begin(), view.selection.end(), each->first);
				if (selected != view.selection.end())
				{
					const std::vector<std::string>& has =
						project.midiParts.at(each->first.first).notes.at(each->first.second).articulations;
					std::vector<std::string>& names = each->second;
					names.erase(std::remove_if(names.begin(), names.end(), [&has](const std::string& name)
									{ return std::find(has.begin(), has.end(), name) == has.end(); }),
						names.end());
					if (!names.empty())
					{
						++each;
						continue;
					}
					view.selection.erase(selected);
				}
				each = view.selectedArticulations.erase(each);
			}
			view.eventSelection.erase(
				std::remove_if(view.eventSelection.begin(), view.eventSelection.end(),
					[&](ObjectId event) { return editablePartOf(project, editable, event) == 0; }),
				view.eventSelection.end());
		}

		bool isNoteTrack(const Project& project, ObjectId id)
		{
			const auto found = project.trackById.find(id);
			return found != project.trackById.end() &&
				(found->second.type == TrackType::Midi || found->second.type == TrackType::Instrument);
		}

		void requireNoteTrack(const Project& project, ObjectId track)
		{
			require(project.trackById.count(track) != 0, "no track " + std::to_string(track));
			require(
				isNoteTrack(project, track), "track " + std::to_string(track) + " isn't a MIDI or instrument track");
		}

		// The track of an in-place editor's name; 0 for the lower zone's editor and the editor window.
		ObjectId inPlaceTrack(const std::string& editor)
		{
			const std::string prefix = g_inPlace;
			return editor.rfind(prefix, 0) == 0 ? std::stoull(editor.substr(prefix.size())) : 0;
		}

		std::string inPlaceName(ObjectId track)
		{
			return g_inPlace + std::to_string(track);
		}

		// lower, window, or inPlace: and a MIDI or instrument track's id: digits of a 64-bit number, so up to 20 since
		// track ids come from the project file (#1159), and without leading zeros, so that a track's editor has one
		// name.
		void requireEditor(const Project& project, const std::string& editor)
		{
			if (editor == "lower" || editor == "window")
				return;
			const std::string prefix = g_inPlace;
			const char* digits = editor.data() + std::min(editor.size(), prefix.size());
			const char* end = editor.data() + editor.size();
			ObjectId track = 0;
			const std::from_chars_result parsed = std::from_chars(digits, end, track);
			require(editor.rfind(prefix, 0) == 0 && digits != end && parsed.ec == std::errc() && parsed.ptr == end &&
					editor == inPlaceName(track),
				"editor must be lower, window or inPlace:<track>, not " + editor);
			requireNoteTrack(project, track);
		}

		// Every MIDI part of a track, in order.
		std::vector<ObjectId> midiPartsOf(const Project& project, ObjectId track)
		{
			std::vector<ObjectId> parts;
			for (const auto& [id, part] : project.midiParts)
				if (part.track == track)
					parts.push_back(id);
			return ordered(project, parts);
		}

		// The editor's view now: closed and empty once another project is open, without what is gone since. An open
		// in-place editor shows every MIDI part of its track, and closes with its track.
		EditorView current(const Views& views, const ProjectModel& model, const std::string& editor)
		{
			const auto found = views.find(editor);
			EditorView view = found != views.end() ? found->second : EditorView();
			if (view.project != model.openedRevision())
				view = EditorView();
			const Project& project = model.project();
			if (const ObjectId track = inPlaceTrack(editor); track != 0 && view.open)
			{
				if (isNoteTrack(project, track))
					view.parts = midiPartsOf(project, track);
				else
					view = EditorView();
			}
			view.parts = ordered(project, view.parts);
			if (std::find(view.parts.begin(), view.parts.end(), view.activePart) == view.parts.end())
				view.activePart = view.parts.empty() ? 0 : view.parts.front();
			view.globalTracks.erase(std::remove_if(view.globalTracks.begin(), view.globalTracks.end(),
										[&project](ObjectId id) { return !isGlobal(project, id); }),
				view.globalTracks.end());
			dropUneditable(project, view);
			return view;
		}

		// Show Drum Sounds in Use by Instrument (#1493, p. 1234): the pitches of the track's drum sounds whose pads its
		// instrument plays, the built-in drum machine of a drum track or of the instrument track a MIDI track plays to,
		// through the track's drum map's O-notes; null for an instrument that can't tell.
		Json soundsInUse(const Project& project, ObjectId track)
		{
			const Channel* drums = drumChannelOf(project, track);
			if (const auto found = project.trackById.find(track); drums == nullptr && found != project.trackById.end())
				if (const auto own = project.channels.find(found->second.channel);
					own != project.channels.end() && own->second.format == ChannelFormat::None)
					if (const auto output = project.channels.find(own->second.output);
						output != project.channels.end() && output->second.instrument == "drumMachine")
						drums = &output->second;
			if (drums == nullptr)
				return nullptr;
			const std::vector<int> pads = playingDrumPads(*drums);
			const DrumMap* map = midi::drumMapOf(project, track);
			Json pitches = Json::array();
			for (int pitch = 0; pitch < g_drumSounds; ++pitch)
				if (std::binary_search(pads.begin(), pads.end(),
						map != nullptr ? int{map->sounds.at(static_cast<size_t>(pitch)).oNote} : pitch))
					pitches.push_back(pitch);
			return pitches;
		}

		// The names of the track's drum sounds by pitch (p. 1239), as the editors show them: its drum map's, else with
		// the GM Default name list General MIDI's; null for neither.
		Json soundNames(const Project& project, ObjectId track)
		{
			static const DrumMap gm = midi::gmDrumMap();
			const DrumMap* map = midi::drumMapOf(project, track);
			if (map == nullptr)
			{
				const auto found = project.trackById.find(track);
				if (found == project.trackById.end() || !found->second.gmNames)
					return nullptr;
				map = &gm;
			}
			Json names = Json::array();
			for (const DrumSound& sound : map->sounds)
				names.push_back(sound.name);
			return names;
		}

		Json viewJson(const Project& project, const std::string& editor, const EditorView& view)
		{
			const ObjectId activeTrack = trackOf(project, view.activePart);
			const std::vector<ObjectId> editable = editableParts(project, view);
			Json selection = Json::array();
			for (const auto& [part, note] : view.selection)
			{
				selection.push_back({{"part", part}, {"note", note}});
				if (const auto found = view.selectedArticulations.find({part, note});
					found != view.selectedArticulations.end())
					selection.back()["articulations"] = found->second;
			}
			Json lanes = Json::array();
			for (const auto& [type, number] : view.lanes)
				lanes.push_back({{"type", type}, {"number", number}});
			Json controllerSelection = Json::array();
			for (const auto& [part, controller] : view.controllerSelection)
				controllerSelection.push_back({{"part", part}, {"controller", controller}});
			return {{"editor", editor}, {"open", view.open}, {"parts", view.parts}, {"activePart", view.activePart},
				{"activeTrack", activeTrack}, {"editable", editable}, {"editingMode", view.editingMode},
				{"keepContents", view.keepContents}, {"globalTracks", view.globalTracks}, {"selection", selection},
				{"stepInput", view.stepInput}, {"midiInput", view.midiInput}, {"stepPosition", view.stepPosition},
				{"moveInsert", view.moveInsert}, {"recordPitch", view.recordPitch},
				{"recordVelocity", view.recordVelocity}, {"recordReleaseVelocity", view.recordReleaseVelocity},
				{"keepNotes", view.keepNotes}, {"insertVelocity", view.insertVelocity},
				{"lengthQuantize", view.lengthQuantize}, {"content", view.content},
				{"pitchVisibility", view.pitchVisibility}, {"reverseSounds", view.reverseSounds},
				{"visibleSounds", view.visibleSounds}, {"soundsInUse", soundsInUse(project, activeTrack)},
				{"soundNames", soundNames(project, activeTrack)}, {"soloInstrument", view.soloInstrument},
				{"showNoteLength", view.showNoteLength}, {"gridType", view.gridType}, {"lanes", lanes},
				{"showLanes", view.showLanes}, {"soundSlotsLane", view.soundSlotsLane},
				{"collapsedGroups", view.collapsedGroups}, {"autoSelectControllers", view.autoSelectControllers},
				{"controllerSelection", controllerSelection}, {"eventSelection", view.eventSelection},
				{"showExpression", view.showExpression}, {"expressionDoubleClick", view.expressionDoubleClick},
				{"expressionType", view.expressionType}, {"acousticFeedback", view.acousticFeedback}};
		}

		// The controller events in the ranges of the selected notes, from a note's start to its end (p. 1188).
		std::vector<std::pair<ObjectId, ObjectId>> controllersOfNotes(const Project& project, const EditorView& view)
		{
			std::vector<std::pair<ObjectId, ObjectId>> found;
			for (const auto& [partId, noteId] : view.selection)
			{
				const MidiPart& part = project.midiParts.at(partId);
				const Note& note = part.notes.at(noteId);
				for (const auto& [id, controller] : part.controllers)
					if (controller.position >= note.position && controller.position < note.position + note.length &&
						std::find(found.begin(), found.end(), std::make_pair(partId, id)) == found.end())
						found.emplace_back(partId, id);
			}
			return found;
		}

		// The MIDI parts, or the audio parts, of a track, or of every track in a folder.
		void collectParts(const Project& project, ObjectId track, bool audio, std::vector<ObjectId>& parts)
		{
			if (audio)
			{
				for (const auto& [id, part] : project.audioParts)
					if (part.track == track)
						parts.push_back(id);
			}
			else
			{
				for (const auto& [id, part] : project.midiParts)
					if (part.track == track)
						parts.push_back(id);
			}
			for (const ObjectId child : project.trackById.at(track).children)
				collectParts(project, child, audio, parts);
		}

		std::vector<ObjectId> partsOf(const Project& project, ObjectId track, bool audio)
		{
			require(project.trackById.count(track) != 0, "no track " + std::to_string(track));
			std::vector<ObjectId> parts;
			collectParts(project, track, audio, parts);
			require(
				!parts.empty(), "track " + std::to_string(track) + " has no " + (audio ? "audio" : "MIDI") + " parts");
			return ordered(project, parts);
		}

		// The view closed: no parts, Keep Editor Contents off (p. 1153).
		EditorView closed(EditorView view)
		{
			view.open = false;
			view.parts.clear();
			view.activePart = 0;
			view.keepContents = false;
			view.selection.clear();
			view.controllerSelection.clear();
			view.eventSelection.clear();
			view.content = "key";
			return view;
		}

		// Solo Instrument (p. 1223): the selected notes of the open drum editors that have it on, as editor.get has
		// them now. The render snapshot solos them on the tracks with a drum map, also one assigned after they were
		// selected. current() never opens a view or turns on the drum editor or Solo Instrument, so the stored view
		// rules out the others before current() costs a copy, the parts ordered and the selection filtered on every
		// edit.
		std::vector<std::pair<ObjectId, ObjectId>> soloNotes(const Views& views, const ProjectModel& model)
		{
			std::vector<std::pair<ObjectId, ObjectId>> notes;
			for (const auto& [name, stored] : views)
				if (stored.open && stored.content == "drum" && stored.soloInstrument)
					if (const EditorView view = current(views, model, name);
						view.open && view.content == "drum" && view.soloInstrument)
						notes.insert(notes.end(), view.selection.begin(), view.selection.end());
			std::sort(notes.begin(), notes.end());
			notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
			return notes;
		}

		// The tracks whose in-place editors are open, in the track list's order.
		Json openInPlace(const Views& views, const ProjectModel& model)
		{
			Json tracks = Json::array();
			for (const ObjectId track : model.project().trackOrder())
				if (views.count(inPlaceName(track)) != 0 && current(views, model, inPlaceName(track)).open)
					tracks.push_back(track);
			return tracks;
		}

		std::vector<ObjectId> ids(const Json& parameters, const char* name)
		{
			return parameters.contains(name) ? parameters.at(name).get<std::vector<ObjectId>>()
											 : std::vector<ObjectId>();
		}

		template <typename T> bool contains(const std::vector<T>& items, const T& item)
		{
			return std::find(items.begin(), items.end(), item) != items.end();
		}

		// The Visibility tab can't show a track hidden in the project window, or a track or folder that holds locked
		// events or parts (p. 1151).
		void requireShowable(const Project& project, ObjectId track, const std::vector<ObjectId>& hidden)
		{
			const std::string name = "track " + std::to_string(track);
			require(project.trackById.count(track) != 0, "no " + name);
			require(!contains(hidden, track), name + " is hidden in the project window");
			const auto inside = [&project, track](ObjectId id) // the track, or a track inside it
			{
				for (auto found = project.trackById.find(id); found != project.trackById.end();
					found = project.trackById.find(found->second.parent))
					if (found->first == track)
						return true;
				return false;
			};
			const auto locked = [&inside](const auto& items)
			{
				return std::any_of(items.begin(), items.end(),
					[&inside](const auto& item) { return item.second.lock && inside(item.second.track); });
			};
			require(!locked(project.midiParts) && !locked(project.audioParts) && !locked(project.audioEvents),
				name + " holds locked events or parts");
		}

		std::vector<ObjectId> checkedParts(const Project& project, const std::vector<ObjectId>& parts)
		{
			for (const ObjectId id : parts)
				require(isPart(project, id), "no MIDI part or audio part " + std::to_string(id));
			const auto audio = [&project](ObjectId id) { return project.audioParts.count(id) != 0; };
			require(std::all_of(parts.begin(), parts.end(), audio) || std::none_of(parts.begin(), parts.end(), audio),
				"parts must be all MIDI parts or all audio parts");
			return ordered(project, parts);
		}

		// Audio parts show in the audio part editor, MIDI parts in the key editor or the list editor.
		void requireContent(const Project& project, const EditorView& view)
		{
			require(std::all_of(view.parts.begin(), view.parts.end(),
						[&](ObjectId part) { return (project.audioParts.count(part) != 0) == showsAudio(view); }),
				"audio parts show in the audio part editor (content audioPart), MIDI parts in the key editor, the "
				"list editor or the drum editor");
		}

		time::Ticks partStart(const Project& project, const MidiPart& part)
		{
			return time::toTicks(part.position, project.tempo, project.sampleRate);
		}

		time::Ticks partEnd(const Project& project, const MidiPart& part)
		{
			return time::toTicks(time::Position{part.position.base, part.position.value + part.length}, project.tempo,
				project.sampleRate);
		}

		// The length of new notes of a pitch on a track: the length quantize's, with link the quantize grid's
		// (p. 1219), with drumMap the snap of the pitch's sound in the track's drum map (Drum-Map Link, p. 1240), or
		// without a map the quantize grid's.
		time::Ticks noteLength(const Project& project, const EditorView& view, ObjectId track, uint8_t pitch)
		{
			if (view.lengthQuantize == "drumMap")
			{
				const DrumMap* map = midi::drumMapOf(project, track);
				return map != nullptr ? map->sounds.at(pitch).snap.ticks() : project.quantize.grid.ticks();
			}
			if (view.lengthQuantize == "link")
				return project.quantize.grid.ticks();
			return midi::noteValueFromString(view.lengthQuantize).value_or(project.quantize.grid).ticks();
		}

		uint16_t velocityValue(double value)
		{
			return static_cast<uint16_t>(std::lround(value * g_maxVelocity));
		}

		// A note played into editor.input.
		struct Played
		{
			uint8_t pitch = 60;
			double velocity = 0.0;
			double releaseVelocity = 0.5;
		};

		std::vector<Played> playedNotes(const Json& items, const EditorView& view)
		{
			std::vector<Played> played;
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Json& item = items[i];
				const std::string name = "notes[" + std::to_string(i) + "]";
				const auto pitch = item.at("pitch").get<int64_t>();
				require(pitch >= 0 && pitch <= 127, name + ".pitch must be 0 to 127");
				const Played note{static_cast<uint8_t>(pitch), item.value("velocity", view.insertVelocity),
					item.value("releaseVelocity", 0.5)};
				require(note.velocity >= 0.0 && note.velocity <= 1.0 && note.releaseVelocity >= 0.0 &&
						note.releaseVelocity <= 1.0,
					name + ": velocities must be 0 to 1");
				played.push_back(note);
			}
			require(!played.empty(), "notes must have at least one note");
			std::sort(played.begin(), played.end(), [](const Played& a, const Played& b) { return a.pitch < b.pitch; });
			return played;
		}

		// A new note of a part from a played one, with the velocities the view records.
		Note newNote(Project& project, const EditorView& view, const Played& played, time::Ticks position,
			time::Ticks length, uint8_t channel)
		{
			Note note;
			note.id = project.newId();
			note.position = position;
			note.length = length;
			note.pitch = played.pitch;
			note.channel = channel;
			note.velocity = velocityValue(view.recordVelocity ? played.velocity : view.insertVelocity);
			if (view.recordReleaseVelocity)
				note.releaseVelocity = velocityValue(played.releaseVelocity);
			return note;
		}

		// Whether notes played on the MIDI inputs go into the editor: its step or MIDI input is on.
		bool takesInput(const EditorView& view)
		{
			return view.open && (view.stepInput || view.midiInput) && !showsAudio(view);
		}

		// Notes played on the MIDI inputs into the key editors (editor.played, #679): the keys down, and the notes of
		// the chord they play, which goes in once they are all up.
		struct LiveInput
		{
			bool listening = false; // midi.activityListen is on for the key editors
			std::string editor;     // the editor whose step or MIDI input went on last
			std::set<int> down;
			Json chord = Json::array();
		};

		// The editor that notes played go into: the one whose step or MIDI input went on last, else the first that
		// takes input; empty for none.
		std::string inputEditor(const LiveInput& live, const Views& views, const ProjectModel& model)
		{
			if (!live.editor.empty() && takesInput(current(views, model, live.editor)))
				return live.editor;
			for (const auto& each : views)
				if (takesInput(current(views, model, each.first)))
					return each.first;
			return {};
		}

		// Listens to the MIDI inputs (midi.activityListen) while an editor takes input; each start begins afresh.
		void listen(LiveInput& live, bool on, CommandContext& context)
		{
			if (on == live.listening)
				return;
			live.listening = on;
			live.down.clear();
			live.chord = Json::array();
			context.call("midi.activityListen", {{"on", on}, {"listener", "keyEditor"}, {"post", "editor.played"}});
		}
	}

	void registerEditorCommands(ProjectModel& model, Engine& engine)
	{
		auto views = std::make_shared<Views>();
		auto live = std::make_shared<LiveInput>();
		model.setSoloNotesSource([views, &model] { return soloNotes(*views, model); });
		// A change of the project that changes an open editor's parts (removes or adds them, brings them back or
		// orders them anew) or closes it, as a new project does, announces the editor's view, so that clients that
		// follow events learn of it (#625).
		model.setChangeFollower(
			[views, &model](CommandContext& context)
			{
				for (auto& [editor, stored] : *views)
				{
					if (!stored.open)
						continue;
					const EditorView view = current(*views, model, editor);
					if (view.open == stored.announcedOpen && view.parts == stored.announced)
						continue;
					if (stored.project != model.openedRevision())
						stored = view; // another project
					stored.announcedOpen = view.open;
					stored.announced = view.parts;
					context.emit("editor.changed", viewJson(model.project(), editor, view));
				}
			});
		// The names, so that MCP clients and Lua see them: lower, window or inPlace:<track> (#131). At most inPlace:
		// and the 20 digits of a 64-bit track id, so that the pattern never has to match a long string (#1059, #1159).
		const Json editorSchema = {
			{"type", "string"}, {"pattern", "^(lower|window|inPlace:[0-9]+)$"}, {"maxLength", 28}};
		const Json modeSchema = {
			{"type", "string"}, {"enum", std::vector<std::string>(g_editingModes.begin(), g_editingModes.end())}};
		const Json idsSchema = {{"type", "array"}, {"items", {{"type", "integer"}}}};
		const Json idSchema = {{"type", "integer"}};
		const Json boolean = {{"type", "boolean"}};
		const Json gridTypeSchema = {{"type", "string"}, {"enum", {"quantize", "zoom", "drumMap"}}};
		const Json visibleSoundsSchema = {{"type", "string"}, {"enum", {"all", "withEvents", "inUse"}}};
		const Json expressionTypeSchema = {
			{"type", "string"}, {"enum", std::vector<std::string>(g_expressionNames.begin(), g_expressionNames.end())}};
		const Json selectionSchema = {{"type", "array"},
			{"items",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"part", "note"}},
					{"properties",
						{{"part", idSchema}, {"note", idSchema},
							{"articulations",
								{{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 1}}}}}}}};
		const Json groupsSchema = {{"type", "array"}, {"items", {{"type", "integer"}, {"minimum", 0}}}};
		const Json lanesSchema = {{"type", "array"},
			{"items",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"type"}},
					{"properties",
						{{"type",
							 {{"type", "string"},
								 {"enum",
									 std::vector<std::string>(midi::g_laneTypes.begin(), midi::g_laneTypes.end())}}},
							{"number", idSchema}}}}}};
		const Json contentSchema = {
			{"type", "string"}, {"enum", std::vector<std::string>(g_contents.begin(), g_contents.end())}};
		const Json controllerSelectionSchema = {{"type", "array"},
			{"items",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"part", "controller"}},
					{"properties", {{"part", idSchema}, {"controller", idSchema}}}}}};
		const Json keyEditorSchema = {{"content", contentSchema}, {"controllerSelection", controllerSelectionSchema},
			{"eventSelection", idsSchema}, {"selection", selectionSchema}, {"stepInput", boolean},
			{"midiInput", boolean}, {"stepPosition", idSchema}, {"moveInsert", boolean}, {"recordPitch", boolean},
			{"recordVelocity", boolean}, {"recordReleaseVelocity", boolean}, {"keepNotes", boolean},
			{"insertVelocity", {{"type", "number"}}}, {"lengthQuantize", {{"type", "string"}}}, {"lanes", lanesSchema},
			{"showLanes", boolean}, {"soundSlotsLane", boolean}, {"collapsedGroups", groupsSchema},
			{"autoSelectControllers", boolean}, {"pitchVisibility", boolean}, {"reverseSounds", boolean},
			{"visibleSounds", visibleSoundsSchema}, {"soloInstrument", boolean}, {"showNoteLength", boolean},
			{"gridType", gridTypeSchema}, {"showExpression", boolean}, {"expressionDoubleClick", boolean},
			{"expressionType", expressionTypeSchema}, {"acousticFeedback", boolean}};
		Json viewSchema = {{"type", "object"},
			{"properties",
				{{"editor", editorSchema}, {"open", {{"type", "boolean"}}}, {"parts", idsSchema},
					{"activePart", idSchema}, {"activeTrack", idSchema}, {"editable", idsSchema},
					{"editingMode", modeSchema}, {"keepContents", {{"type", "boolean"}}}, {"globalTracks", idsSchema}}},
			{"required",
				{"editor", "open", "parts", "activePart", "activeTrack", "editable", "editingMode", "keepContents",
					"globalTracks", "selection", "stepInput", "midiInput", "stepPosition", "moveInsert", "recordPitch",
					"recordVelocity", "recordReleaseVelocity", "keepNotes", "insertVelocity", "lengthQuantize",
					"content", "pitchVisibility", "reverseSounds", "visibleSounds", "soundsInUse", "soundNames",
					"soloInstrument", "showNoteLength", "gridType", "lanes", "showLanes", "soundSlotsLane",
					"collapsedGroups", "autoSelectControllers", "controllerSelection", "eventSelection",
					"showExpression", "expressionDoubleClick", "expressionType", "acousticFeedback"}}};
		viewSchema["properties"].update(keyEditorSchema);
		viewSchema["properties"]["soundsInUse"] = {{"type", {"array", "null"}}, {"items", idSchema}};
		viewSchema["properties"]["soundNames"] = {
			{"type", {"array", "null"}}, {"items", {{"type", "string"}}}, {"minItems", 128}, {"maxItems", 128}};
		const std::string viewText =
			"editor: lower (the editor in the project window's lower zone), window (the editor window) or "
			"inPlace:<track>, the in-place editor of a MIDI or instrument track (editor.inPlace), which shows every "
			"MIDI part of its track in the key editor. The view: "
			"open; parts, the MIDI parts or audio parts it shows, in the track list's order and by position; "
			"activePart, the part "
			"active for editing, and activeTrack, its track (0 for none); editingMode, what selecting and editing "
			"reach: allParts, activePart or activeTrack (the active part's track); editable, the parts that reach; "
			"keepContents, whether it keeps its parts when the project window's selection changes; globalTracks, the "
			"global tracks (tempo, signature, chord, ruler, arranger, marker and transpose tracks) the editor window "
			"shows above its events. The key editor's (pp. 1189-1220): "
			"selection, the selected notes, each {part, note}, of the editable parts, with articulations, the "
			"attributes selected of it on the Articulations lane, where not all are: some of the note's own, and a "
			"note whose attributes selected all go is no longer selected; stepInput and midiInput, what "
			"editor.input does with notes played; stepPosition, the step input cursor in project ticks; moveInsert, "
			"recordPitch, recordVelocity, recordReleaseVelocity and keepNotes, how they go in; insertVelocity (0-1), "
			"the velocity of drawn notes and of input that doesn't record it; lengthQuantize, the length of new notes, "
			"a note value such as 1/16, 1/8T or 1/4., link for the quantize grid's (quantize.set), or drumMap for the "
			"snap of the note's sound in its track's drum map (drumMap.add), without one the quantize grid's; notes "
			"played into a track with a drum map take the pitch of the sound whose iNote they are. content: what "
			"shows the parts, key (the key editor), list (the list editor, pp. 1243-1258), drum (the drum editor, "
			"pp. 1221-1242) or pattern (the pattern editor, pp. 895-937: the active part's pattern). The drum "
			"editor's: pitchVisibility (Pitch Visibility On/Off) applies its options reverseSounds, the drum sound "
			"list from the bottom, and visibleSounds, all, withEvents for only the sounds that the parts have notes "
			"of, or inUse for only soundsInUse (pp. 1223-1224, 1234): the pitches of the active track's drum sounds "
			"(through its drum map's O-notes) whose pads its instrument plays, the built-in drum machine of a drum "
			"track (drum.create) or of the instrument track a MIDI track plays to, null for an instrument that can't "
			"tell; with inUse the selection leaves out the notes of the sounds it hides. soundNames: the names of the "
			"active track's drum sounds by pitch (p. 1239), its drum map's, else with its GM Default name list "
			"(track.set gmNames) General MIDI's, else null; the editors show them. soloInstrument (Solo Instrument, "
			"p. 1223) plays, of the tracks with a drum map "
			"that its selected notes are on, only the notes of the selected notes' sounds, while it is open with "
			"content drum, also of the sounds played in through the I-notes (midi.inject without drumSounds), while "
			"auditions with drumSounds still play every sound; showNoteLength shows its notes as boxes of their "
			"length, not "
			"diamonds (p. 1227); gridType, what its notes snap to (p. 1227): quantize, the quantize grid, zoom, a note "
			"value that the editor's zoom chooses (Adapt to Zoom), or drumMap, the snap of each note's sound in the "
			"track's drum map; "
			"controllerSelection, the "
			"list editor's selected controller events, each {part, controller}, of the editable parts. Audio parts "
			"show in the audio part editor (content audioPart, pp. 692-702), whose eventSelection is the selected "
			"audio events of the editable parts. The controller lanes' (pp. "
			"1157-1188): lanes, from the top, each {type, number}: velocity, offVelocity, probability or "
			"velocityVariance (the notes' play probability and velocity variance), or the controller events of a "
			"type: controller (number is the controller), pitchBend, channelPressure, polyPressure (number is the "
			"pitch) or program, or articulations (the directions and attributes of the track's articulation map, "
			"articulationMap.add, pp. 1287-1289); showLanes, whether the controller display shows them; "
			"soundSlotsLane, whether the Articulations lane shows its sound slots lane, and collapsedGroups, the "
			"groups of its map, by index, that it shows collapsed into one row each (pp. 1287-1288); "
			"autoSelectControllers, "
			"whether selecting notes selects the controller events from their start to their end. Per-note "
			"expression (pp. 1303-1313): showExpression shows the notes' expression over them, expressionDoubleClick "
			"makes a double click on a note open the expression editor instead of deleting it, and expressionType is "
			"the expression it edits: volume, pan, tuning, vibrato, expression, brightness or pressure. "
			"acousticFeedback: the editor plays a note while it is moved or drawn, and a note whose velocity the "
			"velocity lane edits (pp. 1192, 1259). View state: no "
			"undo steps, not saved, and a new or loaded project closes the editors. An edit, undo or redo that "
			"changes an open editor's parts (removes, adds, brings back or reorders them) or closes it, and a "
			"project that closes it, emit editor.changed with its view.";
		const auto editorOnly = [&](Json properties = Json::object())
		{
			properties["editor"] = editorSchema;
			return Json{{"type", "object"}, {"properties", properties}, {"required", {"editor"}},
				{"additionalProperties", false}};
		};
		const auto store = [views, live, &model](
							   const std::string& editor, const EditorView& view, CommandContext& context)
		{
			EditorView& stored = (*views)[editor];
			const bool took = stored.project == model.openedRevision() && takesInput(stored);
			stored = view;
			stored.project = model.openedRevision();
			dropUneditable(model.project(), stored);
			stored.announcedOpen = stored.open;
			stored.announced = ordered(model.project(), stored.parts);
			Json result = viewJson(model.project(), editor, stored);
			if (takesInput(stored) && !took)
				live->editor = editor;
			listen(*live, !inputEditor(*live, *views, model).empty(), context);
			model.updateSoloNotes();
			context.emit("editor.changed", result);
			return result;
		};

		engine.registerCommand({"editor.get", 1, "What an editor shows. " + viewText, editorOnly(), viewSchema, true},
			[views, &model](CommandContext& context)
			{
				const std::string editor = context.parameters().at("editor").get<std::string>();
				requireEditor(model.project(), editor);
				return viewJson(model.project(), editor, current(*views, model, editor));
			});

		Json openSchema = editorOnly({{"parts", {{"type", "array"}, {"items", idSchema}, {"minItems", 1}}},
			{"activePart", idSchema}, {"content", contentSchema}});
		openSchema["required"] = {"editor", "parts"};
		engine.registerCommand(
			{"editor.open", 1,
				"Opens an editor with MIDI parts or audio parts, such as the ones selected in the project window, or "
				"shows those parts in it if it is open. activePart becomes the part active for editing, by default "
				"the first. content shows MIDI parts in the key editor, the list editor, the drum editor or the "
				"pattern editor; an open editor keeps its content unless it is given, a closed one opens with the "
				"pattern editor if the first part is a pattern event (midiPart.add pattern), else with the drum "
				"editor if its track has a drum map and the user setting editing.useDrumEditor is on, else with the "
				"key editor. Audio parts open in the audio "
				"part editor (audioPart). Emits editor.changed. " +
					viewText,
				openSchema, viewSchema, false},
			[views, &model, store](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const std::string editor = parameters.at("editor").get<std::string>();
				const Project& project = model.project();
				requireEditor(project, editor);
				require(inPlaceTrack(editor) == 0, "in-place editors open with editor.inPlace");
				EditorView view = current(*views, model, editor);
				view.parts = checkedParts(project, ids(parameters, "parts"));
				require(!view.parts.empty(), "parts must name at least one part");
				view.activePart = parameters.value("activePart", view.parts.front());
				require(contains(view.parts, view.activePart),
					"the active part " + std::to_string(view.activePart) + " isn't one of the parts");
				const bool opens = !view.open;
				view.open = true;
				if (parameters.contains("content"))
					view.content = parameters.at("content").get<std::string>();
				else if (project.audioParts.count(view.parts.front()) != 0)
					view.content = "audioPart";
				else if (const auto first = project.midiParts.find(view.parts.front());
					opens && first != project.midiParts.end() && first->second.pattern != 0)
					view.content = "pattern";
				else if (opens && midi::drumMapOf(project, trackOf(project, view.parts.front())) != nullptr &&
					context.call("settings.get", {{"key", "editing.useDrumEditor"}}).at("value").get<bool>())
					view.content = "drum"; // Use Drum Editor when Drum Map is assigned (pp. 1221, 1603)
				else if (opens || showsAudio(view))
					view.content = "key"; // also over a content that editor.set gave the closed editor
				requireContent(project, view);
				return store(editor, view, context);
			});

		engine.registerCommand(
			{"editor.set", 1,
				"Changes what an open editor shows (reference manual pp. 1142-1156); what isn't given stays. "
				"parts replaces its parts. Then the Visibility tab's changes: exclusive hides every track first, "
				"showTracks shows every MIDI part of these tracks (all tracks in a folder), in the audio part editor "
				"(content audioPart, also one given with it) every audio part, and activates the last; tracks hidden "
				"in the project window (trackVisibility.set) and tracks or folders that hold locked events or parts "
				"can't show, and a folder leaves out its hidden tracks. "
				"hideTracks hides theirs; at least one part stays. activeTrack activates the track's first part shown, "
				"activePart a part shown. editingMode (an in-place editor's allParts or activePart), keepContents and "
				"globalTracks (tempo, signature, chord, ruler, arranger, marker and transpose tracks, the editor "
				"window only) also apply to a closed editor. "
				"selection replaces the selected notes, of the "
				"editable MIDI parts, controllerSelection the selected controller events and eventSelection the "
				"selected "
				"audio events of the editable audio parts. content and the key editor's "
				"settings apply to any editor; turning stepInput on puts the step input cursor at the active part's "
				"start unless stepPosition is given. lanes replaces the controller lanes. With autoSelectControllers "
				"on, a new selection of notes, or turning it on, selects the controller events in the notes' ranges "
				"unless controllerSelection is given. Emits editor.changed. " +
					viewText,
				editorOnly({{"parts", idsSchema}, {"exclusive", {{"type", "boolean"}}}, {"showTracks", idsSchema},
					{"hideTracks", idsSchema}, {"activeTrack", idSchema}, {"activePart", idSchema},
					{"editingMode", modeSchema}, {"keepContents", {{"type", "boolean"}}}, {"globalTracks", idsSchema},
					{"selection", selectionSchema}, {"stepInput", boolean}, {"midiInput", boolean},
					{"stepPosition", idSchema}, {"moveInsert", boolean}, {"recordPitch", boolean},
					{"recordVelocity", boolean}, {"recordReleaseVelocity", boolean}, {"keepNotes", boolean},
					{"insertVelocity", {{"type", "number"}}}, {"lengthQuantize", {{"type", "string"}}},
					{"content", contentSchema}, {"pitchVisibility", boolean}, {"reverseSounds", boolean},
					{"visibleSounds", visibleSoundsSchema}, {"soloInstrument", boolean}, {"showNoteLength", boolean},
					{"gridType", gridTypeSchema}, {"lanes", lanesSchema}, {"showLanes", boolean},
					{"soundSlotsLane", boolean}, {"collapsedGroups", groupsSchema}, {"autoSelectControllers", boolean},
					{"controllerSelection", controllerSelectionSchema}, {"eventSelection", idsSchema},
					{"showExpression", boolean}, {"expressionDoubleClick", boolean},
					{"expressionType", expressionTypeSchema}, {"acousticFeedback", boolean}}),
				viewSchema, false},
			[views, &model, store](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project& project = model.project();
				const std::string editor = parameters.at("editor").get<std::string>();
				requireEditor(project, editor);
				require(inPlaceTrack(editor) == 0 ||
						(!parameters.contains("parts") && !parameters.contains("exclusive") &&
							!parameters.contains("showTracks") && !parameters.contains("hideTracks")),
					"the in-place editor shows its track's parts");
				require(inPlaceTrack(editor) == 0 || parameters.value("content", std::string("key")) == "key",
					"the in-place editor shows the key editor");
				// It shows one track, so its Part Editing Mode menu has All Parts and Active Part (p. 1260).
				require(inPlaceTrack(editor) == 0 || parameters.value("editingMode", std::string()) != "activeTrack",
					"the in-place editor shows one track: its editingMode is allParts or activePart");
				EditorView view = current(*views, model, editor);
				const bool changesParts = parameters.contains("parts") || parameters.contains("exclusive") ||
					parameters.contains("showTracks") || parameters.contains("hideTracks") ||
					parameters.contains("activeTrack") || parameters.contains("activePart");
				require(!changesParts || view.open, "the " + editor + " editor isn't open");
				if (parameters.contains("parts"))
					view.parts = checkedParts(project, ids(parameters, "parts"));
				if (parameters.value("exclusive", false))
					view.parts.clear();
				// The tracks show the parts of the content given with them.
				if (parameters.contains("content"))
					view.content = parameters.at("content").get<std::string>();
				const bool audio = showsAudio(view);
				const std::vector<ObjectId> hidden = hiddenTracks(project);
				for (const ObjectId track : ids(parameters, "showTracks"))
				{
					requireShowable(project, track, hidden);
					std::vector<ObjectId> shown = partsOf(project, track, audio);
					shown.erase(std::remove_if(shown.begin(), shown.end(),
									[&](ObjectId part) { return contains(hidden, trackOf(project, part)); }),
						shown.end());
					require(!shown.empty(), "track " + std::to_string(track) + " has no parts that can show");
					view.parts.insert(view.parts.end(), shown.begin(), shown.end());
					view.activePart = shown.front(); // showing a track activates it (p. 1151)
				}
				for (const ObjectId track : ids(parameters, "hideTracks"))
					for (const ObjectId part : partsOf(project, track, audio))
						view.parts.erase(std::remove(view.parts.begin(), view.parts.end(), part), view.parts.end());
				view.parts = ordered(project, view.parts);
				require(!changesParts || !view.parts.empty(), "at least one track must stay shown");
				if (parameters.contains("activeTrack"))
				{
					const auto track = parameters.at("activeTrack").get<ObjectId>();
					const auto first = std::find_if(view.parts.begin(), view.parts.end(),
						[&](ObjectId part) { return trackOf(project, part) == track; });
					require(first != view.parts.end(), "the editor shows no part of track " + std::to_string(track));
					view.activePart = *first;
				}
				if (parameters.contains("activePart"))
				{
					view.activePart = parameters.at("activePart").get<ObjectId>();
					require(contains(view.parts, view.activePart),
						"the editor doesn't show part " + std::to_string(view.activePart));
				}
				if (!contains(view.parts, view.activePart))
					view.activePart = view.parts.empty() ? 0 : view.parts.front();
				if (parameters.contains("editingMode"))
					view.editingMode = parameters.at("editingMode").get<std::string>();
				if (parameters.contains("keepContents"))
					view.keepContents = parameters.at("keepContents").get<bool>();
				if (parameters.contains("globalTracks"))
				{
					require(editor == "window", "only the editor window shows global tracks");
					const std::vector<ObjectId> given = ids(parameters, "globalTracks");
					for (const ObjectId track : given)
						require(isGlobal(project, track), "track " + std::to_string(track) + " isn't a global track");
					view.globalTracks.clear();
					for (const ObjectId track : project.trackOrder()) // in the track list's order
						if (contains(given, track))
							view.globalTracks.push_back(track);
				}
				// The selected notes or controller events, each once, of the editable parts.
				const auto select = [&](const char* name, const std::string& key, auto events,
										std::vector<std::pair<ObjectId, ObjectId>>& selection)
				{
					if (!parameters.contains(name))
						return;
					const std::vector<ObjectId> editable = editableParts(project, view);
					selection.clear();
					for (const Json& item : parameters.at(name))
					{
						const auto part = item.at("part").get<ObjectId>();
						const auto event = item.at(key).get<ObjectId>();
						require(contains(editable, part),
							"part " + std::to_string(part) + " isn't editable in the " + editor + " editor");
						require(
							project.midiParts.count(part) != 0, "part " + std::to_string(part) + " isn't a MIDI part");
						require((project.midiParts.at(part).*events).count(event) != 0,
							"part " + std::to_string(part) + " has no " + key + " " + std::to_string(event));
						if (!contains(selection, std::make_pair(part, event)))
							selection.emplace_back(part, event);
					}
				};
				select("selection", "note", &MidiPart::notes, view.selection);
				// The attributes selected of a note: some of its own, each once (#1762, #1799); the schema refuses
				// none.
				if (parameters.contains("selection"))
				{
					view.selectedArticulations.clear();
					for (const Json& item : parameters.at("selection"))
						if (item.contains("articulations"))
						{
							const auto part = item.at("part").get<ObjectId>();
							const auto note = item.at("note").get<ObjectId>();
							const std::vector<std::string>& has =
								project.midiParts.at(part).notes.at(note).articulations;
							std::vector<std::string>& names = view.selectedArticulations[{part, note}];
							for (const std::string& name : item.at("articulations").get<std::vector<std::string>>())
							{
								require(std::find(has.begin(), has.end(), name) != has.end(),
									"note " + std::to_string(note) + " has no articulation " + name);
								if (!contains(names, name))
									names.push_back(name);
							}
						}
				}
				select("controllerSelection", "controller", &MidiPart::controllers, view.controllerSelection);
				if (parameters.contains("eventSelection"))
				{
					const std::vector<ObjectId> editable = editableParts(project, view);
					view.eventSelection.clear();
					for (const ObjectId event : ids(parameters, "eventSelection"))
					{
						require(editablePartOf(project, editable, event) != 0,
							"audio event " + std::to_string(event) + " isn't in an editable part of the " + editor +
								" editor");
						if (!contains(view.eventSelection, event))
							view.eventSelection.push_back(event);
					}
				}
				requireContent(project, view);
				const bool stepTurnsOn = parameters.value("stepInput", view.stepInput) && !view.stepInput;
				for (const auto& [name, flag] : {std::pair<const char*, bool*>{"stepInput", &view.stepInput},
						 {"midiInput", &view.midiInput}, {"moveInsert", &view.moveInsert},
						 {"recordPitch", &view.recordPitch}, {"recordVelocity", &view.recordVelocity},
						 {"recordReleaseVelocity", &view.recordReleaseVelocity}, {"keepNotes", &view.keepNotes}})
					if (parameters.contains(name))
						*flag = parameters.at(name).get<bool>();
				if (stepTurnsOn && project.midiParts.count(view.activePart) != 0)
					view.stepPosition = partStart(project, project.midiParts.at(view.activePart));
				if (parameters.contains("stepPosition"))
				{
					view.stepPosition = parameters.at("stepPosition").get<time::Ticks>();
					require(view.stepPosition >= 0, "stepPosition must not be negative");
				}
				if (parameters.contains("insertVelocity"))
				{
					view.insertVelocity = parameters.at("insertVelocity").get<double>();
					require(view.insertVelocity >= 0.0 && view.insertVelocity <= 1.0, "insertVelocity must be 0 to 1");
				}
				if (parameters.contains("lengthQuantize"))
				{
					const std::string length = parameters.at("lengthQuantize").get<std::string>();
					require(length == "link" || length == "drumMap" || midi::noteValueFromString(length).has_value(),
						"lengthQuantize must be a note value such as 1/16, 1/8T or 1/4., link or drumMap");
					view.lengthQuantize = length;
				}
				if (parameters.contains("lanes"))
				{
					view.lanes.clear();
					const Json& lanes = parameters.at("lanes");
					for (size_t i = 0; i < lanes.size(); ++i)
					{
						const std::string name = "lanes[" + std::to_string(i) + "]";
						const std::string type = lanes[i].at("type").get<std::string>();
						require(midi::isLaneType(type), name + ".type must be a lane type, not " += type);
						const int number = lanes[i].value("number", 0);
						require(number >= 0 && number <= 127, name + ".number must be 0 to 127");
						view.lanes.emplace_back(type, number);
					}
				}
				if (parameters.contains("showLanes"))
					view.showLanes = parameters.at("showLanes").get<bool>();
				view.soundSlotsLane = parameters.value("soundSlotsLane", view.soundSlotsLane);
				if (parameters.contains("collapsedGroups"))
					view.collapsedGroups = parameters.at("collapsedGroups").get<std::set<int>>();
				view.pitchVisibility = parameters.value("pitchVisibility", view.pitchVisibility);
				view.reverseSounds = parameters.value("reverseSounds", view.reverseSounds);
				view.visibleSounds = parameters.value("visibleSounds", view.visibleSounds);
				view.soloInstrument = parameters.value("soloInstrument", view.soloInstrument);
				view.showNoteLength = parameters.value("showNoteLength", view.showNoteLength);
				view.gridType = parameters.value("gridType", view.gridType);
				view.showExpression = parameters.value("showExpression", view.showExpression);
				view.expressionDoubleClick = parameters.value("expressionDoubleClick", view.expressionDoubleClick);
				view.expressionType = parameters.value("expressionType", view.expressionType);
				view.acousticFeedback = parameters.value("acousticFeedback", view.acousticFeedback);
				const bool autoSelectTurnsOn = parameters.value("autoSelectControllers", view.autoSelectControllers) &&
					!view.autoSelectControllers;
				if (parameters.contains("autoSelectControllers"))
					view.autoSelectControllers = parameters.at("autoSelectControllers").get<bool>();
				if (!parameters.contains("controllerSelection") && view.autoSelectControllers &&
					(parameters.contains("selection") || autoSelectTurnsOn))
				{
					view.controllerSelection = controllersOfNotes(project, view);
				}
				return store(editor, view, context);
			});

		engine.registerCommand(
			{"editor.close", 1,
				"Closes an editor: it shows no parts, and Keep Editor Contents goes off (p. 1153). Emits "
				"editor.changed. " +
					viewText,
				editorOnly(), viewSchema, false},
			[views, &model, store](CommandContext& context)
			{
				const std::string editor = context.parameters().at("editor").get<std::string>();
				requireEditor(model.project(), editor);
				return store(editor, closed(current(*views, model, editor)), context);
			});

		engine.registerCommand(
			{"editor.inPlace", 1,
				"Opens or closes the in-place editors of MIDI and instrument tracks (reference manual pp. "
				"1259-1261): with open true it opens them, false closes them, and without it each one toggles, as "
				"Edit In-Place does. An in-place editor is the editor inPlace:<track> of editor.get, editor.set, "
				"editor.input and editor.close: the key editor inside the track in the project window, showing every "
				"MIDI part of the track, the first one active; its parts follow the track's and can't be set, and it "
				"closes with its track. View state like the other editors: no undo steps, not saved, and a new or "
				"loaded project closes them. Emits editor.changed for each editor that opens or closes. Without "
				"tracks it changes nothing and stays out of engine.commandLog. Returns tracks, the tracks whose "
				"in-place editors are open, in the track "
				"list's order.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties", {{"tracks", idsSchema}, {"open", boolean}}}},
				{{"type", "object"}, {"required", {"tracks"}}, {"properties", {{"tracks", idsSchema}}}}, false},
			[views, &model, store](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				if (!parameters.contains("tracks"))
					context.leaveOutOfLog();
				const Project& project = model.project();
				std::vector<ObjectId> tracks;
				for (const ObjectId track : ids(parameters, "tracks"))
				{
					requireNoteTrack(project, track);
					if (!contains(tracks, track))
						tracks.push_back(track);
				}
				for (const ObjectId track : tracks)
				{
					const std::string editor = inPlaceName(track);
					EditorView view = current(*views, model, editor);
					if (parameters.value("open", !view.open) == view.open)
						continue;
					if (view.open)
					{
						view = closed(view);
					}
					else
					{
						view.open = true;
						view.content = "key";
						view.parts = midiPartsOf(project, track);
						view.activePart = view.parts.empty() ? 0 : view.parts.front();
					}
					store(editor, view, context);
				}
				return Json{{"tracks", openInPlace(*views, model)}};
			});

		Json inputSchema = editorOnly({{"notes",
			{{"type", "array"}, {"minItems", 1},
				{"items",
					{{"type", "object"}, {"additionalProperties", false}, {"required", {"pitch"}},
						{"properties",
							{{"pitch", idSchema}, {"velocity", {{"type", "number"}}},
								{"releaseVelocity", {{"type", "number"}}}}}}}}}});
		inputSchema["required"] = {"editor", "notes"};
		model.addEdit(engine,
			{"editor.input", 1,
				"Notes played into a key editor, such as a chord from a MIDI keyboard (reference manual pp. "
				"1219-1220): "
				"each a pitch 0-127, velocity 0-1 (default the editor's insertVelocity) and releaseVelocity 0-1 "
				"(default 0.5). With stepInput, they go into the active part at the step input cursor as notes of the "
				"length quantize's length, ending at the part's end at the latest, on the MIDI channel of the part's "
				"last note before the cursor that stays (else its first after it, else channel 0), and the cursor "
				"moves on by a step of the quantize grid; moveInsert first moves the part's notes from the cursor on "
				"by a step, or by the longest inserted note where that is longer, and without keepNotes the notes "
				"that start within the step go. With midiInput (and stepInput off), the first selected note takes the "
				"lowest "
				"note's pitch, velocity and release velocity, as far as recordPitch, recordVelocity and "
				"recordReleaseVelocity say, and the other notes of a chord are added with its position and length; "
				"with keepNotes, all of them are added and the selected note stays. Then the next note of its part "
				"is selected. Velocities that aren't recorded are insertVelocity's and the default release velocity. "
				"One undo step. Returns the part and its notes added or changed.",
				inputSchema,
				{{"type", "object"}, {"required", {"part", "notes"}},
					{"properties", {{"part", idSchema}, {"notes", {{"type", "array"}}}}}},
				false},
			"Note input",
			[views, &model, store](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const std::string editor = parameters.at("editor").get<std::string>();
				requireEditor(project, editor);
				EditorView view = current(*views, model, editor);
				require(view.open, "the " + editor + " editor isn't open");
				require(!showsAudio(view), "the audio part editor takes no notes");
				require(view.stepInput || view.midiInput, "step input and MIDI input are off");
				const std::vector<Played> played = playedNotes(parameters.at("notes"), view);
				Json added = Json::array();
				Json changed = Json::array();
				ObjectId partId = 0;
				if (view.stepInput)
				{
					partId = view.activePart;
					require(partId != 0, "the editor has no active part");
					MidiPart& part = project.midiParts.at(partId);
					const time::Ticks start = partStart(project, part);
					const time::Ticks at = view.stepPosition - start;
					require(at >= 0 && view.stepPosition < partEnd(project, part),
						"the step input cursor isn't within the active part");
					const time::Ticks step = project.quantize.grid.ticks();
					const time::Ticks end = partEnd(project, part) - start;
					// The I-notes (p. 1239), and lengths that end at the part's end at the latest.
					std::vector<std::pair<Played, time::Ticks>> inserted;
					time::Ticks longest = step;
					for (Played each : played)
					{
						each.pitch = midi::inputPitch(project, part.track, each.pitch);
						const time::Ticks length =
							std::min(noteLength(project, view, part.track, each.pitch), end - at);
						inserted.emplace_back(each, length);
						longest = std::max(longest, length);
					}
					std::vector<ObjectId> removed;
					const Note* kept = nullptr; // the last note that stays before the cursor, else the first after
					const auto nearer = [at](const Note& a, const Note& b)
					{
						if ((a.position < at) != (b.position < at))
							return a.position < at;
						return a.position < at ? a.position > b.position : a.position < b.position;
					};
					for (auto& [id, note] : part.notes)
						if (!view.moveInsert && !view.keepNotes && note.position >= at && note.position < at + step)
							removed.push_back(id);
						else if (kept == nullptr || nearer(note, *kept))
							kept = &note;
					// The new notes' channel: the kept note's, or in an empty part channel 1, as drawn notes'.
					const uint8_t channel = kept != nullptr ? kept->channel : uint8_t{0};
					// Move Insert Mode makes room for the inserted notes: a step, or the longest of them.
					for (auto& [id, note] : part.notes)
						if (view.moveInsert && note.position >= at)
						{
							note.position += longest;
							changed.push_back(toJson(note));
						}
					for (const ObjectId id : removed)
						part.notes.erase(id);
					if (!removed.empty())
						context.emit("note.removed", {{"part", partId}, {"notes", removed}});
					view.selection.clear();
					for (const auto& [each, length] : inserted)
					{
						const Note note = newNote(project, view, each, at, length, channel);
						part.notes[note.id] = note;
						added.push_back(toJson(note));
						view.selection.emplace_back(partId, note.id);
					}
					view.stepPosition += step;
				}
				else
				{
					require(!view.selection.empty(), "MIDI input needs a selected note");
					// The first selected note, by position, then pitch.
					auto first = view.selection.front();
					const auto key = [&](const std::pair<ObjectId, ObjectId>& selected)
					{
						const Note& note = project.midiParts.at(selected.first).notes.at(selected.second);
						return std::make_tuple(partStart(project, project.midiParts.at(selected.first)) + note.position,
							note.pitch, selected.second);
					};
					for (const auto& selected : view.selection)
						if (key(selected) < key(first))
							first = selected;
					partId = first.first;
					MidiPart& part = project.midiParts.at(partId);
					Note& target = part.notes.at(first.second);
					const time::Ticks at = target.position;
					const uint8_t pitch = target.pitch;
					std::vector<Played> extra = played;
					for (Played& each : extra) // the I-notes of the track's drum map (p. 1239)
						each.pitch = midi::inputPitch(project, part.track, each.pitch);
					if (!view.keepNotes)
					{
						const Played& lowest = extra.front();
						if (view.recordPitch)
							target.pitch = lowest.pitch;
						if (view.recordVelocity)
							target.velocity = velocityValue(lowest.velocity);
						if (view.recordReleaseVelocity)
							target.releaseVelocity = velocityValue(lowest.releaseVelocity);
						changed.push_back(toJson(target));
						extra.erase(extra.begin());
					}
					const Note shape = target;
					// The next note of the part, before the new ones come.
					std::optional<std::tuple<time::Ticks, uint8_t, ObjectId>> next;
					for (const auto& [id, note] : part.notes)
						if (id != shape.id &&
							std::make_tuple(note.position, note.pitch, id) > std::make_tuple(at, pitch, shape.id) &&
							(!next || std::make_tuple(note.position, note.pitch, id) < *next))
							next = std::make_tuple(note.position, note.pitch, id);
					for (const Played& each : extra)
					{
						const Note note = newNote(project, view, each, shape.position, shape.length, shape.channel);
						part.notes[note.id] = note;
						added.push_back(toJson(note));
					}
					if (next)
						view.selection = {{partId, std::get<2>(*next)}};
				}
				if (!changed.empty())
					context.emit("note.changed", {{"part", partId}, {"notes", changed}});
				if (!added.empty())
					context.emit("note.added", {{"part", partId}, {"notes", added}});
				store(editor, view, context);
				Json notes = changed;
				notes.insert(notes.end(), added.begin(), added.end());
				return Json{{"part", partId}, {"notes", notes}};
			});

		const Json bytesSchema = {{"type", "array"}, {"items", {{"type", "integer"}}}};
		engine.registerCommand(
			{"editor.played", 1,
				"MIDI input played into the key editors' step and MIDI input (reference manual pp. 1219-1220), as "
				"midi.activity takes it: the engine runs it with what the MIDI inputs receive while an editor's step "
				"or MIDI input is on (midi.activityListen with post). received: messages, each with its bytes and "
				"whether its port belongs to all MIDI inputs (inAllInputs, by default true); only those of ports "
				"that do count, and only their notes. The keys pressed until all of them are up again are one chord: "
				"then editor.input runs with its notes, each with its note-on velocity and its note-off velocity "
				"(0.5 for a note-on of velocity 0), in the editor whose step or MIDI input went on last, or else the "
				"first one with it on. Returns the chords it sent, each the notes as editor.input takes them.",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"received"}},
					{"properties",
						{{"received",
							{{"type", "array"},
								{"items",
									{{"type", "object"}, {"required", {"bytes"}},
										{"properties", {{"bytes", bytesSchema}, {"inAllInputs", boolean}}}}}}}}}},
				{{"type", "object"}, {"required", {"chords"}}, {"properties", {{"chords", {{"type", "array"}}}}}},
				false},
			[views, live, &model, &engine](CommandContext& context)
			{
				context.leaveOutOfLog();
				const std::string editor = inputEditor(*live, *views, model);
				listen(*live, !editor.empty(), context);
				Json chords = Json::array();
				for (const Json& each : context.parameters().at("received"))
				{
					const Json& bytes = each.at("bytes");
					if (editor.empty() || !each.value("inAllInputs", true) || bytes.size() < 3)
						continue;
					const int status = bytes.at(0).get<int>() & 0xF0;
					const int pitch = bytes.at(1).get<int>() & 0x7F;
					const int velocity = bytes.at(2).get<int>() & 0x7F;
					if (status == 0x90 && velocity > 0)
					{
						const bool fresh = std::none_of(live->chord.begin(), live->chord.end(),
							[pitch](const Json& note) { return note.at("pitch") == pitch; });
						live->down.insert(pitch);
						if (fresh)
							live->chord.push_back({{"pitch", pitch}, {"velocity", velocity / 127.0}});
					}
					else if ((status == 0x80 || status == 0x90) && live->down.erase(pitch) != 0)
					{
						for (Json& note : live->chord)
							if (note.at("pitch") == pitch)
								note["releaseVelocity"] = status == 0x80 ? velocity / 127.0 : 0.5;
						if (live->down.empty())
						{
							engine.post("editor.input", {{"editor", editor}, {"notes", live->chord}});
							chords.push_back(live->chord);
							live->chord = Json::array();
						}
					}
				}
				return Json{{"chords", chords}};
			});
	}
}
