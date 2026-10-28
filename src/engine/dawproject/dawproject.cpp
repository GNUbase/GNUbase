// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/dawproject/dawproject.h"

#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/file.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <miniz.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr const char* g_extensionEntry = "gnubase.json";
		constexpr int g_extensionFormat = 1;

		// FNV-1a of project.xml: tells whether another application changed it since GNUbase wrote it.
		std::string checksum(const std::string& text)
		{
			uint64_t hash = 14695981039346656037ULL;
			for (const char c : text)
			{
				hash ^= static_cast<unsigned char>(c);
				hash *= 1099511628211ULL;
			}
			std::ostringstream hex;
			hex << std::hex << std::setw(16) << std::setfill('0') << hash;
			return hex.str();
		}

		std::filesystem::path pathOf(const std::string& utf8)
		{
			return std::filesystem::u8path(utf8);
		}

		std::vector<char> readBytes(const std::string& path)
		{
			const detail::File file = detail::openFile(path, "rb");
			if (!file)
				throw DawProjectError("can't read " + path + ": " + detail::lastError());
			std::vector<char> bytes;
			char buffer[65536];
			while (const size_t got = std::fread(buffer, 1, sizeof buffer, file.get()))
				bytes.insert(bytes.end(), buffer, buffer + got);
			return bytes;
		}

		class ZipWriter
		{
		public:
			explicit ZipWriter(std::string path) : m_path(std::move(path)), m_file(detail::openFile(m_path, "wb"))
			{
				if (!m_file)
					throw DawProjectError("can't create " + m_path + ": " + detail::lastError());
				if (mz_zip_writer_init_cfile(&m_zip, m_file.get(), 0) == MZ_FALSE)
					throw DawProjectError("can't create " + m_path);
				m_open = true;
			}

			~ZipWriter()
			{
				if (m_open)
					mz_zip_writer_end(&m_zip);
			}

			ZipWriter(const ZipWriter&) = delete;
			ZipWriter& operator=(const ZipWriter&) = delete;

			void add(const std::string& name, const void* data, size_t size, bool compress)
			{
				const mz_uint level = compress ? MZ_DEFAULT_LEVEL : MZ_NO_COMPRESSION;
				if (mz_zip_writer_add_mem(&m_zip, name.c_str(), data, size, level) == MZ_FALSE)
					throw DawProjectError("can't write " + name + " into " + m_path);
			}

			void add(const std::string& name, const std::string& text) { add(name, text.data(), text.size(), true); }

			void finish()
			{
				const bool done = mz_zip_writer_finalize_archive(&m_zip) != MZ_FALSE;
				mz_zip_writer_end(&m_zip);
				m_open = false;
				if (!done || std::fflush(m_file.get()) != 0)
					throw DawProjectError("can't complete " + m_path);
				m_file.reset();
			}

		private:
			std::string m_path;
			detail::File m_file;
			mz_zip_archive m_zip{};
			bool m_open = false;
		};

		class ZipReader
		{
		public:
			explicit ZipReader(std::string path) : m_path(std::move(path)), m_file(detail::openFile(m_path, "rb"))
			{
				if (!m_file)
					throw DawProjectError("can't open " + m_path + ": " + detail::lastError());
				if (mz_zip_reader_init_cfile(&m_zip, m_file.get(), 0, 0) == MZ_FALSE)
					throw DawProjectError("can't read " + m_path + ": not a DAWproject file");
				m_open = true;
			}

			~ZipReader()
			{
				if (m_open)
					mz_zip_reader_end(&m_zip);
			}

			ZipReader(const ZipReader&) = delete;
			ZipReader& operator=(const ZipReader&) = delete;

			bool contains(const std::string& name)
			{
				return mz_zip_reader_locate_file(&m_zip, name.c_str(), nullptr, 0) >= 0;
			}

			std::string text(const std::string& name)
			{
				size_t size = 0;
				void* data = mz_zip_reader_extract_to_heap(&m_zip, locate(name), &size, 0);
				if (data == nullptr)
					throw DawProjectError("can't read " + name + " in " + m_path);
				std::string text(static_cast<const char*>(data), size);
				mz_free(data);
				return text;
			}

			void extract(const std::string& name, const std::string& target)
			{
				const mz_uint index = locate(name);
				const detail::File file = detail::openFile(target, "wb");
				if (!file)
					throw DawProjectError("can't create " + target + ": " + detail::lastError());
				if (mz_zip_reader_extract_to_cfile(&m_zip, index, file.get(), 0) == MZ_FALSE ||
					std::fflush(file.get()) != 0)
					throw DawProjectError("can't extract " + name + " from " + m_path);
			}

		private:
			mz_uint locate(const std::string& name)
			{
				const int index = mz_zip_reader_locate_file(&m_zip, name.c_str(), nullptr, 0);
				if (index < 0)
					throw DawProjectError(m_path + " has no " + name);
				return static_cast<mz_uint>(index);
			}

			std::string m_path;
			detail::File m_file;
			mz_zip_archive m_zip{};
			bool m_open = false;
		};

		double toBeats(time::Ticks ticks)
		{
			return static_cast<double>(ticks) / static_cast<double>(time::g_ticksPerQuarter);
		}

		time::Ticks fromBeats(double beats)
		{
			return std::llround(beats * static_cast<double>(time::g_ticksPerQuarter));
		}

		std::string xmlId(const char* prefix, ObjectId id)
		{
			return prefix + std::to_string(id);
		}

		bool validColor(const std::string& color)
		{
			return color.size() == 7 && color[0] == '#' &&
				std::all_of(color.begin() + 1, color.end(),
					[](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
		}

		// Writing project.xml.
		class Exporter
		{
		public:
			Exporter(const Project& project, bool embed) : m_project(project), m_embed(embed)
			{
				// Embedded files keep their names, made unique where two would collide.
				std::set<std::string> taken;
				for (const auto& [id, media] : project.media)
				{
					if (!embed)
					{
						m_mediaPaths[id] = media.path;
						continue;
					}
					std::string name = "audio/" + pathOf(media.path).filename().u8string();
					if (!taken.insert(name).second)
					{
						name = "audio/" + std::to_string(id) + "-" + pathOf(media.path).filename().u8string();
						taken.insert(name);
					}
					m_mediaPaths[id] = name;
				}
			}

			const std::map<ObjectId, std::string>& mediaPaths() const { return m_mediaPaths; }

			std::string projectXml()
			{
				pugi::xml_document document;
				pugi::xml_node declaration = document.append_child(pugi::node_declaration);
				declaration.append_attribute("version") = "1.0";
				declaration.append_attribute("encoding") = "UTF-8";
				declaration.append_attribute("standalone") = "yes";
				pugi::xml_node root = document.append_child("Project");
				root.append_attribute("version") = "1.0";
				pugi::xml_node application = root.append_child("Application");
				application.append_attribute("name") = "GNUbase";
				application.append_attribute("version") = version();

				pugi::xml_node transport = root.append_child("Transport");
				pugi::xml_node tempo = transport.append_child("Tempo");
				tempo.append_attribute("unit") = "bpm";
				tempo.append_attribute("value") = m_project.tempo.points().front().bpm;
				tempo.append_attribute("id") = "tempo";
				pugi::xml_node signature = transport.append_child("TimeSignature");
				const time::TimeSignature first = m_project.signatures.changes().front().signature;
				signature.append_attribute("numerator") = first.numerator;
				signature.append_attribute("denominator") = first.denominator;
				signature.append_attribute("id") = "signature";

				pugi::xml_node structure = root.append_child("Structure");
				for (const ObjectId id : m_project.tracks)
					writeTrack(structure, m_project.trackById.at(id));
				for (const Bus& bus : m_project.busses)
					if (bus.direction == BusDirection::Output)
					{
						pugi::xml_node track = structure.append_child("Track");
						track.append_attribute("id") = xmlId("ot", bus.id).c_str();
						track.append_attribute("name") = bus.name.c_str();
						track.append_attribute("contentType") = "audio";
						track.append_attribute("loaded") = true;
						pugi::xml_node channel = track.append_child("Channel");
						channel.append_attribute("id") = xmlId("o", bus.id).c_str();
						channel.append_attribute("role") = "master";
						channel.append_attribute("audioChannels") = bus.channels;
					}

				pugi::xml_node arrangement = root.append_child("Arrangement");
				arrangement.append_attribute("id") = "arrangement";
				pugi::xml_node lanes = arrangement.append_child("Lanes");
				lanes.append_attribute("id") = "lanes";
				lanes.append_attribute("timeUnit") = "beats";
				for (const ObjectId id : m_project.trackOrder())
					writeLanes(lanes, m_project.trackById.at(id));
				writeMarkers(arrangement);
				writeTempo(arrangement);
				writeSignatures(arrangement);

				std::ostringstream xml;
				document.save(xml, "  ", pugi::format_default, pugi::encoding_utf8);
				return xml.str();
			}

		private:
			static const char* contentType(TrackType type)
			{
				switch (type)
				{
				case TrackType::Instrument:
				case TrackType::Midi:
					return "notes";
				case TrackType::Folder:
					return "tracks";
				default:
					return "audio";
				}
			}

			static const char* role(TrackType type)
			{
				if (type == TrackType::Group || type == TrackType::Folder)
					return "submix";
				return type == TrackType::Effect ? "effect" : "regular";
			}

			bool isBus(ObjectId id) const
			{
				return std::any_of(
					m_project.busses.begin(), m_project.busses.end(), [id](const Bus& bus) { return bus.id == id; });
			}

			void writeTrack(pugi::xml_node parent, const Track& track)
			{
				pugi::xml_node node = parent.append_child("Track");
				node.append_attribute("id") = xmlId("t", track.id).c_str();
				node.append_attribute("name") = track.name.c_str();
				if (!track.color.empty())
					node.append_attribute("color") = track.color.c_str();
				node.append_attribute("contentType") = contentType(track.type);
				node.append_attribute("loaded") = true;
				if (track.channel != 0)
				{
					const Channel& channel = m_project.channels.at(track.channel);
					pugi::xml_node element = node.append_child("Channel");
					element.append_attribute("id") = xmlId("c", channel.id).c_str();
					element.append_attribute("role") = role(track.type);
					element.append_attribute("audioChannels") = channel.channels;
					// A MIDI track's output is an instrument, which DAWproject's audio routing can't describe.
					if (channel.channels > 0 && channel.output != 0)
						element.append_attribute("destination") =
							xmlId(isBus(channel.output) ? "o" : "c", channel.output).c_str();
				}
				for (const ObjectId child : track.children)
					writeTrack(node, m_project.trackById.at(child));
			}

			// A position or length in the lane's time unit: beats on musical tracks, seconds on linear ones.
			double laneTime(const Track& track, int64_t value) const
			{
				return track.timeBase == time::TimeBase::Musical ? toBeats(value)
																 : static_cast<double>(value) / m_project.sampleRate;
			}

			void writeLanes(pugi::xml_node lanes, const Track& track)
			{
				if (track.type != TrackType::Audio && track.type != TrackType::Instrument &&
					track.type != TrackType::Midi)
					return;
				pugi::xml_node trackLanes = lanes.append_child("Lanes");
				trackLanes.append_attribute("id") = xmlId("l", track.id).c_str();
				trackLanes.append_attribute("track") = xmlId("t", track.id).c_str();
				trackLanes.append_attribute("timeUnit") =
					track.timeBase == time::TimeBase::Musical ? "beats" : "seconds";
				pugi::xml_node clips = trackLanes.append_child("Clips");
				clips.append_attribute("id") = xmlId("cl", track.id).c_str();
				for (const auto& [id, event] : m_project.audioEvents)
					if (event.track == track.id)
						writeAudioClip(clips, track, event);
				for (const auto& [id, part] : m_project.midiParts)
					if (part.track == track.id)
						writeMidiClip(clips, track, part);
			}

			void writeAudioClip(pugi::xml_node clips, const Track& track, const AudioEvent& event)
			{
				const Clip& clip = m_project.clips.at(event.clip);
				const MediaFile& media = m_project.media.at(clip.media);
				const double seconds = static_cast<double>(event.length) / media.sampleRate;
				double duration = seconds;
				if (track.timeBase == time::TimeBase::Musical)
				{
					const double start = m_project.tempo.toSeconds(event.position.value);
					duration = toBeats(m_project.tempo.toTicks(start + seconds) - event.position.value);
				}
				pugi::xml_node element = clips.append_child("Clip");
				element.append_attribute("time") = laneTime(track, event.position.value);
				element.append_attribute("duration") = duration;
				element.append_attribute("playStart") = static_cast<double>(event.offset) / media.sampleRate;
				element.append_attribute("contentTimeUnit") = "seconds";
				if (event.mute)
					element.append_attribute("enable") = false;
				element.append_attribute("name") = clip.name.c_str();
				pugi::xml_node audio = element.append_child("Audio");
				audio.append_attribute("id") = xmlId("a", event.id).c_str();
				audio.append_attribute("channels") = media.channels;
				audio.append_attribute("sampleRate") = std::llround(media.sampleRate);
				audio.append_attribute("duration") = static_cast<double>(media.length) / media.sampleRate;
				pugi::xml_node file = audio.append_child("File");
				file.append_attribute("path") = m_mediaPaths.at(media.id).c_str();
				if (!m_embed)
					file.append_attribute("external") = true;
			}

			void writeMidiClip(pugi::xml_node clips, const Track& track, const MidiPart& part)
			{
				pugi::xml_node element = clips.append_child("Clip");
				element.append_attribute("time") = laneTime(track, part.position.value);
				element.append_attribute("duration") = laneTime(track, part.length);
				element.append_attribute("playStart") = 0.0;
				element.append_attribute("contentTimeUnit") = "beats";
				element.append_attribute("name") = part.name.c_str();
				// Notes alone, or a lane each for the notes and every controller.
				pugi::xml_node content = element;
				if (!part.controllers.empty())
				{
					content = element.append_child("Lanes");
					content.append_attribute("id") = xmlId("pl", part.id).c_str();
				}
				pugi::xml_node notes = content.append_child("Notes");
				notes.append_attribute("id") = xmlId("n", part.id).c_str();
				for (const auto& [id, note] : part.notes)
				{
					pugi::xml_node noteNode = notes.append_child("Note");
					noteNode.append_attribute("time") = toBeats(note.position);
					noteNode.append_attribute("duration") = toBeats(note.length);
					noteNode.append_attribute("channel") = note.channel;
					noteNode.append_attribute("key") = note.pitch;
					noteNode.append_attribute("vel") = note.velocity / g_maxVelocity;
				}
				std::map<std::pair<int, int>, std::vector<const Controller*>> byController;
				for (const auto& [id, controller] : part.controllers)
					byController[{controller.channel, controller.number}].push_back(&controller);
				for (const auto& [key, controllers] : byController)
				{
					pugi::xml_node points = content.append_child("Points");
					points.append_attribute("id") =
						(xmlId("p", part.id) + "-" + std::to_string(key.first) + "-" + std::to_string(key.second))
							.c_str();
					points.append_attribute("unit") = "normalized";
					pugi::xml_node target = points.append_child("Target");
					target.append_attribute("expression") = "channelController";
					target.append_attribute("channel") = key.first;
					target.append_attribute("controller") = key.second;
					for (const Controller* controller : controllers)
					{
						pugi::xml_node point = points.append_child("RealPoint");
						point.append_attribute("time") = toBeats(controller->position);
						point.append_attribute("value") = controller->value / g_maxControllerValue;
					}
				}
			}

			void writeMarkers(pugi::xml_node arrangement) const
			{
				if (m_project.markers.empty())
					return;
				pugi::xml_node markers = arrangement.append_child("Markers");
				markers.append_attribute("id") = "markers";
				for (const auto& [id, marker] : m_project.markers)
				{
					pugi::xml_node element = markers.append_child("Marker");
					element.append_attribute("time") = toBeats(marker.position);
					element.append_attribute("name") = marker.name.c_str();
					if (!marker.color.empty())
						element.append_attribute("color") = marker.color.c_str();
				}
			}

			void writeTempo(pugi::xml_node arrangement) const
			{
				const std::vector<time::TempoPoint>& points = m_project.tempo.points();
				if (points.size() < 2)
					return;
				pugi::xml_node automation = arrangement.append_child("TempoAutomation");
				automation.append_attribute("id") = "tempoAutomation";
				automation.append_attribute("unit") = "bpm";
				automation.append_child("Target").append_attribute("parameter") = "tempo";
				for (size_t i = 0; i < points.size(); ++i)
				{
					pugi::xml_node point = automation.append_child("RealPoint");
					point.append_attribute("time") = toBeats(points[i].position);
					point.append_attribute("value") = points[i].bpm;
					// DAWproject describes the curve from a point to the next one; GNUbase marks ramps on the next.
					const bool ramp = i + 1 < points.size() && points[i + 1].ramp;
					point.append_attribute("interpolation") = ramp ? "linear" : "hold";
				}
			}

			void writeSignatures(pugi::xml_node arrangement) const
			{
				const std::vector<time::SignatureChange> changes = m_project.signatures.changes();
				if (changes.size() < 2)
					return;
				pugi::xml_node automation = arrangement.append_child("TimeSignatureAutomation");
				automation.append_attribute("id") = "signatureAutomation";
				automation.append_child("Target").append_attribute("parameter") = "signature";
				for (const time::SignatureChange& change : changes)
				{
					pugi::xml_node point = automation.append_child("TimeSignaturePoint");
					point.append_attribute("time") = toBeats(m_project.signatures.barStart(change.bar));
					point.append_attribute("numerator") = change.signature.numerator;
					point.append_attribute("denominator") = change.signature.denominator;
				}
			}

			const Project& m_project;
			std::map<ObjectId, std::string> m_mediaPaths; // the path project.xml uses for each media file
			bool m_embed;
		};

		// Reading project.xml.
		class Importer
		{
		public:
			Importer(ZipReader& zip, std::filesystem::path fileFolder, std::filesystem::path mediaFolder)
				: m_zip(zip), m_fileFolder(std::move(fileFolder)), m_mediaFolder(std::move(mediaFolder))
			{
			}

			Project load(const pugi::xml_node& root)
			{
				readTransport(root);
				for (const pugi::xml_node track : root.child("Structure").children("Track"))
					readTrack(track, 0);
				resolveDestinations();
				const pugi::xml_node arrangement = root.child("Arrangement");
				for (const pugi::xml_node lanes : arrangement.children("Lanes"))
					readLanes(lanes, {});
				readMarkers(arrangement.child("Markers"));
				return std::move(m_project);
			}

			// Where a media file named in project.xml is now: extracted, or where it was referenced.
			std::string resolvedPath(const std::string& xmlPath, bool external)
			{
				const auto found = m_resolved.find(xmlPath);
				if (found != m_resolved.end())
					return found->second;
				std::filesystem::path target;
				if (external)
				{
					target = pathOf(xmlPath);
					if (target.is_relative())
						target = m_fileFolder / target;
				}
				else
				{
					// Only plain relative paths inside the file: nothing may be extracted outside the media folder.
					const std::filesystem::path inside = pathOf(xmlPath).lexically_normal();
					if (inside.empty() || inside.is_absolute() || inside.has_root_name() ||
						*inside.begin() == std::filesystem::path(".."))
						throw DawProjectError("unsafe media path in the file: " + xmlPath);
					target = m_mediaFolder / inside;
					std::error_code error;
					std::filesystem::create_directories(target.parent_path(), error);
					m_zip.extract(xmlPath, target.u8string());
				}
				return m_resolved[xmlPath] = target.u8string();
			}

		private:
			struct Context
			{
				ObjectId track = 0;
				bool seconds = false; // the time unit is seconds rather than beats
				double origin = 0.0;  // where the content's time 0 is on the lane
				bool enabled = true;  // false inside a disabled (muted) clip
			};

			time::Ticks toTicks(double time, bool seconds) const
			{
				return std::max<time::Ticks>(0, seconds ? m_project.tempo.toTicks(time) : fromBeats(time));
			}

			void readTransport(const pugi::xml_node& root)
			{
				const pugi::xml_node transport = root.child("Transport");
				const double bpm = transport.child("Tempo").attribute("value").as_double(120.0);
				std::vector<time::TempoPoint> tempo{{0, bpm, false}};
				bool ramp = false;
				for (const pugi::xml_node point :
					root.child("Arrangement").child("TempoAutomation").children("RealPoint"))
				{
					const time::Ticks position = fromBeats(point.attribute("time").as_double());
					const double value = point.attribute("value").as_double(bpm);
					if (position <= 0)
						tempo.front().bpm = value;
					else
						tempo.push_back({position, value, ramp});
					ramp = std::string(point.attribute("interpolation").as_string("hold")) == "linear";
				}
				const pugi::xml_node signature = transport.child("TimeSignature");
				time::TimeSignature first{
					signature.attribute("numerator").as_int(4), signature.attribute("denominator").as_int(4)};
				std::vector<time::SignatureChange> changes{{0, first}};
				// Points are at beats; a signature changes at a bar, counted with the signature before it.
				double changeBeat = 0.0;
				for (const pugi::xml_node point :
					root.child("Arrangement").child("TimeSignatureAutomation").children("TimeSignaturePoint"))
				{
					const double beat = point.attribute("time").as_double();
					const time::TimeSignature next{
						point.attribute("numerator").as_int(4), point.attribute("denominator").as_int(4)};
					if (beat <= 0.0)
					{
						changes.front().signature = next;
						continue;
					}
					const time::TimeSignature previous = changes.back().signature;
					const double barBeats = previous.numerator * 4.0 / previous.denominator;
					const int64_t bar = changes.back().bar + std::llround((beat - changeBeat) / barBeats);
					if (bar > changes.back().bar)
					{
						changes.push_back({bar, next});
						changeBeat = beat;
					}
				}
				try
				{
					m_project.tempo = time::TempoMap(tempo);
					m_project.signatures = time::SignatureMap(changes);
				}
				catch (const std::invalid_argument& error)
				{
					throw DawProjectError(
						std::string("the file's tempo or time signature is invalid: ") + error.what());
				}
			}

			Bus& mainOutput()
			{
				return *std::find_if(m_project.busses.begin(), m_project.busses.end(),
					[](const Bus& bus) { return bus.direction == BusDirection::Output; });
			}

			void readTrack(const pugi::xml_node& node, ObjectId parent)
			{
				const pugi::xml_node channelNode = node.child("Channel");
				const std::string role = channelNode.attribute("role").as_string("regular");
				const std::string content = node.attribute("contentType").as_string("");
				const bool hasChildren = static_cast<bool>(node.child("Track"));
				const int audioChannels = channelNode.attribute("audioChannels").as_int(2);
				if (role == "master" && !hasChildren)
				{
					// The first master channel becomes the main output, any other one another output bus.
					Bus bus = m_firstMaster ? mainOutput() : Bus{m_project.newId(), "", BusDirection::Output, 2};
					bus.name = node.attribute("name").as_string("Stereo Out");
					bus.channels = std::clamp(audioChannels, 1, g_maxAudioChannels);
					if (m_firstMaster)
						mainOutput() = bus;
					else
						m_project.busses.push_back(bus);
					m_firstMaster = false;
					m_channels[channelNode.attribute("id").as_string()] = bus.id;
					return;
				}

				TrackType type = TrackType::Audio;
				if (hasChildren || content == "tracks")
					type = TrackType::Folder;
				else if (role == "effect")
					type = TrackType::Effect;
				else if (role == "submix")
					type = TrackType::Group;
				else if (content.find("notes") != std::string::npos)
					type = !channelNode.empty() && audioChannels > 0 ? TrackType::Instrument : TrackType::Midi;

				Track track;
				track.id = m_project.newId();
				track.type = type;
				track.name = node.attribute("name").as_string(trackTypeName(type));
				const std::string color = node.attribute("color").as_string("");
				track.color = validColor(color) ? color : std::string();
				track.parent = parent;
				if (type != TrackType::Folder || !channelNode.empty())
				{
					Channel channel;
					channel.id = m_project.newId();
					channel.track = track.id;
					channel.channels = type == TrackType::Midi ? 0 : std::clamp(audioChannels, 1, g_maxAudioChannels);
					if (type != TrackType::Midi)
						channel.output = mainOutput().id;
					if (type == TrackType::Audio)
						for (const Bus& bus : m_project.busses)
							if (bus.direction == BusDirection::Input)
							{
								channel.input = bus.id;
								break;
							}
					track.channel = channel.id;
					m_project.channels[channel.id] = channel;
					m_channels[channelNode.attribute("id").as_string()] = channel.id;
					if (const pugi::xml_attribute destination = channelNode.attribute("destination"))
						m_destinations.emplace_back(channel.id, destination.as_string());
				}
				(parent == 0 ? m_project.tracks : m_project.trackById.at(parent).children).push_back(track.id);
				m_project.trackById[track.id] = track;
				m_tracks[node.attribute("id").as_string()] = track.id;
				for (const pugi::xml_node child : node.children("Track"))
					readTrack(child, track.id);
			}

			// Outputs to busses and group channels; other destinations stay on the main output.
			void resolveDestinations()
			{
				for (const auto& [channelId, destination] : m_destinations)
				{
					const auto target = m_channels.find(destination);
					if (target == m_channels.end() || target->second == channelId)
						continue;
					const bool bus = std::any_of(m_project.busses.begin(), m_project.busses.end(),
						[&target](const Bus& b) { return b.id == target->second; });
					const auto channel = m_project.channels.find(target->second);
					const bool group = channel != m_project.channels.end() &&
						(m_project.trackById.at(channel->second.track).type == TrackType::Group ||
							m_project.trackById.at(channel->second.track).type == TrackType::Folder);
					if (bus || group)
						m_project.channels.at(channelId).output = target->second;
				}
				// A group routed into a group must not close a loop; such outputs fall back to the main output.
				for (auto& [id, channel] : m_project.channels)
				{
					std::set<ObjectId> seen{id};
					for (ObjectId next = channel.output; m_project.channels.count(next) != 0;
						next = m_project.channels.at(next).output)
						if (!seen.insert(next).second)
						{
							channel.output = mainOutput().id;
							break;
						}
				}
			}

			void readLanes(const pugi::xml_node& lanes, Context context)
			{
				if (const pugi::xml_attribute unit = lanes.attribute("timeUnit"))
					context.seconds = std::string(unit.as_string()) == "seconds";
				if (const pugi::xml_attribute track = lanes.attribute("track"))
				{
					const auto found = m_tracks.find(track.as_string());
					context.track = found != m_tracks.end() ? found->second : 0;
				}
				for (const pugi::xml_node child : lanes.children())
				{
					const std::string name = child.name();
					if (name == "Lanes")
						readLanes(child, context);
					else if (name == "Clips")
						for (const pugi::xml_node clip : child.children("Clip"))
							readClip(clip, context);
				}
			}

			void readClip(const pugi::xml_node& clip, const Context& context)
			{
				if (context.track == 0)
					return;
				const double start = context.origin + clip.attribute("time").as_double();
				const double duration = clip.attribute("duration").as_double();
				const double playStart = clip.attribute("playStart").as_double(0.0);
				Context content = context;
				if (const pugi::xml_attribute unit = clip.attribute("contentTimeUnit"))
					content.seconds = std::string(unit.as_string()) == "seconds";
				content.origin = content.seconds == context.seconds ? start - playStart : 0.0;
				content.enabled = context.enabled && clip.attribute("enable").as_bool(true);
				const TrackType type = m_project.trackById.at(context.track).type;
				MidiPart* part = nullptr;
				const auto partFor = [&]() -> MidiPart&
				{
					if (part == nullptr)
						part = &addPart(context, start, duration, clip.attribute("name").as_string(""));
					return *part;
				};
				readContent(clip, context, content, start, duration, playStart, type, partFor);
			}

			template <typename PartFor> void readContent(const pugi::xml_node& node, const Context& lane,
				const Context& content, double start, double duration, double playStart, TrackType type,
				PartFor& partFor)
			{
				const bool notes = type == TrackType::Instrument || type == TrackType::Midi;
				const ClipTiming timing{
					content.seconds, playStart, lane.seconds ? start : secondsBetween(0.0, start, false)};
				for (const pugi::xml_node child : node.children())
				{
					const std::string name = child.name();
					if (name == "Notes" && notes)
					{
						readNotes(child, partFor(), timing);
					}
					else if (name == "Points" && notes)
					{
						readPoints(child, partFor(), timing);
					}
					else if (name == "Lanes")
					{
						readContent(child, lane, content, start, duration, playStart, type, partFor);
					}
					else if (name == "Clips")
					{
						for (const pugi::xml_node clip : child.children("Clip"))
							readClip(clip, content);
					}
					else if ((name == "Audio" || name == "Warps") && type == TrackType::Audio)
					{
						const pugi::xml_node audio = name == "Audio" ? child : child.child("Audio");
						const double offset = content.seconds ? playStart : secondsBetween(0.0, playStart, false);
						addAudioEvent(lane, start, duration, offset, audio, !content.enabled);
					}
				}
			}

			double secondsBetween(double from, double to, bool seconds) const
			{
				if (seconds)
					return to - from;
				return m_project.tempo.toSeconds(fromBeats(to)) - m_project.tempo.toSeconds(fromBeats(from));
			}

			MidiPart& addPart(const Context& lane, double start, double duration, const std::string& name)
			{
				MidiPart part;
				part.id = m_project.newId();
				part.track = lane.track;
				part.name = name.empty() ? m_project.trackById.at(lane.track).name : name;
				const time::Ticks position = toTicks(start, lane.seconds);
				part.position = {time::TimeBase::Musical, position};
				part.length = std::max<time::Ticks>(1, toTicks(start + duration, lane.seconds) - position);
				return m_project.midiParts[part.id] = std::move(part);
			}

			// Where content of a clip lies: notes and points count from the clip's playStart, in the content's unit.
			struct ClipTiming
			{
				bool seconds = false;
				double playStart = 0.0;
				double startSeconds = 0.0; // the clip's start on the timeline
			};

			time::Ticks partTicks(const MidiPart& part, const ClipTiming& timing, double time) const
			{
				if (!timing.seconds)
					return fromBeats(time - timing.playStart);
				return m_project.tempo.toTicks(timing.startSeconds + time - timing.playStart) - part.position.value;
			}

			void readNotes(const pugi::xml_node& notes, MidiPart& part, const ClipTiming& timing)
			{
				for (const pugi::xml_node element : notes.children("Note"))
				{
					Note note;
					note.id = m_project.newId();
					note.position =
						std::max<time::Ticks>(0, partTicks(part, timing, element.attribute("time").as_double()));
					note.length = std::max<time::Ticks>(1, fromBeats(element.attribute("duration").as_double()));
					note.pitch = static_cast<uint8_t>(std::clamp(element.attribute("key").as_int(60), 0, 127));
					note.channel = static_cast<uint8_t>(std::clamp(element.attribute("channel").as_int(0), 0, 15));
					const double velocity = std::clamp(element.attribute("vel").as_double(0.8), 0.0, 1.0);
					note.velocity = static_cast<uint16_t>(std::lround(velocity * g_maxVelocity));
					part.notes[note.id] = note;
				}
			}

			void readPoints(const pugi::xml_node& points, MidiPart& part, const ClipTiming& timing)
			{
				const pugi::xml_node target = points.child("Target");
				if (std::string(target.attribute("expression").as_string("")) != "channelController")
					return;
				const int number = std::clamp(target.attribute("controller").as_int(0), 0, 127);
				const int channel = std::clamp(target.attribute("channel").as_int(0), 0, 15);
				for (const pugi::xml_node point : points.children("RealPoint"))
				{
					Controller controller;
					controller.id = m_project.newId();
					controller.position =
						std::max<time::Ticks>(0, partTicks(part, timing, point.attribute("time").as_double()));
					controller.number = static_cast<uint8_t>(number);
					controller.channel = static_cast<uint8_t>(channel);
					const double value = std::clamp(point.attribute("value").as_double(0.0), 0.0, 1.0);
					controller.value = static_cast<uint32_t>(std::llround(value * g_maxControllerValue));
					part.controllers[controller.id] = controller;
				}
			}

			ObjectId clipFor(const pugi::xml_node& audio)
			{
				const pugi::xml_node file = audio.child("File");
				const std::string xmlPath = file.attribute("path").as_string("");
				if (xmlPath.empty())
					return 0;
				const auto found = m_clips.find(xmlPath);
				if (found != m_clips.end())
					return found->second;

				MediaFile media;
				media.id = m_project.newId();
				media.path = resolvedPath(xmlPath, file.attribute("external").as_bool(false));
				try
				{
					const AudioFileInfo info = AudioFileReader::open(media.path)->info();
					media.sampleRate = info.sampleRate;
					media.channels = info.channels;
					media.length = info.frames;
				}
				catch (const AudioFileError&)
				{
					// A missing or unreadable file: keep what project.xml says about it.
					media.sampleRate = audio.attribute("sampleRate").as_double(48000.0);
					media.channels = audio.attribute("channels").as_int(2);
					media.length = std::llround(audio.attribute("duration").as_double() * media.sampleRate);
				}
				media.channels = std::clamp(media.channels, 1, g_maxAudioChannels);
				m_project.media[media.id] = media;
				const Clip clip{m_project.newId(), pathOf(xmlPath).stem().u8string(), media.id};
				m_project.clips[clip.id] = clip;
				return m_clips[xmlPath] = clip.id;
			}

			void addAudioEvent(const Context& lane, double start, double duration, double offset,
				const pugi::xml_node& audio, bool mute)
			{
				const ObjectId clipId = clipFor(audio);
				if (clipId == 0)
					return;
				const MediaFile& media = m_project.media.at(m_project.clips.at(clipId).media);
				AudioEvent event;
				event.id = m_project.newId();
				event.track = lane.track;
				event.clip = clipId;
				event.mute = mute;
				event.position = {time::TimeBase::Musical, toTicks(start, lane.seconds)};
				event.offset = std::clamp<time::Samples>(
					std::llround(offset * media.sampleRate), 0, std::max<time::Samples>(0, media.length - 1));
				const double seconds = secondsBetween(start, start + duration, lane.seconds);
				event.length =
					std::min<time::Samples>(std::llround(seconds * media.sampleRate), media.length - event.offset);
				if (event.length > 0)
					m_project.audioEvents[event.id] = event;
			}

			void readMarkers(const pugi::xml_node& markers)
			{
				const bool seconds = std::string(markers.attribute("timeUnit").as_string("beats")) == "seconds";
				for (const pugi::xml_node element : markers.children("Marker"))
				{
					Marker marker;
					marker.id = m_project.newId();
					marker.position = toTicks(element.attribute("time").as_double(), seconds);
					marker.name = element.attribute("name").as_string("");
					const std::string color = element.attribute("color").as_string("");
					marker.color = validColor(color) ? color : std::string();
					m_project.markers[marker.id] = marker;
				}
			}

			ZipReader& m_zip;
			std::filesystem::path m_fileFolder;
			std::filesystem::path m_mediaFolder;
			Project m_project = newProject();
			bool m_firstMaster = true;
			std::map<std::string, ObjectId> m_tracks;   // project.xml track id -> track
			std::map<std::string, ObjectId> m_channels; // project.xml channel id -> channel or bus
			std::vector<std::pair<ObjectId, std::string>> m_destinations;
			std::map<std::string, ObjectId> m_clips;       // media path in project.xml -> clip
			std::map<std::string, std::string> m_resolved; // media path in project.xml -> path on disk
		};
	}

	void saveDawProject(const Project& project, const std::string& path, const DawProjectSaveOptions& options)
	{
		Exporter exporter(project, options.embedMedia);
		const std::string xml = exporter.projectXml();
		Json document = toJson(project);
		document["nextId"] = project.nextId;
		Json media = Json::object();
		for (const auto& [id, mediaPath] : exporter.mediaPaths())
			media[std::to_string(id)] = mediaPath;
		const Json extension = {{"format", g_extensionFormat}, {"application", std::string("GNUbase ") + version()},
			{"projectXml", checksum(xml)}, {"media", media}, {"project", document}};

		pugi::xml_document metadata;
		metadata.append_child("MetaData").append_child("Title").text() = project.name.c_str();
		std::ostringstream metadataXml;
		metadata.save(metadataXml, "  ", pugi::format_default, pugi::encoding_utf8);

		// Written next to the file and then moved over it, so a failed save leaves the old file as it was.
		const std::string temporary = path + ".saving";
		{
			ZipWriter zip(temporary);
			zip.add("project.xml", xml);
			zip.add("metadata.xml", metadataXml.str());
			zip.add(g_extensionEntry, extension.dump(1, '\t'));
			if (options.embedMedia)
				for (const auto& [id, mediaPath] : exporter.mediaPaths())
				{
					const std::vector<char> bytes = readBytes(project.media.at(id).path);
					zip.add(mediaPath, bytes.data(), bytes.size(), false);
				}
			zip.finish();
		}
		std::error_code error;
		std::filesystem::rename(pathOf(temporary), pathOf(path), error);
		if (error)
		{
			std::filesystem::remove(pathOf(temporary), error);
			throw DawProjectError("can't save " + path + ": " + error.message());
		}
	}

	Project loadDawProject(const std::string& path, const std::string& mediaFolder)
	{
		ZipReader zip(path);
		const std::string xml = zip.text("project.xml");
		pugi::xml_document document;
		const pugi::xml_parse_result parsed = document.load_buffer(xml.data(), xml.size());
		if (!parsed)
			throw DawProjectError("can't read project.xml in " + path + ": " + parsed.description());
		const pugi::xml_node root = document.child("Project");
		if (!root)
			throw DawProjectError(path + " has no DAWproject project");

		Importer importer(zip, pathOf(path).parent_path(), pathOf(mediaFolder));
		Project fromXml = importer.load(root);
		if (!zip.contains(g_extensionEntry))
			return fromXml;
		try
		{
			const Json extension = Json::parse(zip.text(g_extensionEntry));
			if (extension.value("format", 0) != g_extensionFormat || extension.value("projectXml", "") != checksum(xml))
				return fromXml;
			Project project = projectFromJson(extension.at("project"));
			const Json& media = extension.at("media");
			for (auto& [id, file] : project.media)
			{
				const auto stored = media.find(std::to_string(id));
				if (stored != media.end())
					file.path =
						importer.resolvedPath(stored->get<std::string>(), !zip.contains(stored->get<std::string>()));
			}
			return project;
		}
		catch (const std::exception&)
		{
			return fromXml; // a damaged extension entry: project.xml still describes the project
		}
	}
}
