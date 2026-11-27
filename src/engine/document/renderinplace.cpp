// SPDX-License-Identifier: GPL-3.0-or-later
// Render in place and freeze (#116, docs/features/render-in-place.md): track.renderInPlace, track.freeze and
// track.unfreeze. Both render with mixdown.export, from a point inside the channel's strip, before they change the
// project, so a render is bit-identical to an export of the same range; then they change it in one undo step.
#include "engine/document/renderinplace.h"
#include "engine/audiofile/audiofile.h"
#include "engine/document/channelformat.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/rendersnapshot.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/jobs/jobs.h"
#include "engine/media/backup.h"
#include "engine/time/tempomap.h"
#include <atomic>
#include <set>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iterator>
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
		namespace fs = std::filesystem;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		class RenderProgress final : public JobContext
		{
		public:
			RenderProgress(JobContext& job, size_t index, size_t count)
				: m_job(job), m_index(static_cast<double>(index)), m_count(static_cast<double>(count))
			{
			}
			bool cancelled() const override { return m_job.cancelled(); }
			void progress(double fraction, const std::string& detail) override
			{
				m_job.progress((m_index + fraction) / m_count, detail);
			}
			Json command(std::string name, Json parameters) override
			{
				return m_job.command(std::move(name), std::move(parameters));
			}

		private:
			JobContext& m_job;
			double m_index;
			double m_count;
		};

		// Where what plays on a channel starts and ends, in the project's samples: its events and notes, and on an
		// instrument channel the notes of the MIDI channels that play it.
		struct Span
		{
			std::optional<time::Samples> start;
			time::Samples end = 0;
		};

		struct EventRender
		{
			time::Samples start = 0;
			time::Samples end = 0;
			std::vector<ObjectId> events;
		};

		bool feedsInstrument(const RenderSnapshot& snapshot, const RenderChannel& channel, ObjectId instrument)
		{
			const bool output = channel.type == TrackType::Midi && channel.output.kind == RenderTarget::Kind::Channel &&
				snapshot.channels.at(static_cast<size_t>(channel.output.index)).id == instrument;
			return output ||
				std::any_of(channel.midiSendOutputs.begin(), channel.midiSendOutputs.end(), [&](int target)
					{ return target >= 0 && snapshot.channels.at(static_cast<size_t>(target)).id == instrument; });
		}

		// Event and part boundaries, rather than note boundaries, define separate files and adjacent blocks.
		std::vector<EventRender> eventRenders(
			const Project& project, const RenderSnapshot& snapshot, const Track& track, const Json& parameters)
		{
			const std::string mode = parameters.value("mode", std::string("oneEvent"));
			std::set<ObjectId> selected;
			if (parameters.contains("events"))
				for (const auto& event : parameters.at("events"))
					selected.insert(event.get<ObjectId>());
			std::vector<EventRender> events;
			const auto add = [&](ObjectId id, time::Samples start, time::Samples end)
			{
				if (!selected.empty() && selected.count(id) == 0)
					return;
				if (parameters.contains("start"))
				{
					start = std::max(start, parameters.at("start").get<time::Samples>());
					end = std::min(end, parameters.at("end").get<time::Samples>());
				}
				if (end > start)
					events.push_back({start, end, {id}});
			};
			std::set<ObjectId> inputs{track.id};
			for (const auto& channel : snapshot.channels)
			{
				if (feedsInstrument(snapshot, channel, track.channel))
					inputs.insert(channel.track);
				if (channel.track != track.id)
					continue;
				for (const auto& event : channel.audioEvents)
					if (project.audioEvents.at(event.id).part == 0 || selected.count(event.id) != 0)
					{
						const double ratio =
							snapshot.sampleRate / snapshot.media.at(static_cast<size_t>(event.media)).sampleRate;
						add(event.id, event.start,
							event.start +
								static_cast<time::Samples>(std::llround(static_cast<double>(event.length) * ratio)));
					}
			}
			const auto parts = [&](const auto& collection)
			{
				for (const auto& [id, part] : collection)
					if (inputs.count(part.track) != 0)
					{
						auto end = part.position;
						end.value += part.length;
						add(id, time::toSamples(part.position, project.tempo, project.sampleRate),
							time::toSamples(end, project.tempo, project.sampleRate));
					}
			};
			parts(project.audioParts);
			parts(project.midiParts);
			std::sort(events.begin(), events.end(), [](const auto& a, const auto& b)
				{ return std::tie(a.start, a.end, a.events) < std::tie(b.start, b.end, b.events); });
			std::vector<EventRender> groups;
			for (auto& event : events)
			{
				if (groups.empty() || mode == "separateEvents" ||
					(mode == "blockEvents" && event.start > groups.back().end))
				{
					groups.push_back(std::move(event));
				}
				else
				{
					groups.back().end = std::max(groups.back().end, event.end);
					groups.back().events.insert(groups.back().events.end(), event.events.begin(), event.events.end());
				}
			}
			return groups;
		}

		Span spanOf(const RenderSnapshot& snapshot, ObjectId channel)
		{
			Span span;
			const auto add = [&span](time::Samples start, time::Samples end)
			{
				span.start = std::min(span.start.value_or(start), start);
				span.end = std::max(span.end, end);
			};
			for (const RenderChannel& candidate : snapshot.channels)
			{
				const bool plays = candidate.output.kind == RenderTarget::Kind::Channel &&
					candidate.type == TrackType::Midi &&
					snapshot.channels[static_cast<size_t>(candidate.output.index)].id == channel;
				if (candidate.id != channel && !plays)
					continue;
				for (const RenderAudioEvent& event : candidate.audioEvents)
				{
					const double ratio =
						snapshot.sampleRate / snapshot.media[static_cast<size_t>(event.media)].sampleRate;
					add(event.start,
						event.start +
							static_cast<time::Samples>(std::llround(static_cast<double>(event.length) * ratio)));
				}
				for (const RenderNote& note : candidate.notes)
					add(note.start, note.end);
			}
			return span;
		}

		const Track& renderable(const Project& project, ObjectId id)
		{
			const auto track = project.trackById.find(id);
			require(track != project.trackById.end(), "there is no track " + std::to_string(id));
			require(track->second.type == TrackType::Audio || track->second.type == TrackType::Instrument,
				"only audio and instrument tracks render; track " + std::to_string(id) + " is neither");
			return track->second;
		}

		std::vector<ObjectId> trackList(const Json& parameters)
		{
			auto ids = parameters.at("tracks").get<std::vector<ObjectId>>();
			// Kept here too: render helpers also run handlers directly, without the command schema validator.
			require(!ids.empty(), "tracks must not be empty");
			std::vector<ObjectId> sorted = ids;
			std::sort(sorted.begin(), sorted.end());
			require(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "tracks names a track twice");
			return ids;
		}

		// A file name from a track's name.
		std::string fileNameOf(const Track& track)
		{
			return track.name.empty() ? "Track " + std::to_string(track.id) : track.name;
		}

		// The tail in seconds after a range that ends at end: tail, or tailBars bars of the signature there, at the
		// tempo there and after (Tail Mode, reference manual p. 1473).
		double tailAfter(const Project& project, const Json& parameters, time::Samples end)
		{
			if (!parameters.contains("tailBars"))
				return parameters.value("tail", 0.0);
			const double seconds = static_cast<double>(end) / project.sampleRate;
			const double tick = project.tempo.toTicksExact(seconds);
			const time::TimeSignature signature = project.signatures.signatureAt(static_cast<time::Ticks>(tick));
			const double bar = static_cast<double>(time::g_ticksPerQuarter) * 4.0 * signature.numerator /
				static_cast<double>(signature.denominator);
			return project.tempo.toSecondsExact(tick + (parameters.at("tailBars").get<double>() * bar)) - seconds;
		}

		// Mix down to One Audio File (reference manual p. 1473): sums the files, rendered from one start, into a new
		// file in format, as long as the longest. Integer formats round without dither.
		Json mixFiles(
			const std::vector<std::string>& paths, const fs::path& path, const std::string& format, double rate)
		{
			constexpr std::array<std::pair<const char*, SampleFormat>, 5> formats = {
				{{"int16", SampleFormat::Int16}, {"int24", SampleFormat::Int24}, {"int32", SampleFormat::Int32},
					{"float32", SampleFormat::Float32}, {"float64", SampleFormat::Float64}}};
			std::vector<std::unique_ptr<AudioFileReader>> readers;
			readers.reserve(paths.size());
			for (const std::string& file : paths)
				readers.push_back(AudioFileReader::open(file));
			const int channels = readers.front()->info().channels;
			for (const auto& reader : readers)
				require(reader->info().channels == channels, "the tracks to mix down put out different formats");
			WriteOptions options;
			for (const auto& [name, value] : formats)
				if (format == name)
					options.sampleFormat = value;
			options.sampleRate = rate;
			options.channels = channels;
			const auto writer = AudioFileWriter::create(path.u8string(), options);
			constexpr size_t chunk = 4096;
			const auto width = static_cast<size_t>(channels);
			std::vector<float> sum(chunk * width);
			std::vector<float> read(chunk * width);
			time::Samples frames = 0;
			for (size_t got = chunk; got == chunk;)
			{
				got = 0;
				std::fill(sum.begin(), sum.end(), 0.0f);
				for (const auto& reader : readers) // in the tracks' order, so the sum is the same every time
				{
					const size_t n = reader->read(read.data(), chunk);
					for (size_t i = 0; i < n * width; ++i)
						sum[i] += read[i];
					got = std::max(got, n);
				}
				writer->write(sum.data(), got);
				frames += static_cast<time::Samples>(got);
			}
			writer->close();
			return {{"path", path.u8string()}, {"frames", frames}};
		}
	}

	bool lockedByFreeze(const Project& project, const Channel& channel)
	{
		const auto frozen = [&project](ObjectId track)
		{
			const auto found = project.trackById.find(track);
			return found != project.trackById.end() && found->second.frozen.has_value();
		};
		if (frozen(channel.track))
			return true;
		const auto track = project.trackById.find(channel.track);
		if (track == project.trackById.end() || track->second.type != TrackType::Midi)
			return false;
		const auto output = project.channels.find(channel.output);
		return output != project.channels.end() && frozen(output->second.track);
	}

	struct ProjectModel::TrackRender
	{
		Json parameters;
		bool freeze = false;
		std::vector<ObjectId> tracks;
		std::vector<ObjectId> lockedTracks;
		std::vector<time::Samples> starts;
		std::vector<time::Samples> ends;
		std::vector<Jobs::Work> work;
		std::vector<Json> files;
		Json input;
		std::set<ObjectId> inputObjects;
		// Mix down to One Audio File: the files go into one of this name (a free one), and then away; or empty.
		fs::path mixdown;
		std::string sampleFormat;
		double sampleRate = 0.0;
		uint64_t job = 0;
		std::atomic<bool> ready{false};
	};

	Json ProjectModel::renderInput(const Project& project, const TrackRender& render)
	{
		const auto snapshot = compile(project, 0);
		const Json whole = toJson(project);
		Json input = Json::object();
		const bool preFader = render.freeze ||
			render.parameters.value("processing", std::string("channelSettings")) != "completeSignalPath";
		for (const char* key : {"sampleRate", "tempo", "panLaw", "rootKey", "transpose", "autoFades"})
			if (whole.contains(key))
				input[key] = whole.at(key);
		std::set<ObjectId> tracks;
		std::map<ObjectId, std::set<size_t>> ranges;
		for (size_t i = 0; i < render.tracks.size(); ++i)
		{
			const auto track = project.trackById.find(render.tracks[i]);
			if (track == project.trackById.end())
				return nullptr;
			tracks.insert(track->first);
			ranges[track->first].insert(i);
			// MIDI outputs and MIDI sends can feed a rendered instrument from another track.
			for (const auto& channel : snapshot->channels)
			{
				if (feedsInstrument(*snapshot, channel, track->second.channel))
				{
					tracks.insert(channel.track);
					ranges[channel.track].insert(i);
				}
			}
		}

		for (const ObjectId id : tracks)
		{
			const Track& track = project.trackById.at(id);
			Json value = toJson(track);
			for (const char* key : {"name", "color", "picture", "gmNames", "groupEditing", "versions", "activeVersion"})
				value.erase(key);
			input["tracks"].push_back(value);
			Json channel = toJson(project.channels.at(track.channel));
			for (const char* key :
				{"notes", "listen", "monitor", "recordEnable", "input", "midiInput", "inputTransformer"})
				channel.erase(key);
			if (preFader && track.type != TrackType::Midi)
				for (const char* key : {"volume", "pan", "panner", "panLeft", "panRight", "panBypass", "sends", "mute",
						 "solo", "soloDefeat"})
					channel.erase(key);
			if (render.freeze)
			{
				channel.erase("eq");
				channel.erase("strip");
			}
			input["channels"].push_back(channel);
			for (const ObjectId slot : project.channels.at(track.channel).slots)
				input["slots"].push_back(toJson(project.slots.at(slot)));
			for (const auto& [laneId, lane] : project.automation)
				if (lane.track == id)
				{
					const auto type = lane.target.type;
					const bool fader = type == AutomationTargetType::Volume || type == AutomationTargetType::Pan ||
						type == AutomationTargetType::PanLeft || type == AutomationTargetType::PanRight ||
						type == AutomationTargetType::Mute;
					if (!preFader || track.type == TrackType::Midi || !fader)
						input["automation"].push_back(toJson(lane));
				}
		}
		std::set<ObjectId> maps;
		std::set<ObjectId> articulations;
		std::set<ObjectId> patterns;
		for (const auto id : tracks)
		{
			maps.insert(project.trackById.at(id).drumMap);
			articulations.insert(project.trackById.at(id).articulationMap);
		}
		for (const auto& [id, part] : project.midiParts)
			if (tracks.count(part.track) != 0 && part.pattern != 0)
				patterns.insert(part.pattern);
		for (const auto& [key, ids] : std::vector<std::pair<std::string, std::set<ObjectId>>>{
				 {"drumMaps", maps}, {"articulationMaps", articulations}, {"patterns", patterns}})
			for (const auto& item : whole.at(key))
				if (ids.count(item.at("id").get<ObjectId>()) != 0)
					input[key].push_back(item);
		std::set<ObjectId> events;
		for (const auto& channel : snapshot->channels)
			if (tracks.count(channel.track) != 0)
				for (const auto& event : channel.audioEvents)
				{
					const auto& media = snapshot->media.at(static_cast<size_t>(event.media));
					const auto length = static_cast<time::Samples>(
						std::llround(static_cast<double>(event.length) * snapshot->sampleRate / media.sampleRate));
					for (const size_t i : ranges.at(channel.track))
						if (event.start < render.ends[i] && event.start + length > render.starts[i])
							events.insert(event.id);
				}
		for (const ObjectId id : events)
		{
			const auto found = project.audioEvents.find(id);
			if (found == project.audioEvents.end())
				continue; // the synthetic event of a frozen track
			const AudioEvent& event = found->second;
			input["events"].push_back(toJson(event));
			if (event.part != 0)
				input["audioParts"].push_back(toJson(project.audioParts.at(event.part)));
			const Clip& clip = project.clips.at(event.clip);
			input["clips"].push_back(toJson(clip));
			input["media"].push_back(toJson(project.media.at(clip.media)));
		}
		for (const auto& [id, part] : project.midiParts)
			if (tracks.count(part.track) != 0)
			{
				const auto start =
					time::rebase(part.position, time::TimeBase::Linear, project.tempo, project.sampleRate).value;
				auto finish = part.position;
				finish.value += part.length;
				const auto end = time::rebase(finish, time::TimeBase::Linear, project.tempo, project.sampleRate).value;
				for (const size_t i : ranges.at(part.track))
					if (start < render.ends[i] && end > render.starts[i])
					{
						input["parts"].push_back(toJson(part));
						break;
					}
			}
		return input;
	}

	void ProjectModel::checkRenderMove(ObjectId track) const
	{
		for (const auto& [token, render] : m_trackRenders)
			for (const ObjectId id : render->lockedTracks)
			{
				ObjectId parent = id;
				while (parent != 0)
				{
					if (parent == track)
						throw CommandError("the track is being rendered");
					parent = m_project.trackById.at(parent).parent;
				}
			}
	}

	bool ProjectModel::affectsRender(const ProjectChange& change, const TrackRender& render)
	{
		if (change.settingsBefore && change.settingsAfter)
		{
			const auto& before = *change.settingsBefore;
			const auto& after = *change.settingsAfter;
			if (std::tie(before.sampleRate, before.tempo, before.panLaw, before.rootKey, before.transpose,
					before.autoFades) !=
				std::tie(after.sampleRate, after.tempo, after.panLaw, after.rootKey, after.transpose, after.autoFades))
				return true;
		}
		const auto known = [&render](const auto& changes)
		{
			return std::any_of(changes.begin(), changes.end(),
				[&render](const auto& object) { return render.inputObjects.count(object.id) != 0; });
		};
		if (known(change.tracks) || known(change.channels) || known(change.slots) || known(change.audioEvents) ||
			known(change.audioParts) || known(change.midiParts) || known(change.automation) || known(change.clips) ||
			known(change.media) || known(change.drumMaps) || known(change.articulationMaps) || known(change.patterns))
			return true;
		// New objects are not in the starting input. Check their owners on either side, including rerouting a MIDI
		// track into the rendered instrument. An event outside the range can still pass the detailed comparison.
		const auto onTrack = [&render](const auto& changes)
		{
			for (const auto& change : changes)
				for (const auto* value : {&change.before, &change.after})
					if (*value && render.inputObjects.count((*value)->track) != 0)
						return true;
			return false;
		};
		if (onTrack(change.audioEvents) || onTrack(change.audioParts) || onTrack(change.midiParts) ||
			onTrack(change.automation))
			return true;
		for (const auto& slot : change.slots)
			for (const auto* value : {&slot.before, &slot.after})
				if (*value && render.inputObjects.count((*value)->channel) != 0)
					return true;
		for (const auto& channel : change.channels)
			for (const auto* value : {&channel.before, &channel.after})
				if (*value)
				{
					if (render.inputObjects.count((*value)->track) != 0 ||
						render.inputObjects.count((*value)->output) != 0)
						return true;
					for (const auto& send : (*value)->midiSends.slots)
						if (render.inputObjects.count(send.output) != 0)
							return true;
				}
		return false;
	}

	std::string ProjectModel::renderAction(uint64_t token) const
	{
		return m_trackRenders.at(token)->freeze ? "Freeze" : "Render in place";
	}

	void ProjectModel::checkRenderLocks(const Project& project, const std::vector<const ProjectChange*>& changes) const
	{
		for (const auto& [token, render] : m_trackRenders)
			if (token != m_applyingRender &&
				std::any_of(changes.begin(), changes.end(),
					[render = render.get()](const auto* change) { return affectsRender(*change, *render); }) &&
				renderInput(project, *render) != render->input)
				throw CommandError("the affected track or range is being rendered");
	}

	void ProjectModel::endTrackRender(uint64_t token, CommandContext& context)
	{
		const auto found = m_trackRenders.find(token);
		if (found == m_trackRenders.end())
			return;
		context.emit("track.renderingChanged",
			{{"tracks", found->second->lockedTracks}, {"job", found->second->job}, {"rendering", false}});
		m_trackRenders.erase(found);
	}

	Json ProjectModel::startTrackRender(
		Engine& engine, const std::shared_ptr<TrackRender>& render, CommandContext& context)
	{
		for (const auto& [token, pending] : m_trackRenders)
			for (const ObjectId id : render->tracks)
				require(std::find(pending->tracks.begin(), pending->tracks.end(), id) == pending->tracks.end(),
					"the track is being rendered");
		const uint64_t token = ++m_nextRender;
		render->input = renderInput(m_project, *render);
		for (const auto& value : render->input)
			if (value.is_array())
				for (const auto& object : value)
					if (object.is_object() && object.contains("id"))
						render->inputObjects.insert(object.at("id").get<ObjectId>());
		for (const auto& track : render->input.at("tracks"))
			render->lockedTracks.push_back(track.at("id").get<ObjectId>());
		m_trackRenders[token] = render;
		Json started;
		try
		{
			started = engine.startJob(render->freeze ? "track.freeze" : "track.renderInPlace",
				[render, token](JobContext& job)
				{
					// The work owns sample leases: release them on this thread before engine shutdown destroys
					// the audio engine, even when the stopped command thread cannot remove the pending render.
					const std::vector<Jobs::Work> work = std::move(render->work);
					std::vector<std::string> written;
					try
					{
						for (size_t i = 0; i < work.size(); ++i)
						{
							job.checkCancelled();
							RenderProgress progress(job, i, work.size());
							Json file = work[i](progress).at("files").at(0);
							written.push_back(file.at("path").get<std::string>());
							render->files.push_back(std::move(file));
						}
						job.checkCancelled();
						if (!render->mixdown.empty())
						{
							const std::vector<std::string> parts = written;
							const fs::path path =
								createFreeFile(render->mixdown.parent_path(), render->mixdown.filename());
							written.push_back(path.u8string());
							Json mixed = mixFiles(parts, path, render->sampleFormat, render->sampleRate);
							removeFiles(parts);
							written = {path.u8string()};
							render->files = {std::move(mixed)};
						}
						render->ready.store(true, std::memory_order_release);
						return job.command("track.finishRender", {{"token", token}});
					}
					catch (...)
					{
						removeFiles(written);
						try
						{
							job.command("track.endRender", {{"token", token}});
						}
						catch (...)
						{
							if (!job.cancelled())
								throw; // shutdown can leave no command thread to unlock
						}
						if (job.cancelled())
							throw JobCancelled();
						throw;
					}
				});
			render->job = started.at("job").at("id").get<uint64_t>();
			if (context.call("job.get", {{"job", render->job}}).at("job").at("state") != "running")
			{
				m_trackRenders.erase(token); // starting the job thread failed
				return started;
			}
		}
		catch (...)
		{
			m_trackRenders.erase(token);
			throw;
		}
		context.emit(
			"track.renderingChanged", {{"tracks", render->lockedTracks}, {"job", render->job}, {"rendering", true}});
		return started;
	}

	void ProjectModel::registerRenderCommands(Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const Json tail = {{"type", "number"}, {"minimum", 0}, {"maximum", 60}};
		// Freeze files go into the Freeze folder in the project's folder (reference manual p. 220), or for a project
		// without a file into the folder its recordings go to; without that folder there is none (#2001).
		const auto freezeFolder = [this]
		{
			const fs::path folder = m_file.empty() ? unsavedMediaFolder(m_settingsDirectory)
												   : fs::absolute(fs::u8path(m_file)).parent_path();
			require(!folder.empty(), "there is no folder for the freeze files: save the project first");
			return folder / "Freeze";
		};

		engine.registerCommand(
			{"track.freeze", 2,
				"Freezes audio and instrument tracks (reference manual pp. 219-220): renders what each plays after its "
				"plugin slots, before the strip's other sections (the instrument, the events, the pre section and the "
				"inserts), offline from the project's start to the end of its last event or note, with up to tail "
				"seconds more (0 to 60, default 30) that end after the last sample a 24-bit file doesn't hold as "
				"silence, into a 32-bit float WAV file named after the track in the Freeze folder of the project's "
				"folder (for a project without a file, of the folder its recordings go to, and refused without that "
				"folder). The track then plays the "
				"file from the project's start instead of its events, parts, instrument, pre section and slots, which "
				"stay as they are; the strip's other sections, the fader, pan, mute and sends still play, with their "
				"automation. Its monitor and record enable buttons go off, and those of the MIDI tracks that play it, "
				"and can't be turned on while it is frozen (see channel.set). Renders as mixdown.export does, and is "
				"refused when it is, for a frozen or disabled track and for one with nothing to play. One undo step. "
				"includeInserts false (Include Inserts for Instrument Tracks, p. 992; default true) renders an "
				"instrument track's instrument only, before its pre section and slots, which then still play on the "
				"file with their automation (the track's frozen.inserts is false); an audio track's file holds them in "
				"any case. Returns a cancellable background job; its result holds the tracks. Affected tracks are "
				"locked until the job applies the freeze as one undo step, fails or is cancelled.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties", {{"tracks", ids}, {"tail", tail}, {"includeInserts", {{"type", "boolean"}}}}},
					{"required", {"tracks"}}},
				{{"type", "object"}, {"properties", {{"job", Jobs::schema()}}}, {"required", {"job"}}}, false},
			[this, freezeFolder, &engine](CommandContext& context)
			{
				const Project& project = m_project;
				const Json& parameters = context.parameters();
				const std::vector<ObjectId> tracks = trackList(parameters);
				const double seconds = parameters.value("tail", 30.0);
				const bool inserts = parameters.value("includeInserts", true);
				const RenderSnapshot& snapshot = m_snapshots.latest();
				std::vector<Span> spans;
				for (const ObjectId id : tracks)
				{
					const Track& track = renderable(project, id);
					require(!track.frozen, track.name + " is frozen already");
					require(track.enabled, track.name + " is disabled");
					spans.push_back(spanOf(snapshot, track.channel));
					require(spans.back().start.has_value(), track.name + " has nothing to freeze");
				}
				const fs::path folder = freezeFolder();
				auto render = std::make_shared<TrackRender>();
				render->parameters = parameters;
				render->freeze = true;
				render->tracks = tracks;
				for (size_t i = 0; i < tracks.size(); ++i)
				{
					const Track& track = project.trackById.at(tracks[i]);
					render->starts.push_back(0);
					render->ends.push_back(spans[i].end);
					render->work.push_back(engine.prepareMixdown({{"folder", folder.u8string()},
						{"name", fileNameOf(track)}, {"channels", {track.channel}}, {"start", 0}, {"end", spans[i].end},
						{"point", inserts || track.type != TrackType::Instrument ? "inserts" : "source"},
						{"tail", seconds}, {"sampleFormat", "float32"}}));
				}
				return startTrackRender(engine, render, context);
			});
		addEdit(engine,
			{"track.unfreeze", 1,
				"Unfreezes frozen tracks: they play their events, instrument and plugin slots again. Their freeze "
				"files stay, so the step can be undone. One undo step. Returns the tracks.",
				{{"type", "object"}, {"additionalProperties", false}, {"properties", {{"tracks", ids}}},
					{"required", {"tracks"}}},
				{{"type", "object"}, {"properties", {{"tracks", {{"type", "array"}}}}}, {"required", {"tracks"}}},
				false},
			"Unfreeze",
			[](Project& project, CommandContext& context)
			{
				const std::vector<ObjectId> tracks = trackList(context.parameters());
				for (const ObjectId id : tracks)
				{
					const Track& track = renderable(project, id);
					require(track.frozen.has_value(), track.name + " isn't frozen");
				}
				Json result = Json::array();
				for (const ObjectId id : tracks)
				{
					Track& track = project.trackById.at(id);
					track.frozen.reset();
					result.push_back(toJson(track));
					context.emit("track.changed", {{"track", result.back()}});
				}
				return Json{{"tracks", std::move(result)}};
			});
		const Json relinkEntry = {{"type", "object"}, {"additionalProperties", false},
			{"properties", {{"track", integer}, {"from", {{"type", "string"}}}, {"path", {{"type", "string"}}}}},
			{"required", {"track", "from", "path"}}};
		addEdit(engine,
			{"track.relinkFreeze", 1,
				"Points frozen tracks at other freeze files, such as copies that media.prepareArchive made, as one "
				"undo step. An entry applies only if its track is still frozen and plays from, and the new file can "
				"be read and has the freeze file's sample rate, channels and length; otherwise it is skipped, and "
				"with project every entry is skipped unless that project is still open (project.revision's "
				"project). Returns the tracks it relinked and the ones it skipped.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties", {{"files", {{"type", "array"}, {"items", relinkEntry}}}, {"project", integer}}},
					{"required", {"files"}}},
				{{"type", "object"}, {"properties", {{"relinked", ids}, {"skipped", ids}}},
					{"required", {"relinked", "skipped"}}},
				false},
			"Relink freeze files",
			[this](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				// Files copied for another project don't belong to this one.
				const bool otherProject =
					parameters.contains("project") && parameters.at("project").get<uint64_t>() != m_openedRevision;
				Json relinked = Json::array();
				Json skipped = Json::array();
				for (const Json& entry : parameters.at("files"))
				{
					const auto id = entry.at("track").get<ObjectId>();
					const std::string path = entry.at("path").get<std::string>();
					require(!path.empty(), "path must not be empty");
					const auto track = project.trackById.find(id);
					const auto fits = [&path](const Freeze& freeze)
					{
						try
						{
							const AudioFileInfo info = AudioFileReader::open(path)->info();
							return info.sampleRate == freeze.sampleRate && info.channels == freeze.channels &&
								info.frames == freeze.length;
						}
						catch (const AudioFileError&)
						{
							return false;
						}
					};
					if (otherProject || track == project.trackById.end() || !track->second.frozen ||
						track->second.frozen->path != entry.at("from").get<std::string>() ||
						!fits(*track->second.frozen))
					{
						skipped.push_back(id);
						continue;
					}
					track->second.frozen->path = path;
					context.emit("track.changed", {{"track", toJson(track->second)}});
					relinked.push_back(id);
				}
				return Json{{"relinked", std::move(relinked)}, {"skipped", std::move(skipped)}};
			});

		const Json processing = {{"type", "string"}, {"enum", {"dry", "channelSettings", "completeSignalPath"}}};
		engine.registerCommand(
			{"track.renderInPlace", 2,
				"Renders audio and instrument tracks in place (reference manual pp. 1471-1477): what each plays from "
				"start to end, in project samples (a range selection; default: from the start of its first event or "
				"note to the end of its last), with up to tail seconds more (0 to 60, default 0) that end after the "
				"last sample a 24-bit file doesn't hold as silence, into a WAV file (sampleFormat as in "
				"mixdown.export, default float32) in the project's media folder (for a project without a file, the "
				"folder its recordings go to, and refused without that folder), named name or after the track, and "
				"puts it as one event at start on a "
				"new audio track below the track, named after it with (Rendered), which plays through the same "
				"output. processing: dry renders what plays on the channel, before its pre section and plugin slots, "
				"and the new track gets the channel's slots, strip, fader, pan and sends; channelSettings (the "
				"default) renders after the slots and the strip, before the fader, and the new track gets the fader, "
				"pan and sends; completeSignalPath renders after the fader and panner, in the output's format, and "
				"the new track gets no settings. sourceTracks: keep (the default), mute, disable or remove the "
				"tracks rendered; hideSourceTracks true hides them in the project window unless they are removed, as "
				"trackVisibility.set does, which undo leaves; sourceEvents mute (default keep) mutes the events and "
				"parts in events, which it "
				"needs. Renders as mixdown.export does, and is refused when it is, for a disabled track, "
				"with dry for a frozen track (its freeze file is already through the slots that dry copies), and "
				"with completeSignalPath for a silent track: muted, or silenced by another track's solo. One undo "
				"step when the background job finishes. Returns a cancellable job whose result holds the new tracks, "
				"media files and events, in order. mode: oneEvent (default), separateEvents (one file per event or "
				"part), or blockEvents (combine adjacent or overlapping events and parts). Each source gets one "
				"destination track. events optionally limits the input to audio event, audio part and MIDI part ids "
				"on the selected tracks, excluding other material even where it overlaps or plays in the tail. "
				"start/end clips those event ranges. Event selection and separate/block modes require unfrozen "
				"tracks. Affected tracks and rendered events are locked until the job ends. tailBars (Tail Mode in "
				"bars and beats, p. 1473; 0 to 16) gives the tail in bars of the time signature at the range's "
				"end instead of tail. folder (File Path) renders into that folder, an absolute path, instead of the "
				"media folder. mixdown true (Mix down to One Audio File) sums the tracks' renders from the start "
				"of the earliest range to the end of the latest into one file on one new track, named name and "
				"(Rendered), or Mixdown (Rendered), below the first track; it needs several tracks that play "
				"through one output, completeSignalPath and oneEvent.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties",
						{{"tracks", ids}, {"events", {{"type", "array"}, {"items", integer}, {"minItems", 1}}},
							{"mode", {{"type", "string"}, {"enum", {"oneEvent", "separateEvents", "blockEvents"}}}},
							{"start", {{"type", "integer"}, {"minimum", 0}}},
							{"end", {{"type", "integer"}, {"minimum", 0}}}, {"processing", processing}, {"tail", tail},
							{"sampleFormat",
								{{"type", "string"}, {"enum", {"int16", "int24", "int32", "float32", "float64"}}}},
							{"sourceTracks", {{"type", "string"}, {"enum", {"keep", "mute", "disable", "remove"}}}},
							{"hideSourceTracks", {{"type", "boolean"}}},
							{"sourceEvents", {{"type", "string"}, {"enum", {"keep", "mute"}}}},
							{"name", {{"type", "string"}, {"minLength", 1}}},
							{"tailBars", {{"type", "number"}, {"minimum", 0}, {"maximum", 16}}},
							{"folder", {{"type", "string"}, {"minLength", 1}}}, {"mixdown", {{"type", "boolean"}}}}},
					{"required", {"tracks"}}},
				{{"type", "object"}, {"properties", {{"job", Jobs::schema()}}}, {"required", {"job"}}}, false},
			[this, &engine](CommandContext& context)
			{
				const Project& project = m_project;
				const Json& parameters = context.parameters();
				const std::vector<ObjectId> tracks = trackList(parameters);
				require(parameters.contains("start") == parameters.contains("end"), "give both start and end, or none");
				require(
					parameters.value("sourceEvents", std::string("keep")) == "keep" || parameters.contains("events"),
					"sourceEvents mute needs events, the events and parts to mute");
				if (parameters.contains("start"))
					require(parameters.at("end").get<time::Samples>() > parameters.at("start").get<time::Samples>(),
						"the range is empty: end must be after start");
				require(!parameters.contains("tail") || !parameters.contains("tailBars"),
					"give tail or tailBars, not both");
				require(!parameters.contains("folder") ||
						fs::u8path(parameters.at("folder").get<std::string>()).is_absolute(),
					"folder must be an absolute path");
				const bool mixdown = parameters.value("mixdown", false);
				if (mixdown)
				{
					require(tracks.size() > 1, "mixdown needs several tracks");
					require(parameters.value("processing", std::string()) == "completeSignalPath",
						"mixdown renders the complete signal path: give processing completeSignalPath");
					require(parameters.value("mode", std::string("oneEvent")) == "oneEvent",
						"mixdown makes one event: mode must be oneEvent");
					const auto output = [&project](ObjectId id)
					{
						const auto track = project.trackById.find(id);
						return track == project.trackById.end() ? ObjectId{0}
																: project.channels.at(track->second.channel).output;
					};
					for (const ObjectId id : tracks)
						require(
							output(id) == output(tracks.front()), "mixdown needs tracks that play through one output");
				}
				if (parameters.contains("events"))
				{
					require(!parameters.at("events").empty(), "events must not be empty");
					std::set<ObjectId> selected;
					for (const auto& value : parameters.at("events"))
					{
						const ObjectId id = value.get<ObjectId>();
						require(selected.insert(id).second, "events names an event twice");
						ObjectId owner = 0;
						const auto find = [&](const auto& collection)
						{
							const auto found = collection.find(id);
							if (found != collection.end())
								owner = found->second.track;
						};
						find(project.audioEvents);
						find(project.audioParts);
						find(project.midiParts);
						require(owner != 0, "there is no audio event or part " + std::to_string(id));
						require(std::find(tracks.begin(), tracks.end(), owner) != tracks.end(),
							"events must belong to the selected tracks");
					}
				}
				const bool split =
					parameters.contains("events") || parameters.value("mode", std::string("oneEvent")) != "oneEvent";
				const std::string mode = parameters.value("processing", std::string("channelSettings"));
				const char* point = "output";
				if (mode == "dry")
					point = "source";
				else if (mode == "channelSettings")
					point = "strip";
				const RenderSnapshot& snapshot = m_snapshots.latest();
				std::vector<std::vector<EventRender>> groups;
				for (const ObjectId id : tracks)
				{
					const Track& track = renderable(project, id);
					require(track.enabled, track.name + " is disabled");
					// A freeze file is already through the pre section and the slots that dry copies onto the new
					// track.
					require(mode != "dry" || !track.frozen, track.name + " is frozen: unfreeze it to render it dry");
					require(!split || !track.frozen, track.name + " is frozen: unfreeze it to render events or parts");
					// The complete signal path ends after the mute, through which solo silences the other tracks.
					const auto render = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
						[&track](const RenderChannel& channel) { return channel.id == track.channel; });
					require(mode != "completeSignalPath" || render == snapshot.channels.end() || !render->silent,
						track.name + " is silent: unmute it, or solo it too, to render its complete signal path");
					if (split)
					{
						groups.push_back(eventRenders(project, snapshot, track, parameters));
						require(!groups.back().empty(), track.name + " has no events or parts in the render range");
						continue;
					}
					Span span = spanOf(snapshot, track.channel);
					if (parameters.contains("start"))
						span = {parameters.at("start").get<time::Samples>(), parameters.at("end").get<time::Samples>()};
					require(span.start.has_value(), track.name + " has nothing to render");
					require(span.end > *span.start, "the range is empty: end must be after start");
					groups.push_back({{*span.start, span.end, {}}});
				}
				fs::path folder;
				if (parameters.contains("folder"))
					folder = fs::u8path(parameters.at("folder").get<std::string>());
				else
					folder =
						m_file.empty() ? unsavedMediaFolder(m_settingsDirectory) : mediaFolderFor(fs::u8path(m_file));
				require(!folder.empty(), "there is no folder to render into: save the project first");
				auto render = std::make_shared<TrackRender>();
				render->parameters = parameters;
				if (mixdown)
				{
					// Every track from one start to one end, so that their files add up sample by sample.
					time::Samples start = groups.front().front().start;
					time::Samples end = groups.front().front().end;
					for (const auto& track : groups)
					{
						start = std::min(start, track.front().start);
						end = std::max(end, track.front().end);
					}
					for (auto& track : groups)
						track.front() = {start, end, track.front().events};
					// A file in folder, whatever the name has in it: "AC/DC" is no subfolder, "../Mix" no path out of
					// it.
					render->mixdown =
						folder / fs::u8path(safeFileName(parameters.value("name", std::string("Mixdown"))) + ".wav");
					render->sampleFormat = parameters.value("sampleFormat", std::string("float32"));
					render->sampleRate = project.sampleRate;
				}
				for (size_t i = 0; i < tracks.size(); ++i)
				{
					const Track& track = project.trackById.at(tracks[i]);
					for (const auto& group : groups[i])
					{
						render->tracks.push_back(track.id);
						render->starts.push_back(group.start);
						render->ends.push_back(group.end);
						Json request = {{"folder", folder.u8string()},
							{"name", mixdown ? fileNameOf(track) : parameters.value("name", fileNameOf(track))},
							{"channels", {track.channel}}, {"start", group.start}, {"end", group.end}, {"point", point},
							{"tail", tailAfter(project, parameters, group.end)},
							{"sampleFormat",
								mixdown ? std::string("float32")
										: parameters.value("sampleFormat", std::string("float32"))}};
						if (split)
							request["renderEvents"] = group.events;
						render->work.push_back(engine.prepareMixdown(request));
					}
				}
				return startTrackRender(engine, render, context);
			});

		const Json tokenSchema = {{"type", "object"}, {"properties", {{"token", integer}}}, {"required", {"token"}},
			{"additionalProperties", false}};
		engine.registerCommand(
			{"track.endRender", 1, "Releases a background render's track locks after failure or cancellation.",
				tokenSchema, {{"type", "object"}}, false},
			[this](CommandContext& context)
			{
				require(context.internal(), "only a render job can release its tracks");
				endTrackRender(context.parameters().at("token").get<uint64_t>(), context);
				return Json::object();
			});
		engine.registerCommand({"track.renderLocks", 1, "The tracks locked by background rendering, with each job id.",
								   CommandInfo{}.parameters,
								   Json::parse(R"({"type":"object","properties":{"locks":{"type":"array","items":{
					"type":"object","properties":{"tracks":{"type":"array","items":{"type":"integer"}},
					"job":{"type":"integer"}},"required":["tracks","job"]}}},"required":["locks"]})"),
								   true},
			[this](CommandContext&)
			{
				Json locks = Json::array();
				for (const auto& [token, render] : m_trackRenders)
					locks.push_back({{"tracks", render->lockedTracks}, {"job", render->job}});
				return Json{{"locks", std::move(locks)}};
			});
		// The completion is an ordinary document edit: rollback, events and undo stay on the command thread.
		addEdit(engine,
			{"track.finishRender", 1,
				"Applies a prepared background track render as one undo step; only its engine job may call it.",
				tokenSchema,
				{{"type", "object"}, {"properties", {{"tracks", {{"type", "array"}}}, {"media", ids}, {"events", ids}}},
					{"required", {"tracks"}}},
				false},
			"Render tracks",
			[this, &engine](Project& project, CommandContext& context)
			{
				require(context.internal(), "only a render job can apply its result");
				const uint64_t token = context.parameters().at("token").get<uint64_t>();
				const auto found = m_trackRenders.find(token);
				require(found != m_trackRenders.end(), "there is no pending track render");
				const auto render = found->second;
				require(render->ready.load(std::memory_order_acquire), "the render is not ready");
				const Json& parameters = render->parameters;
				const auto& tracks = render->tracks;
				const auto& files = render->files;
				// job.cancel and completion are ordered by the command thread.
				require(!engine.jobCancelled(render->job), "render cancelled");
				if (render->freeze)
				{
					Json result = Json::array();
					for (size_t i = 0; i < tracks.size(); ++i)
					{
						Track& track = project.trackById.at(tracks[i]);
						const Channel& channel = project.channels.at(track.channel);
						track.frozen = Freeze{files[i].at("path").get<std::string>(), project.sampleRate,
							channelCount(channel.format), files[i].at("frames").get<time::Samples>(),
							track.type != TrackType::Instrument || parameters.value("includeInserts", true)};
						result.push_back(toJson(track));
						context.emit("track.changed", {{"track", result.back()}});
					}
					// The buttons the freezes lock (lockedByFreeze) go off.
					for (auto& [id, channel] : project.channels)
						if ((channel.monitor || channel.recordEnable) && lockedByFreeze(project, channel))
						{
							channel.monitor = channel.recordEnable = false;
							context.emit("channel.changed", {{"channel", toJson(channel)}});
						}
					return Json{{"tracks", std::move(result)}};
				}
				const std::string mode = parameters.value("processing", std::string("channelSettings"));
				std::vector<Span> spans;
				std::vector<std::string> written;
				for (size_t i = 0; i < files.size(); ++i)
				{
					spans.push_back({render->starts[i], render->ends[i]});
					written.push_back(files[i].at("path").get<std::string>());
				}
				const auto run = [this, &project, &context](const char* command, const Json& commandParameters)
				{
					CommandContext inner(commandParameters, &context);
					return m_edits.at(command).handler(project, inner);
				};
				Json added = Json::array();
				Json media = Json::array();
				Json events = Json::array();
				std::map<ObjectId, ObjectId> destinations;

				for (size_t i = 0; i < files.size(); ++i)
				{
					const Track& source = project.trackById.at(tracks[i]);
					const Channel from = project.channels.at(source.channel);
					const std::vector<ObjectId>& siblings =
						source.parent == 0 ? project.tracks : project.trackById.at(source.parent).children;
					const Json mediaFile = run("media.add", {{"path", written[i]}}).at("media");
					const std::string name =
						!render->mixdown.empty() ? parameters.value("name", std::string("Mixdown")) : source.name;
					Json trackParameters = {{"type", "audio"}, {"name", name + " (Rendered)"},
						{"timeBase", timeBaseName(source.timeBase)}, {"parent", source.parent},
						{"index",
							std::distance(siblings.begin(), std::find(siblings.begin(), siblings.end(), source.id)) +
								1}};
					if (mode == "completeSignalPath")
						trackParameters["channels"] = mediaFile.at("channels");
					else
						trackParameters["format"] = formatName(from.format);
					if (from.output != 0)
						trackParameters["output"] = from.output;
					const bool first = destinations.count(source.id) == 0;
					if (first)
					{
						destinations[source.id] =
							run("track.add", trackParameters).at("track").at("id").get<ObjectId>();
						added.push_back(destinations.at(source.id));
					}
					const ObjectId track = destinations.at(source.id);
					const auto clip =
						run("clip.add", {{"media", mediaFile.at("id")}}).at("clip").at("id").get<ObjectId>();
					const time::Position at = time::rebase({time::TimeBase::Linear, *spans[i].start},
						project.trackById.at(track).timeBase, project.tempo, project.sampleRate);
					const Json event = run("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", at.value}})
										   .at("event")
										   .at("id");
					// The render holds the global transposition: as recorded events (#1368, reference manual
					// p. 420), it follows the project root key, or with transpose events is independent of it.
					const bool transposed = !project.transpose.events.empty();
					if (transposed || project.rootKey >= 0)
						run("audioEvent.set",
							{{"event", event}, {"rootKey", transposed ? std::string() : rootKeyName(project.rootKey)},
								{"globalTranspose", transposed ? "independent" : "follow"}});
					events.push_back(event);
					media.push_back(mediaFile.at("id"));
					if (!first)
						continue;

					// The settings that weren't rendered.
					Channel& channel = project.channels.at(project.trackById.at(track).channel);
					if (mode != "completeSignalPath")
					{
						channel.sends = from.sends;
						channel.volume = from.volume;
						channel.pan = from.pan;
						channel.panner = from.panner;
						channel.panLeft = from.panLeft;
						channel.panRight = from.panRight;
						channel.panBypass = from.panBypass;
					}
					std::vector<ObjectId> slots;
					if (mode == "dry")
					{
						channel.sections = from.sections;
						for (const ObjectId id : from.slots)
						{
							Slot slot = project.slots.at(id);
							slot.id = project.newId();
							slot.channel = channel.id;
							channel.slots.push_back(slot.id);
							slots.push_back(slot.id);
							project.slots[slot.id] = std::move(slot);
						}
					}
					context.emit("channel.changed", {{"channel", toJson(channel)}});
					for (const ObjectId slot : slots)
						context.emit("plugin.added", {{"slot", describeSlot(project, slot)}});
				}

				if (!render->mixdown.empty()) // the source options apply to every track mixed down
					for (const ObjectId id : tracks)
						destinations.emplace(id, destinations.begin()->second);
				// The source options (reference manual pp. 1473, 1476), the events before their tracks can go.
				if (parameters.value("sourceEvents", std::string("keep")) == "mute")
					run("event.set", {{"events", parameters.at("events")}, {"mute", true}});
				const std::string sources = parameters.value("sourceTracks", std::string("keep"));
				if (parameters.value("hideSourceTracks", false) && sources != "remove")
				{
					// The stored list keeps removed hidden tracks for undo; trackVisibility.set refuses them.
					std::vector<ObjectId> hidden;
					for (const ObjectId id : project.trackVisibility.hidden)
						if (project.trackById.count(id) != 0)
							hidden.push_back(id);
					for (const auto& [id, destination] : destinations)
						hidden.push_back(id);
					run("trackVisibility.set", {{"hidden", hidden}});
				}
				for (const auto& [id, destination] : destinations)
				{
					if (sources == "mute")
						run("channel.set", {{"channel", project.trackById.at(id).channel}, {"mute", true}});
					else if (sources == "disable")
						run("track.set", {{"track", id}, {"enabled", false}});
					else if (sources == "remove")
						run("track.remove", {{"track", id}});
				}
				return Json{{"tracks", std::move(added)}, {"media", std::move(media)}, {"events", std::move(events)}};
			});
	}
}
