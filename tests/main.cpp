// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"

#include <cstdio>
#include <cstring>

// ponytail: hand-rolled check until the Catch2 harness lands (#3)
int main()
{
	if (std::strcmp(gnubase::engine::version(), GNUBASE_VERSION) != 0)
	{
		std::printf("engine reports version %s, expected %s\n", gnubase::engine::version(), GNUBASE_VERSION);
		return 1;
	}
	return 0;
}
