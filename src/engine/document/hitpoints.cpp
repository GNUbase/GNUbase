// SPDX-License-Identifier: GPL-3.0-or-later
// Hitpoints and slices (#143, reference manual pp. 636-647): the hitpoints are in the clip, so each command is one undo
// step and changes nothing if it fails (ProjectModel::addEdit). Detecting them reads the clip's whole file, so it runs
// as a job (analysis::OnsetDetector) that sets them with clip.setHitpoints; inserting or moving one measures the
// sound where it goes from a short read of the file on the command thread. What is made of them is made by the
// commands that make such things, run as part of event.fromHitpoints (ProjectModel::runEdit), so that it is one undo
// step with their events.
#include "engine/document/hitpoints.h"

#include "engine/analysis/onsets.h"
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/jobs/jobs.h"
#include "engine/time/tempomap.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	std::vector<bool> activeHitpoints(const Clip& clip)
	{
		const HitpointFilter& filter = clip.hitpointFilter;
		std::vector<bool> active(clip.hitpoints.size(), false);
		std::optional<time::Samples> last; // the one before that counts
		for (size_t i = 0; i < clip.hitpoints.size(); ++i)
		{
			const Hitpoint& hitpoint = clip.hitpoints[i];
			if (hitpoint.disabled)
				continue;
			if (!hitpoint.locked &&
				(hitpoint.peak < filter.threshold || hitpoint.intensity < filter.intensity ||
					(last && hitpoint.position - *last < filter.minimumLength)))
				continue;
			active[i] = true;
			last = hitpoint.position;
		}
		return active;
	}

	namespace
	{
		constexpr size_t g_readFrames = 65536;
		constexpr double g_defaultVelocity = 100.0 / 127.0;
		constexpr int g_defaultPitch = 36; // C1
		constexpr time::Ticks g_defaultNoteLength = time::g_ticksPerQuarter / 4;

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

		Clip& clipOf(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("clip").get<ObjectId>();
			const auto found = project.clips.find(id);
			require(found != project.clips.end(), "no clip " + std::to_string(id));
			return found->second;
		}

		time::Samples clipLength(const Project& project, const Clip& clip)
		{
			return project.media.at(clip.media).length;
		}

		Hitpoint& hitpointAt(Clip& clip, time::Samples position)
		{
			const auto found = std::find_if(clip.hitpoints.begin(), clip.hitpoints.end(),
				[position](const Hitpoint& hitpoint) { return hitpoint.position == position; });
			require(found != clip.hitpoints.end(),
				"clip " + std::to_string(clip.id) + " has no hitpoint at " + std::to_string(position));
			return *found;
		}

		// Checks where a hitpoint goes and keeps them by position.
		void settle(const Project& project, Clip& clip, time::Samples position)
		{
			const time::Samples length = clipLength(project, clip);
			require(position >= 0 && position < length,
				"a hitpoint must be within the clip (0 to " + std::to_string(length - 1) + ")");
			std::stable_sort(clip.hitpoints.begin(), clip.hitpoints.end(),
				[](const Hitpoint& a, const Hitpoint& b) { return a.position < b.position; });
			const auto twice = std::adjacent_find(clip.hitpoints.begin(), clip.hitpoints.end(),
				[](const Hitpoint& a, const Hitpoint& b) { return a.position == b.position; });
			require(twice == clip.hitpoints.end(),
				"clip " + std::to_string(clip.id) + " has a hitpoint at " + std::to_string(position) + " already");
		}

		Json hitpointsResult(const Clip& clip, CommandContext& context)
		{
			Json json = toJson(clip);
			Json hitpoints = json.at("hitpoints");
			context.emit("clip.changed", {{"clip", std::move(json)}});
			return {{"clip", clip.id}, {"hitpoints", std::move(hitpoints)}};
		}

		// The sound at a frame of a media file, as the detection measures the onsets it finds.
		analysis::Onset measure(const MediaFile& media, time::Samples position)
		{
			try
			{
				const std::unique_ptr<AudioFileReader> reader = AudioFileReader::open(media.path);
				const auto channels = static_cast<size_t>(std::max(reader->info().channels, 1));
				const time::Samples from =
					std::max<time::Samples>(0, position - analysis::OnsetDetector::lead(media.sampleRate));
				const auto count =
					static_cast<size_t>(position - from + analysis::OnsetDetector::window(media.sampleRate));
				std::vector<float> buffer(count * channels);
				reader->seek(from);
				const size_t read = reader->read(buffer.data(), count);
				return analysis::OnsetDetector::measure(media.sampleRate, reader->info().channels, buffer.data(), read,
					static_cast<size_t>(position - from));
			}
			catch (const AudioFileError& error)
			{
				throw CommandError("can't read " + media.path + ": " + error.what());
			}
		}

		// Gives a hitpoint inserted or moved by hand the peak and intensity of the sound where it is, so that the
		// filters and MIDI notes' velocities treat it as if detected there (#753). The intensity is relative to the
		// sharpest the detection found: the rise at the clip's other hitpoint of the highest intensity, preferring
		// detected (unlocked) ones, whose intensity isn't capped at 1, divided by that intensity; without one, the
		// hitpoint is the sharpest if its level rises at all.
		void measureHitpoint(const Project& project, const Clip& clip, Hitpoint& hitpoint)
		{
			const MediaFile& media = project.media.at(clip.media);
			const analysis::Onset sound = measure(media, hitpoint.position);
			const auto better = [](const Hitpoint& other, const Hitpoint* than)
			{
				if (than == nullptr || other.locked != than->locked)
					return than == nullptr || !other.locked;
				return other.intensity > than->intensity;
			};
			const Hitpoint* reference = nullptr;
			for (const Hitpoint& other : clip.hitpoints)
				if (&other != &hitpoint && other.intensity > 0.0 && better(other, reference))
					reference = &other;
			double sharpest = 0.0;
			if (reference != nullptr)
				sharpest = measure(media, reference->position).rise / reference->intensity;
			const double rise = std::max(sound.rise, 0.0);
			hitpoint.peak = sound.peak;
			if (sharpest > 0.0)
				hitpoint.intensity = std::min(rise / sharpest, 1.0);
			else
				hitpoint.intensity = rise > 0.0 ? 1.0 : 0.0;
		}

		// The hitpoints that count from frame from to before to, by position.
		std::vector<const Hitpoint*> counting(const Clip& clip, time::Samples from, time::Samples to)
		{
			const std::vector<bool> active = activeHitpoints(clip);
			std::vector<const Hitpoint*> found;
			for (size_t i = 0; i < clip.hitpoints.size(); ++i)
				if (active[i] && clip.hitpoints[i].position >= from && clip.hitpoints[i].position < to)
					found.push_back(&clip.hitpoints[i]);
			return found;
		}

		// Where a clip frame of an event plays, in project samples.
		time::Samples sampleOf(const Project& project, const AudioEvent& event, time::Samples frame)
		{
			const EventSpan span = eventSpan(project, event);
			return span.start + std::llround(static_cast<double>(frame - event.offset) * span.ratio);
		}

		time::Ticks ticksOf(const Project& project, time::Samples sample)
		{
			return project.tempo.toTicks(sample, project.sampleRate);
		}

		// A project sample in a track's time base.
		int64_t inTrack(const Project& project, ObjectId track, time::Samples sample)
		{
			return time::rebase({time::TimeBase::Linear, sample}, project.trackById.at(track).timeBase, project.tempo,
				project.sampleRate)
				.value;
		}
	}

	std::vector<Hitpoint> detectHitpoints(const MediaFile& media, JobContext* job)
	{
		std::vector<analysis::Onset> onsets;
		try
		{
			const std::unique_ptr<AudioFileReader> reader = AudioFileReader::open(media.path);
			const int channels = reader->info().channels;
			const auto frames = static_cast<double>(std::max<time::Samples>(1, reader->info().frames));
			analysis::OnsetDetector detector(media.sampleRate, channels);
			std::vector<float> buffer(g_readFrames * static_cast<size_t>(std::max(channels, 1)));
			size_t done = 0;
			for (size_t read = g_readFrames; read == g_readFrames;)
			{
				if (job != nullptr)
					job->checkCancelled();
				read = reader->read(buffer.data(), g_readFrames);
				detector.feed(buffer.data(), read);
				done += read;
				if (job != nullptr)
					job->progress(std::min(1.0, static_cast<double>(done) / frames), media.path);
			}
			onsets = detector.finish();
		}
		catch (const AudioFileError& error)
		{
			throw CommandError("can't read " + media.path + ": " + error.what());
		}
		double sharpest = 0.0;
		for (const analysis::Onset& onset : onsets)
			sharpest = std::max(sharpest, onset.rise);
		std::vector<Hitpoint> hitpoints;
		for (const analysis::Onset& onset : onsets)
			if (onset.frame < media.length)
				hitpoints.push_back(
					{onset.frame, onset.peak, sharpest > 0.0 ? onset.rise / sharpest : 0.0, false, false});
		return hitpoints;
	}

	void registerHitpointCommands(ProjectModel& model, Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json boolean = {{"type", "boolean"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const Json hitpointSchema = objectSchema({{"position", integer}, {"peak", number}, {"intensity", number},
													 {"locked", boolean}, {"disabled", boolean}, {"active", boolean}},
			{"position", "peak", "intensity", "locked", "disabled", "active"});
		const Json resultSchema = objectSchema(
			{{"clip", integer}, {"hitpoints", {{"type", "array"}, {"items", hitpointSchema}}}}, {"clip", "hitpoints"});
		const std::string hitpointsText =
			"A clip's hitpoints are in its hitpoints, by position: each with its position in the clip's sample "
			"frames, its peak (dBFS, the loudest sample in the 20 ms from there) and intensity (0-1, how sharply the "
			"level rises there, 1 for the sharpest the detection found), whether it is locked and disabled, and "
			"active, whether it counts: not disabled, and locked or let through by the clip's hitpointFilter "
			"(clip.setHitpointFilter). Emits clip.changed. Returns the clip's id and its hitpoints.";

		engine.registerCommand(
			{"clip.detectHitpoints", 1,
				"Finds where sounds start in a clip (calculating hitpoints, reference manual p. 637) and makes them "
				"its hitpoints, in place of those it has then: where the level, or that of the high frequencies, "
				"rises by 6 dB or more within 5 ms from at least -60 dBFS, 20 ms apart at least. Edit Hitpoints "
				"restores the hitpoints found this way after Remove All. It reads the clip's whole file, so it runs "
				"as a job and can't be part of a history.transaction: this returns at once, job.progress events say "
				"how much of the file it read, job.cancel stops it, and once it is done it sets them with "
				"clip.setHitpoints, one undo step, whose result is the job's result. A clip removed meanwhile, another "
				"project opened or the clip's media file relinked fails the job.",
				objectSchema({{"clip", integer}}, {"clip"}), objectSchema({{"job", Jobs::schema()}}, {"job"}), false},
			[&model, &engine](CommandContext& context)
			{
				const auto id = context.parameters().at("clip").get<ObjectId>();
				const Project& project = model.project();
				const auto found = project.clips.find(id);
				require(found != project.clips.end(), "no clip " + std::to_string(id));
				return Json{{"job",
					engine.jobs().start("clip.detectHitpoints",
						[id, media = project.media.at(found->second.media), opened = model.openedRevision()](
							JobContext& job)
						{
							Json hitpoints = Json::array();
							for (const Hitpoint& hitpoint : detectHitpoints(media, &job))
								hitpoints.push_back({{"position", hitpoint.position}, {"peak", hitpoint.peak},
									{"intensity", hitpoint.intensity}});
							return job.command("clip.setHitpoints",
								{{"clip", id}, {"hitpoints", std::move(hitpoints)}, {"project", opened},
									{"path", media.path}});
						})}};
			});

		model.addEdit(engine,
			{"clip.setHitpoints", 1,
				"Makes hitpoints a clip's hitpoints, in place of those it had: what clip.detectHitpoints found, or "
				"hitpoints from elsewhere. Each has its position (a clip frame, each one once), peak (dBFS) and "
				"intensity (0-1), and may say whether it is locked and disabled (default false); active is ignored, "
				"so that a clip's hitpoints can be given as they come. With project (project.revision's), it fails if "
				"another project was opened since; with path, if the clip's media file has another path now (it was "
				"relinked), so that a job's hitpoints never go to another clip or file. " +
					hitpointsText,
				objectSchema(
					{{"clip", integer}, {"project", integer}, {"path", {{"type", "string"}}},
						{"hitpoints",
							{{"type", "array"},
								{"items",
									objectSchema({{"position", integer}, {"peak", number}, {"intensity", number},
													 {"locked", boolean}, {"disabled", boolean}, {"active", boolean}},
										{"position", "peak", "intensity"})}}}},
					{"clip", "hitpoints"}),
				resultSchema, false},
			"Set hitpoints",
			[&model](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				require(!parameters.contains("project") ||
						parameters.at("project").get<uint64_t>() == model.openedRevision(),
					"another project was opened meanwhile");
				Clip& clip = clipOf(project, parameters);
				require(!parameters.contains("path") ||
						parameters.at("path").get<std::string>() == project.media.at(clip.media).path,
					"clip " + std::to_string(clip.id) + "'s media file was relinked meanwhile");
				const time::Samples length = clipLength(project, clip);
				std::vector<Hitpoint> hitpoints;
				for (const Json& given : parameters.at("hitpoints"))
				{
					Hitpoint hitpoint;
					hitpoint.position = given.at("position").get<time::Samples>();
					hitpoint.peak = given.at("peak").get<double>();
					hitpoint.intensity = given.at("intensity").get<double>();
					hitpoint.locked = given.value("locked", false);
					hitpoint.disabled = given.value("disabled", false);
					require(hitpoint.position >= 0 && hitpoint.position < length,
						"a hitpoint must be within the clip (0 to " + std::to_string(length - 1) + ")");
					require(std::isfinite(hitpoint.peak), "a hitpoint's peak must be a number of dBFS");
					require(hitpoint.intensity >= 0.0 && hitpoint.intensity <= 1.0,
						"a hitpoint's intensity must be from 0 to 1");
					hitpoints.push_back(hitpoint);
				}
				std::stable_sort(hitpoints.begin(), hitpoints.end(),
					[](const Hitpoint& a, const Hitpoint& b) { return a.position < b.position; });
				const auto twice = std::adjacent_find(hitpoints.begin(), hitpoints.end(),
					[](const Hitpoint& a, const Hitpoint& b) { return a.position == b.position; });
				if (twice != hitpoints.end())
					throw CommandError("two hitpoints at " + std::to_string(twice->position));
				clip.hitpoints = std::move(hitpoints);
				return hitpointsResult(clip, context);
			});

		model.addEdit(engine,
			{"clip.setHitpointFilter", 1,
				"Changes which of a clip's hitpoints count (the Hitpoints section's filters, reference manual p. 638): "
				"threshold, the peak in dBFS they need (-60 to 0, -60 lets every one through); intensity, the "
				"intensity they need (0-1); minimumLength, the clip frames they must start after the one before that "
				"counts. Locked hitpoints always count, disabled ones never. What isn't given stays. " +
					hitpointsText,
				objectSchema(
					{{"clip", integer}, {"threshold", number}, {"intensity", number}, {"minimumLength", integer}},
					{"clip"}),
				resultSchema, false},
			"Filter hitpoints",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				HitpointFilter& filter = clip.hitpointFilter;
				filter.threshold = parameters.value("threshold", filter.threshold);
				filter.intensity = parameters.value("intensity", filter.intensity);
				filter.minimumLength = parameters.value("minimumLength", filter.minimumLength);
				require(filter.threshold >= g_minHitpointThreshold && filter.threshold <= 0.0,
					"threshold must be from -60 to 0 dBFS");
				require(filter.intensity >= 0.0 && filter.intensity <= 1.0, "intensity must be from 0 to 1");
				require(filter.minimumLength >= 0, "minimumLength must not be negative");
				return hitpointsResult(clip, context);
			});

		model.addEdit(engine,
			{"clip.addHitpoint", 1,
				"Inserts a hitpoint into a clip at position, a clip frame (Alt-click with Edit Hitpoints, reference "
				"manual p. 640). It is locked, so that it counts whatever the filters, and gets the peak and "
				"intensity of the sound there, measured from the clip's file as the detection measures those it "
				"finds, the intensity relative to the sharpest the detection found (the rise at the hitpoint of the "
				"highest intensity, detected ones first, measured again). Fails if the clip's file can't be read. " +
					hitpointsText,
				objectSchema({{"clip", integer}, {"position", integer}}, {"clip", "position"}), resultSchema, false},
			"Insert hitpoint",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				const auto position = parameters.at("position").get<time::Samples>();
				clip.hitpoints.push_back({position, g_minHitpointThreshold, 0.0, true, false});
				settle(project, clip, position);
				measureHitpoint(project, clip, hitpointAt(clip, position));
				return hitpointsResult(clip, context);
			});

		model.addEdit(engine,
			{"clip.setHitpoint", 1,
				"Edits the hitpoint of a clip at position (reference manual p. 640): moveTo moves it to that clip "
				"frame, which locks it unless locked is given and measures its peak and intensity there as "
				"clip.addHitpoint does (failing if the file can't be read); locked locks or unlocks it (Lock "
				"Hitpoint), disabled "
				"disables or enables it (Disable Hitpoint); both false reset it (Enable/Unlock Hitpoints). What isn't "
				"given stays. " +
					hitpointsText,
				objectSchema({{"clip", integer}, {"position", integer}, {"moveTo", integer}, {"locked", boolean},
								 {"disabled", boolean}},
					{"clip", "position"}),
				resultSchema, false},
			"Change hitpoint",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				Hitpoint& hitpoint = hitpointAt(clip, parameters.at("position").get<time::Samples>());
				if (parameters.contains("moveTo"))
				{
					hitpoint.position = parameters.at("moveTo").get<time::Samples>();
					hitpoint.locked = true;
				}
				hitpoint.locked = parameters.value("locked", hitpoint.locked);
				hitpoint.disabled = parameters.value("disabled", hitpoint.disabled);
				const time::Samples position = hitpoint.position;
				settle(project, clip, position);
				if (parameters.contains("moveTo"))
					measureHitpoint(project, clip, hitpointAt(clip, position));
				return hitpointsResult(clip, context);
			});

		model.addEdit(engine,
			{"clip.removeHitpoints", 1,
				"Removes all of a clip's hitpoints, detected and inserted (Remove All, reference manual p. 638); "
				"clip.detectHitpoints finds them again. " +
					hitpointsText,
				objectSchema({{"clip", integer}}, {"clip"}), resultSchema, false},
			"Remove hitpoints",
			[](Project& project, CommandContext& context)
			{
				Clip& clip = clipOf(project, context.parameters());
				clip.hitpoints.clear();
				return hitpointsResult(clip, context);
			});

		model.addEdit(engine,
			{"event.fromHitpoints", 1,
				"Makes something of the hitpoints of an audio event's clip that count and are inside the section it "
				"plays (the Hitpoints section's Create section, reference manual pp. 638-647). create: slices cuts "
				"every unlocked event that plays the clip at its hitpoints and puts the slices into an audio part "
				"in its place (slices of an event in a part stay in it), returning the parts, or those slices, in "
				"events; events cuts this event at them, returning it and the new events in events; regions makes a "
				"region of the clip from the event's start to the first hitpoint, from each to the next and from the "
				"last to the event's end, returning them in regions; markers adds a position marker at each to the "
				"active marker track (adding one without), returning them in markers; groove makes the project's "
				"quantize settings a groove of them, as long as the event and starting where it does "
				"(quantize.set), returning those settings in quantize; midiNotes adds a MIDI part as long as the "
				"event with a note at each, to track (a MIDI or instrument track, such as the first selected one) or "
				"to a new MIDI track named after the clip, returning track and part. The notes have pitch (default "
				"36, C1) and length (ticks, default a sixteenth); velocityMode dynamic (the default) gives the "
				"loudest hitpoint velocity 1 and the others less as their peaks are quieter, fixed gives every note "
				"velocity (0-1, default 100/127).",
				objectSchema({{"event", integer},
								 {"create",
									 {{"type", "string"},
										 {"enum", {"slices", "events", "regions", "markers", "groove", "midiNotes"}}}},
								 {"velocityMode", {{"type", "string"}, {"enum", {"dynamic", "fixed"}}}},
								 {"velocity", number}, {"pitch", integer}, {"length", integer}, {"track", integer}},
					{"event", "create"}),
				objectSchema({{"events", ids}, {"regions", ids}, {"markers", ids}, {"quantize", {{"type", "object"}}},
					{"track", integer}, {"part", integer}}),
				false},
			"Create from hitpoints",
			[&model](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto id = parameters.at("event").get<ObjectId>();
				const auto found = project.audioEvents.find(id);
				require(found != project.audioEvents.end(), "no audio event " + std::to_string(id));
				const AudioEvent event = found->second;
				const Clip clip = project.clips.at(event.clip);
				const time::Samples end = event.offset + event.length;
				const std::vector<const Hitpoint*> points = counting(clip, event.offset, end);
				require(!points.empty(),
					"audio event " + std::to_string(id) + " has no hitpoints that count: detect them first");
				const std::string create = parameters.at("create").get<std::string>();
				const auto run = [&](const char* command, const Json& commandParameters)
				{ return model.runEdit(command, project, commandParameters, context); };

				if (create == "slices" || create == "events")
				{
					require(!event.lock, "audio event " + std::to_string(id) + " is locked");
					require(points.size() > 1 || points.front()->position > event.offset,
						"audio event " + std::to_string(id) + " has no hitpoints inside it to cut it at");
					std::vector<ObjectId> cut = {id};
					if (create == "slices")
						for (const auto& [other, each] : project.audioEvents)
							if (other != id && each.clip == clip.id && !each.lock)
								cut.push_back(other);
					Json made = Json::array();
					for (const ObjectId each : cut)
					{
						const AudioEvent& audio = project.audioEvents.at(each);
						Json positions = Json::array();
						for (const Hitpoint* point : counting(clip, audio.offset + 1, audio.offset + audio.length))
							positions.push_back(sampleOf(project, audio, point->position));
						if (positions.empty())
							continue;
						const bool inPart = audio.part != 0;
						Json pieces = run("event.split", {{"events", {each}}, {"positions", positions}}).at("events");
						pieces.insert(pieces.begin(), each);
						if (create == "events" || inPart)
							made.insert(made.end(), pieces.begin(), pieces.end());
						else
							made.push_back(run("event.toPart", {{"events", pieces}}).at("events").at(0));
					}
					return Json{{"events", made}};
				}
				if (create == "regions")
				{
					std::vector<time::Samples> edges = {event.offset};
					for (const Hitpoint* point : points)
						if (point->position > event.offset)
							edges.push_back(point->position);
					edges.push_back(end);
					Json regions = Json::array();
					for (size_t i = 1; i < edges.size(); ++i)
						regions.push_back(
							run("clip.addRegion", {{"clip", clip.id}, {"start", edges[i - 1]}, {"end", edges[i]}})
								.at("region")
								.at("id"));
					return Json{{"regions", regions}};
				}
				if (create == "markers")
				{
					if (project.trackById.count(project.activeMarkerTrack) == 0)
						run("track.add", {{"type", "marker"}});
					const ObjectId track = project.activeMarkerTrack;
					Json markers = Json::array();
					for (const Hitpoint* point : points)
						markers.push_back(run("marker.add",
							{{"track", track},
								{"position", inTrack(project, track, sampleOf(project, event, point->position))}})
								.at("marker")
								.at("id"));
					return Json{{"markers", markers}};
				}
				const EventSpan span = eventSpan(project, event);
				const time::Ticks start = ticksOf(project, span.start);
				const time::Ticks length = std::max<time::Ticks>(1, ticksOf(project, span.end) - start);
				const auto at = [&](const Hitpoint* point)
				{
					return std::clamp<time::Ticks>(
						ticksOf(project, sampleOf(project, event, point->position)) - start, 0, length - 1);
				};
				if (create == "groove")
				{
					Json grooved = Json::array();
					time::Ticks last = -1;
					for (const Hitpoint* point : points)
						if (const time::Ticks position = at(point); position > last)
						{
							grooved.push_back({{"position", position}});
							last = position;
						}
					const Json settings = {{"groove", {{"length", length}, {"points", grooved}}}, {"origin", start}};
					return Json{{"quantize", run("quantize.set", {{"settings", settings}})}};
				}
				// midiNotes
				ObjectId track = parameters.value("track", ObjectId{0});
				if (track != 0)
				{
					const auto target = project.trackById.find(track);
					require(target != project.trackById.end() &&
							(target->second.type == TrackType::Midi || target->second.type == TrackType::Instrument),
						"track " + std::to_string(track) + " isn't a MIDI or instrument track");
				}
				else
				{
					track =
						run("track.add", {{"type", "midi"}, {"name", clip.name}}).at("track").at("id").get<ObjectId>();
				}
				const int64_t partStart = inTrack(project, track, span.start);
				const ObjectId part = run("midiPart.add",
					{{"track", track}, {"position", partStart},
						{"length", std::max<int64_t>(1, inTrack(project, track, span.end) - partStart)},
						{"name", clip.name}})
										  .at("part")
										  .at("id")
										  .get<ObjectId>();
				const bool dynamic = parameters.value("velocityMode", std::string("dynamic")) == "dynamic";
				const double velocity = parameters.value("velocity", g_defaultVelocity);
				const int pitch = parameters.value("pitch", g_defaultPitch);
				const time::Ticks noteLength = parameters.value("length", g_defaultNoteLength);
				require(velocity >= 0.0 && velocity <= 1.0, "velocity must be from 0 to 1");
				require(pitch >= 0 && pitch <= 127, "pitch must be from 0 to 127");
				require(noteLength > 0, "length must be positive");
				double loudest = points.front()->peak;
				for (const Hitpoint* point : points)
					loudest = std::max(loudest, point->peak);
				Json notes = Json::array();
				for (const Hitpoint* point : points)
					notes.push_back({{"position", at(point)}, {"length", noteLength}, {"pitch", pitch},
						{"velocity",
							dynamic ? std::clamp(std::pow(10.0, (point->peak - loudest) / 20.0), 1.0 / 127.0, 1.0)
									: velocity}});
				run("note.add", {{"part", part}, {"notes", notes}});
				return Json{{"track", track}, {"part", part}};
			});
	}
}
