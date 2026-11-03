#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs clang-tidy on a file unless it passed before on exactly the same input, so that CI checks only what changed.
# CMake puts it in front of clang-tidy when GNUBASE_CLANG_TIDY_CACHE names a folder (CMakeLists.txt):
#   tools/clang-tidy-cache.sh <folder> <clang-tidy> [option...] <file> -- <compile command>
# The key is a hash of the command line, clang-tidy's version, the configuration it uses for the file, and the file
# with the full text of everything it includes, as clang's -frewrite-includes writes it: comments (NOLINT), macro
# definitions and inactive code too. Only a pass is stored, as an empty file named after the key; a finding is never
# stored, so it fails every run. Without a key (the compiler isn't clang, or preprocessing fails) clang-tidy just runs.
set -u
cache=$1
shift
for ((end = 1; end <= $#; end++)); do
	[ "${!end}" = -- ] && break
done
file=$((end - 1))
file=${!file}
key=$(
	set -o pipefail
	{
		printf '%s\0' "$@" && "$1" --version && "$1" --dump-config "$file" &&
			"${@:end+1}" -w -E -frewrite-includes -o -
	} 2>/dev/null | sha256sum
) && key=$cache/${key%% *} || key=
if [ -n "$key" ] && [ -f "$key" ]; then
	touch "$key" # marks it as used, for CI to drop the others
	exit 0
fi
"$@"
status=$?
if [ $status -eq 0 ] && [ -n "$key" ]; then
	mkdir -p "$cache" && : > "$key"
fi
exit $status
