// SPDX-License-Identifier: GPL-3.0-or-later
// State blob: saves a binary state of "Size" bytes of pseudo-random data generated from "Seed", followed by a
// checksum, and refuses to load a state whose checksum or length is wrong. Hosts use it to test that state survives
// a round trip byte for byte, also when it is large and the streams read or write it in pieces.
#include "testplugin.h"

#include <cstring>

namespace gnubase::testplugins
{
	namespace
	{
		constexpr uint32_t g_seedId = 1;
		constexpr uint32_t g_sizeId = 2;
		constexpr uint32_t g_magic = 0x42534247; // "GBSB"
		constexpr uint32_t g_version = 1;
		constexpr size_t g_headerSize = 4 * sizeof(uint32_t);

		uint32_t checksum(const uint8_t* data, size_t size)
		{
			uint32_t hash = 2166136261U; // FNV-1a
			for (size_t i = 0; i < size; ++i)
				hash = (hash ^ data[i]) * 16777619U;
			return hash;
		}

		void putWord(uint8_t* at, uint32_t value)
		{
			std::memcpy(at, &value, sizeof(value));
		}

		uint32_t getWord(const uint8_t* at)
		{
			uint32_t value = 0;
			std::memcpy(&value, at, sizeof(value));
			return value;
		}

		class StateBlob : public TestPlugin
		{
		public:
			StateBlob() : TestPlugin({seedParam(), sizeParam()}) {}

			std::vector<uint8_t> saveState() const override
			{
				double seed = 0.0;
				double size = 0.0;
				paramValue(g_seedId, seed);
				paramValue(g_sizeId, size);
				const auto payload = static_cast<uint32_t>(size);
				std::vector<uint8_t> bytes(g_headerSize + payload + sizeof(uint32_t));
				putWord(bytes.data(), g_magic);
				putWord(bytes.data() + 4, g_version);
				putWord(bytes.data() + 8, static_cast<uint32_t>(seed));
				putWord(bytes.data() + 12, payload);
				uint32_t x = (static_cast<uint32_t>(seed) * 2654435761U) + 1U; // xorshift32 state, never 0
				for (uint32_t i = 0; i < payload; ++i)
				{
					x ^= x << 13;
					x ^= x >> 17;
					x ^= x << 5;
					bytes[g_headerSize + i] = static_cast<uint8_t>(x);
				}
				const size_t end = g_headerSize + payload;
				putWord(bytes.data() + end, checksum(bytes.data(), end));
				return bytes;
			}

			bool loadState(const uint8_t* data, size_t size) override
			{
				if (data == nullptr || size < g_headerSize + sizeof(uint32_t))
					return false;
				const size_t end = size - sizeof(uint32_t);
				if (getWord(data) != g_magic || getWord(data + 4) != g_version ||
					getWord(data + 12) != end - g_headerSize || getWord(data + end) != checksum(data, end))
					return false;
				setParam(g_seedId, getWord(data + 8));
				setParam(g_sizeId, getWord(data + 12));
				return true;
			}

		private:
			static ParamInfo seedParam()
			{
				ParamInfo info;
				info.id = g_seedId;
				info.name = "Seed";
				info.maxValue = 65535.0;
				info.defaultValue = 1.0;
				info.stepped = true;
				return info;
			}

			static ParamInfo sizeParam()
			{
				ParamInfo info;
				info.id = g_sizeId;
				info.name = "Size";
				info.maxValue = 16.0 * 1024 * 1024;
				info.defaultValue = 64.0 * 1024;
				info.stepped = true;
				info.unit = "bytes";
				return info;
			}
		};
	}

	const Descriptor& pluginDescriptor()
	{
		static const Descriptor descriptor{"org.gnubase.test.stateblob", "GNUbase Test State Blob",
			"Saves and checks a binary state of adjustable size", false};
		return descriptor;
	}

	std::unique_ptr<TestPlugin> createPlugin(Host& /*host*/)
	{
		return std::make_unique<StateBlob>();
	}
}
