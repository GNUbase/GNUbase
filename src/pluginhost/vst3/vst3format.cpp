// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginhost/vst3/vst3format.h"

#include "pluginhost/vst3/hostobjects.h"
#include "pluginhost/vst3/vst3ns.h"
#include "pluginhost/vst3/vst3plugin.h"

#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/vst/hosting/module.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::pluginhost
{
	namespace
	{
		// CLAP's feature names for VST3's sub-categories ("Fx|Delay", "Instrument|Synth").
		std::vector<std::string> features(const std::vector<std::string>& subCategories)
		{
			static const std::pair<const char*, const char*> names[] = {{"Fx", "audio-effect"},
				{"Instrument", "instrument"}, {"Analyzer", "analyzer"}, {"Synth", "synthesizer"},
				{"Sampler", "sampler"}, {"Drum", "drum"}, {"EQ", "equalizer"}, {"Dynamics", "compressor"},
				{"Pitch Shift", "pitch-shifter"}, {"Tools", "utility"}, {"Spatial", "surround"}};
			std::vector<std::string> result;
			for (const std::string& category : subCategories)
			{
				std::string feature;
				for (const auto& [vst3Name, name] : names)
				{
					if (category == vst3Name)
						feature = name;
				}
				if (feature.empty())
				{
					for (const char c : category)
						feature += c == ' ' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				}
				if (!feature.empty() && std::find(result.begin(), result.end(), feature) == result.end())
					result.push_back(feature);
			}
			const bool typed = std::find(result.begin(), result.end(), "instrument") != result.end() ||
				std::find(result.begin(), result.end(), "audio-effect") != result.end();
			if (!typed)
				result.insert(result.begin(), "audio-effect");
			return result;
		}

		class Vst3Factory final : public plugin::Factory
		{
		public:
			explicit Vst3Factory(VST3::Hosting::Module::Ptr module) : m_module(std::move(module))
			{
				const VST3::Hosting::PluginFactory& factory = m_module->getFactory();
				factory.setHostContext(static_cast<vst::Vst::IHostApplication*>(&m_application));
				const std::string factoryVendor = factory.info().vendor();
				for (const VST3::Hosting::ClassInfo& info : factory.classInfos())
				{
					if (info.category() != kVstAudioEffectClass)
						continue;
					plugin::Descriptor descriptor;
					descriptor.formatId = info.ID().toString();
					descriptor.id = "vst3:" + descriptor.formatId;
					descriptor.format = "vst3";
					descriptor.name = info.name();
					descriptor.vendor = info.vendor().empty() ? factoryVendor : info.vendor();
					descriptor.version = info.version();
					descriptor.features = features(info.subCategories());
					m_descriptors.push_back(std::move(descriptor));
					m_classIds.push_back(info.ID());
				}
			}

			size_t count() const override { return m_descriptors.size(); }
			const plugin::Descriptor& descriptor(size_t index) const override { return m_descriptors.at(index); }

			std::unique_ptr<plugin::Plugin> create(std::string_view id, plugin::Host& host) override
			{
				for (size_t i = 0; i < m_descriptors.size(); ++i)
				{
					if (m_descriptors[i].id == id)
						return std::make_unique<vst3::Vst3Plugin>(m_module, m_classIds[i], m_descriptors[i], host);
				}
				return nullptr;
			}

		private:
			VST3::Hosting::Module::Ptr m_module;
			vst3::HostApplication m_application;
			std::vector<plugin::Descriptor> m_descriptors;
			std::vector<VST3::UID> m_classIds;
		};

		class Vst3Format final : public Format
		{
		public:
			std::string_view name() const override { return "vst3"; }

			std::unique_ptr<plugin::Factory> load(const std::string& path) override
			{
				std::string error;
				VST3::Hosting::Module::Ptr module = VST3::Hosting::Module::create(path, error);
				if (!module)
					throw std::runtime_error("cannot load the VST3 module " + path + ": " + error);
				return std::make_unique<Vst3Factory>(std::move(module));
			}
		};
	}

	std::unique_ptr<Format> makeVst3Format()
	{
		return std::make_unique<Vst3Format>();
	}
}
