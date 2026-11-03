// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI editors' common framework (#126, reference manual pp. 1132-1156): which parts the lower zone's editor and
// the editor window show, the part active for editing, the part editing mode, Keep Editor Contents and the global
// tracks shown above the notes. The commands keep it on the command thread, outside the project, so they are no undo
// steps and don't make the project modified.
#include "engine/document/editors.h"

#include "engine/document/projectmodel.h"
#include "engine/engine.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr std::array<const char*, 2> g_editors = {"lower", "window"};
		// The part editing modes (p. 1143): what selecting and editing reach.
		constexpr std::array<const char*, 3> g_editingModes = {"allParts", "activePart", "activeTrack"};

		struct EditorView
		{
			bool open = false;
			uint64_t project = 0; // the project it belongs to (ProjectModel::openedRevision)
			std::vector<ObjectId> parts;
			ObjectId activePart = 0;
			std::string editingMode = "allParts";
			bool keepContents = false;
			std::vector<ObjectId> globalTracks;
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
				(found->second.type == TrackType::Marker || found->second.type == TrackType::Ruler);
		}

		// The parts that still exist, once each, in the track list's order and on each track by position.
		std::vector<ObjectId> ordered(const Project& project, std::vector<ObjectId> parts)
		{
			std::map<ObjectId, size_t> index;
			const std::vector<ObjectId> order = project.trackOrder();
			for (size_t i = 0; i < order.size(); ++i)
				index[order[i]] = i;
			parts.erase(std::remove_if(parts.begin(), parts.end(),
							[&project](ObjectId id) { return project.midiParts.count(id) == 0; }),
				parts.end());
			const auto key = [&](ObjectId id)
			{
				const MidiPart& part = project.midiParts.at(id);
				return std::make_tuple(index[part.track], part.position.value, id);
			};
			std::sort(parts.begin(), parts.end(), [&](ObjectId a, ObjectId b) { return key(a) < key(b); });
			parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
			return parts;
		}

		// The editor's view now: closed and empty once another project is open, without what is gone since.
		EditorView current(const Views& views, const ProjectModel& model, const std::string& editor)
		{
			const auto found = views.find(editor);
			EditorView view = found != views.end() ? found->second : EditorView();
			if (view.project != model.openedRevision())
				view = EditorView();
			const Project& project = model.project();
			view.parts = ordered(project, view.parts);
			if (std::find(view.parts.begin(), view.parts.end(), view.activePart) == view.parts.end())
				view.activePart = view.parts.empty() ? 0 : view.parts.front();
			view.globalTracks.erase(std::remove_if(view.globalTracks.begin(), view.globalTracks.end(),
										[&project](ObjectId id) { return !isGlobal(project, id); }),
				view.globalTracks.end());
			return view;
		}

		ObjectId trackOf(const Project& project, ObjectId part)
		{
			return part == 0 ? 0 : project.midiParts.at(part).track;
		}

		Json viewJson(const Project& project, const std::string& editor, const EditorView& view)
		{
			const ObjectId activeTrack = trackOf(project, view.activePart);
			std::vector<ObjectId> editable;
			for (const ObjectId part : view.parts)
				if (view.editingMode == "allParts" || (view.editingMode == "activePart" && part == view.activePart) ||
					(view.editingMode == "activeTrack" && trackOf(project, part) == activeTrack))
					editable.push_back(part);
			return {{"editor", editor}, {"open", view.open}, {"parts", view.parts}, {"activePart", view.activePart},
				{"activeTrack", activeTrack}, {"editable", editable}, {"editingMode", view.editingMode},
				{"keepContents", view.keepContents}, {"globalTracks", view.globalTracks}};
		}

		// The MIDI parts of a track, or of every track in a folder.
		void collectParts(const Project& project, ObjectId track, std::vector<ObjectId>& parts)
		{
			for (const auto& [id, part] : project.midiParts)
				if (part.track == track)
					parts.push_back(id);
			for (const ObjectId child : project.trackById.at(track).children)
				collectParts(project, child, parts);
		}

		std::vector<ObjectId> partsOf(const Project& project, ObjectId track)
		{
			require(project.trackById.count(track) != 0, "no track " + std::to_string(track));
			std::vector<ObjectId> parts;
			collectParts(project, track, parts);
			require(!parts.empty(), "track " + std::to_string(track) + " has no MIDI parts");
			return ordered(project, parts);
		}

		std::vector<ObjectId> ids(const Json& parameters, const char* name)
		{
			return parameters.contains(name) ? parameters.at(name).get<std::vector<ObjectId>>()
											 : std::vector<ObjectId>();
		}

		bool contains(const std::vector<ObjectId>& ids, ObjectId id)
		{
			return std::find(ids.begin(), ids.end(), id) != ids.end();
		}

		std::vector<ObjectId> checkedParts(const Project& project, const std::vector<ObjectId>& parts)
		{
			for (const ObjectId id : parts)
				require(project.midiParts.count(id) != 0, "no MIDI part " + std::to_string(id));
			return ordered(project, parts);
		}
	}

	void registerEditorCommands(ProjectModel& model, Engine& engine)
	{
		auto views = std::make_shared<Views>();
		const Json editorSchema = {
			{"type", "string"}, {"enum", std::vector<std::string>(g_editors.begin(), g_editors.end())}};
		const Json modeSchema = {
			{"type", "string"}, {"enum", std::vector<std::string>(g_editingModes.begin(), g_editingModes.end())}};
		const Json idsSchema = {{"type", "array"}, {"items", {{"type", "integer"}}}};
		const Json idSchema = {{"type", "integer"}};
		const Json viewSchema = {{"type", "object"},
			{"properties",
				{{"editor", editorSchema}, {"open", {{"type", "boolean"}}}, {"parts", idsSchema},
					{"activePart", idSchema}, {"activeTrack", idSchema}, {"editable", idsSchema},
					{"editingMode", modeSchema}, {"keepContents", {{"type", "boolean"}}}, {"globalTracks", idsSchema}}},
			{"required",
				{"editor", "open", "parts", "activePart", "activeTrack", "editable", "editingMode", "keepContents",
					"globalTracks"}}};
		const std::string viewText =
			"editor: lower (the editor in the project window's lower zone) or window (the editor window). The view: "
			"open; parts, the MIDI parts it shows, in the track list's order and by position; activePart, the part "
			"active for editing, and activeTrack, its track (0 for none); editingMode, what selecting and editing "
			"reach: allParts, activePart or activeTrack (the active part's track); editable, the parts that reach; "
			"keepContents, whether it keeps its parts when the project window's selection changes; globalTracks, the "
			"marker and ruler tracks the editor window shows above its events. View state: no undo steps, not saved, "
			"and a new or loaded project closes the editors.";
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
			Json result = viewJson(model.project(), editor, view);
			context.emit("editor.changed", result);
			return result;
		};

		engine.registerCommand(
			{"editor.get", 1, "What a MIDI editor shows. " + viewText, editorOnly(), viewSchema, true},
			[views, &model](CommandContext& context)
			{
				const std::string editor = context.parameters().at("editor").get<std::string>();
				return viewJson(model.project(), editor, current(*views, model, editor));
			});

		Json openSchema = editorOnly(
			{{"parts", {{"type", "array"}, {"items", idSchema}, {"minItems", 1}}}, {"activePart", idSchema}});
		openSchema["required"] = {"editor", "parts"};
		engine.registerCommand(
			{"editor.open", 1,
				"Opens a MIDI editor with MIDI parts, such as the ones selected in the project window, or shows "
				"those parts in it if it is open. activePart becomes the part active for editing, by default the "
				"first. Emits editor.changed. " +
					viewText,
				openSchema, viewSchema, false},
			[views, &model, store](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const std::string editor = parameters.at("editor").get<std::string>();
				EditorView view = current(*views, model, editor);
				view.parts = checkedParts(model.project(), ids(parameters, "parts"));
				require(!view.parts.empty(), "parts must name at least one MIDI part");
				view.activePart = parameters.value("activePart", view.parts.front());
				require(contains(view.parts, view.activePart),
					"the active part " + std::to_string(view.activePart) + " isn't one of the parts");
				view.open = true;
				return store(editor, view, context);
			});

		engine.registerCommand(
			{"editor.set", 1,
				"Changes what an open MIDI editor shows (reference manual pp. 1142-1156); what isn't given stays. "
				"parts replaces its parts. Then the Visibility tab's changes: exclusive hides every track first, "
				"showTracks shows every MIDI part of these tracks (all tracks in a folder) and activates the last, "
				"hideTracks hides theirs; at least one part stays. activeTrack activates the track's first part shown, "
				"activePart a part shown. editingMode, keepContents and globalTracks (marker and ruler tracks, the "
				"editor window only) also apply to a closed editor. Emits editor.changed. " +
					viewText,
				editorOnly({{"parts", idsSchema}, {"exclusive", {{"type", "boolean"}}}, {"showTracks", idsSchema},
					{"hideTracks", idsSchema}, {"activeTrack", idSchema}, {"activePart", idSchema},
					{"editingMode", modeSchema}, {"keepContents", {{"type", "boolean"}}}, {"globalTracks", idsSchema}}),
				viewSchema, false},
			[views, &model, store](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project& project = model.project();
				const std::string editor = parameters.at("editor").get<std::string>();
				EditorView view = current(*views, model, editor);
				const bool changesParts = parameters.contains("parts") || parameters.contains("exclusive") ||
					parameters.contains("showTracks") || parameters.contains("hideTracks") ||
					parameters.contains("activeTrack") || parameters.contains("activePart");
				require(!changesParts || view.open, "the " + editor + " editor isn't open");
				if (parameters.contains("parts"))
					view.parts = checkedParts(project, ids(parameters, "parts"));
				if (parameters.value("exclusive", false))
					view.parts.clear();
				for (const ObjectId track : ids(parameters, "showTracks"))
				{
					const std::vector<ObjectId> shown = partsOf(project, track);
					view.parts.insert(view.parts.end(), shown.begin(), shown.end());
					view.activePart = shown.front(); // showing a track activates it (p. 1151)
				}
				for (const ObjectId track : ids(parameters, "hideTracks"))
					for (const ObjectId part : partsOf(project, track))
						view.parts.erase(std::remove(view.parts.begin(), view.parts.end(), part), view.parts.end());
				view.parts = ordered(project, view.parts);
				require(!view.open || !view.parts.empty(), "at least one track must stay shown");
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
				return store(editor, view, context);
			});

		engine.registerCommand(
			{"editor.close", 1,
				"Closes a MIDI editor: it shows no parts, and Keep Editor Contents goes off (p. 1153). Emits "
				"editor.changed. " +
					viewText,
				editorOnly(), viewSchema, false},
			[views, &model, store](CommandContext& context)
			{
				const std::string editor = context.parameters().at("editor").get<std::string>();
				EditorView view = current(*views, model, editor);
				view.open = false;
				view.parts.clear();
				view.activePart = 0;
				view.keepContents = false;
				return store(editor, view, context);
			});
	}
}
