// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/displays.h"

#include <algorithm>

namespace gnubase::ui
{
	namespace
	{
		Rect intersection(const Rect& a, const Rect& b)
		{
			const int left = std::max(a.x, b.x);
			const int top = std::max(a.y, b.y);
			const int right = std::min(a.x + a.width, b.x + b.width);
			const int bottom = std::min(a.y + a.height, b.y + b.height);
			if (right <= left || bottom <= top)
				return {};
			return {left, top, right - left, bottom - top};
		}
	}

	int64_t overlap(const Rect& a, const Rect& b)
	{
		const Rect shared = intersection(a, b);
		return static_cast<int64_t>(shared.width) * shared.height;
	}

	const Display* displayOf(const Rect& window, const std::vector<Display>& displays)
	{
		const Display* best = nullptr;
		int64_t bestOverlap = 0;
		for (const Display& display : displays)
		{
			const int64_t shared = overlap(window, display.bounds);
			if (shared > bestOverlap)
			{
				best = &display;
				bestOverlap = shared;
			}
		}
		return best;
	}

	bool reachable(const Rect& window, const std::vector<Display>& displays)
	{
		const Rect top{window.x, window.y, window.width, std::min(window.height, g_reachableHeight)};
		const int width = std::min(window.width, g_reachableWidth);
		return std::any_of(displays.begin(), displays.end(),
			[&](const Display& display)
			{
				const Rect shared = intersection(top, display.usable);
				return shared.width >= width && shared.height >= top.height && shared.height > 0;
			});
	}

	Rect centred(Size size, const Display& display)
	{
		const Rect& area = display.usable;
		const int width = std::min(size.width, area.width);
		const int height = std::min(size.height, area.height);
		return {area.x + ((area.width - width) / 2), area.y + ((area.height - height) / 2), width, height};
	}

	Rect place(const Rect& window, const std::vector<Display>& displays)
	{
		if (displays.empty() || reachable(window, displays))
			return window;
		return centred(window.size(), displays.front());
	}
}
