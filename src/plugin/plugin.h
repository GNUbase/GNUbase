// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "plugin/events.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The GNUbase plugin API, also called the plugin wrapper (docs/plugin-api.md). Every plugin format is adapted to it in
// the plugin host process, GNUbase's built-in plugins implement it in the engine, and the engine hosts both through
// it. It is modeled on CLAP: a small core interface plus extensions, looked up by id.
//
// Every method says on which thread it may be called:
// - [main]: the instance's main thread. In the plugin host process that is the thread that runs the OS event loop;
//   in the engine it is the command thread.
// - [audio]: the thread that processes the instance. It changes between blocks (the audio thread, a worker, a
//   prefetch thread), but only one thread processes an instance at a time. Real-time rules apply.
// - [any]: any thread, real-time safe.
namespace gnubase::plugin
{
	// The version of this API. The plugin IPC (#62) checks it when a host process connects.
	inline constexpr uint32_t g_apiVersion = 1;

	// What a plugin is, from its format's metadata. Main thread, available without creating an instance.
	struct Descriptor
	{
		std::string id;       // unique across formats: "<format>:<format id>", e.g. "builtin:gain"
		std::string format;   // "builtin", "clap", "vst3", "au", "lv2" or "fst"
		std::string formatId; // the format's own id: CLAP id, VST3 class id, AU type/subtype/manufacturer, LV2 URI, ...
		std::string name;
		std::string vendor;
		std::string version;
		std::vector<std::string> features; // CLAP's feature names: "instrument", "audio-effect", "note-effect", ...
	};

	// How a block is processed (docs/anticipative-processing.md).
	enum class ProcessMode : uint8_t
	{
		Realtime, // for the audio device, now
		Prefetch, // ahead of the audio device, in larger blocks; still in time
		Offline,  // as fast as possible or slower, e.g. an export
	};

	// The transport at a frame of a block. A fixed layout without pointers, like Event.
	struct alignas(8) Transport
	{
		enum Flags : uint8_t
		{
			Playing = 1,
			Recording = 2,
			Cycle = 4,        // cycle playback is on; cycleStart and cycleEnd are valid
			TempoValid = 8,   // tempo is valid
			MusicalValid = 16 // the positions in quarter notes, the bar and the time signature are valid
		};

		uint32_t flags = 0;
		uint16_t timeSignatureNumerator = 4;
		uint16_t timeSignatureDenominator = 4;
		int64_t position = 0;        // project position in samples
		double positionQuarters = 0; // project position in quarter notes
		double tempo = 120.0;        // quarter notes per minute
		double barStart = 0.0;       // the start of the current bar, in quarter notes
		int32_t bar = 0;             // the current bar, counted from 0
		uint32_t reserved = 0;
		double cycleStart = 0.0; // in quarter notes
		double cycleEnd = 0.0;
	};

	static_assert(std::is_trivially_copyable_v<Transport> && sizeof(Transport) == 64);

	// The transport from a frame of a block on. A new segment starts where cycle playback jumps or the tempo changes.
	struct TransportSegment
	{
		uint32_t offset = 0;
		uint32_t reserved = 0;
		Transport transport;
	};

	// One audio port's channels for a block.
	struct AudioBuffer
	{
		float* const* channels = nullptr; // channelCount pointers to the block's frames; inputs are read only
		uint32_t channelCount = 0;
		// Bit c: every sample of channel c has the value of its first sample, e.g. silence. The host sets it on inputs;
		// a plugin may set it on outputs.
		uint64_t constantMask = 0;
	};

	// What process() gets. Input and output buffers never overlap.
	struct Process
	{
		uint32_t frames = 0;
		int64_t steadyTime = -1; // stream frame of the block's first frame: counts on while the device runs; -1 unknown
		ProcessMode mode = ProcessMode::Realtime;
		const AudioBuffer* audioInputs = nullptr; // one per input port, in port order
		uint32_t audioInputCount = 0;
		AudioBuffer* audioOutputs = nullptr; // one per output port
		uint32_t audioOutputCount = 0;
		const EventList& eventsIn;                   // sorted by offset
		EventList& eventsOut;                        // empty on entry; the plugin adds events in order of their offsets
		const TransportSegment* transport = nullptr; // at least one; the first starts at offset 0
		uint32_t transportCount = 0;
	};

	enum class ProcessStatus : uint8_t
	{
		Error,              // the outputs are invalid; the host outputs silence and reports the plugin
		Continue,           // keep processing
		ContinueIfNotQuiet, // keep processing while the inputs or outputs are not quiet, then put the plugin to sleep
		Tail,               // keep processing for the tail (Tail extension) after the inputs are quiet
		Sleep,              // quiet: skip the plugin until it gets events or input that is not quiet
	};

	// What a plugin may ask of its host. A plugin gets it when it is created and keeps it for its lifetime.
	class Host
	{
	public:
		Host() = default;
		virtual ~Host() = default;
		Host(const Host&) = delete;
		Host& operator=(const Host&) = delete;

		// [any] Deactivate and activate the plugin again, e.g. because its ports or latency changed.
		virtual void requestRestart() = 0;
		// [any] Process the plugin again after it was put to sleep (ContinueIfNotQuiet).
		virtual void requestProcess() = 0;
		// [any] Call Plugin::onMainThread soon.
		virtual void requestCallback() = 0;

		// [main] The host's extension with this id, or nullptr. See extension<Ext>().
		virtual void* extension(std::string_view id) = 0;
	};

	// A plugin instance. Its lifecycle is CLAP's:
	//   created --init--> inactive --activate--> active --startProcessing--> processing
	//   processing --stopProcessing--> active --deactivate--> inactive --destroyed
	class Plugin
	{
	public:
		Plugin() = default;
		virtual ~Plugin() = default; // [main] only while inactive
		Plugin(const Plugin&) = delete;
		Plugin& operator=(const Plugin&) = delete;

		// [any]
		virtual const Descriptor& descriptor() const = 0;

		// [main] Once, before every other call except descriptor().
		virtual bool init() = 0;

		// [main] Allocate what processing needs. Every block has from minFrames to maxFrames frames.
		virtual bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) = 0;
		// [main]
		virtual void deactivate() = 0;

		// [audio] Before the first block after activation or after stopProcessing.
		virtual bool startProcessing() noexcept = 0;
		// [audio]
		virtual void stopProcessing() noexcept = 0;
		// [audio] Forget the past: clear delay lines, end voices. The parameters keep their values.
		virtual void reset() noexcept = 0;
		// [audio] One block. Real-time rules apply.
		virtual ProcessStatus process(const Process& process) noexcept = 0;

		// [main] The plugin's extension with this id, or nullptr. Hosts look extensions up once after init() and keep
		// the pointers, which stay valid for the instance's lifetime. See extension<Ext>() and provide<Exts...>().
		virtual void* extension(std::string_view id) = 0;

		// [main] The host called this after requestCallback().
		virtual void onMainThread() {}
	};

	// Creates the plugins of one plugin file, or the built-in ones. The scanner (#66) lists them.
	class Factory
	{
	public:
		Factory() = default;
		virtual ~Factory() = default;
		Factory(const Factory&) = delete;
		Factory& operator=(const Factory&) = delete;

		// [main]
		virtual size_t count() const = 0;
		virtual const Descriptor& descriptor(size_t index) const = 0;
		// [main] A new instance, not initialized yet, or nullptr for an unknown id.
		virtual std::unique_ptr<Plugin> create(std::string_view id, Host& host) = 0;
	};

	// The extension Ext of a plugin or host, or nullptr: extension<Params>(plugin). Every extension is a class with a
	// static extensionId(); an id names one version of an extension, so a changed extension gets a new id.
	template <typename Ext, typename Object> Ext* extension(Object& object)
	{
		return static_cast<Ext*>(object.extension(Ext::extensionId()));
	}

	// Answers Plugin::extension or Host::extension for the extensions an object implements:
	//   void* extension(std::string_view id) override { return provide<Params, State>(this, id); }
	// It casts to the extension's base class before erasing the type, so extension<Ext>() gets the right address.
	template <typename Self> void* provide(Self* /*self*/, std::string_view /*id*/)
	{
		return nullptr;
	}

	template <typename Ext, typename... Exts, typename Self> void* provide(Self* self, std::string_view id)
	{
		if (id == Ext::extensionId())
			return static_cast<Ext*>(self);
		return provide<Exts...>(self, id);
	}
}
