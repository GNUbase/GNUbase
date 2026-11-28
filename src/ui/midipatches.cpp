// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/midipatches.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace gnubase::ui::midipatches
{
	using engine::Json;

	namespace
	{
		std::string patchValue(const Json& program, const Json& bank)
		{
			if (!program.is_number_integer())
				return "off";
			return std::to_string(program.get<int>()) + ":" + (bank.is_number_integer() ? bank.dump() : "");
		}
	}

	std::string value(const Json& channel)
	{
		return patchValue(channel.value("program", Json()), channel.value("bank", Json()));
	}

	widgets::Options options(const Json& devices, const Json& channel)
	{
		widgets::Options options{{"off", "Off"}};
		const Json bank = channel.value("bank", Json());
		const auto device = devices.find(channel.value("device", std::string()));
		if (device == devices.end())
		{
			for (int program = 0; program < 128; ++program)
				options.emplace_back(patchValue(program, bank), std::to_string(program + 1));
			return options;
		}
		// The bank that the Bank Assignment (p. 1109) gives the track's MIDI channel, else all of them. A track on Any
		// uses channel 1's bank: GNUbase's choice, the manual doesn't say.
		Json banks = device->at("banks");
		const Json midiChannel = channel.value("midiChannel", Json());
		const Json assigned =
			device->value("bankAssignment", Json::object())
				.value(std::to_string(midiChannel.is_number_integer() ? midiChannel.get<int>() + 1 : 1), Json());
		if (assigned.is_number_integer() && assigned.get<int64_t>() >= 0 && assigned.get<size_t>() < banks.size())
			banks = Json::array({banks.at(assigned.get<size_t>())});
		for (const Json& deviceBank : banks)
			for (const Json& patch : deviceBank.at("patches"))
			{
				std::string name;
				for (const std::string& part : {banks.size() > 1 ? deviceBank.at("name").get<std::string>() : "",
						 patch.value("folder", std::string()), patch.at("name").get<std::string>()})
					if (!part.empty())
						name += (name.empty() ? "" : " / ") + part;
				options.emplace_back(patchValue(patch.at("program"), patch.value("bank", Json())), name);
			}
		const std::string current = value(channel);
		if (std::none_of(options.begin(), options.end(), [&](const auto& option) { return option.first == current; }))
			options.emplace_back(current, "Program " + std::to_string(channel.at("program").get<int>() + 1));
		return options;
	}

	Json choose(const Json& channel, const std::string& value)
	{
		const size_t colon = value.find(':');
		if (colon == std::string::npos)
			return channel.value("program", Json()).is_null() ? Json() : Json{{"program", nullptr}};
		const Json program = std::stoi(value.substr(0, colon));
		const Json bank = colon + 1 < value.size() ? Json(std::stoi(value.substr(colon + 1))) : Json(nullptr);
		if (channel.value("program", Json()) == program && channel.value("bank", Json()) == bank)
			return {};
		return {{"program", program}, {"bank", bank}};
	}
}
