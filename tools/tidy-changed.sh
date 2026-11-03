#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs clang-tidy on the C++ files a branch changes, so that most of CI's clang-tidy findings turn up before a push:
# the changed and new .cpp files under src/ and tests/, and the .cpp file next to each changed header. It reports only
# findings on the lines the branch changed, since the rest passed CI already (and Windows' headers bring a few findings
# that CI's don't). It needs a compile database, not a build: the first run configures one in build/tidy. CI checks
# more (every line of every file that includes a changed header) on Linux, where libstdc++ and the non-Windows #ifdef
# branches can bring findings that Windows doesn't see, so CI still has the last word.
# Usage: tools/tidy-changed.sh [base]    base: where the branch starts from (default origin/main)
# Windows (Git Bash) uses Visual Studio 2026's Ninja, compiler and clang-tidy; Linux and macOS a clang-tidy 22 on the
# PATH (CONTRIBUTING.md). CLANG_TIDY overrides the clang-tidy to use. Exit status: 0 no findings, 1 findings, 2 error.
set -u
root=$(git rev-parse --show-toplevel) || exit 2
cd "$root" || exit 2
start=$(git merge-base "${1:-origin/main}" HEAD) || exit 2
build=build/tidy
# One configuration: multi-config generators list every file once per configuration.
options="-DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CONFIGURATION_TYPES=Debug"

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
	vs=$("$(cygpath -u "$PROGRAMFILES") (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest \
		-property installationPath | tr -d '\r')
	tidy=${CLANG_TIDY:-$vs/VC/Tools/Llvm/x64/bin/clang-tidy.exe}
	configure()
	{
		# A script file, since cmd can't take the quoted paths of the developer environment on its command line.
		local script=$build/configure.cmd
		printf '@call "%s\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul || exit /b 1\r\n' "$vs" > "$script"
		printf 'cmake --preset windows-x64-ninja -B %s %s\r\n' "$build" "$options" >> "$script"
		cmd //c "$(cygpath -w "$script")"
	}
	# clang-tidy matches the line filter's names against Windows paths.
	nativeNames() { sed 's#/#\\\\#g'; }
	;;
*)
	tidy=${CLANG_TIDY:-clang-tidy}
	preset=linux-clang
	[ "$(uname -s)" = Darwin ] && preset=macos
	# shellcheck disable=SC2086 # $options holds several arguments
	configure() { cmake --preset "$preset" -B "$build" $options; }
	nativeNames() { cat; }
	;;
esac

if [ ! -f "$build/compile_commands.json" ]; then
	echo "Configuring $build for its compile database"
	mkdir -p "$build" && configure > "$build/configure.log" 2>&1 || { echo "Configuring failed, see $build/configure.log"; exit 2; }
fi

# Only files the compile database knows: others belong to another platform or to no GNUbase target.
mapfile -t files < <(
	{
		git diff --name-only --diff-filter=d "$start" -- src tests
		git ls-files --others --exclude-standard -- src tests
	} | sed -n -e 's/\.h$/.cpp/p' -e '/\.cpp$/p' | sort -u |
		while IFS= read -r file; do
			[ -f "$file" ] && grep -qF "/$file\"" "$build/compile_commands.json" && echo "$file"
		done
)
if [ ${#files[@]} -eq 0 ]; then
	echo "No changed C++ files to check."
	exit 0
fi
echo "clang-tidy on ${#files[@]} file(s): ${files[*]}"

# The changed lines as clang-tidy's line filter: {"name": file, "lines": [[first, last], ...]} per file; a new file
# without "lines" counts with all of them.
filter=$(
	{
		git diff -U0 --no-color "$start" -- src tests | awk '
			/^\+\+\+ b\// { file = substr($0, 7) }
			/^@@ / {
				split(substr($3, 2), range, ",")
				count = range[2] == "" ? 1 : range[2]
				if (count > 0) {
					separator = file in lines ? "," : ""
					lines[file] = lines[file] separator "[" range[1] "," (range[1] + count - 1) "]"
				}
			}
			END { for (file in lines) print "{\"name\":\"" file "\",\"lines\":[" lines[file] "]}" }'
		git ls-files --others --exclude-standard -- src tests | sed 's/.*/{"name":"&"}/'
	} | nativeNames | paste -s -d, -
)

# One clang-tidy per file and hardware thread; each prints its findings in one piece when it's done.
printf '%s\n' "${files[@]}" | xargs -P "$(nproc)" -I{} bash -c \
	'out=$("$0" -p "$1" --quiet --warnings-as-errors="*" --line-filter="[$2]" "$3" 2>&1); status=$?
	[ $status -ne 0 ] && printf "%s\n" "$out"; exit $status' "$tidy" "$build" "$filter" {}
status=$?
[ $status -eq 0 ] && echo "No findings." && exit 0
[ $status -eq 123 ] && echo "clang-tidy found problems (above)." && exit 1
exit 2
