// SPDX-License-Identifier: GPL-3.0-or-later
// Fades and crossfades in the event display (#138, reference manual pp. 381-397): the fade lines and handles of audio
// events, dragging a handle, the Fades menu's commands and the fade dialogs. docs/features/fades.md describes them.
#include "engine/document/serialize.h"
#include "platform/log.h"
#include "ui/eventdisplay.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr double g_handleSize = 8.0; // dp at the top of a lane, and around a handle, that grab it

		engine::Fade fadeOf(const Json& json)
		{
			engine::Fade fade;
			fade.length = json.value("length", int64_t{0});
			fade.shape = engine::fadeShapeFromName(json.value("shape", std::string("linear")))
							 .value_or(engine::FadeShape::Linear);
			return fade;
		}

		std::string ids(const std::vector<uint64_t>& events)
		{
			std::string text;
			for (const uint64_t id : events)
				text += (text.empty() ? "" : " ") + std::to_string(id);
			return text;
		}
	}

	void EventDisplay::readFades(const Json& event, double rate, double timeRate)
	{
		AudioFades fades;
		fades.rate = rate;
		fades.fadeRoom = event.value("fadeRoom", event.at("length").get<int64_t>());
		fades.timeRate = timeRate;
		fades.length = event.at("length").get<int64_t>();
		fades.in = fadeOf(event.at("fadeIn"));
		fades.out = fadeOf(event.at("fadeOut"));
		if (const Json& crossfade = event.at("crossfade"); crossfade.is_object())
		{
			fades.crossfade.from = crossfade.at("from").get<uint64_t>();
			fades.crossfade.shape = fadeOf({{"shape", crossfade.at("shape")}}).shape;
			fades.crossfade.outShape = fadeOf({{"shape", crossfade.at("outShape")}}).shape;
			fades.crossfade.length = crossfade.at("length").get<int64_t>();
			fades.crossfade.outLength = crossfade.at("outLength").get<int64_t>();
		}
		fades.offset = event.at("offset").get<int64_t>();
		fades.clip = event.at("clip").get<uint64_t>();
		fades.volume = event.at("volume").get<double>();
		fades.curve = engine::volumeCurveFromJson(event.at("volumeCurve"));
		m_fades[event.at("id").get<uint64_t>()] = fades;
	}

	std::vector<uint64_t> EventDisplay::selectedAudio() const
	{
		std::vector<uint64_t> events;
		for (const uint64_t id : m_selection)
			if (m_fades.count(id) != 0)
				events.push_back(id);
		return events;
	}

	std::vector<uint64_t> EventDisplay::selectedMidi() const
	{
		std::vector<uint64_t> parts;
		for (const Shown& shown : m_events)
			if ((m_laneTypes[shown.lane] == "midi" || m_laneTypes[shown.lane] == "instrument") &&
				std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
				parts.push_back(shown.id);
		return parts;
	}

	double EventDisplay::draggedFade(uint64_t event, bool in) const
	{
		const AudioFades& fades = m_fades.at(event);
		if (!(fades.rate > 0.0))
			return 0.0;
		const double length = static_cast<double>(fades.fadeRoom) / fades.rate;
		const double own = static_cast<double>((in ? fades.in : fades.out).length) / fades.rate;
		const double other = static_cast<double>((in ? fades.out : fades.in).length) / fades.rate;
		const bool dragged = (m_drag == Drag::FadeIn && in) || (m_drag == Drag::FadeOut && !in);
		// The pressed event can go during the drag, with an undo or a remote client: then no fade changes.
		const auto found = m_fades.find(m_dragEvent);
		if (!dragged || !m_dragMoved || found == m_fades.end() ||
			!std::binary_search(m_selection.begin(), m_selection.end(), event))
			return own;
		// Every selected event gets the fade that the pressed one gets (reference manual p. 384).
		const AudioFades& pressed = found->second;
		const double from = static_cast<double>((in ? pressed.in : pressed.out).length) / pressed.rate;
		const double to = from + ((in ? m_dragDx : -m_dragDx) * m_view.scale);
		return std::clamp(to, 0.0, std::max(0.0, length - other));
	}

	int EventDisplay::fadeHandleAt(const Shown& shown, double x, double y) const
	{
		// Only selected events show their handles (showFades), so only theirs can be grabbed.
		const auto fades = m_fades.find(shown.id);
		if (fades == m_fades.end() || !(fades->second.rate > 0.0) ||
			!std::binary_search(m_selection.begin(), m_selection.end(), shown.id) ||
			y + m_view.top - m_laneTops[shown.lane] > g_handleSize)
			return 0;
		const double in = (shown.start + draggedFade(shown.id, true) - m_view.start) / m_view.scale;
		const double out = (shown.end - draggedFade(shown.id, false) - m_view.start) / m_view.scale;
		const double toIn = std::abs(x - in);
		const double toOut = std::abs(x - out);
		if (std::min(toIn, toOut) > g_handleSize)
			return 0;
		return toIn <= toOut ? -1 : 1;
	}

	bool EventDisplay::fadeDown(const Shown& shown, double x, double y)
	{
		const int handle = fadeHandleAt(shown, x, y);
		if (handle == 0)
			return false;
		m_dragEvent = shown.id;
		m_drag = handle < 0 ? Drag::FadeIn : Drag::FadeOut;
		return true;
	}

	void EventDisplay::fadeUp()
	{
		const bool in = m_drag == Drag::FadeIn;
		Json commands = Json::array();
		for (const uint64_t id : m_fades.count(m_dragEvent) != 0 ? selectedAudio() : std::vector<uint64_t>())
		{
			const AudioFades& fades = m_fades.at(id);
			const engine::Fade& other = in ? fades.out : fades.in;
			const int64_t length = std::clamp<int64_t>(
				std::llround(draggedFade(id, in) * fades.rate), 0, std::max<int64_t>(0, fades.fadeRoom - other.length));
			commands.push_back({{"command", "audioEvent.setFades"},
				{"parameters", {{"event", id}, {in ? "fadeIn" : "fadeOut", {{"length", length}}}}}});
		}
		m_drag = Drag::None;
		m_dragMoved = false;
		m_dragDx = 0.0;
		if (!commands.empty())
			edit("history.transaction", {{"description", "Change fades"}, {"commands", std::move(commands)}});
		showFades();
	}

	void EventDisplay::showFades()
	{
		if (!alive() || m_fadeLines == nullptr)
			return;
		std::vector<FadeLines::Lane> lanes;
		lanes.reserve(m_laneTracks.size());
		// As high as the event lanes: setLanes' lane tops, which give lanes (#99) and automation lanes their own
		// heights.
		for (size_t i = 0; i < m_laneTracks.size(); ++i)
			lanes.push_back(
				{static_cast<float>(i + 1 < m_laneTops.size() ? m_laneTops[i + 1] - m_laneTops[i]
															  : trackHeight(m_laneTracks[i]) * m_view.trackZoom),
					{}, {}, {}, {}});
		// The selected events where a drag moves them, as setLanes shows them.
		std::vector<Shown> events = m_events;
		if (m_drag == Drag::Events && m_dragMoved)
			for (Shown& event : events)
				if (std::binary_search(m_selection.begin(), m_selection.end(), event.id) &&
					static_cast<int64_t>(event.lane) + m_dragLanes >= 0)
				{
					const double length = event.end - event.start;
					event.start = m_dragTicks ? m_time.tempo.toSeconds(m_time.tempo.toTicks(event.start) + *m_dragTicks)
											  : event.start + (m_dragDx * m_view.scale);
					event.end = event.start + length;
					event.lane = static_cast<size_t>(static_cast<int64_t>(event.lane) + m_dragLanes);
				}
		std::map<uint64_t, const Shown*> shown;
		for (const Shown& event : events)
			if (const auto fades = m_fades.find(event.id);
				fades != m_fades.end() && fades->second.rate > 0.0 && event.lane < lanes.size())
				shown[event.id] = &event;
		// The crossfades that play (crossfadeApplies): they replace the fades at the ends they join, over the overlap
		// unless they have lengths of their own.
		std::map<uint64_t, uint64_t> fadesInto; // the event a crossfade comes from, and the one it goes to
		for (const auto& [id, event] : shown)
		{
			const auto from = shown.find(m_fades.at(id).crossfade.from);
			if (from != shown.end() && from->second->lane == event->lane && from->second->start < event->start &&
				event->start < from->second->end && from->second->end <= event->end && !from->second->mute &&
				!event->mute)
				fadesInto[from->first] = id;
		}
		for (const auto& [id, event] : shown)
		{
			const AudioFades& fades = m_fades.at(id);
			FadeLines::Lane& lane = lanes[event->lane];
			const double in = draggedFade(id, true);
			const double out = draggedFade(id, false);
			const bool crossfadesIn = fades.crossfade.from != 0 && fadesInto.count(fades.crossfade.from) != 0 &&
				fadesInto.at(fades.crossfade.from) == id;
			if (crossfadesIn)
			{
				const Shown& from = *shown.at(fades.crossfade.from);
				const AudioFades& before = m_fades.at(from.id);
				const double overlap = from.end - event->start;
				const double inLength =
					fades.crossfade.length > 0 ? static_cast<double>(fades.crossfade.length) / fades.rate : overlap;
				const double outLength = fades.crossfade.outLength > 0
					? static_cast<double>(fades.crossfade.outLength) / before.rate
					: overlap;
				lane.fades.push_back({event->start, inLength, fades.crossfade.shape, true});
				lane.fades.push_back({from.end - outLength, outLength, fades.crossfade.outShape, false});
			}
			else if (in > 0.0)
			{
				lane.fades.push_back({event->start, in, fades.in.shape, true});
			}
			if (out > 0.0 && fadesInto.count(id) == 0)
				lane.fades.push_back({event->end - out, out, fades.out.shape, false});
			if (std::binary_search(m_selection.begin(), m_selection.end(), id))
				lane.handles.insert(lane.handles.end(), {event->start + in, event->end - out});
			showVolume(lane, *event);
		}
		m_fadeLines->setLanes(std::move(lanes));
		showWaveVolumes();
		showVolumeReadout();
	}

	bool EventDisplay::openSelectedCrossfade()
	{
		// A crossfade that plays (as showFades has it) between two selected events, or into or out of the one selected
		// (reference manual p. 389): not one with another neighbour of a selected event, nor one that rests.
		std::map<uint64_t, const Shown*> shown;
		for (const Shown& event : m_events)
			shown[event.id] = &event;
		const std::vector<uint64_t> audio = selectedAudio();
		const auto selected = [&audio](uint64_t id) { return std::binary_search(audio.begin(), audio.end(), id); };
		for (const auto& [to, fades] : m_fades)
		{
			const auto from = shown.find(fades.crossfade.from);
			const auto into = shown.find(to);
			if (from == shown.end() || into == shown.end() ||
				!(audio.size() == 1 ? selected(from->first) || selected(to) : selected(from->first) && selected(to)))
				continue;
			const Shown& out = *from->second;
			const Shown& in = *into->second;
			if (out.lane == in.lane && out.start < in.start && in.start < out.end && out.end <= in.end && !out.mute &&
				!in.mute)
			{
				openFadeDialog("crossfade.rml", {{"from", std::to_string(out.id)}, {"to", std::to_string(to)}});
				return true;
			}
		}
		return false;
	}

	bool EventDisplay::openCrossfadeAt(const Shown& shown, double seconds)
	{
		// The crossfades into and out of the event, where the two events overlap (reference manual p. 389).
		for (const auto& [to, fades] : m_fades)
		{
			if (fades.crossfade.from == 0 || (to != shown.id && fades.crossfade.from != shown.id))
				continue;
			const auto from = std::find_if(m_events.begin(), m_events.end(),
				[&fades = fades](const Shown& event) { return event.id == fades.crossfade.from; });
			const auto into = std::find_if(
				m_events.begin(), m_events.end(), [to = to](const Shown& event) { return event.id == to; });
			if (from != m_events.end() && into != m_events.end() && seconds >= into->start && seconds < from->end)
			{
				openFadeDialog("crossfade.rml", {{"from", std::to_string(from->id)}, {"to", std::to_string(to)}});
				return true;
			}
		}
		return false;
	}

	void EventDisplay::openFadeDialog(
		const char* file, const std::vector<std::pair<std::string, std::string>>& attributes)
	{
		if (m_dialog)
			return;
		const std::string path = std::filesystem::u8path(m_root->GetOwnerDocument()->GetSourceURL())
									 .replace_filename(file)
									 .generic_u8string();
		Rml::ElementDocument* dialog = m_root->GetContext()->LoadDocument(path);
		if (dialog == nullptr)
		{
			logging::warning("The event display can't open the dialog {}", path);
			return;
		}
		for (const auto& [name, value] : attributes)
			dialog->SetAttribute(name, value);
		dialog->Show(Rml::ModalFlag::Modal);
		m_dialog = dialog->GetObserverPtr();
		m_dialogForObjects = !attributes.empty(); // events, or from and to
	}

	void EventDisplay::applyStandardFade(bool in)
	{
		const std::vector<uint64_t> events = selectedAudio();
		if (events.empty())
			return;
		const char* key = in ? "editing.defaultFadeIn" : "editing.defaultFadeOut";
		m_binding.send("settings.get", {{"key", key}},
			[this, alive = std::weak_ptr<bool>(m_alive), events, in](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				const Json& fade = result.value.at("value");
				Json commands = Json::array();
				// In each event's own clip's frames, as far as the other fade leaves room (p. 385).
				for (const uint64_t id : events)
					if (const auto found = m_fades.find(id); found != m_fades.end() && found->second.rate > 0.0)
					{
						const AudioFades& fades = found->second;
						const int64_t room = std::max<int64_t>(0, fades.fadeRoom - (in ? fades.out : fades.in).length);
						const int64_t length = std::min(
							room, static_cast<int64_t>(std::llround(fade.at("length").get<double>() * fades.rate)));
						commands.push_back({{"command", "audioEvent.setFades"},
							{"parameters",
								{{"event", id},
									{in ? "fadeIn" : "fadeOut", {{"length", length}, {"shape", fade.at("shape")}}}}}});
					}
				if (!commands.empty())
					edit("history.transaction",
						{{"description", in ? "Apply Standard Fade In" : "Apply Standard Fade Out"},
							{"commands", std::move(commands)}});
			});
	}

	void EventDisplay::finishSliceJob(const Json& job)
	{
		if (job.at("state") == "running" || m_sliceJobs.erase(job.at("id").get<uint64_t>()) == 0)
			return;
		if (job.at("state") == "failed")
			showRefusal(job.at("error").get<std::string>());
	}

	void EventDisplay::addFadeActions()
	{
		m_subscriptions.push_back(
			m_binding.on("job.finished", [this](const engine::Event& event) { finishSliceJob(event.data.at("job")); }));
		// range.fades on the selection range, if there is one.
		const auto onRange = [this](const char* fade)
		{
			if (!m_range || m_range->end <= m_range->start)
				return false;
			edit("range.fades",
				{{"start", m_range->start}, {"end", m_range->end}, {"tracks", m_range->tracks}, {"fade", fade}});
			return true;
		};
		const auto toRange = [onRange](const char* fade) { return [onRange, fade] { onRange(fade); }; };
		// Remove Fades on a selection range removes the crossfades in it (reference manual p. 396).
		const auto remove = [this, onRange](bool in, bool out)
		{
			return [this, in, out, onRange]
			{
				if (in && out && onRange("remove"))
					return;
				if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
					edit("event.removeFades", {{"events", events}, {"fadeIn", in}, {"fadeOut", out}});
			};
		};
		const auto slices = [this](const char* command, const char* mode)
		{
			return [this, command, mode]
			{
				auto events = selectedAudio();
				if (m_project.contains("audioEvents"))
					for (const Json& event : m_project.at("audioEvents"))
					{
						const auto part = event.value("part", uint64_t{0});
						if (part != 0 && std::binary_search(m_selection.begin(), m_selection.end(), part))
							events.push_back(event.at("id").get<uint64_t>());
					}
				if (!events.empty())
				{
					Json parameters = {{"events", events}};
					if (mode != nullptr)
						parameters["mode"] = mode;
					edit(command, std::move(parameters));
				}
			};
		};
		m_actions.insert({{"audio.closeGapsCrossfade", slices("event.closeGaps", "crossfade")},
			{"audio.closeGapsTimeStretch", slices("event.closeGaps", "timeStretch")},
			{"audio.deleteOverlaps", slices("event.deleteOverlaps", nullptr)}, {"audio.fadeInToRange", toRange("in")},
			{"audio.fadeOutFromRange", toRange("out")}, {"audio.adjustFadesToRange", toRange("adjust")},
			{"audio.removeFadeIn", remove(true, false)}, {"audio.removeFadeOut", remove(false, true)},
			{"audio.removeFades", remove(true, true)},
			{"audio.fadeEditor",
				[this]
				{
					if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
						openFadeDialog("fade-editor.rml", {{"events", ids(events)}});
				}},
			// The audio functions (#147, docs/features/audio-functions.md), whose dialogs work on the selected audio
			// events as the fade dialog does.
			{"audio.detectSilence",
				[this]
				{
					if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
						openFadeDialog("detect-silence.rml", {{"events", ids(events)}});
				}},
			{"audio.spectrumAnalyzer",
				[this]
				{
					if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
						openFadeDialog("spectrum-analyzer.rml", {{"events", ids(events)}});
				}},
			{"audio.statistics",
				[this]
				{
					if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
						openFadeDialog("statistics.rml", {{"events", ids(events)}});
				}},
			// Create Sampler Track (#180, reference manual p. 711): from the first selected audio event's section.
			{"audio.createSamplerTrack",
				[this]
				{
					if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
						edit("sampler.create", {{"event", events.front()}});
				}},
			// Realtime Processing (#144, docs/features/warp.md): of the selected audio events.
			{"audio.flatten",
				[this]
				{
					if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
						edit("event.flatten", {{"events", events}});
				}},
			{"audio.unstretch",
				[this]
				{
					if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
						edit("event.unstretch", {{"events", events}});
				}},
			// Crossfade (reference manual pp. 388-389): crossfades the events a selection range crosses, or the
			// selected events, or opens the crossfade editor for the crossfade between them.
			{"audio.crossfade",
				[this, onRange]
				{
					const std::vector<uint64_t> events = selectedAudio();
					if (!onRange("crossfade") && !openSelectedCrossfade() && !events.empty())
						edit("event.crossfade", {{"events", events}});
				}},
			// The quantize panel's Open Crossfade Editor (p. 379): only the editor.
			{"audio.crossfadeEditor", [this] { openSelectedCrossfade(); }},
			{"audio.applyStandardFadeIn", [this] { applyStandardFade(true); }},
			{"audio.applyStandardFadeOut", [this] { applyStandardFade(false); }}});
	}
}
