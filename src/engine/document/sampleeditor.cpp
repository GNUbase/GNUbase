// SPDX-License-Identifier: GPL-3.0-or-later
// The sample editor (#140, reference manual pp. 613-635): the audio event it shows and its selection range, kept on the
// command thread outside the project like the MIDI editors' views (editors.cpp), so they are no undo steps and don't
// make the project modified; and the regions of clips (pp. 624-626), which are in the project, so each region command
// is one undo step and changes nothing if it fails (ProjectModel::addEdit).
#include "engine/document/sampleeditor.h"

#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/resampler.h"
#include "engine/document/channelformat.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/document/snap.h"
#include "engine/engine.h"
#include "engine/media/backup.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		struct SampleView
		{
			bool open = false;
			uint64_t project = 0;             // the project it belongs to (ProjectModel::openedRevision)
			ObjectId event = 0;               // kept while the event is gone, so that it comes back with an undo
			time::Samples selectionStart = 0; // clip frames; no selection while start and end are the same
			time::Samples selectionEnd = 0;
		};

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

		const AudioEvent* eventOf(const Project& project, ObjectId id)
		{
			const auto found = project.audioEvents.find(id);
			return found != project.audioEvents.end() ? &found->second : nullptr;
		}

		time::Samples clipLength(const Project& project, const AudioEvent& event)
		{
			return project.media.at(project.clips.at(event.clip).media).length;
		}

		// The view now: closed once another project is open; without a selection while its event is gone.
		SampleView current(const SampleView& stored, const ProjectModel& model)
		{
			SampleView view = stored.project == model.openedRevision() ? stored : SampleView();
			const AudioEvent* event = eventOf(model.project(), view.event);
			if (event == nullptr)
			{
				view.selectionStart = view.selectionEnd = 0;
				return view;
			}
			const time::Samples length = clipLength(model.project(), *event);
			view.selectionStart = std::min(view.selectionStart, length);
			view.selectionEnd = std::clamp(view.selectionEnd, view.selectionStart, length);
			return view;
		}

		Json viewJson(const Project& project, const SampleView& view)
		{
			Json json = {{"open", view.open}, {"event", 0}, {"clip", 0}, {"track", 0}, {"media", 0}, {"length", 0},
				{"offset", 0}, {"eventLength", 0}, {"clipStart", 0}, {"ratio", 1.0},
				{"selectionStart", view.selectionStart}, {"selectionEnd", view.selectionEnd}};
			const AudioEvent* event = eventOf(project, view.event);
			if (event == nullptr)
				return json;
			const EventSpan span = eventSpan(project, *event);
			json["event"] = event->id;
			json["clip"] = event->clip;
			json["track"] = event->track;
			json["media"] = project.clips.at(event->clip).media;
			json["length"] = clipLength(project, *event);
			json["offset"] = event->offset;
			json["eventLength"] = event->length;
			json["clipStart"] = span.start - std::llround(static_cast<double>(event->offset) * span.ratio);
			json["ratio"] = span.ratio;
			return json;
		}

		Clip& clipOf(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("clip").get<ObjectId>();
			const auto found = project.clips.find(id);
			require(found != project.clips.end(), "no clip " + std::to_string(id));
			return found->second;
		}

		ClipRegion& regionOf(Clip& clip, const Json& parameters)
		{
			const auto id = parameters.at("region").get<ObjectId>();
			const auto found = std::find_if(
				clip.regions.begin(), clip.regions.end(), [id](const ClipRegion& region) { return region.id == id; });
			require(found != clip.regions.end(),
				"clip " + std::to_string(clip.id) + " has no region " + std::to_string(id));
			return *found;
		}

		// Checks a region's section against its clip and keeps the regions by start.
		void settle(const Project& project, Clip& clip, const ClipRegion& region)
		{
			const time::Samples length = project.media.at(clip.media).length;
			require(region.start >= 0 && region.start < region.end && region.end <= length,
				"a region must start before it ends, within the clip (0 to " + std::to_string(length) +
					" sample frames)");
			std::sort(clip.regions.begin(), clip.regions.end(), [](const ClipRegion& a, const ClipRegion& b)
				{ return std::tie(a.start, a.id) < std::tie(b.start, b.id); });
		}

		// Region and a number that no region of the clip has in its name, from the count of regions on.
		std::string newRegionName(const Clip& clip)
		{
			for (size_t number = clip.regions.size() + 1;; ++number)
			{
				std::string name = "Region " + std::to_string(number);
				if (std::none_of(clip.regions.begin(), clip.regions.end(),
						[&name](const ClipRegion& region) { return region.name == name; }))
					return name;
			}
		}

		Json regionResult(const Clip& clip, ObjectId region, CommandContext& context)
		{
			context.emit("clip.changed", {{"clip", toJson(clip)}});
			for (const ClipRegion& each : clip.regions)
				if (each.id == region)
					return {{"clip", clip.id}, {"region", toJson(each)}};
			return {{"clip", clip.id}};
		}

		struct ProcessRange
		{
			time::Samples start = 0;
			time::Samples end = 0;
			time::Samples inserted = 0;

			bool contains(time::Samples frame) const { return frame >= start && frame < end; }

			time::Samples move(time::Samples frame) const
			{
				if (frame <= start)
					return frame;
				return frame < end ? start : frame - (end - start) + inserted;
			}

			// An anchor at an insertion belongs to the audio moved right; a range edge stays on the left.
			time::Samples moveAnchor(time::Samples frame) const
			{
				return frame == start ? start + inserted : move(frame);
			}
		};

		void processWarp(ClipWarp& warp, const ProcessRange& range, time::Samples length)
		{
			warp.gridStart = std::min(range.moveAnchor(warp.gridStart), std::max<time::Samples>(0, length - 1));
			warp.beats.erase(std::remove_if(warp.beats.begin(), warp.beats.end(),
								 [&](const BeatEdit& beat) { return range.contains(beat.frame); }),
				warp.beats.end());
			for (BeatEdit& beat : warp.beats)
				beat.frame = range.moveAnchor(beat.frame);
			warp.beats.erase(std::remove_if(warp.beats.begin(), warp.beats.end(), [&](const BeatEdit& beat)
								 { return beat.frame <= warp.gridStart || beat.frame >= length; }),
				warp.beats.end());
			warp.markers.erase(std::remove_if(warp.markers.begin(), warp.markers.end(), [&](const WarpMarker& marker)
								   { return range.contains(marker.position) || range.contains(marker.target); }),
				warp.markers.end());
			for (WarpMarker& marker : warp.markers)
			{
				marker.position = range.moveAnchor(marker.position);
				marker.target = range.moveAnchor(marker.target);
			}
			warp.markers.erase(std::remove_if(warp.markers.begin(), warp.markers.end(),
								   [&](const WarpMarker& marker)
								   {
									   return marker.position <= 0 || marker.target <= 0 || marker.position >= length ||
										   marker.target >= length;
								   }),
				warp.markers.end());
			warp.quantizeOrigin.reset();
			warp.created.clear();
		}

		struct ProcessClipboard
		{
			MediaFile media;
			std::filesystem::path path; // prepared before committing, so cleanup cannot allocate
			~ProcessClipboard()
			{
				std::error_code ignored;
				std::filesystem::remove(path, ignored);
			}
		};

		struct ProcessProgress
		{
			time::Samples done = 0;
			time::Samples total = 0;
		};

		// Bounded interleaved blocks, including short-read checks; the silent section needs no input file.
		void writeSection(AudioFileWriter& writer, const MediaFile& media, time::Samples start, time::Samples end,
			JobContext& job, ProcessProgress& progress, bool silence = false, const Json& draw = Json::object())
		{
			std::unique_ptr<AudioFileReader> reader;
			if (!silence)
			{
				reader = AudioFileReader::open(media.path);
				require(reader->info().channels == media.channels && reader->info().sampleRate == media.sampleRate,
					"the audio file's format changed");
				reader->seek(start);
			}
			constexpr size_t block = 65536;
			std::vector<float> buffer(block * static_cast<size_t>(media.channels), 0.0F);
			for (time::Samples at = start; at < end;)
			{
				job.checkCancelled();
				const auto count = static_cast<size_t>(std::min<time::Samples>(block, end - at));
				const size_t read = reader ? reader->read(buffer.data(), count) : count;
				require(read != 0, "file ends before the requested range");
				if (!draw.empty())
					for (size_t frame = 0; frame < read; ++frame)
						buffer[(frame * static_cast<size_t>(media.channels)) + draw.at("channel").get<size_t>()] =
							draw.at("samples").at(static_cast<size_t>(at - start) + frame).get<float>();
				writer.write(buffer.data(), read);
				at += static_cast<time::Samples>(read);
				progress.done += static_cast<time::Samples>(read);
				job.progress(
					0.95 * static_cast<double>(progress.done) / static_cast<double>(progress.total), media.path);
			}
		}

		// Clipboard conversion is streamed on the job thread. Use the engine's default speaker arrangements;
		// uncommon channel counts preserve matching channel indices and leave additional channels silent.
		void writeClipboard(AudioFileWriter& writer, const MediaFile& media, const MediaFile& target, JobContext& job,
			ProcessProgress& progress)
		{
			if (media.sampleRate == target.sampleRate && media.channels == target.channels)
			{
				writeSection(writer, media, 0, media.length, job, progress);
				return;
			}
			const auto reader = AudioFileReader::open(media.path);
			require(reader->info().channels == media.channels && reader->info().sampleRate == media.sampleRate,
				"the audio file's format changed");
			constexpr size_t block = 65536;
			const auto from = static_cast<size_t>(media.channels);
			const auto to = static_cast<size_t>(target.channels);
			std::vector<float> gains =
				conversionGains(defaultFormat(media.channels), defaultFormat(target.channels), 1.0);
			if (gains.empty())
			{
				gains.resize(from * to, 0.0F);
				for (size_t channel = 0; channel < std::min(from, to); ++channel)
					gains[(channel * from) + channel] = 1.0F;
			}
			Resampler resampler(media.sampleRate, target.sampleRate, media.channels, block);
			std::vector<float> input(block * from);
			std::vector<float> converted;
			std::vector<float> mixed;
			const auto write = [&]
			{
				job.checkCancelled();
				const size_t frames = converted.size() / from;
				mixed.assign(frames * to, 0.0F);
				for (size_t out = 0; out < to; ++out)
					for (size_t in = 0; in < from; ++in)
						if (const float gain = gains[(out * from) + in]; gain != 0.0F)
							for (size_t frame = 0; frame < frames; ++frame)
								mixed[(frame * to) + out] += converted[(frame * from) + in] * gain;
				if (frames != 0)
					writer.write(mixed.data(), frames);
				progress.done += static_cast<time::Samples>(frames);
				job.progress(0.95 * static_cast<double>(progress.done) /
						static_cast<double>(std::max<int64_t>(1, progress.total)),
					media.path);
				converted.clear();
			};
			for (time::Samples at = 0; at < media.length;)
			{
				job.checkCancelled();
				const auto count = static_cast<size_t>(std::min<time::Samples>(block, media.length - at));
				const size_t read = reader->read(input.data(), count);
				require(read != 0, "file ends before the requested range");
				resampler.process(input.data(), read, converted);
				write();
				at += static_cast<time::Samples>(read);
			}
			resampler.finish(converted);
			write();
		}

		MediaFile writeProcessFile(const MediaFile& source, ProcessRange range, const MediaFile* paste,
			const std::string& operation, const std::filesystem::path& folder, JobContext& job,
			std::vector<std::string>& written, ProcessProgress& progress, const Json& draw = Json::object())
		{
			std::filesystem::create_directories(folder);
			MediaFile output = source;
			output.path = createFreeFile(folder, "Sample Edit.wav").u8string();
			written.push_back(output.path);
			WriteOptions options;
			options.sampleFormat = SampleFormat::Float32;
			options.sampleRate = source.sampleRate;
			options.channels = source.channels;
			const auto writer = AudioFileWriter::create(output.path, options);
			if (operation == "copy")
			{
				writeSection(*writer, source, range.start, range.end, job, progress);
				output.length = range.end - range.start;
			}
			else
			{
				writeSection(*writer, source, 0, range.start, job, progress);
				if (operation == "draw")
					writeSection(*writer, source, range.start, range.end, job, progress, false, draw);
				else if (paste != nullptr)
					writeClipboard(*writer, *paste, source, job, progress);
				else if (range.inserted > 0)
					writeSection(*writer, source, 0, range.inserted, job, progress, true);
				writeSection(*writer, source, range.end, source.length, job, progress);
				output.length = source.length - (range.end - range.start) + range.inserted;
			}
			writer->close();
			return output;
		}

		// All checks also cover copies in inactive track versions. Their event/part ids remain project-wide ids.
		void checkProcess(const Project& project, const AudioEvent& event, bool all)
		{
			const auto affected = [&](const AudioEvent& each)
			{ return all ? each.clip == event.clip : each.id == event.id; };
			forEveryAudioEvent(project,
				[&](const AudioEvent& each)
				{
					if (affected(each))
					{
						require(!each.lock, "an affected audio event is locked");
						require(!project.trackById.at(each.track).frozen, "an affected track is frozen");
					}
				});
			forEveryEventAndPart(
				project, [](const auto&, bool) {},
				[&](const AudioPart& part, bool)
				{
					if (part.lock)
						forEveryAudioEvent(project, [&](const AudioEvent& each)
							{ require(!affected(each) || each.part != part.id, "an affected audio part is locked"); });
				},
				[](const auto&, bool) {});
		}

		Json applyProcess(Project& project, ProjectModel& model, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			require(context.internal(), "only a Process job can apply its result");
			require(context.call("project.revision").at("revision") == parameters.at("revision"),
				"project changed meanwhile; process again");
			AudioEvent& selected = project.audioEvents.at(parameters.at("event").get<ObjectId>());
			const ObjectId original = selected.clip;
			const bool all = parameters.value("sharedCopies", std::string("all")) == "all";
			checkProcess(project, selected, all);
			Clip clip = project.clips.at(original);
			ProcessRange range{parameters.at("start").get<time::Samples>(), parameters.at("end").get<time::Samples>(),
				parameters.at("inserted").get<time::Samples>()};
			const Json& file = parameters.at("media");
			MediaFile media{project.newId(), file.at("path").get<std::string>(), file.at("sampleRate").get<double>(),
				file.at("channels").get<int>(), file.at("length").get<time::Samples>()};
			if (parameters.at("operation") == "draw")
				range = {media.length, media.length, 0}; // Sample replacement keeps every frame's position.
			std::map<ObjectId, std::pair<time::Samples, time::Samples>> fadeEdges;
			forEveryAudioEvent(project,
				[&](const AudioEvent& event)
				{
					if (event.clip != original || (!all && event.id != selected.id))
						return;
					const EventSpan span = eventSpan(project, event);
					const auto frame = [&](double samples)
					{
						return event.offset +
							std::clamp<time::Samples>(
								std::llround(eventFrames(project, event, samples)), 0, event.length);
					};
					fadeEdges[event.id] = {range.move(frame(static_cast<double>(event.fadeIn.length) * span.ratio)),
						range.move(frame(static_cast<double>(span.end - span.start) -
							static_cast<double>(event.fadeOut.length) * span.ratio))};
				});
			project.media[media.id] = media;
			context.emit("media.added", {{"media", toJson(media)}});
			clip.media = media.id;
			processWarp(clip.warp, range, media.length);
			if (!all)
			{
				clip.id = project.newId();
				selected.clip = clip.id;
				for (ClipRegion& region : clip.regions)
					region.id = project.newId();
			}
			for (ClipRegion& region : clip.regions)
			{
				region.start = range.move(region.start);
				region.end = range.move(region.end);
			}
			clip.regions.erase(std::remove_if(clip.regions.begin(), clip.regions.end(),
								   [](const ClipRegion& region) { return region.start >= region.end; }),
				clip.regions.end());
			std::sort(clip.regions.begin(), clip.regions.end(), [](const ClipRegion& a, const ClipRegion& b)
				{ return std::tie(a.start, a.id) < std::tie(b.start, b.id); });
			clip.hitpoints.erase(std::remove_if(clip.hitpoints.begin(), clip.hitpoints.end(), [&](const Hitpoint& point)
									 { return point.position >= range.start && point.position < range.end; }),
				clip.hitpoints.end());
			for (Hitpoint& point : clip.hitpoints)
				point.position =
					point.position == range.start ? range.start + range.inserted : range.move(point.position);
			const time::Samples oldLength = project.media.at(project.clips.at(original).media).length;
			clip.fadeIn.length = range.move(clip.fadeIn.length);
			clip.fadeOut.length = media.length - range.move(oldLength - clip.fadeOut.length);
			project.clips[clip.id] = clip;
			context.emit(all ? "clip.changed" : "clip.added", {{"clip", toJson(clip)}});
			std::vector<ObjectId> removed;
			forEveryEventAndPart(
				project,
				[&](AudioEvent& event, bool active)
				{
					if (event.clip != clip.id)
						return;
					const auto end = range.move(event.offset + event.length);
					event.offset = range.move(event.offset);
					event.length = end - event.offset;
					if (event.length == 0)
						removed.push_back(event.id);
					const auto room = event.length > 0 ? fadeRoom(project, event) : 0;
					if (parameters.at("operation") != "draw")
					{
						const auto [fadeInEnd, fadeOutStart] = fadeEdges.at(event.id);
						const double ratio = project.sampleRate / media.sampleRate;
						const auto played = [&](time::Samples frame)
						{
							return std::llround(
								eventSamples(project, event, static_cast<double>(frame - event.offset)) / ratio);
						};
						event.fadeIn.length = std::clamp<time::Samples>(played(fadeInEnd), 0, room);
						event.fadeOut.length =
							std::clamp<time::Samples>(room - played(fadeOutStart), 0, room - event.fadeIn.length);
					}
					event.crossfade.length = std::min(event.crossfade.length, room);
					for (VolumePoint& point : event.volumeCurve)
						point.position = std::min(range.move(point.position), media.length);
					if (event.snapPoint)
						event.snapPoint =
							std::clamp(range.move(*event.snapPoint), event.offset, std::max(event.offset, end - 1));
					event.quantizeOrigin.reset();
					event.sliceOrigin.reset();
					event.slicedFrom = 0;
					require(eventSpan(project, event).end <= time::g_maxPosition,
						"the edited event exceeds the project end");
					if (active && event.length > 0)
						context.emit("audioEvent.changed", {{"event", toJson(event)}});
				},
				[](const auto&, bool) {}, [](const auto&, bool) {});
			for (const ObjectId id : removed)
				if (project.audioEvents.count(id) != 0)
					model.runEdit("event.remove", project, {{"events", {id}}}, context);
			for (auto& [id, track] : project.trackById)
			{
				bool changed = false;
				for (TrackVersion& version : track.versions)
				{
					for (const AudioEvent& event : version.audioEvents)
						changed = changed || event.clip == clip.id;
					version.audioEvents.erase(std::remove_if(version.audioEvents.begin(), version.audioEvents.end(),
												  [](const AudioEvent& event) { return event.length == 0; }),
						version.audioEvents.end());
				}
				if (changed)
					context.emit("track.changed", {{"track", toJson(track)}});
			}
			std::map<ObjectId, time::Samples> lengths;
			forEveryAudioEvent(project,
				[&](const AudioEvent& event)
				{
					if (event.clip == clip.id)
						lengths[event.id] = fadeRoom(project, event);
				});
			forEveryEventAndPart(
				project,
				[&](AudioEvent& event, bool active)
				{
					const auto before = event.crossfade.outLength;
					if (const auto from = lengths.find(event.crossfade.from); from != lengths.end())
						event.crossfade.outLength = std::min(event.crossfade.outLength, from->second);
					if (std::find(removed.begin(), removed.end(), event.crossfade.from) != removed.end())
					{
						event.crossfade = Crossfade();
					}
					if (active && before != event.crossfade.outLength)
						context.emit("audioEvent.changed", {{"event", toJson(event)}});
				},
				[](const auto&, bool) {}, [](const auto&, bool) {});
			return {{"clip", clip.id}, {"media", media.id}, {"length", media.length}};
		}

		void registerProcessCommands(ProjectModel& model, Engine& engine)
		{
			const Json integer = {{"type", "integer"}, {"minimum", 0}, {"maximum", time::g_maxPosition}};
			const Json operation = {
				{"type", "string"}, {"enum", {"cut", "copy", "paste", "delete", "insertSilence", "draw"}}};
			const Json properties = {{"event", integer}, {"operation", operation}, {"start", integer}, {"end", integer},
				{"channel", integer},
				{"samples",
					{{"type", "array"}, {"minItems", 1}, {"maxItems", 65536},
						{"items", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}}}},
				{"sharedCopies", {{"type", "string"}, {"enum", {"newVersion", "all"}}}}};
			const Json mediaSchema =
				objectSchema({{"id", integer}, {"path", {{"type", "string"}}}, {"sampleRate", {{"type", "number"}}},
								 {"channels", integer}, {"length", integer}},
					{"path", "sampleRate", "channels", "length"});
			Json completion = properties;
			completion.update(
				{{"revision", integer}, {"inserted", integer}, {"media", mediaSchema}, {"clipboard", mediaSchema}});
			const Json completionSchema =
				objectSchema(completion, {"event", "operation", "start", "end", "revision", "inserted"});
			const Json resultSchema =
				objectSchema({{"clip", integer}, {"media", integer}, {"length", integer}, {"copied", integer}});
			auto clipboard = std::make_shared<std::shared_ptr<ProcessClipboard>>();
			model.addEdit(engine,
				{"sampleEditor.applyProcess", 1,
					"Internal Process job edit: applies the rendered media at revision as one undo step. Moves clip "
					"ranges "
					"and event references with the replacement from start to end and inserted frames. Engine-posted "
					"work only. "
					"Emits media.added, clip.added or clip.changed, audioEvent.changed/removed and track.changed; "
					"returns clip, media, length.",
					completionSchema, resultSchema, false},
				"Process clip range",
				[&model](Project& project, CommandContext& context) { return applyProcess(project, model, context); });
			engine.registerCommand({"sampleEditor.finishProcess", 1,
									   "Internal Process job completion. Applies an edit, then replaces the clipboard "
									   "for Cut or Copy only after "
									   "success. Engine-posted work only; clients use sampleEditor.process. Returns "
									   "clip, media, length or copied frames.",
									   completionSchema, resultSchema, false},
				[clipboard](CommandContext& context)
				{
					require(context.internal(), "only a Process job can finish its result");
					const Json& parameters = context.parameters();
					std::shared_ptr<ProcessClipboard> copied;
					if (parameters.contains("clipboard"))
					{
						copied = std::make_shared<ProcessClipboard>();
						const Json& file = parameters.at("clipboard");
						copied->media = {0, file.at("path").get<std::string>(), file.at("sampleRate").get<double>(),
							file.at("channels").get<int>(), file.at("length").get<time::Samples>()};
						copied->path = std::filesystem::u8path(copied->media.path);
					}
					Json result = parameters.at("operation") == "copy"
						? Json{{"copied", copied->media.length}}
						: context.call("sampleEditor.applyProcess", parameters);
					if (copied)
						*clipboard = std::move(copied);
					return result;
				});
			engine.registerCommand(
				{"sampleEditor.process", 3,
					"Basic Commands on a clip range (reference manual pp. 619-620). event "
					"identifies the clip; start/end are "
					"clip frames, start before end. operation: cut copies then closes the gap, copy "
					"replaces the engine audio "
					"clipboard without undo, paste replaces the range, delete closes the gap, "
					"insertSilence inserts the range's "
					"length at its start. draw replaces start..end with samples in one zero-based channel, "
					"one sample per frame (at most 65536, -1 to 1), preserving timing and the other channels. "
					"Drawing corresponds to the Draw tool (reference manual p. 634) and requires no active warping "
					"or edited definition grid. sharedCopies is required "
					"for shared clips: newVersion "
					"edits only this event, all edits "
					"all copies, including inactive track versions. Sampler tracks and drum pads count as users too. "
					"Without another user, omitting sharedCopies edits the clip in place and keeps its region ids. "
					"Locked events/parts and frozen "
					"tracks refuse edits. Grid points and warp anchors inside replaced audio are removed; surviving "
					"anchors move with the samples and keep their beat numbers. Event fades follow played time "
					"through the old and new warp. Paste converts to the target's "
					"sample rate and default speaker arrangement, using engine downmix gains and unity mono "
					"duplication. "
					"Unknown channel counts map by index with silent extra channels. Writes new "
					"float WAV files in the media "
					"folder (for a project without a file, the folder its recordings go to, refused without that "
					"folder) as a cancellable job; returns job (its result: clip, media, length or "
					"copied frames). One undo step "
					"per edit; original files stay intact. Fails if the project changes during an "
					"edit. Clipboard survives undo "
					"and project changes. Not usable inside history.transaction.",
					objectSchema(properties, {"event", "operation", "start", "end"}),
					objectSchema({{"job", Jobs::schema()}}, {"job"}), false},
				[&model, &engine, clipboard](CommandContext& context)
				{
					Json parameters = context.parameters();
					const Project& project = model.project();
					const AudioEvent* event = eventOf(project, parameters.at("event").get<ObjectId>());
					require(event != nullptr, "no audio event");
					const Clip& clip = project.clips.at(event->clip);
					const MediaFile source = project.media.at(clip.media);
					const std::string operation = parameters.at("operation").get<std::string>();
					const bool copy = operation == "copy";
					ProcessRange range{
						parameters.at("start").get<time::Samples>(), parameters.at("end").get<time::Samples>(), 0};
					require(range.start < range.end && range.end <= source.length,
						"range must lie within the clip, start before end");
					if (operation == "draw")
					{
						require(!clip.warp.musical && clip.warp.markers.empty() && clip.warp.beats.empty() &&
								clip.warp.gridStart == 0,
							"Drawing requires no active warping or edited definition grid; flatten active warping "
							"first");
						require(
							parameters.contains("channel") && parameters.at("channel").get<int64_t>() < source.channels,
							"draw requires a channel in the clip");
						require(parameters.contains("samples") &&
								parameters.at("samples").size() == static_cast<size_t>(range.end - range.start),
							"draw sample count must match the range");
						range.inserted = range.end - range.start;
					}
					else
					{
						require(!parameters.contains("samples") && !parameters.contains("channel"),
							"samples and channel require draw");
					}
					if (!copy)
					{
						size_t copies = 0;
						forEveryAudioEvent(
							project, [&](const AudioEvent& each) { copies += each.clip == clip.id ? 1 : 0; });
						forEverySamplerClip(project, [&](ObjectId id) { copies += id == clip.id ? 1 : 0; });
						require(copies <= 1 || parameters.contains("sharedCopies"),
							"choose sharedCopies: newVersion or all");
						checkProcess(project, *event, parameters.value("sharedCopies", std::string("all")) == "all");
					}
					const std::shared_ptr<ProcessClipboard> paste = operation == "paste" ? *clipboard : nullptr;
					if (operation == "paste")
					{
						require(paste != nullptr, "the audio clipboard is empty");
						range.inserted =
							Resampler::outputLength(paste->media.length, paste->media.sampleRate, source.sampleRate);
					}
					if (operation == "insertSilence")
					{
						range.inserted = range.end - range.start;
						range.end = range.start;
					}
					require(range.inserted <= time::g_maxPosition - (source.length - (range.end - range.start)),
						"the edited clip exceeds the maximum length");
					parameters["start"] = range.start;
					parameters["end"] = range.end;
					parameters["inserted"] = range.inserted;
					parameters["revision"] = context.call("project.revision").at("revision");
					const auto folder = model.file().empty() ? unsavedMediaFolder(engine.settingsDirectory())
															 : mediaFolderFor(std::filesystem::u8path(model.file()));
					require(!folder.empty(), "there is no folder for the processed audio: save the project first");
					return engine.startJob("sampleEditor.process",
						[source = source, range, paste, operation = operation, folder = folder,
							parameters = std::move(parameters)](JobContext& job) mutable
						{
							std::vector<std::string> written;
							time::Samples total = source.length - (range.end - range.start) + range.inserted;
							if (operation == "copy")
								total = range.end - range.start;
							else if (operation == "cut")
								total = source.length;
							ProcessProgress progress{0, total};
							try
							{
								job.progress(0.0, source.path);
								job.checkCancelled();
								if (operation == "cut" || operation == "copy")
									parameters["clipboard"] = toJson(writeProcessFile(
										source, range, nullptr, "copy", folder, job, written, progress));
								if (operation != "copy")
									parameters["media"] = toJson(writeProcessFile(source, range,
										paste ? &paste->media : nullptr, operation, folder, job, written, progress,
										operation == "draw" ? parameters : Json::object()));
								job.checkCancelled();
								return job.command("sampleEditor.finishProcess", parameters);
							}
							catch (...)
							{
								removeFiles(written);
								throw;
							}
						});
				});
		}

	}

	void registerSampleEditorCommands(ProjectModel& model, Engine& engine)
	{
		registerProcessCommands(model, engine);
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json string = {{"type", "string"}};
		const Json boolean = {{"type", "boolean"}};
		const Json viewSchema = {{"type", "object"},
			{"properties",
				{{"open", boolean}, {"event", integer}, {"clip", integer}, {"track", integer}, {"media", integer},
					{"length", integer}, {"offset", integer}, {"eventLength", integer}, {"clipStart", integer},
					{"ratio", number}, {"selectionStart", integer}, {"selectionEnd", integer}}},
			{"required",
				{"open", "event", "clip", "track", "media", "length", "offset", "eventLength", "clipStart", "ratio",
					"selectionStart", "selectionEnd"}}};
		const std::string viewText =
			"The view: open; event, the audio event it shows, with its clip, track and media file (0 while it shows "
			"none, or its event is gone: an undo brings it back); length, the clip's length in sample frames; offset "
			"and eventLength, the section of the clip the event plays (clip frames); clipStart, the project sample "
			"where the clip's frame 0 would play, and ratio, project samples per clip frame, so frame f plays at "
			"clipStart + f * ratio; selectionStart and selectionEnd, the selection range in clip frames, none while "
			"they are the same. View state: no undo steps, not saved, and a new or loaded project closes it.";
		auto views = std::make_shared<SampleView>();
		const auto store = [views, &model](const SampleView& view, CommandContext& context)
		{
			*views = view;
			views->project = model.openedRevision();
			Json result = viewJson(model.project(), *views);
			context.emit("sampleEditor.changed", result);
			return result;
		};

		engine.registerCommand(
			{"sampleEditor.get", 1, "What the sample editor shows. " + viewText, objectSchema({}), viewSchema, true},
			[views, &model](CommandContext&) { return viewJson(model.project(), current(*views, model)); });

		engine.registerCommand(
			{"sampleEditor.open", 1,
				"Opens the sample editor (reference manual p. 613) with an audio event, such as the one double-clicked "
				"or selected in the project window, or shows that event in it if it is open; the selection range goes. "
				"Emits sampleEditor.changed. " +
					viewText,
				objectSchema({{"event", integer}}, {"event"}), viewSchema, false},
			[views, &model, store](CommandContext& context)
			{
				const auto id = context.parameters().at("event").get<ObjectId>();
				require(eventOf(model.project(), id) != nullptr, "no audio event " + std::to_string(id));
				SampleView view = current(*views, model);
				view.open = true;
				if (view.event != id)
					view.selectionStart = view.selectionEnd = 0;
				view.event = id;
				return store(view, context);
			});

		engine.registerCommand(
			{"sampleEditor.set", 1,
				"Changes the open sample editor's selection range (reference manual pp. 618, 635). select: all (the "
				"whole clip), none, event (the section the event plays) or loop (between the locators, within the "
				"clip; none if they are outside it). Or selectionStart and selectionEnd, in clip frames, from 0 to the "
				"clip's length; one of them alone keeps the other. With zeroCrossing (Snap to Zero Crossing), both "
				"ends move to the nearest frame within 50 ms where the audio crosses zero. Emits "
				"sampleEditor.changed. " +
					viewText,
				objectSchema({{"select", {{"type", "string"}, {"enum", {"all", "none", "event", "loop"}}}},
					{"selectionStart", integer}, {"selectionEnd", integer}, {"zeroCrossing", boolean}}),
				viewSchema, false},
			[views, &model, store](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project& project = model.project();
				SampleView view = current(*views, model);
				require(view.open, "the sample editor isn't open");
				const AudioEvent* event = eventOf(project, view.event);
				require(event != nullptr, "the sample editor shows no audio event");
				const time::Samples length = clipLength(project, *event);
				time::Samples start = parameters.value("selectionStart", view.selectionStart);
				time::Samples end = parameters.value("selectionEnd", view.selectionEnd);
				const std::string select = parameters.value("select", std::string());
				if (select == "all")
				{
					std::tie(start, end) = std::make_pair(time::Samples{0}, length);
				}
				else if (select == "none")
				{
					start = end = 0;
				}
				else if (select == "event")
				{
					std::tie(start, end) = std::make_pair(event->offset, event->offset + event->length);
				}
				else if (select == "loop")
				{
					const Json transport = context.call("transport.get", Json::object());
					const EventSpan span = eventSpan(project, *event);
					const auto frame = [&](const char* locator)
					{
						const auto at = transport.at(locator).get<time::Samples>();
						return event->offset +
							std::llround(eventFrames(project, *event, static_cast<double>(at - span.start)));
					};
					start = std::clamp<time::Samples>(frame("left"), 0, length);
					end = std::clamp<time::Samples>(frame("right"), 0, length);
					if (end <= start)
						start = end = 0;
				}
				require(start >= 0 && start <= end && end <= length,
					"the selection range must start before it ends, within the clip (0 to " + std::to_string(length) +
						" sample frames)");
				if (parameters.value("zeroCrossing", false) && start < end)
				{
					const MediaFile& media = project.media.at(project.clips.at(event->clip).media);
					start = zeroCrossingFrame(media, start, 0, length - 1);
					end = std::max(start, zeroCrossingFrame(media, end, 0, length - 1));
				}
				view.selectionStart = start;
				view.selectionEnd = end;
				return store(view, context);
			});

		engine.registerCommand(
			{"sampleEditor.close", 1, "Closes the sample editor. Emits sampleEditor.changed. " + viewText,
				objectSchema({}), viewSchema, false},
			[store](CommandContext& context) { return store(SampleView(), context); });

		const Json regionSchema = objectSchema(
			{{"id", integer}, {"name", string}, {"start", integer}, {"end", integer}}, {"id", "name", "start", "end"});
		const Json regionResultSchema = objectSchema({{"clip", integer}, {"region", regionSchema}}, {"clip", "region"});
		model.addEdit(engine,
			{"clip.addRegion", 1,
				"Adds a region to a clip (Add Region, Event or Range as Region, reference manual pp. 620, 624-625): a "
				"named section of its audio, from start to end in the clip's sample frames, start before end; name "
				"defaults to Region and a number that no other region of the clip has in its name. A clip's regions "
				"are in its regions, by start. Emits clip.changed. Returns the clip's id and the region.",
				objectSchema({{"clip", integer}, {"start", integer}, {"end", integer}, {"name", string}},
					{"clip", "start", "end"}),
				regionResultSchema, false},
			"Add region",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				const ClipRegion region{project.newId(), parameters.value("name", newRegionName(clip)),
					parameters.at("start").get<time::Samples>(), parameters.at("end").get<time::Samples>()};
				clip.regions.push_back(region);
				settle(project, clip, region);
				return regionResult(clip, region.id, context);
			});

		model.addEdit(engine,
			{"clip.setRegion", 1,
				"Renames a clip's region or moves its start or end (clip frames; reference manual p. 625); what isn't "
				"given stays. Emits clip.changed. Returns the clip's id and the region.",
				objectSchema(
					{{"clip", integer}, {"region", integer}, {"name", string}, {"start", integer}, {"end", integer}},
					{"clip", "region"}),
				regionResultSchema, false},
			"Change region",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				ClipRegion& region = regionOf(clip, parameters);
				region.name = parameters.value("name", region.name);
				region.start = parameters.value("start", region.start);
				region.end = parameters.value("end", region.end);
				const ClipRegion changed = region;
				settle(project, clip, changed);
				return regionResult(clip, changed.id, context);
			});

		model.addEdit(engine,
			{"clip.removeRegion", 1,
				"Removes a region from a clip (Remove Region, reference manual p. 626). Emits clip.changed.",
				objectSchema({{"clip", integer}, {"region", integer}}, {"clip", "region"}),
				objectSchema({{"clip", integer}}, {"clip"}), false},
			"Remove region",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				const ObjectId id = regionOf(clip, parameters).id;
				clip.regions.erase(std::remove_if(clip.regions.begin(), clip.regions.end(),
									   [id](const ClipRegion& region) { return region.id == id; }),
					clip.regions.end());
				return regionResult(clip, id, context);
			});
	}
}
