// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI editors' common framework (#126, reference manual pp. 1132-1156): which parts the lower zone's editor and
// the editor window show, the part active for editing, the part editing mode, Keep Editor Contents and the global
// tracks shown above the notes; and the key editor's (#127, pp. 1189-1220): the selected notes and step and MIDI input,
// which editor.input applies; and whether an editor shows the key editor or the list editor (#130, pp. 1243-1258),
// whose selection also has controller events; and the audio part editor's (#146, pp. 692-702), whose parts are audio
// parts and whose selection is audio events. The commands keep the views on the command thread, outside the project,
// so they are no undo steps and don't make the project modified; editor.input's notes are.
#include "engine/document/editors.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/controllerlanes.h"
#include "engine/midi/drummaps.h"
#include "engine/midi/quantizesettings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
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
			// The drum editor's (#882): Pitch Visibility On/Off and its option Reverse Drum Sound List (pp. 1223-1224,
			// 1234), and Show Note Length On/Off (p. 1227).
			bool pitchVisibility = false;
			bool reverseSounds = false;
			bool showNoteLength = false;
			// The controller lanes (#128): each a type (midi::g_laneTypes) and a controller number or pitch.
			std::vector<std::pair<std::string, int>> lanes = {{"velocity", 0}};
			bool showLanes = true;
			bool autoSelectControllers = false;
			std::vector<std::pair<ObjectId, ObjectId>> controllerSelection; // each a part and a controller event of it
			// The audio part editor (#146): the selected audio events of its parts.
			std::vector<ObjectId> eventSelection;
		};

		using Views = std::map<std::string, EditorView>;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		bool isGlobal(const Project& project, ObjectId id)
		{
			const auto found = project.trackById.find(id);
			return found != project.trackById.end() &&
				(found->second.type == TrackType::Marker || found->second.type == TrackType::Ruler ||
					found->second.type == TrackType::Arranger || found->second.type == TrackType::Transpose ||
					found->second.type == TrackType::Chord);
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

		void dropUneditable(const Project& project, EditorView& view)
		{
			const std::vector<ObjectId> editable = editableParts(project, view);
			dropUneditable(project, editable, view.selection, &MidiPart::notes);
			dropUneditable(project, editable, view.controllerSelection, &MidiPart::controllers);
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

		// lower, window, or inPlace: and a MIDI or instrument track's id.
		void requireEditor(const Project& project, const std::string& editor)
		{
			if (editor == "lower" || editor == "window")
				return;
			const std::string prefix = g_inPlace;
			const std::string digits = editor.substr(std::min(editor.size(), prefix.size()));
			require(editor.rfind(prefix, 0) == 0 && !digits.empty() && digits.size() < 20 &&
					std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }),
				"editor must be lower, window or inPlace:<track>, not " + editor);
			requireNoteTrack(project, inPlaceTrack(editor));
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

		Json viewJson(const Project& project, const std::string& editor, const EditorView& view)
		{
			const ObjectId activeTrack = trackOf(project, view.activePart);
			const std::vector<ObjectId> editable = editableParts(project, view);
			Json selection = Json::array();
			for (const auto& [part, note] : view.selection)
				selection.push_back({{"part", part}, {"note", note}});
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
				{"showNoteLength", view.showNoteLength}, {"lanes", lanes}, {"showLanes", view.showLanes},
				{"autoSelectControllers", view.autoSelectControllers}, {"controllerSelection", controllerSelection},
				{"eventSelection", view.eventSelection}};
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
	}

	void registerEditorCommands(ProjectModel& model, Engine& engine)
	{
		auto views = std::make_shared<Views>();
		// The names, so that MCP clients and Lua see them: lower, window or inPlace:<track> (#131). At most inPlace:
		// and the 19 digits requireEditor takes, so that the pattern never has to match a long string (#1059).
		const Json editorSchema = {
			{"type", "string"}, {"pattern", "^(lower|window|inPlace:[0-9]+)$"}, {"maxLength", 27}};
		const Json modeSchema = {
			{"type", "string"}, {"enum", std::vector<std::string>(g_editingModes.begin(), g_editingModes.end())}};
		const Json idsSchema = {{"type", "array"}, {"items", {{"type", "integer"}}}};
		const Json idSchema = {{"type", "integer"}};
		const Json boolean = {{"type", "boolean"}};
		const Json selectionSchema = {{"type", "array"},
			{"items",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"part", "note"}},
					{"properties", {{"part", idSchema}, {"note", idSchema}}}}}};
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
			{"showLanes", boolean}, {"autoSelectControllers", boolean}, {"pitchVisibility", boolean},
			{"reverseSounds", boolean}, {"showNoteLength", boolean}};
		Json viewSchema = {{"type", "object"},
			{"properties",
				{{"editor", editorSchema}, {"open", {{"type", "boolean"}}}, {"parts", idsSchema},
					{"activePart", idSchema}, {"activeTrack", idSchema}, {"editable", idsSchema},
					{"editingMode", modeSchema}, {"keepContents", {{"type", "boolean"}}}, {"globalTracks", idsSchema}}},
			{"required",
				{"editor", "open", "parts", "activePart", "activeTrack", "editable", "editingMode", "keepContents",
					"globalTracks", "selection", "stepInput", "midiInput", "stepPosition", "moveInsert", "recordPitch",
					"recordVelocity", "recordReleaseVelocity", "keepNotes", "insertVelocity", "lengthQuantize",
					"content", "pitchVisibility", "reverseSounds", "showNoteLength", "lanes", "showLanes",
					"autoSelectControllers", "controllerSelection", "eventSelection"}}};
		viewSchema["properties"].update(keyEditorSchema);
		const std::string viewText =
			"editor: lower (the editor in the project window's lower zone), window (the editor window) or "
			"inPlace:<track>, the in-place editor of a MIDI or instrument track (editor.inPlace), which shows every "
			"MIDI part of its track in the key editor. The view: "
			"open; parts, the MIDI parts or audio parts it shows, in the track list's order and by position; "
			"activePart, the part "
			"active for editing, and activeTrack, its track (0 for none); editingMode, what selecting and editing "
			"reach: allParts, activePart or activeTrack (the active part's track); editable, the parts that reach; "
			"keepContents, whether it keeps its parts when the project window's selection changes; globalTracks, the "
			"marker and ruler tracks the editor window shows above its events. The key editor's (pp. 1189-1220): "
			"selection, the selected notes, each {part, note}, of the editable parts; stepInput and midiInput, what "
			"editor.input does with notes played; stepPosition, the step input cursor in project ticks; moveInsert, "
			"recordPitch, recordVelocity, recordReleaseVelocity and keepNotes, how they go in; insertVelocity (0-1), "
			"the velocity of drawn notes and of input that doesn't record it; lengthQuantize, the length of new notes, "
			"a note value such as 1/16, 1/8T or 1/4., link for the quantize grid's (quantize.set), or drumMap for the "
			"snap of the note's sound in its track's drum map (drumMap.add), without one the quantize grid's; notes "
			"played into a track with a drum map take the pitch of the sound whose iNote they are. content: what "
			"shows the parts, key (the key editor), list (the list editor, pp. 1243-1258), drum (the drum editor, "
			"pp. 1221-1242) or pattern (the pattern editor, pp. 895-937: the active part's pattern). The drum "
			"editor's: pitchVisibility (Pitch Visibility On/Off) applies its option reverseSounds, the drum sound "
			"list from the bottom (pp. 1223-1224, 1234); showNoteLength shows its notes as boxes of their length, not "
			"diamonds "
			"(p. 1227); "
			"controllerSelection, the "
			"list editor's selected controller events, each {part, controller}, of the editable parts. Audio parts "
			"show in the audio part editor (content audioPart, pp. 692-702), whose eventSelection is the selected "
			"audio events of the editable parts. The controller lanes' (pp. "
			"1157-1188): lanes, from the top, each {type, number}: velocity, offVelocity, probability or "
			"velocityVariance (the notes' play probability and velocity variance), or the controller events of a "
			"type: controller (number is the controller), pitchBend, channelPressure, polyPressure (number is the "
			"pitch) or program; showLanes, whether the controller display shows them; autoSelectControllers, "
			"whether selecting notes selects the controller events from their start to their end. View state: no "
			"undo steps, not saved, and a new or loaded project closes the editors.";
		const auto editorOnly = [&](Json properties = Json::object())
		{
			properties["editor"] = editorSchema;
			return Json{{"type", "object"}, {"properties", properties}, {"required", {"editor"}},
				{"additionalProperties", false}};
		};
		const auto store = [views, &model](const std::string& editor, const EditorView& view, CommandContext& context)
		{
			EditorView& stored = (*views)[editor];
			stored = view;
			stored.project = model.openedRevision();
			dropUneditable(model.project(), stored);
			Json result = viewJson(model.project(), editor, stored);
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
				else if (showsAudio(view))
					view.content = "key";
				requireContent(project, view);
				return store(editor, view, context);
			});

		engine.registerCommand(
			{"editor.set", 1,
				"Changes what an open editor shows (reference manual pp. 1142-1156); what isn't given stays. "
				"parts replaces its parts. Then the Visibility tab's changes: exclusive hides every track first, "
				"showTracks shows every MIDI part of these tracks (all tracks in a folder), in the audio part editor "
				"(content audioPart, also one given with it) every audio part, and activates the last, "
				"hideTracks hides theirs; at least one part stays. activeTrack activates the track's first part shown, "
				"activePart a part shown. editingMode (an in-place editor's allParts or activePart), keepContents and "
				"globalTracks (marker and ruler tracks, the editor window only) also apply to a closed editor. "
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
					{"showNoteLength", boolean}, {"lanes", lanesSchema}, {"showLanes", boolean},
					{"autoSelectControllers", boolean}, {"controllerSelection", controllerSelectionSchema},
					{"eventSelection", idsSchema}}),
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
				for (const ObjectId track : ids(parameters, "showTracks"))
				{
					const std::vector<ObjectId> shown = partsOf(project, track, audio);
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
				view.pitchVisibility = parameters.value("pitchVisibility", view.pitchVisibility);
				view.reverseSounds = parameters.value("reverseSounds", view.reverseSounds);
				view.showNoteLength = parameters.value("showNoteLength", view.showNoteLength);
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
				"length quantize's length, and the cursor moves on by a step of the quantize grid; moveInsert first "
				"moves the part's notes from the cursor on by a step, and without keepNotes the notes that start "
				"within the step go. With midiInput (and stepInput off), the first selected note takes the lowest "
				"note's pitch, velocity and release velocity, as far as recordPitch, recordVelocity and "
				"recordReleaseVelocity say, and the other notes of a chord are added with its position and length; "
				"with keepNotes, all of them are added and the selected note stays. Then the next note of its part "
				"is selected. Velocities that aren't recorded are insertVelocity's and the default release velocity. "
				"One undo step. Returns the part and its notes added or changed.",
				inputSchema,
				{{"type", "object"}, {"required", {"part", "notes"}},
					{"properties", {{"part", idSchema}, {"notes", {{"type", "array"}}}}}},
				false},
			"Step input",
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
					uint8_t channel = 0;
					std::vector<ObjectId> removed;
					for (auto& [id, note] : part.notes)
					{
						if (view.moveInsert && note.position >= at)
						{
							note.position += step;
							changed.push_back(toJson(note));
						}
						else if (!view.keepNotes && note.position >= at && note.position < at + step)
						{
							removed.push_back(id);
						}
						channel = note.channel;
					}
					for (const ObjectId id : removed)
						part.notes.erase(id);
					if (!removed.empty())
						context.emit("note.removed", {{"part", partId}, {"notes", removed}});
					view.selection.clear();
					for (Played each : played)
					{
						each.pitch = midi::inputPitch(project, part.track, each.pitch); // the I-notes (p. 1239)
						const Note note = newNote(
							project, view, each, at, noteLength(project, view, part.track, each.pitch), channel);
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
	}
}
