// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/timeformat.h"

#include "platform/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		constexpr std::array<TimeFormat, 5> g_formats{TimeFormat::BarsBeats, TimeFormat::Seconds, TimeFormat::Timecode,
			TimeFormat::Samples, TimeFormat::UserFrames};

		constexpr time::Ticks g_sixteenth = time::g_ticksPerQuarter / 4;
		constexpr time::Ticks g_displayTick = g_sixteenth / time::g_defaultDisplayTicksPerSixteenth;

		// More marks than any ruler is wide in pixels.
		constexpr size_t g_maxMarks = 20000;

		// A step in a format's own units, and how many parts minor ticks divide it into.
		struct Step
		{
			double length = 1.0;
			int parts = 1;
		};

		// The first of steps (shortest first) at least minimum long; past the last, decimal multiples of it.
		Step pick(const std::vector<Step>& steps, double minimum)
		{
			for (const Step& step : steps)
				if (step.length >= minimum * (1.0 - 1e-9))
					return step;
			const double unit = steps.back().length;
			const auto [factor, parts] = decimalStep(minimum / unit);
			return {factor * unit, parts};
		}

		// Seconds in round steps, from a millisecond to an hour.
		std::vector<Step> secondSteps()
		{
			return {{0.001, 1}, {0.002, 2}, {0.005, 5}, {0.01, 5}, {0.02, 4}, {0.05, 5}, {0.1, 5}, {0.2, 4}, {0.5, 5},
				{1, 5}, {2, 4}, {5, 5}, {10, 5}, {15, 3}, {30, 3}, {60, 4}, {120, 4}, {300, 5}, {600, 5}, {900, 3},
				{1800, 3}, {3600, 4}};
		}

		// Frames in steps of frames, then of round seconds.
		std::vector<Step> frameSteps(int framesPerSecond)
		{
			std::vector<Step> steps;
			for (const Step& frames : {Step{1, 1}, Step{2, 2}, Step{5, 5}, Step{10, 5}})
				if (frames.length < framesPerSecond)
					steps.push_back(frames);
			const int secondParts = framesPerSecond % 5 == 0 ? 5 : 4; // whole frames each
			for (const Step& seconds : secondSteps())
				if (seconds.length >= 1.0)
					steps.push_back(
						{seconds.length * framesPerSecond, seconds.length == 1.0 ? secondParts : seconds.parts});
			return steps;
		}

		double framesPerSecond(time::FrameRate rate)
		{
			return rate.framesPerSecond / (rate.pullDown ? 1.001 : 1.0);
		}

		time::FrameRate userRate(const TimeContext& context)
		{
			return {std::max(1, context.userFramesPerSecond), false, false};
		}

		std::string timecode(double seconds, time::FrameRate rate)
		{
			return time::formatTimecode(time::toTimecode(seconds, rate), rate);
		}

		// Marks of a unit that grows linearly with time, perSecond units a second: seconds, frames and samples. The
		// labels count from offset seconds at the project start.
		void linearMarks(double from, double to, double minimum, double perSecond, const std::vector<Step>& steps,
			const std::function<std::string(double units, double step)>& label, std::vector<RulerMark>& marks,
			double offset = 0.0)
		{
			from = std::max(from, 0.0);
			const Step step = pick(steps, minimum * perSecond);
			const size_t first = marks.size();
			evenMarks((from + offset) * perSecond, (to + offset) * perSecond, step.length, step.parts,
				[&label, &step](double units) { return label(units, step.length); }, marks);
			for (size_t i = first; i < marks.size(); ++i)
				marks[i].position = (marks[i].position / perSecond) - offset;
		}

		// What seconds, timecode and user frames show at the project start.
		double startOffset(TimeFormat format, const TimeContext& context)
		{
			const bool clock =
				format == TimeFormat::Seconds || format == TimeFormat::Timecode || format == TimeFormat::UserFrames;
			return clock ? context.startTime : 0.0;
		}

		// "5", "5.2", "5.2.3" or "5.2.3.60": a position as bars, beats, sixteenths and display ticks, without the
		// parts that are zero.
		std::string musicalLabel(const time::BarsBeats& position)
		{
			std::string text = std::to_string(position.bar + 1);
			if (position.beat == 0 && position.sixteenth == 0 && position.tick == 0)
				return text;
			text += "." + std::to_string(position.beat + 1);
			if (position.sixteenth == 0 && position.tick == 0)
				return text;
			text += "." + std::to_string(position.sixteenth + 1);
			return position.tick == 0 ? text : text + "." + std::to_string(position.tick);
		}

		time::Ticks beatLength(const time::TimeSignature& signature)
		{
			return time::g_ticksPerQuarter * 4 / signature.denominator;
		}

		// Bars and beats: labels every few bars, every beat, every sixteenth or every few display ticks, whichever is
		// the finest that keeps them minimum seconds apart where the tempo is fastest and the bars or beats shortest.
		void musicalMarks(
			const TimeContext& context, double from, double to, double minimum, std::vector<RulerMark>& marks)
		{
			from = std::max(from, 0.0);
			if (!(to > from) || !(minimum > 0.0))
				return;
			const time::TempoMap& tempo = context.tempo;
			const time::SignatureMap& signatures = context.signatures;
			double needed = 0.0; // ticks
			for (int i = 0; i <= 32; ++i)
			{
				const double at = from + ((to - from) * i / 32.0);
				needed = std::max(needed, static_cast<double>(tempo.toTicks(at + minimum) - tempo.toTicks(at)));
			}
			needed *= 1.0 - 1e-6; // ticks are rounded
			time::Ticks bar = std::numeric_limits<time::Ticks>::max();
			time::Ticks beat = std::numeric_limits<time::Ticks>::max();
			for (const time::SignatureChange& change : signatures.changes())
			{
				beat = std::min(beat, beatLength(change.signature));
				bar = std::min(bar, beatLength(change.signature) * change.signature.numerator);
			}
			const time::Ticks first = tempo.toTicks(from);
			const time::Ticks last = tempo.toTicks(to);
			const auto add = [&](time::Ticks position, bool labelled)
			{
				if (position >= first && position < last)
				{
					time::BarsBeats shown = signatures.toBarsBeats(position);
					shown.bar += context.barOffset;
					marks.push_back({tempo.toSeconds(position), labelled ? musicalLabel(shown) : ""});
				}
			};

			if (static_cast<double>(beat) < needed)
			{
				// Every n bars, n a power of two; minor ticks at the beats of single bars, else at up to 4 parts.
				int64_t n = 1;
				while (static_cast<double>(n * bar) < needed)
					n *= 2;
				const int64_t parts = std::min<int64_t>(n, 4);
				for (int64_t b = signatures.toBarsBeats(first).bar / n * n;
					signatures.barStart(b) < last && marks.size() < g_maxMarks; b += n)
				{
					const time::Ticks start = signatures.barStart(b);
					add(start, true);
					if (n == 1)
					{
						const time::TimeSignature signature = signatures.signatureAt(start);
						for (int i = 1; i < signature.numerator; ++i)
							add(start + (i * beatLength(signature)), false);
					}
					else
					{
						for (int64_t i = 1; i < parts; ++i)
							add(signatures.barStart(b + (i * n / parts)), false);
					}
				}
				return;
			}

			// Within bars: beats, sixteenths or display ticks.
			time::Ticks step = 0; // 0: a beat, whose length depends on the bar's signature
			int parts = 4;
			if (static_cast<double>(g_sixteenth) < needed)
			{
				step = 0;
			}
			else
			{
				step = g_sixteenth;
				for (const auto& [ticks, minor] : {std::pair<int, int>{1, 1}, {5, 5}, {15, 3}, {30, 2}, {60, 2}})
					if (static_cast<double>(ticks * g_displayTick) >= needed)
					{
						step = ticks * g_displayTick;
						parts = minor;
						break;
					}
			}
			for (int64_t b = signatures.toBarsBeats(first).bar; signatures.barStart(b) < last; ++b)
			{
				const time::Ticks start = signatures.barStart(b);
				const time::Ticks end = signatures.barStart(b + 1);
				const time::Ticks length = step == 0 ? beatLength(signatures.signatureAt(start)) : step;
				const int divisions =
					step == 0 ? static_cast<int>(std::clamp<time::Ticks>(length / g_sixteenth, 1, 4)) : parts;
				for (time::Ticks at = start; at < end && at < last && marks.size() < g_maxMarks; at += length)
				{
					add(at, true);
					for (int i = 1; i < divisions; ++i)
						add(at + (length * i / divisions), false);
				}
			}
		}
	}

	std::string timeFormatName(TimeFormat format)
	{
		switch (format)
		{
		case TimeFormat::BarsBeats:
			return "barsBeats";
		case TimeFormat::Seconds:
			return "seconds";
		case TimeFormat::Timecode:
			return "timecode";
		case TimeFormat::Samples:
			return "samples";
		case TimeFormat::UserFrames:
			return "userFrames";
		}
		return {};
	}

	std::optional<TimeFormat> timeFormatFromName(const std::string& name)
	{
		for (const TimeFormat format : g_formats)
			if (timeFormatName(format) == name)
				return format;
		return std::nullopt;
	}

	namespace
	{
		// The project setup's frame rate, start time and bar offset; the defaults for a project without them.
		void readSetup(const engine::Json& project, TimeContext& context)
		{
			context.frameRate = time::frameRateFromName(project.value("frameRate", "25")).value_or(time::g_fps25);
			context.startTime = project.value("startTime", 0.0);
			context.barOffset = project.value("barOffset", 0);
		}
	}

	TimeContext projectTime(const engine::Json& project)
	{
		TimeContext context;
		readSetup(project, context);
		context.sampleRate = project.at("sampleRate").get<double>();
		std::vector<time::TempoPoint> tempo;
		for (const engine::Json& point : project.at("tempo"))
			tempo.push_back(
				{point.at("position").get<time::Ticks>(), point.at("bpm").get<double>(), point.at("ramp").get<bool>()});
		std::vector<time::SignatureChange> signatures;
		for (const engine::Json& change : project.at("signatures"))
			signatures.push_back({change.at("bar").get<int64_t>(),
				{change.at("numerator").get<int>(), change.at("denominator").get<int>()}});
		try
		{
			context.tempo = time::TempoMap(tempo);
			context.signatures = time::SignatureMap(signatures);
		}
		catch (const std::invalid_argument& error)
		{
			logging::warning("The UI can't read the project's tempo or signatures: {}", error.what());
		}
		return context;
	}

	namespace
	{
		// A time in the format without the start time and bar offset, as lengths show it.
		std::string formatClock(TimeFormat format, double seconds, const TimeContext& context)
		{
			switch (format)
			{
			case TimeFormat::BarsBeats:
				return time::formatBarsBeats(context.signatures.toBarsBeats(context.tempo.toTicks(seconds)));
			case TimeFormat::Seconds:
				return time::formatSeconds(seconds);
			case TimeFormat::Timecode:
				return timecode(seconds, context.frameRate);
			case TimeFormat::Samples:
				return std::to_string(std::llround(seconds * context.sampleRate));
			case TimeFormat::UserFrames:
				return timecode(seconds, userRate(context));
			}
			return {};
		}
	}

	std::string formatTime(TimeFormat format, double seconds, const TimeContext& context)
	{
		if (format == TimeFormat::BarsBeats)
		{
			time::BarsBeats position = context.signatures.toBarsBeats(context.tempo.toTicks(seconds));
			position.bar += context.barOffset;
			return time::formatBarsBeats(position);
		}
		return formatClock(format, seconds + startOffset(format, context), context);
	}

	namespace
	{
		// The text between separators, without the spaces around it; nothing if a part is empty.
		std::optional<std::vector<std::string>> split(const std::string& text, const std::string& separators)
		{
			std::vector<std::string> parts(1);
			const size_t first = text.find_first_not_of(' ');
			const size_t last = text.find_last_not_of(' ');
			const std::string trimmed =
				first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
			for (const char c : trimmed)
				if (separators.find(c) != std::string::npos)
					parts.emplace_back();
				else if (c != ' ')
					parts.back() += c;
			for (const std::string& part : parts)
				if (part.empty())
					return std::nullopt;
			return parts;
		}

		std::optional<int64_t> integer(const std::string& text)
		{
			if (text.empty() || text.size() > 15 || text.find_first_not_of("0123456789") != std::string::npos)
				return std::nullopt;
			return std::stoll(text);
		}

		// Up to count integers, the last ones of the text's parts first: {frames, seconds, minutes, hours}.
		std::optional<std::vector<int64_t>> fromRight(
			const std::string& text, const std::string& separators, size_t count)
		{
			const auto parts = split(text, separators);
			if (!parts || parts->size() > count)
				return std::nullopt;
			std::vector<int64_t> numbers(count, 0);
			for (size_t i = 0; i < parts->size(); ++i)
			{
				const auto number = integer((*parts)[parts->size() - 1 - i]);
				if (!number)
					return std::nullopt;
				numbers[i] = *number;
			}
			return numbers;
		}

		std::optional<double> timecodeSeconds(const std::string& text, time::FrameRate rate)
		{
			// Hours past what a timecode holds aren't cut to fewer.
			const auto numbers = fromRight(text, ":;", 4);
			if (!numbers || (*numbers)[0] >= rate.framesPerSecond || (*numbers)[1] >= 60 || (*numbers)[2] >= 60 ||
				(*numbers)[3] > std::numeric_limits<int>::max())
				return std::nullopt;
			return time::toSeconds({false, static_cast<int>((*numbers)[3]), static_cast<int>((*numbers)[2]),
									   static_cast<int>((*numbers)[1]), static_cast<int>((*numbers)[0])},
				rate);
		}

		// Bars, beats, sixteenths and display ticks, as many as typed; the rest are zero. A signed bar may have a minus
		// sign, for the bars a negative bar offset shows before bar 1.
		std::optional<time::BarsBeats> barsBeats(const std::string& text, bool signedBar = false)
		{
			const auto parts = split(text, " .,:;");
			if (!parts || parts->size() > 4)
				return std::nullopt;
			std::array<int64_t, 4> numbers{};
			for (size_t i = 0; i < parts->size(); ++i)
			{
				const std::string& part = (*parts)[i];
				const bool negative = signedBar && i == 0 && part.size() > 1 && part[0] == '-';
				const auto number = integer(negative ? part.substr(1) : part);
				if (!number || *number > std::numeric_limits<int>::max())
					return std::nullopt;
				numbers.at(i) = negative ? -*number : *number;
			}
			return time::BarsBeats{
				numbers[0], static_cast<int>(numbers[1]), static_cast<int>(numbers[2]), static_cast<int>(numbers[3])};
		}

		// A position typed in the format, without the start time, as lengths are typed; bars count from the bar
		// offset.
		std::optional<double> parseClock(TimeFormat format, const std::string& text, const TimeContext& context)
		{
			switch (format)
			{
			case TimeFormat::BarsBeats:
			{
				const auto parts = split(text, " .,:;");
				std::optional<time::BarsBeats> position = barsBeats(text, true);
				if (!parts || !position || position->bar - context.barOffset < 1)
					return std::nullopt;
				// One-based, but for display ticks; what is left out is the first.
				const size_t typed = parts->size();
				if ((typed > 1 && position->beat < 1) || (typed > 2 && position->sixteenth < 1))
					return std::nullopt;
				position->bar -= 1 + context.barOffset;
				position->beat = std::max(position->beat - 1, 0);
				position->sixteenth = std::max(position->sixteenth - 1, 0);
				return context.tempo.toSeconds(context.signatures.toTicks(*position));
			}
			case TimeFormat::Seconds:
			{
				const auto parts = split(text, ":");
				if (!parts || parts->size() > 3)
					return std::nullopt;
				const std::string& last = parts->back();
				const size_t point = last.find('.');
				const auto whole = integer(last.substr(0, point));
				const auto fraction =
					point == std::string::npos ? std::optional<int64_t>(0) : integer(last.substr(point + 1));
				if (!whole || !fraction)
					return std::nullopt;
				auto seconds = static_cast<double>(*whole);
				if (point != std::string::npos)
					seconds +=
						static_cast<double>(*fraction) / std::pow(10.0, static_cast<double>(last.size() - point - 1));
				double unit = 60.0;
				for (size_t i = parts->size() - 1; i-- > 0; unit *= 60.0)
				{
					const auto number = integer((*parts)[i]);
					if (!number)
						return std::nullopt;
					seconds += static_cast<double>(*number) * unit;
				}
				return seconds;
			}
			case TimeFormat::Timecode:
				return timecodeSeconds(text, context.frameRate);
			case TimeFormat::Samples:
			{
				const auto parts = split(text, "");
				const auto samples = parts ? integer(parts->front()) : std::nullopt;
				if (!samples)
					return std::nullopt;
				return static_cast<double>(*samples) / context.sampleRate;
			}
			case TimeFormat::UserFrames:
				return timecodeSeconds(text, userRate(context));
			}
			return std::nullopt;
		}
	}

	std::optional<double> parseTime(TimeFormat format, const std::string& text, const TimeContext& context)
	{
		std::optional<double> seconds = parseClock(format, text, context);
		if (!seconds)
			return std::nullopt;
		*seconds -= startOffset(format, context);
		if (*seconds < 0.0)
			return std::nullopt;
		return seconds;
	}

	namespace
	{
		// Where a length in bars from a start goes: the start's place in its bar, that many bars later, so that
		// lengths count the bars the ruler shows across signature changes.
		time::Ticks barsLater(const time::SignatureMap& signatures, time::Ticks start, int64_t bars)
		{
			const int64_t bar = signatures.toBarsBeats(start).bar;
			return signatures.barStart(bar + bars) + (start - signatures.barStart(bar));
		}
	}

	std::string formatLength(TimeFormat format, double from, double to, const TimeContext& context)
	{
		if (format != TimeFormat::BarsBeats)
			return formatClock(format, to - from, context);
		// Whole bars from the start, then the rest in the beats of the signature there.
		const time::SignatureMap& signatures = context.signatures;
		const time::Ticks start = context.tempo.toTicks(from);
		const time::Ticks end = context.tempo.toTicks(to);
		int64_t bars = std::max<int64_t>(0, signatures.toBarsBeats(end).bar - signatures.toBarsBeats(start).bar);
		while (bars > 0 && barsLater(signatures, start, bars) > end)
			--bars;
		const time::Ticks at = barsLater(signatures, start, bars);
		time::BarsBeats length = time::SignatureMap(signatures.signatureAt(at)).toBarsBeats(end - at);
		length.bar += bars;
		return std::to_string(length.bar) + "." + std::to_string(length.beat) + "." + std::to_string(length.sixteenth) +
			"." + std::to_string(length.tick);
	}

	std::optional<double> parseLength(
		TimeFormat format, const std::string& text, double from, const TimeContext& context)
	{
		if (format != TimeFormat::BarsBeats)
			return parseClock(format, text, context);
		const std::optional<time::BarsBeats> length = barsBeats(text);
		if (!length)
			return std::nullopt;
		const time::Ticks at = barsLater(context.signatures, context.tempo.toTicks(from), length->bar);
		const time::Ticks ticks = at +
			time::SignatureMap(context.signatures.signatureAt(at))
				.toTicks({0, length->beat, length->sixteenth, length->tick});
		return context.tempo.toSeconds(ticks) - from;
	}

	bool readTimeContext(const engine::Json& project, TimeContext& context)
	{
		try
		{
			std::vector<time::TempoPoint> tempo;
			for (const engine::Json& point : project.at("tempo"))
				tempo.push_back({point.at("position").get<time::Ticks>(), point.at("bpm").get<double>(),
					point.at("ramp").get<bool>()});
			std::vector<time::SignatureChange> signatures;
			for (const engine::Json& change : project.at("signatures"))
				signatures.push_back({change.at("bar").get<int64_t>(),
					{change.at("numerator").get<int>(), change.at("denominator").get<int>()}});
			time::TempoMap tempoMap(tempo);
			time::SignatureMap signatureMap(signatures);
			readSetup(project, context);
			context.sampleRate = project.at("sampleRate").get<double>();
			context.tempo = std::move(tempoMap);
			context.signatures = std::move(signatureMap);
			return true;
		}
		catch (const std::exception&)
		{
			return false;
		}
	}

	RulerScale rulerScale(TimeFormat format, const TimeContext& context)
	{
		RulerScale scale;
		switch (format)
		{
		case TimeFormat::BarsBeats:
			scale.marks = [shared = std::make_shared<const TimeContext>(context)](
							  double from, double to, double minimum, std::vector<RulerMark>& marks)
			{ musicalMarks(*shared, from, to, minimum, marks); };
			break;
		case TimeFormat::Seconds:
			scale.marks = [start = context.startTime](
							  double from, double to, double minimum, std::vector<RulerMark>& marks)
			{
				linearMarks(
					from, to, minimum, 1.0, secondSteps(),
					[](double seconds, double step)
					{
						const std::string text = time::formatSeconds(seconds);
						return step >= 1.0 ? text.substr(0, text.size() - 4) : text; // no ".000"
					},
					marks, start);
			};
			break;
		case TimeFormat::Timecode:
		case TimeFormat::UserFrames:
		{
			const time::FrameRate rate = format == TimeFormat::Timecode ? context.frameRate : userRate(context);
			scale.marks = [rate, start = context.startTime](
							  double from, double to, double minimum, std::vector<RulerMark>& marks)
			{
				linearMarks(
					from, to, minimum, framesPerSecond(rate), frameSteps(rate.framesPerSecond),
					[rate](double frame, double /*step*/)
					{ return time::formatTimecode(time::fromFrameNumber(std::llround(frame), rate), rate); }, marks,
					start);
			};
			break;
		}
		case TimeFormat::Samples:
			scale.marks = [rate = context.sampleRate](
							  double from, double to, double minimum, std::vector<RulerMark>& marks)
			{
				const auto [step, parts] = decimalStep(std::max(minimum * rate, 1.0));
				linearMarks(
					from, to, minimum, rate, {{step, parts}},
					[](double sample, double /*step*/) { return std::to_string(std::llround(sample)); }, marks);
			};
			break;
		}
		return scale;
	}
}
