// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/devices.h"

#include "engine/document/document.h"
#include "engine/engine.h"

#include <array>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>

namespace gnubase::engine::midi
{
	namespace
	{
		// General MIDI's programs, in eight-program families.
		constexpr std::array<const char*, 16> g_gmFamilies = {"Piano", "Chromatic Percussion", "Organ", "Guitar",
			"Bass", "Strings", "Ensemble", "Brass", "Reed", "Pipe", "Synth Lead", "Synth Pad", "Synth Effects",
			"Ethnic", "Percussive", "Sound Effects"};
		constexpr std::array<const char*, 128> g_gmPrograms = {"Acoustic Grand Piano", "Bright Acoustic Piano",
			"Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavi",
			"Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
			"Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica",
			"Tango Accordion", "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)",
			"Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar",
			"Guitar Harmonics", "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass",
			"Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin", "Viola", "Cello", "Contrabass",
			"Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani", "String Ensemble 1",
			"String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice",
			"Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section",
			"Synth Brass 1", "Synth Brass 2", "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe",
			"English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle",
			"Shakuhachi", "Whistle", "Ocarina", "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)",
			"Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)",
			"Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)",
			"Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)", "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)",
			"FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar",
			"Banjo", "Shamisen", "Koto", "Kalimba", "Bag pipe", "Fiddle", "Shanai", "Tinkle Bell", "Agogo",
			"Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
			"Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause",
			"Gunshot"};

		Json gmBanks()
		{
			Json patches = Json::array();
			for (size_t program = 0; program < g_gmPrograms.size(); ++program)
				patches.push_back(
					{{"name", g_gmPrograms[program]}, {"folder", g_gmFamilies[program / 8]}, {"program", program}});
			return Json::array({{{"name", "GM"}, {"patches", std::move(patches)}}});
		}

		Json nameSchema()
		{
			return {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}, {"required", {"name"}},
				{"additionalProperties", false}};
		}

		Json banksSchema()
		{
			const Json patch = {{"type", "object"},
				{"properties",
					{{"name", {{"type", "string"}}}, {"folder", {{"type", "string"}}},
						{"program", {{"type", "integer"}, {"minimum", 0}, {"maximum", 127}}},
						{"bank", {{"type", "integer"}, {"minimum", 0}, {"maximum", g_maxBank}}}}},
				{"required", {"name", "program"}}};
			const Json bank = {{"type", "object"},
				{"properties", {{"name", {{"type", "string"}}}, {"patches", {{"type", "array"}, {"items", patch}}}}},
				{"required", {"name", "patches"}}};
			return {{"type", "array"}, {"items", bank}};
		}

		// Bank Assignment (reference manual p. 1109): the bank, by its index, that MIDI channels "1" to "16" use.
		Json assignmentSchema()
		{
			Json channels = Json::object();
			for (int channel = 1; channel <= 16; ++channel)
				channels[std::to_string(channel)] = {{"type", "integer"}, {"minimum", 0}};
			return {{"type", "object"}, {"properties", std::move(channels)}, {"additionalProperties", false}};
		}
	}

	Json devicesSchema()
	{
		return {{"type", "object"},
			{"additionalProperties",
				{{"type", "object"}, {"properties", {{"banks", banksSchema()}, {"bankAssignment", assignmentSchema()}}},
					{"required", {"banks"}}}}};
	}

	void registerDeviceCommands(Engine& engine)
	{
		const Json device = {{"type", "object"},
			{"properties",
				{{"name", {{"type", "string"}}}, {"banks", banksSchema()}, {"bankAssignment", assignmentSchema()}}},
			{"required", {"name", "banks"}}};
		Json installSchema = nameSchema();
		installSchema["properties"]["template"] = {{"type", "string"}, {"enum", {"empty", "gm"}}};
		engine.registerCommand(
			{"midiDevice.install", 1,
				"Installs a MIDI device (reference manual pp. 1109-1114): a representation of an external "
				"instrument with its patches by name, which a MIDI track's channel names (channel.set device) to "
				"choose its program and bank by patch. name must be new. template gm gives it General MIDI's 128 "
				"programs in the bank GM, in folders by family; empty (the default) gives it no banks, for patches "
				"of its own. Devices are the user setting midi.devices, where settings.set edits their banks and "
				"patches. Returns the device.",
				installSchema, {{"type", "object"}, {"properties", {{"device", device}}}, {"required", {"device"}}},
				false},
			[&engine](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto name = parameters.at("name").get<std::string>();
				if (name.empty())
					throw CommandError("a MIDI device needs a name");
				const Json banks = parameters.value("template", "empty") == "gm" ? gmBanks() : Json::array();
				engine.modifySetting(context, "midi.devices",
					[&](const Json& value)
					{
						if (value.contains(name))
							throw CommandError("there is already a MIDI device " + name);
						Json changed = value;
						changed[name] = {{"banks", banks}};
						return changed;
					});
				return Json{{"device", {{"name", name}, {"banks", banks}}}};
			});
		Json editSchema = nameSchema();
		editSchema["properties"]["banks"] = banksSchema();
		editSchema["properties"]["bankAssignment"] = assignmentSchema();
		editSchema["required"].push_back("banks");
		engine.registerCommand(
			{"midiDevice.set", 1,
				"Replaces an installed MIDI device's patch banks (reference manual pp. 1111-1114). Each bank has a "
				"name and patches; each patch has a name, program (0 to 127), optional bank (0 to 16383) and folder. "
				"bankAssignment, the Bank Assignment (p. 1109), replaces the bank that MIDI channels use, by index "
				"in banks, keyed by the channel's number from \"1\" to \"16\": the program selector of a MIDI track "
				"on such a channel (Any is 1) offers only that bank's patches. Without it the device keeps its "
				"assignment by index and drops indices past the last bank, so a caller that removes a bank passes the "
				"shifted assignment. Edits only this device in the user setting midi.devices, "
				"announces settings.changed, and returns the device. Tracks keep their selected program and bank.",
				editSchema, {{"type", "object"}, {"properties", {{"device", device}}}, {"required", {"device"}}},
				false},
			[&engine](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto name = parameters.at("name").get<std::string>();
				const Json& banks = parameters.at("banks");
				if (parameters.contains("bankAssignment"))
					for (const auto& [channel, bank] : parameters.at("bankAssignment").items())
						if (bank.get<size_t>() >= banks.size())
							throw CommandError(
								"MIDI channel " + channel + " is assigned a bank the device doesn't have");
				Json device;
				engine.modifySetting(context, "midi.devices",
					[&](const Json& value)
					{
						if (!value.contains(name))
							throw CommandError("there is no MIDI device " + name);
						Json changed = value;
						device = value.at(name);
						device["banks"] = banks;
						Json assignment =
							parameters.value("bankAssignment", device.value("bankAssignment", Json::object()));
						for (auto channel = assignment.begin(); channel != assignment.end();)
							channel =
								channel->get<size_t>() < banks.size() ? std::next(channel) : assignment.erase(channel);
						device.erase("bankAssignment");
						if (!assignment.empty())
							device["bankAssignment"] = std::move(assignment);
						changed[name] = device;
						return changed;
					});
				device["name"] = name;
				return Json{{"device", std::move(device)}};
			});
		engine.registerCommand({"midiDevice.remove", 1,
								   "Removes an installed MIDI device (midiDevice.install) by name. Tracks that name it "
								   "keep their program and bank.",
								   nameSchema(), Json::object(), false},
			[&engine](CommandContext& context)
			{
				const auto name = context.parameters().at("name").get<std::string>();
				engine.modifySetting(context, "midi.devices",
					[&](const Json& value)
					{
						if (!value.contains(name))
							throw CommandError("there is no MIDI device " + name);
						Json changed = value;
						changed.erase(name);
						return changed;
					});
				return Json::object();
			});
	}
}
