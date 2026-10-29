// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "plugin/events.h"
#include "plugin/plugin.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The extensions of the GNUbase plugin API (docs/plugin-api.md). Plugin extensions are implemented by plugins and
// looked up with extension<Ext>(plugin); host extensions are implemented by hosts and looked up with
// extension<Ext>(host). The thread tags are explained in plugin.h.
//
// One class often implements several extensions, so every extension's method names are unique across all extensions:
// they carry the extension's name (audioPortCount, paramCount, saveState, guiShow).
namespace gnubase::plugin
{
	// ---- Audio ports ----

	enum class ChannelLayout : uint8_t
	{
		Other, // channelCount channels without a defined order
		Mono,
		Stereo,
		Surround,  // AudioPortInfo::speakers says which
		Ambisonic, // ACN order, SN3D normalization
	};

	struct AudioPortInfo
	{
		enum Flags : uint8_t
		{
			Main = 1,     // the main input or output; other inputs are side-chain inputs, other outputs auxiliary ones
			Optional = 2, // the plugin works when the host leaves it unconnected (inactive)
		};

		uint32_t id = 0; // stable
		std::string name;
		uint32_t channelCount = 0;
		ChannelLayout layout = ChannelLayout::Other;
		uint64_t speakers = 0; // Surround: one bit per speaker, in the order of WAVE_FORMAT_EXTENSIBLE
		uint32_t flags = 0;
	};

	class AudioPorts
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.audio-ports"; }

		// [main]
		virtual uint32_t audioPortCount(bool input) const = 0;
		virtual bool audioPortInfo(bool input, uint32_t index, AudioPortInfo& info) const = 0;

	protected:
		~AudioPorts() = default;
	};

	// A layout the host asks for, e.g. a surround layout for a plugin that adapts to its channel.
	struct AudioPortRequest
	{
		bool input = false;
		uint32_t index = 0;
		uint32_t channelCount = 0;
		ChannelLayout layout = ChannelLayout::Other;
		uint64_t speakers = 0;
	};

	class ConfigurableAudioPorts
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.configurable-audio-ports"; }

		// [main, inactive] Applies all requests or none.
		virtual bool applyAudioPortRequests(const std::vector<AudioPortRequest>& requests) = 0;

	protected:
		~ConfigurableAudioPorts() = default;
	};

	class AudioPortsActivation
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.audio-ports-activation"; }

		// [main] Whether setAudioPortActiveWhileProcessing works. VST3's activateBus doesn't (setup state only); CLAP
		// plugins say so themselves.
		virtual bool audioPortsActivatableWhileProcessing() const = 0;

		// [main, inactive] Turns a port on or off, e.g. a side-chain input or the unused outputs of a multi-output
		// instrument. An inactive port gets an AudioBuffer without channels. Without the [audio] path below, switching
		// a port of an active plugin means a restart.
		virtual bool setAudioPortActive(bool input, uint32_t index, bool active) = 0;

		// [audio] Only if audioPortsActivatableWhileProcessing(): the same between two blocks, without a restart, so a
		// side-chain can be switched during playback. The port keeps its channels in the block; while it is off, the
		// host passes silence with the constant mask set, and the plugin ignores it.
		virtual bool setAudioPortActiveWhileProcessing(bool input, uint32_t index, bool active) noexcept = 0;

	protected:
		~AudioPortsActivation() = default;
	};

	class HostAudioPorts
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.host-audio-ports"; }

		// [main] The ports changed; the host reads them again. Only while inactive, or after requestRestart().
		virtual void rescanAudioPorts() = 0;

	protected:
		~HostAudioPorts() = default;
	};

	// ---- Note ports ----

	struct NotePortInfo
	{
		// How a note port wants its events. The engine always sends notes; the adapters convert them to MIDI where a
		// plugin wants MIDI, with the same helper for every format.
		enum Dialect : uint8_t
		{
			Notes = 1,   // NoteOn, NoteOff, NoteChoke, NoteExpression
			Midi = 2,    // MIDI 1.0
			MidiMpe = 4, // MIDI 1.0 with MPE
			Midi2 = 8,   // MIDI 2.0 universal MIDI packets
		};

		uint32_t id = 0;
		std::string name;
		uint32_t dialects = 0; // Dialect bits
		Dialect preferred = Notes;
	};

	class NotePorts
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.note-ports"; }

		// [main]
		virtual uint32_t notePortCount(bool input) const = 0;
		virtual bool notePortInfo(bool input, uint32_t index, NotePortInfo& info) const = 0;

	protected:
		~NotePorts() = default;
	};

	class HostNotePorts
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.host-note-ports"; }

		enum Rescan : uint8_t
		{
			Names = 1, // only names changed: any time
			All = 2,   // ports, ids or dialects changed: only while inactive, or after requestRestart()
		};

		// [main] The note ports changed; the host reads them again.
		virtual void rescanNotePorts(uint32_t what) = 0;

	protected:
		~HostNotePorts() = default;
	};

	// ---- Parameters ----

	struct ParamInfo
	{
		enum Flags : uint16_t
		{
			Stepped = 1, // stepCount + 1 values from minimum to maximum, see stepCount
			Periodic = 2,
			Hidden = 4,
			ReadOnly = 8, // the plugin sets it, e.g. a gain reduction meter
			Bypass = 16,  // the plugin's own bypass: 0 off, 1 on
			Automatable = 32,
			AutomatablePerNote = 64, // a ParamValue event that names a note changes only its voices
			Modulatable = 128,       // takes ParamMod events
			ModulatablePerNote = 256,
			Enum = 512,             // Stepped, and every value has a name (paramValueToText)
			RequiresProcess = 1024, // a change only takes effect in process(), not in flush()
		};

		uint32_t id = 0; // stable across versions of the plugin; what events, automation and saved projects use
		uint32_t flags = 0;
		std::string name;
		std::string module; // the group the parameter belongs to, as a path: "Oscillators/Osc 1"
		double minimum = 0.0;
		double maximum = 1.0;
		double defaultValue = 0.0;
		// Stepped: the values are minimum + k * (maximum - minimum) / stepCount for k from 0 to stepCount. A CLAP
		// stepped parameter from 0 to 4 has the step count 4; a VST3 one with the step count 4 has the range 0 to 1
		// and the values 0, 0.25, ... 1. 0 for parameters that aren't stepped.
		uint32_t stepCount = 0;
	};

	// Values are plain values in the parameter's range. Formats whose values are normalized (VST3, FST) declare the
	// range 0 to 1 for every parameter, stepped ones included, so automation is stored as the format's own values.
	class Params
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.params"; }

		// [main]
		virtual uint32_t paramCount() const = 0;
		virtual bool paramInfo(uint32_t index, ParamInfo& info) const = 0;
		virtual bool paramValue(uint32_t param, double& value) const = 0;
		virtual bool paramValueToText(uint32_t param, double value, std::string& text) const = 0;
		virtual bool paramTextToValue(uint32_t param, std::string_view text, double& value) const = 0;

		// [main while not processing, audio while processing] Applies parameter events without processing audio,
		// and reports the plugin's own changes and gestures, like process() does.
		virtual void flushParams(const EventList& in, EventList& out) noexcept = 0;

	protected:
		~Params() = default;
	};

	class HostParams
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.host-params"; }

		enum Rescan : uint8_t
		{
			Values = 1, // values changed, e.g. after a preset was loaded
			Text = 2,   // valueToText gives other texts
			Info = 4,   // names, modules or flags changed
			All = 8,    // parameters were added or removed, or ranges changed: only while inactive
		};

		// [main]
		virtual void rescanParams(uint32_t what) = 0;
		// [any] Call Params::flushParams soon, or process the plugin: it has changes to report.
		virtual void requestParamFlush() = 0;

	protected:
		~HostParams() = default;
	};

	// ---- State and presets ----

	enum class StateContext : uint8_t
	{
		Project,   // saving or loading a project
		Preset,    // a preset: no project-specific data such as sample paths that depend on the project
		Duplicate, // copying the plugin to another slot
	};

	// The state is the format's own preset file: a .vstpreset for VST3, an .aupreset for AU, a state blob for CLAP,
	// Turtle for LV2, an .fxp for FST. DAWproject stores it as it is (#72).
	class State
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.state"; }

		// [main]
		virtual bool saveState(std::vector<uint8_t>& data, StateContext context) = 0;
		virtual bool loadState(const uint8_t* data, size_t size, StateContext context) = 0;

	protected:
		~State() = default;
	};

	class HostState
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.host-state"; }

		// [main] The state changed in a way parameters don't show, e.g. a sample was loaded in the editor.
		virtual void markStateDirty() = 0;

	protected:
		~HostState() = default;
	};

	struct PresetInfo
	{
		enum Flags : uint8_t
		{
			Factory = 1,
			User = 2,
			Favorite = 4,
		};

		std::string name;
		std::string location; // a file path, or empty for presets inside the plugin
		std::string key;      // the preset within its location: a program number, a CLAP load key, an LV2 URI
		std::string category;
		uint32_t flags = 0;
	};

	class Presets
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.presets"; }

		// [main] The presets the plugin knows about: its programs and the files in its format's preset folders.
		virtual std::vector<PresetInfo> presets() = 0;
		// [main] Loads one. Changed parameters are reported through HostParams::rescanParams.
		virtual bool loadPreset(const PresetInfo& preset) = 0;

	protected:
		~Presets() = default;
	};

	// ---- Latency, tail, rendering ----

	class Latency
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.latency"; }

		// [main, active] The frames of delay the plugin adds to its outputs.
		virtual uint32_t latency() const = 0;

	protected:
		~Latency() = default;
	};

	class HostLatency
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.host-latency"; }

		// [main] The latency changed. The host reads it again when it activates the plugin the next time, so a
		// plugin that changes it while active also calls Host::requestRestart.
		virtual void latencyChanged() = 0;

	protected:
		~HostLatency() = default;
	};

	inline constexpr uint32_t g_infiniteTail = 0xffffffff;

	class Tail
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.tail"; }

		// [main, audio] How many frames the plugin keeps sounding after its inputs went quiet, or g_infiniteTail.
		virtual uint32_t tail() const noexcept = 0;

	protected:
		~Tail() = default;
	};

	class HostTail
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.host-tail"; }

		// [main] The tail changed; the host reads it again. A remote plugin's proxy caches the tail, so a plugin that
		// doesn't call this keeps its old one.
		virtual void tailChanged() = 0;

	protected:
		~HostTail() = default;
	};

	class Render
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.render"; }

		// [main] The plugin depends on real time, e.g. hardware: offline and prefetch blocks must come in real time.
		virtual bool hasHardRealtimeRequirement() const = 0;

	protected:
		~Render() = default;
	};

	// ---- Notes ----

	struct NoteName
	{
		std::string name;
		int16_t port = -1;
		int16_t channel = -1;
		int16_t key = -1;
	};

	class NoteNames
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.note-names"; }

		// [main] Names of keys, e.g. of a drum kit.
		virtual std::vector<NoteName> noteNames() = 0;

	protected:
		~NoteNames() = default;
	};

	// ---- Voices ----

	struct VoiceInfo
	{
		enum Flags : uint8_t
		{
			OverlappingNotes = 1, // the plugin plays several voices on one key and channel, told apart by note id
		};

		uint32_t voiceCount = 0;    // the voices the plugin plays at most now, e.g. 1 in mono mode
		uint32_t voiceCapacity = 0; // the most it can play
		uint32_t flags = 0;
	};

	// How many voices a plugin plays, so per-note modulators (pp. 1049-1077) and expressions know what to address.
	class Voices
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.voices"; }

		// [main, active]
		virtual bool voiceInfo(VoiceInfo& info) const = 0;

	protected:
		~Voices() = default;
	};

	class HostVoices
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.host-voices"; }

		// [main] The voice info changed; the host reads it again.
		virtual void voiceInfoChanged() = 0;

	protected:
		~HostVoices() = default;
	};

	// ---- Editors ----

	enum class WindowApi : uint8_t
	{
		Win32,   // HWND in handle
		Cocoa,   // NSView* in handle
		X11,     // Window in handle
		Wayland, // floating windows only; WindowHandle::exported names a window of another process
	};

	// A window of the plugin host process, or of GNUbase for guiSetTransient. Native handles are 64 bits in every
	// process. A Wayland window of another process is named by its xdg-foreign handle, a string, which links it to a
	// native Wayland window only: an XWayland window can't be linked to a Wayland one.
	struct WindowHandle
	{
		WindowApi api = WindowApi::Win32;
		uint64_t handle = 0;  // Win32, Cocoa, X11, and a Wayland window of the same process (wl_surface*)
		std::string exported; // Wayland, another process: the handle from xdg-foreign's export
	};

	// A key press in an editor, for GNUbase's key commands.
	struct KeyEvent
	{
		enum Modifiers : uint8_t
		{
			Shift = 1,
			Control = 2,
			Alt = 4,     // Option on macOS
			Command = 8, // Command on macOS, the Windows key, Super on Linux
		};

		uint32_t scancode = 0;  // USB HID keyboard usage, layout independent: the values of SDL's scancodes
		uint32_t character = 0; // the Unicode character the key produced, 0 for none
		uint32_t modifiers = 0;
		bool down = true;
		bool repeat = false;
	};

	class Gui
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.gui"; }

		// [main] Embedded editors go into a window of the host process (guiSetParent); floating ones open their own.
		virtual bool guiIsApiSupported(WindowApi api, bool floating) const = 0;
		virtual bool guiCreate(WindowApi api, bool floating) = 0;
		virtual void guiDestroy() = 0;
		virtual bool guiSetScale(double scale) = 0; // false: the plugin follows the OS scale itself
		virtual bool guiSize(uint32_t& width, uint32_t& height) const = 0;
		virtual bool guiCanResize() const = 0;
		virtual bool guiAdjustSize(uint32_t& width, uint32_t& height) const = 0; // to the nearest size the editor takes
		virtual bool guiSetSize(uint32_t width, uint32_t height) = 0;
		virtual bool guiSetParent(const WindowHandle& parent) = 0;
		virtual bool guiSetTransient(const WindowHandle& owner) = 0; // floating: stay in front of this window
		virtual bool guiShow() = 0;
		virtual bool guiHide() = 0;

	protected:
		~Gui() = default;
	};

	class HostGui
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.host-gui"; }

		// [main]
		virtual bool guiRequestResize(uint32_t width, uint32_t height) = 0;
		virtual bool guiRequestShow() = 0;
		virtual bool guiRequestHide() = 0;
		virtual void guiClosed(bool destroyed) = 0; // the user closed a floating editor
		// A key press or release the editor didn't handle. The host runs the key command bound to it, so space starts
		// playback while an editor has the focus.
		virtual void guiKeyUnhandled(const KeyEvent& key) = 0;

	protected:
		~HostGui() = default;
	};

	// ---- Remote controls and context menus ----

	inline constexpr uint32_t g_noParam = 0xffffffff;

	// A page of eight parameters for hardware controllers and the editor's focus controls.
	struct RemoteControlsPage
	{
		std::string section;
		std::string name;
		uint32_t pageId = 0;
		std::array<uint32_t, 8> params{}; // parameter ids; g_noParam for an unused control
	};

	class RemoteControls
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.remote-controls"; }

		// [main]
		virtual std::vector<RemoteControlsPage> remoteControlsPages() = 0;

	protected:
		~RemoteControls() = default;
	};

	struct MenuItem
	{
		enum class Kind : uint8_t
		{
			Entry,
			Separator,
			BeginSubmenu,
			EndSubmenu,
		};

		Kind kind = Kind::Entry;
		uint32_t action = 0;
		std::string label;
		bool enabled = true;
		bool checked = false;
	};

	class ContextMenu
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.context-menu"; }

		// [main] The plugin's entries for its whole editor (param = g_noParam) or one parameter. GNUbase shows them in
		// its own menus, e.g. the editor's functions menu and a parameter's context menu.
		virtual bool populateContextMenu(uint32_t param, std::vector<MenuItem>& items) = 0;
		virtual bool performContextMenuAction(uint32_t param, uint32_t action) = 0;

	protected:
		~ContextMenu() = default;
	};

	// The other way round: the plugin's editor shows GNUbase's menu for one of its parameters (automation, MIDI learn,
	// remote controls) when the user right-clicks a knob. VST3's IComponentHandler3, CLAP's host context menu.
	class HostContextMenu
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.host-context-menu"; }

		// [main] GNUbase's entries for the parameter (or the whole plugin: g_noParam), for a plugin that builds the
		// menu itself, and running one of them.
		virtual bool populateHostContextMenu(uint32_t param, std::vector<MenuItem>& items) = 0;
		virtual bool performHostContextMenuAction(uint32_t param, uint32_t action) = 0;
		// [main] Or GNUbase shows the menu itself, at a position in the editor's window, in its pixels.
		virtual bool hostContextMenuCanPopup() const = 0;
		virtual bool hostContextMenuPopup(uint32_t param, int32_t x, int32_t y) = 0;

	protected:
		~HostContextMenu() = default;
	};

	// ---- What the host tells the plugin ----

	class ThreadCheck
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.thread-check"; }

		// [any]
		virtual bool isMainThread() const noexcept = 0;
		virtual bool isAudioThread() const noexcept = 0;

	protected:
		~ThreadCheck() = default;
	};

	struct TrackInfo
	{
		std::string name;
		uint32_t color = 0; // 0xRRGGBB
		uint32_t channelCount = 0;
		bool bus = false; // an input, output or group bus rather than a track
	};

	class HostTrackInfo
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.host-track-info"; }

		// [main] The channel the plugin is inserted in.
		virtual bool trackInfo(TrackInfo& info) const = 0;

	protected:
		~HostTrackInfo() = default;
	};

	class TrackInfoListener
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.track-info-listener"; }

		// [main] The channel's name, color or channel count changed; the plugin reads HostTrackInfo again.
		virtual void trackInfoChanged() = 0;

	protected:
		~TrackInfoListener() = default;
	};

	// ---- GNUbase extensions: beyond what any one format defines ----

	// A low-latency mode for playing live: the plugin turns off look-ahead and similar features, so its latency
	// drops. The engine turns it on for plugins on live paths while latency is constrained (reference manual p. 994).
	class LowLatency
	{
	public:
		static constexpr std::string_view extensionId() { return "gnubase.low-latency"; }

		// [main, inactive] The latency is read again on activation.
		virtual bool setLowLatency(bool enabled) = 0;
		virtual bool lowLatency() const = 0;

	protected:
		~LowLatency() = default;
	};
}
