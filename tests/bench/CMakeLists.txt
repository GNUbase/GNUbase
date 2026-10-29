# SPDX-License-Identifier: GPL-3.0-or-later
# Benchmarks, built with -DGNUBASE_BENCHMARKS=ON. Not tests: they print measurements for the design notes in docs/.

# Time-stretch libraries (docs/time-stretch.md). Rubber Band Library is measured through the engine's wrapper.
# Signalsmith Stretch, which GNUbase doesn't ship, joins in when GNUBASE_BENCH_SIGNALSMITH_DIRS lists the folder with
# signalsmith-stretch.h and the folder that contains signalsmith-linear/ (its dependency).
set(GNUBASE_BENCH_SIGNALSMITH_DIRS "" CACHE STRING "Include folders for Signalsmith Stretch and signalsmith-linear")
add_executable(gnubase-timestretch-bench timestretch_bench.cpp)
target_link_libraries(gnubase-timestretch-bench PRIVATE gnubase-engine gnubase-rtcheck)
if(GNUBASE_BENCH_SIGNALSMITH_DIRS)
	target_include_directories(gnubase-timestretch-bench SYSTEM PRIVATE ${GNUBASE_BENCH_SIGNALSMITH_DIRS})
	target_compile_definitions(gnubase-timestretch-bench PRIVATE GNUBASE_BENCH_SIGNALSMITH
		$<$<CXX_COMPILER_ID:MSVC>:_USE_MATH_DEFINES NOMINMAX>)
endif()
gnubase_target_defaults(gnubase-timestretch-bench)
