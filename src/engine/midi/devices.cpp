// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/devices.h"

#include "engine/document/document.h"
#include "engine/engine.h"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
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
			// folders: the folders that the bank lists even without patches (New Folder, reference manual p. 1111),
			// as paths with / between nested folders, as a patch's folder.
			const Json bank = {{"type", "object"},
				{"properties",
					{{"name", {{"type", "string"}}}, {"patches", {{"type", "array"}, {"items", patch}}},
						{"folders", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
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

		// Export Setup and Import Setup (reference manual pp. 1110, 1115): MIDI devices in an XML file of GNUbase's,
		// <MidiDevices> with a <Device name> each, its <Bank name>s with their <Patch name program bank folder>s and
		// <Folder name>s and its <BankAssignment channel bank>s. Programs and banks count from 0, as in midi.devices.
		std::string writeSetup(const Json& devices)
		{
			pugi::xml_document document;
			pugi::xml_node root = document.append_child("MidiDevices");
			for (const auto& [name, device] : devices.items())
			{
				pugi::xml_node node = root.append_child("Device");
				node.append_attribute("name") = name.c_str();
				for (const Json& bank : device.at("banks"))
				{
					pugi::xml_node bankNode = node.append_child("Bank");
					bankNode.append_attribute("name") = bank.at("name").get<std::string>().c_str();
					for (const Json& patch : bank.at("patches"))
					{
						pugi::xml_node patchNode = bankNode.append_child("Patch");
						patchNode.append_attribute("name") = patch.at("name").get<std::string>().c_str();
						patchNode.append_attribute("program") = patch.at("program").get<int>();
						if (patch.contains("bank"))
							patchNode.append_attribute("bank") = patch.at("bank").get<int>();
						if (patch.contains("folder"))
							patchNode.append_attribute("folder") = patch.at("folder").get<std::string>().c_str();
					}
					for (const Json& folder : bank.value("folders", Json::array()))
						bankNode.append_child("Folder").append_attribute("name") = folder.get<std::string>().c_str();
				}
				const Json assignment = device.value("bankAssignment", Json::object());
				for (const auto& [channel, bank] : assignment.items())
				{
					pugi::xml_node assigned = node.append_child("BankAssignment");
					assigned.append_attribute("channel") = channel.c_str();
					assigned.append_attribute("bank") = bank.get<int>();
				}
			}
			std::ostringstream text;
			document.save(text, "\t", pugi::format_default, pugi::encoding_utf8);
			return text.str();
		}

		// An attribute's whole number into to[key], if the element has it; subject names it in the error, e.g. "the
		// program of patch P of A".
		void readNumber(
			Json& to, const std::string& key, const pugi::xml_attribute& attribute, const std::string& subject)
		{
			if (!attribute)
				return;
			const std::string_view text = attribute.value();
			long long number = 0;
			const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
			if (error != std::errc() || end != text.data() + text.size())
				throw CommandError(subject + ", " + std::string(text) + ", isn't a whole number");
			to[key] = number;
		}

		// The devices of a setup file, in its order, as midiDevice.set takes them: {name, banks, bankAssignment}.
		Json readSetup(const std::string& path)
		{
			std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
			if (!in)
				throw CommandError("can't open " + path);
			const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
			pugi::xml_document document;
			const pugi::xml_node root =
				document.load_string(text.c_str()) ? document.child("MidiDevices") : pugi::xml_node();
			if (!root)
				throw CommandError(path + " isn't a MIDI device setup");
			Json devices = Json::array();
			Json byName = Json::object();
			for (const pugi::xml_node node : root.children("Device"))
			{
				const std::string name = node.attribute("name").value();
				if (name.empty())
					throw CommandError("a MIDI device in the setup has no name");
				if (byName.contains(name))
					throw CommandError("the setup has the MIDI device " + name + " twice");
				Json banks = Json::array();
				for (const pugi::xml_node bankNode : node.children("Bank"))
				{
					Json patches = Json::array();
					for (const pugi::xml_node patchNode : bankNode.children("Patch"))
					{
						Json patch = {{"name", patchNode.attribute("name").value()}};
						const std::string of = " of patch " + patch.at("name").get<std::string>() + " of " + name;
						readNumber(patch, "program", patchNode.attribute("program"), "the program" + of);
						readNumber(patch, "bank", patchNode.attribute("bank"), "the bank" + of);
						if (const pugi::xml_attribute folder = patchNode.attribute("folder"))
							patch["folder"] = folder.value();
						patches.push_back(std::move(patch));
					}
					Json bank = {{"name", bankNode.attribute("name").value()}, {"patches", std::move(patches)}};
					for (const pugi::xml_node folder : bankNode.children("Folder"))
						bank["folders"].push_back(folder.attribute("name").value());
					banks.push_back(std::move(bank));
				}
				Json device = {{"banks", std::move(banks)}};
				Json assignment = Json::object();
				for (const pugi::xml_node assigned : node.children("BankAssignment"))
				{
					const std::string channel = assigned.attribute("channel").value();
					readNumber(assignment, channel, assigned.attribute("bank"),
						std::string("the bank of MIDI channel ").append(channel).append(" of ").append(name));
				}
				if (!assignment.empty())
					device["bankAssignment"] = std::move(assignment);
				byName[name] = device;
				device["name"] = name;
				devices.push_back(std::move(device));
			}
			if (const std::string wrong = validate(byName, devicesSchema(), "the setup's MIDI devices"); !wrong.empty())
				throw CommandError(wrong);
			for (const auto& [name, device] : byName.items())
			{
				const Json assignment = device.value("bankAssignment", Json::object());
				for (const auto& [channel, bank] : assignment.items())
					if (bank.get<size_t>() >= device.at("banks").size())
						throw CommandError(std::string("MIDI channel ")
								.append(channel)
								.append(" of ")
								.append(name)
								.append(" is assigned a bank it doesn't have"));
			}
			return devices;
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
				"name, patches and optional folders; each patch has a name, program (0 to 127), optional bank (0 to "
				"16383) and folder, a path with / between nested folders. folders lists folders that the bank keeps "
				"even while no patch is in them (New Folder). "
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
		const Json pathSchema = {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}},
			{"required", {"path"}}, {"additionalProperties", false}};
		const Json devicesResult = {{"type", "object"},
			{"properties", {{"devices", {{"type", "array"}, {"items", device}}}}}, {"required", {"devices"}}};
		engine.registerCommand(
			{"midiDevice.export", 1,
				"Export Setup (reference manual p. 1110): writes every installed MIDI device, with its banks, patches "
				"and Bank Assignment, into a MIDI device setup file at path, XML that midiDevice.readSetup and "
				"midiDevice.import read.",
				pathSchema, Json::object(), true, false, true},
			[](CommandContext& context)
			{
				const std::string path = context.parameters().at("path").get<std::string>();
				std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
				out << writeSetup(context.call("settings.get", {{"key", "midi.devices"}}).at("value"));
				out.close();
				if (!out)
					throw CommandError("can't write " + path);
				return Json::object();
			});
		engine.registerCommand(
			{"midiDevice.readSetup", 1,
				"Reads a MIDI device setup file (midiDevice.export) at path and returns its devices in the file's "
				"order, as midiDevice.set takes them, without installing any: for the choice of devices that Import "
				"Setup offers (reference manual p. 1115).",
				pathSchema, devicesResult, true},
			[](CommandContext& context)
			{ return Json{{"devices", readSetup(context.parameters().at("path").get<std::string>())}}; });
		Json importSchema = pathSchema;
		importSchema["properties"]["devices"] = {{"type", "array"}, {"items", {{"type", "string"}}}};
		engine.registerCommand(
			{"midiDevice.import", 1,
				"Import Setup (reference manual p. 1115): installs MIDI devices from a setup file at path "
				"(midiDevice.export), those named in devices or else all of them. A name that is already installed "
				"or that the file doesn't have installs none. Returns the devices installed.",
				importSchema, devicesResult, false},
			[&engine](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Json read = readSetup(parameters.at("path").get<std::string>());
				Json chosen = parameters.contains("devices") ? Json::array() : read;
				for (const Json& name : parameters.value("devices", Json::array()))
				{
					const auto found = std::find_if(
						read.begin(), read.end(), [&name](const Json& each) { return each.at("name") == name; });
					if (found == read.end())
						throw CommandError("the setup has no MIDI device " + name.get<std::string>());
					chosen.push_back(*found);
				}
				engine.modifySetting(context, "midi.devices",
					[&chosen](const Json& value)
					{
						Json changed = value;
						for (Json each : chosen)
						{
							const auto name = each.at("name").get<std::string>();
							if (changed.contains(name))
								throw CommandError("there is already a MIDI device " + name);
							each.erase("name");
							changed[name] = std::move(each);
						}
						return changed;
					});
				return Json{{"devices", std::move(chosen)}};
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
