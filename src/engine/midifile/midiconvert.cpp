// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midifile/midiconvert.h"

#include "engine/midi/midivalues.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace gnubase::engine::midifile
{
	namespace
	{
		using time::g_ticksPerQuarter;
		using time::Ticks;

		constexpr double g_microsecondsPerMinute = 60000000.0;
		constexpr uint32_t g_maxTempo = 0xFFFFFF; // microseconds per quarter note: three bytes
		constexpr Ticks g_rampStep = g_ticksPerQuarter / 4;

		// Bank select: the bank's upper and lower seven bits.
		constexpr uint8_t g_bankSelect = 0;
		constexpr uint8_t g_bankSelectLsb = 32;

		// Rounds to the nearest tick of the other resolution.
		Ticks rescale(Ticks value, Ticks from, Ticks to)
		{
			const Ticks product = value * to;
			return product >= 0 ? (product + (from / 2)) / from : -((-product + (from / 2)) / from);
		}

		// The order of events at one tick.
		enum class Rank : uint8_t
		{
			Meta,
			NoteOff,
			Other,
			NoteOn,
		};

		struct Ranked
		{
			Rank rank = Rank::Other;
			Event event;
		};

		void sortEvents(std::vector<Ranked>& events)
		{
			std::stable_sort(events.begin(), events.end(), [](const Ranked& a, const Ranked& b)
				{ return std::tie(a.event.tick, a.rank) < std::tie(b.event.tick, b.rank); });
		}

		Track toTrack(std::vector<Ranked> events, int64_t end)
		{
			sortEvents(events);
			Track track;
			track.events.reserve(events.size());
			for (Ranked& ranked : events)
				track.events.push_back(std::move(ranked.event));
			track.end = std::max(end, track.events.empty() ? int64_t{0} : track.events.back().tick);
			return track;
		}

		class Exporter
		{
		public:
			Exporter(const Project& project, const ExportOptions& options) : m_project(project), m_options(options) {}

			File run()
			{
				std::vector<ObjectId> tracks = m_options.tracks;
				if (tracks.empty())
				{
					for (const ObjectId id : m_project.trackOrder())
						if (isMidi(m_project.trackById.at(id)))
							tracks.push_back(id);
				}
				else
				{
					for (const ObjectId id : tracks)
					{
						const auto track = m_project.trackById.find(id);
						if (track == m_project.trackById.end())
							throw std::invalid_argument("no track " + std::to_string(id));
						if (!isMidi(track->second))
							throw std::invalid_argument(
								"track " + std::to_string(id) + " is not a MIDI or instrument track");
					}
					// In track order, each once.
					const std::vector<ObjectId> order = m_project.trackOrder();
					std::vector<ObjectId> sorted;
					for (const ObjectId id : order)
						if (std::find(tracks.begin(), tracks.end(), id) != tracks.end())
							sorted.push_back(id);
					tracks = std::move(sorted);
				}

				std::vector<std::vector<Ranked>> trackEvents;
				trackEvents.reserve(tracks.size());
				for (const ObjectId id : tracks)
					trackEvents.push_back(trackContent(m_project.trackById.at(id)));
				const int64_t end = m_options.end == std::numeric_limits<Ticks>::max()
					? toFile(std::max(m_lastPartEnd, m_options.start))
					: toFile(m_options.end);

				File file;
				file.format = m_options.format;
				file.ticksPerQuarter = m_options.ticksPerQuarter;
				std::vector<Ranked> conductor = conductorEvents();
				if (m_options.format == 0)
				{
					for (std::vector<Ranked>& events : trackEvents)
						for (Ranked& ranked : events)
							if (ranked.rank != Rank::Meta) // the track name: the file has the project's only
								conductor.push_back(std::move(ranked));
					file.tracks.push_back(toTrack(std::move(conductor), end));
					return file;
				}
				file.tracks.push_back(toTrack(std::move(conductor), end));
				for (std::vector<Ranked>& events : trackEvents)
					file.tracks.push_back(toTrack(std::move(events), end));
				return file;
			}

		private:
			static bool isMidi(const engine::Track& track)
			{
				return track.type == TrackType::Midi || track.type == TrackType::Instrument;
			}

			bool inRange(Ticks tick) const { return tick >= m_options.start && tick < m_options.end; }

			int64_t toFile(Ticks tick) const
			{
				return rescale(tick - m_options.start, g_ticksPerQuarter, m_options.ticksPerQuarter);
			}

			static Ranked meta(int64_t tick, uint8_t type, std::vector<uint8_t> data)
			{
				return {Rank::Meta, metaEvent(tick, type, std::move(data))};
			}

			static Ranked channel(int64_t tick, Rank rank, uint8_t status, uint8_t data1, uint8_t data2 = 0)
			{
				return {rank, channelEvent(tick, status, data1, data2)};
			}

			// The project's tempo as jumps: ramps become a jump per sixteenth note, each with the tempo that takes
			// as long as the ramp does over that sixteenth.
			std::vector<std::pair<Ticks, double>> tempoSteps() const
			{
				const time::TempoMap& tempo = m_project.tempo;
				const std::vector<time::TempoPoint>& points = tempo.points();
				std::vector<std::pair<Ticks, double>> steps;
				for (size_t i = 0; i < points.size(); ++i)
				{
					if (i + 1 < points.size() && points[i + 1].ramp)
					{
						for (Ticks from = points[i].position; from < points[i + 1].position; from += g_rampStep)
						{
							const Ticks to = std::min(from + g_rampStep, points[i + 1].position);
							const double quarters =
								static_cast<double>(to - from) / static_cast<double>(g_ticksPerQuarter);
							steps.emplace_back(from, quarters * 60.0 / (tempo.toSeconds(to) - tempo.toSeconds(from)));
						}
					}
					else
					{
						steps.emplace_back(points[i].position, points[i].bpm);
					}
				}
				return steps;
			}

			static std::vector<uint8_t> tempoData(double bpm)
			{
				const auto microseconds = static_cast<uint32_t>(std::clamp<double>(
					std::round(g_microsecondsPerMinute / bpm), 1.0, static_cast<double>(g_maxTempo)));
				return {static_cast<uint8_t>(microseconds >> 16), static_cast<uint8_t>(microseconds >> 8),
					static_cast<uint8_t>(microseconds)};
			}

			static std::vector<uint8_t> signatureData(const time::TimeSignature& signature)
			{
				uint8_t power = 0;
				while ((1 << power) < signature.denominator)
					++power;
				// MIDI clocks (24 per quarter note) per beat, and 8 thirty-second notes per quarter note.
				return {static_cast<uint8_t>(std::min(signature.numerator, 255)), power,
					static_cast<uint8_t>(96 / signature.denominator), 8};
			}

			std::vector<Ranked> conductorEvents() const
			{
				std::vector<Ranked> events;
				events.push_back({Rank::Meta, metaText(0, g_metaTrackName, m_project.name)});

				// The tempo at the range's start, then its changes in the range; repeats left out.
				std::vector<uint8_t> previous;
				const std::vector<std::pair<Ticks, double>> steps = tempoSteps();
				for (size_t i = 0; i < steps.size(); ++i)
				{
					const bool inEffectAtStart = steps[i].first <= m_options.start &&
						(i + 1 == steps.size() || steps[i + 1].first > m_options.start);
					const bool changesInRange = steps[i].first > m_options.start && steps[i].first < m_options.end;
					if (!inEffectAtStart && !changesInRange)
						continue;
					std::vector<uint8_t> data = tempoData(steps[i].second);
					if (data == previous)
						continue;
					previous = data;
					events.push_back(meta(inEffectAtStart ? 0 : toFile(steps[i].first), g_metaTempo, std::move(data)));
				}

				const time::SignatureMap& signatures = m_project.signatures;
				events.push_back(meta(0, g_metaTimeSignature, signatureData(signatures.signatureAt(m_options.start))));
				for (const time::SignatureChange& change : signatures.changes())
				{
					const Ticks at = signatures.barStart(change.bar);
					if (at > m_options.start && at < m_options.end)
						events.push_back(meta(toFile(at), g_metaTimeSignature, signatureData(change.signature)));
				}

				if (m_options.markers)
				{
					std::vector<const Marker*> markers;
					for (const auto& [id, marker] : m_project.markers)
						if (inRange(marker.position))
							markers.push_back(&marker);
					std::stable_sort(markers.begin(), markers.end(),
						[](const Marker* a, const Marker* b) { return a->position < b->position; });
					for (const Marker* marker : markers)
						events.push_back({Rank::Meta, metaText(toFile(marker->position), g_metaMarker, marker->name)});
				}
				return events;
			}

			std::vector<Ranked> trackContent(const engine::Track& track)
			{
				std::vector<Ranked> events;
				events.push_back({Rank::Meta, metaText(0, g_metaTrackName, track.name)});
				std::vector<const MidiPart*> parts;
				for (const auto& [id, part] : m_project.midiParts)
					if (part.track == track.id)
						parts.push_back(&part);
				const auto start = [this](const MidiPart* part)
				{ return time::toTicks(part->position, m_project.tempo, m_project.sampleRate); };
				std::stable_sort(parts.begin(), parts.end(),
					[&start](const MidiPart* a, const MidiPart* b) { return start(a) < start(b); });
				for (const MidiPart* part : parts)
					addPart(*part, events);
				return events;
			}

			void addPart(const MidiPart& part, std::vector<Ranked>& events)
			{
				const time::TempoMap& tempo = m_project.tempo;
				const Ticks partStart = time::toTicks(part.position, tempo, m_project.sampleRate);
				const Ticks partEnd = time::toTicks(
					time::Position{part.position.base, part.position.value + part.length}, tempo, m_project.sampleRate);
				if (partEnd > m_options.start && partStart < m_options.end)
					m_lastPartEnd = std::max(m_lastPartEnd, std::min(partEnd, m_options.end));

				std::vector<const Note*> notes;
				notes.reserve(part.notes.size());
				for (const auto& [id, note] : part.notes)
					notes.push_back(&note);
				std::stable_sort(notes.begin(), notes.end(), [](const Note* a, const Note* b)
					{ return std::tie(a->position, a->pitch) < std::tie(b->position, b->pitch); });
				for (const Note* note : notes)
				{
					const Ticks at = partStart + note->position;
					if (at >= partEnd || !inRange(at))
						continue;
					const Ticks ends = std::min({at + note->length, partEnd, m_options.end});
					const int64_t on = toFile(at);
					const int64_t off = std::max(toFile(ends), on + 1);
					const auto channelBits = static_cast<uint8_t>(note->channel & 0x0FU);
					events.push_back(channel(on, Rank::NoteOn, static_cast<uint8_t>(0x90U | channelBits), note->pitch,
						midi::noteOnVelocityToMidi1(note->velocity)));
					events.push_back(channel(off, Rank::NoteOff, static_cast<uint8_t>(0x80U | channelBits), note->pitch,
						midi::velocityToMidi1(note->releaseVelocity)));
				}

				for (const auto& [id, controller] : part.controllers)
				{
					const Ticks at = partStart + controller.position;
					if (at >= partEnd || !inRange(at))
						continue;
					addController(controller, toFile(at), events);
				}
			}

			static void addController(const Controller& controller, int64_t tick, std::vector<Ranked>& events)
			{
				const auto channelBits = static_cast<uint8_t>(controller.channel & 0x0FU);
				const auto status = [channelBits](uint8_t kind) { return static_cast<uint8_t>(kind | channelBits); };
				switch (controller.type)
				{
				case ControllerType::Control:
					events.push_back(channel(
						tick, Rank::Other, status(0xB0), controller.number, midi::controllerToMidi1(controller.value)));
					break;
				case ControllerType::PitchBend:
				{
					const uint16_t bend = midi::pitchBendToMidi1(controller.value);
					events.push_back(channel(tick, Rank::Other, status(0xE0), static_cast<uint8_t>(bend & 0x7FU),
						static_cast<uint8_t>(bend >> 7)));
					break;
				}
				case ControllerType::ChannelPressure:
					events.push_back(
						channel(tick, Rank::Other, status(0xD0), midi::controllerToMidi1(controller.value)));
					break;
				case ControllerType::PolyPressure:
					events.push_back(channel(
						tick, Rank::Other, status(0xA0), controller.number, midi::controllerToMidi1(controller.value)));
					break;
				case ControllerType::Program:
					if (controller.value <= g_maxBank)
					{
						events.push_back(channel(tick, Rank::Other, status(0xB0), g_bankSelect,
							static_cast<uint8_t>(controller.value >> 7)));
						events.push_back(channel(tick, Rank::Other, status(0xB0), g_bankSelectLsb,
							static_cast<uint8_t>(controller.value & 0x7FU)));
					}
					events.push_back(channel(tick, Rank::Other, status(0xC0), controller.number));
					break;
				case ControllerType::SysEx:
					if (controller.data.size() >= 2)
					{
						Event event;
						event.tick = tick;
						event.status = g_statusSysEx;
						event.data.assign(controller.data.begin() + 1, controller.data.end());
						events.push_back({Rank::Other, std::move(event)});
					}
					break;
				}
			}

			const Project& m_project;
			const ExportOptions& m_options;
			Ticks m_lastPartEnd = 0;
		};

		// Reads one file track's channel events into tracks: one, or one per channel for type 0.
		class TrackReader
		{
		public:
			TrackReader(int ticksPerQuarter, bool byChannel)
				: m_ticksPerQuarter(ticksPerQuarter), m_byChannel(byChannel)
			{
			}

			void read(const midifile::Track& track)
			{
				for (const Event& event : track.events)
				{
					if (event.status == g_statusMeta && event.meta == g_metaTrackName && !m_hasName)
					{
						m_name.assign(event.data.begin(), event.data.end());
						m_hasName = true;
					}
					else if (event.status == g_statusSysEx || event.status == g_statusSysExContinue)
					{
						sysEx(event);
					}
					else if (event.status >= 0x80 && event.status < 0xF0)
					{
						channelMessage(event);
					}
				}
				const int64_t end = std::max(track.end, track.events.empty() ? int64_t{0} : track.events.back().tick);
				for (uint8_t c = 0; c < 16; ++c)
					flushBank(c);
				for (const auto& [key, open] : m_open)
					for (const Open& note : open)
						finish(key.first, key.second, note, end, g_defaultReleaseVelocity);
				m_open.clear();
			}

			// The tracks read, in channel order for type 0.
			std::vector<ImportedTrack> take()
			{
				std::vector<ImportedTrack> tracks;
				for (auto& [key, track] : m_tracks)
				{
					if (track.notes.empty() && track.controllers.empty())
						continue;
					if (m_byChannel)
					{
						const std::string channel = "Channel " + std::to_string(key + 1);
						track.name = m_name.empty() ? channel : m_name + " (" + channel + ")";
					}
					else
					{
						track.name = m_name;
					}
					tracks.push_back(std::move(track));
				}
				return tracks;
			}

		private:
			struct Open
			{
				int64_t tick = 0;
				uint8_t velocity = 0;
			};

			struct PendingBank
			{
				int64_t tick = 0;
				std::optional<uint8_t> msb;
				std::optional<uint8_t> lsb;
			};

			Ticks toProject(int64_t tick) const { return rescale(tick, m_ticksPerQuarter, g_ticksPerQuarter); }

			ImportedTrack& target(uint8_t channel) { return m_tracks[m_byChannel ? channel : 0]; }

			void addController(uint8_t channel, int64_t tick, ControllerType type, uint8_t number, uint32_t value)
			{
				Controller controller;
				controller.position = toProject(tick);
				controller.type = type;
				controller.number = number;
				controller.value = value;
				controller.channel = channel;
				target(channel).controllers.push_back(std::move(controller));
			}

			void finish(uint8_t channel, uint8_t pitch, const Open& open, int64_t off, uint16_t releaseVelocity)
			{
				Note note;
				note.position = toProject(open.tick);
				note.length = std::max<Ticks>(1, toProject(off) - note.position);
				note.pitch = pitch;
				note.velocity = midi::velocityFromMidi1(open.velocity);
				note.releaseVelocity = releaseVelocity;
				note.channel = channel;
				target(channel).notes.push_back(note);
			}

			// Bank select that no program change took up stays as controllers.
			void flushBank(uint8_t channel)
			{
				std::optional<PendingBank>& pending = m_bank[channel];
				if (!pending)
					return;
				if (pending->msb)
					addController(channel, pending->tick, ControllerType::Control, g_bankSelect,
						midi::controllerFromMidi1(*pending->msb));
				if (pending->lsb)
					addController(channel, pending->tick, ControllerType::Control, g_bankSelectLsb,
						midi::controllerFromMidi1(*pending->lsb));
				pending.reset();
			}

			void channelMessage(const Event& event)
			{
				const auto channel = static_cast<uint8_t>(event.status & 0x0FU);
				const auto kind = static_cast<uint8_t>(event.status & 0xF0U);
				std::optional<PendingBank>& pending = m_bank[channel];
				const bool bankSelect = kind == 0xB0 && (event.data1 == g_bankSelect || event.data1 == g_bankSelectLsb);
				const bool takesBank = bankSelect || kind == 0xC0;
				if (pending && (!takesBank || pending->tick != event.tick))
					flushBank(channel);

				switch (kind)
				{
				case 0x90:
					if (event.data2 != 0)
					{
						m_open[{channel, event.data1}].push_back({event.tick, event.data2});
						break;
					}
					noteOff(channel, event.data1, event.tick, 64); // a note-on with velocity 0 is a note-off
					break;
				case 0x80:
					noteOff(channel, event.data1, event.tick, event.data2);
					break;
				case 0xA0:
					addController(channel, event.tick, ControllerType::PolyPressure, event.data1,
						midi::controllerFromMidi1(event.data2));
					break;
				case 0xB0:
					if (bankSelect)
					{
						if (!pending)
							pending = PendingBank{event.tick, {}, {}};
						(event.data1 == g_bankSelect ? pending->msb : pending->lsb) = event.data2;
						break;
					}
					addController(channel, event.tick, ControllerType::Control, event.data1,
						midi::controllerFromMidi1(event.data2));
					break;
				case 0xC0:
				{
					uint32_t bank = g_noBank;
					if (pending && pending->msb)
					{
						bank = (static_cast<uint32_t>(*pending->msb) << 7) | pending->lsb.value_or(0);
						pending.reset();
					}
					else
					{
						flushBank(channel);
					}
					addController(channel, event.tick, ControllerType::Program, event.data1, bank);
					break;
				}
				case 0xD0:
					addController(channel, event.tick, ControllerType::ChannelPressure, 0,
						midi::controllerFromMidi1(event.data1));
					break;
				default: // 0xE0
					addController(channel, event.tick, ControllerType::PitchBend, 0,
						midi::pitchBendFromMidi1(static_cast<uint16_t>(event.data1 | (event.data2 << 7))));
					break;
				}
			}

			void noteOff(uint8_t channel, uint8_t pitch, int64_t tick, uint8_t velocity)
			{
				const auto found = m_open.find({channel, pitch});
				if (found == m_open.end() || found->second.empty())
					return; // a note-off without a note: nothing to end
				std::deque<Open>& open = found->second;
				finish(channel, pitch, open.front(), tick, midi::velocityFromMidi1(velocity));
				open.pop_front();
			}

			// A SysEx message, possibly sent in packets: an 0xF0 packet without the closing 0xF7, then 0xF7 packets
			// up to the one that ends with it. 0xF7 packets outside a message are raw MIDI data, which has no place in
			// the project. Messages with data bytes above 127 are left out.
			void sysEx(const Event& event)
			{
				if (event.status == g_statusSysEx)
				{
					m_sysEx.assign(1, g_statusSysEx);
					m_sysExTick = event.tick;
				}
				else if (m_sysEx.empty())
				{
					return;
				}
				m_sysEx.insert(m_sysEx.end(), event.data.begin(), event.data.end());
				if (m_sysEx.back() != 0xF7)
					return;
				const bool valid =
					std::all_of(m_sysEx.begin() + 1, m_sysEx.end() - 1, [](uint8_t byte) { return byte < 0x80; });
				if (valid && m_sysEx.size() >= 2)
				{
					Controller controller;
					controller.position = toProject(m_sysExTick);
					controller.type = ControllerType::SysEx;
					controller.data = m_sysEx;
					target(0).controllers.push_back(std::move(controller));
				}
				m_sysEx.clear();
			}

			int m_ticksPerQuarter;
			bool m_byChannel;
			std::string m_name;
			bool m_hasName = false;
			std::map<int, ImportedTrack> m_tracks;
			std::map<std::pair<uint8_t, uint8_t>, std::deque<Open>> m_open; // by channel and pitch, oldest first
			std::array<std::optional<PendingBank>, 16> m_bank;
			std::vector<uint8_t> m_sysEx;
			int64_t m_sysExTick = 0;
		};

		struct TimedMeta
		{
			int64_t tick = 0;
			const Event* event = nullptr;
		};

		// The meta events of one type in every track, by tick.
		std::vector<TimedMeta> metaEvents(const File& file, uint8_t type)
		{
			std::vector<TimedMeta> found;
			for (const midifile::Track& track : file.tracks)
				for (const Event& event : track.events)
					if (event.status == g_statusMeta && event.meta == type)
						found.push_back({event.tick, &event});
			std::stable_sort(
				found.begin(), found.end(), [](const TimedMeta& a, const TimedMeta& b) { return a.tick < b.tick; });
			return found;
		}

		time::TempoMap importTempo(const File& file)
		{
			std::vector<time::TempoPoint> points{{0, 120.0, false}};
			for (const TimedMeta& meta : metaEvents(file, g_metaTempo))
			{
				const std::vector<uint8_t>& data = meta.event->data;
				if (data.size() < 3)
					continue;
				const uint32_t microseconds = (uint32_t{data[0]} << 16) | (uint32_t{data[1]} << 8) | data[2];
				if (microseconds == 0)
					continue;
				const double bpm = std::clamp(g_microsecondsPerMinute / microseconds, time::g_minBpm, time::g_maxBpm);
				const Ticks at = rescale(meta.tick, file.ticksPerQuarter, g_ticksPerQuarter);
				if (at == points.back().position)
					points.back().bpm = bpm;
				else if (bpm != points.back().bpm)
					points.push_back({at, bpm, false});
			}
			return time::TempoMap(points);
		}

		time::SignatureMap importSignatures(const File& file)
		{
			std::vector<time::SignatureChange> changes{{0, {}}};
			for (const TimedMeta& meta : metaEvents(file, g_metaTimeSignature))
			{
				const std::vector<uint8_t>& data = meta.event->data;
				if (data.size() < 2 || data[0] == 0 || data[1] > 4)
					continue;
				const time::TimeSignature signature{data[0], 1 << data[1]};
				const time::SignatureMap bars(changes);
				const Ticks at = rescale(meta.tick, file.ticksPerQuarter, g_ticksPerQuarter);
				const int64_t bar = bars.toBarsBeats(at).bar;
				const int64_t changeBar = bars.barStart(bar) == at ? bar : bar + 1;
				if (changeBar == changes.back().bar)
					changes.back().signature = signature;
				else if (!(changes.back().signature == signature))
					changes.push_back({changeBar, signature});
			}
			return time::SignatureMap(changes);
		}
	}

	File exportProject(const Project& project, const ExportOptions& options)
	{
		return Exporter(project, options).run();
	}

	Import importFile(const File& file)
	{
		Import result;
		result.tempo = importTempo(file);
		result.signatures = importSignatures(file);
		for (const TimedMeta& meta : metaEvents(file, g_metaMarker))
		{
			Marker marker;
			marker.position = rescale(meta.tick, file.ticksPerQuarter, g_ticksPerQuarter);
			marker.name.assign(meta.event->data.begin(), meta.event->data.end());
			result.markers.push_back(std::move(marker));
		}
		for (const midifile::Track& track : file.tracks)
		{
			TrackReader reader(file.ticksPerQuarter, file.format == 0);
			reader.read(track);
			for (ImportedTrack& imported : reader.take())
				result.tracks.push_back(std::move(imported));
		}
		return result;
	}

	PartSpan partSpan(const ImportedTrack& track, const time::SignatureMap& bars)
	{
		Ticks first = std::numeric_limits<Ticks>::max();
		Ticks last = 0;
		for (const Note& note : track.notes)
		{
			first = std::min(first, note.position);
			last = std::max(last, note.position + note.length);
		}
		for (const Controller& controller : track.controllers)
		{
			first = std::min(first, controller.position);
			last = std::max(last, controller.position + 1);
		}
		if (first > last)
			return {};
		const Ticks start = bars.barStart(bars.toBarsBeats(first).bar);
		const int64_t lastBar = bars.toBarsBeats(last).bar;
		const Ticks end = bars.barStart(lastBar) == last ? last : bars.barStart(lastBar + 1);
		return {start, end - start};
	}
}
