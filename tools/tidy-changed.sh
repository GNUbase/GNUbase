#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs clang-tidy on the C++ files a branch changes, so that most of CI's clang-tidy findings turn up before a push:
# the changed and new .cpp files under src/ and tests/, and the .cpp file next to each changed header. It reports only
# findings on the lines the branch changed, since the rest passed CI already (and Windows' headers bring a few findings
# that CI's don't). It needs a compile database, not a build: it configures one in build/tidy, again when a changed
# file isn't in it yet. CI checks more (every line of every file that includes a changed header) on Linux, where
# libstdc++ and the non-Windows #ifdef branches can bring findings that Windows doesn't see, so CI has the last word.
# Usage: tools/tidy-changed.sh [base]    base: where the branch starts from (default origin/main)
# Windows (Git Bash) uses Visual Studio 2026's Ninja, compiler and clang-tidy; Linux a clang-tidy 22 on the PATH
# (CONTRIBUTING.md). Not macOS yet: its universal build gives every file two -arch flags, which clang-tidy refuses.
# CLANG_TIDY overrides the clang-tidy to use. Exit status: 0 no findings, 1 findings, 2 error.
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
	# clang-tidy matches the line filter's names as suffixes of the paths clang uses: a source file's has backslashes;
	# a header's is the include folder (src or tests) with backslashes, then the #include's path as written.
	names() { printf '%s\n' "${1//\//\\\\}" "${1#*/}"; }
	;;
Linux)
	tidy=${CLANG_TIDY:-clang-tidy}
	# shellcheck disable=SC2086 # $options holds several arguments
	configure() { cmake --preset linux-clang -B "$build" $options; }
	names() { printf '%s\n' "$1"; }
	;;
*)
	echo "Only Windows and Linux for now; CI checks the pull request."
	exit 2
	;;
esac
if ! "$tidy" --version 2>/dev/null | grep -q 'version 22\.'; then
	echo "No clang-tidy 22 at $tidy (CONTRIBUTING.md says how to install it, CLANG_TIDY where it is)."
	exit 2
fi

reconfigure()
{
	echo "Configuring $build for its compile database"
	mkdir -p "$build" && configure > "$build/configure.log" 2>&1 || { echo "Configuring failed, see $build/configure.log"; exit 2; }
}
[ -f "$build/compile_commands.json" ] || reconfigure

# The diff with fixed prefixes, whatever the user's git configuration says.
changes() { git diff --no-color --no-ext-diff --src-prefix=a/ --dst-prefix=b/ "$@" "$start" -- src tests; }
mapfile -t candidates < <(
	{
		changes --name-only --diff-filter=d
		git ls-files --others --exclude-standard -- src tests
	} | sed -n -e 's/\.h$/.cpp/p' -e '/\.cpp$/p' | sort -u | while IFS= read -r file; do [ -f "$file" ] && echo "$file"; done
)
inDatabase() { grep -qF "/$1\"" "$build/compile_commands.json"; }
for file in "${candidates[@]}"; do
	inDatabase "$file" || { reconfigure; break; } # a file new to CMake since the last configure
done
files=() skipped=()
for file in "${candidates[@]}"; do
	if inDatabase "$file"; then files+=("$file"); else skipped+=("$file"); fi
done
[ ${#skipped[@]} -gt 0 ] && echo "Not checked, in no target of this platform: ${skipped[*]}"

# The changed lines as clang-tidy's line filter: {"name": file, "lines": [[first, last], ...]} per file; a new file
# without "lines" counts with all of them.
filter=$(
	{
		changes -U0 | awk '
			/^\+\+\+ b\// { file = substr($0, 7) }
			/^@@ / {
				split(substr($3, 2), range, ",")
				count = range[2] == "" ? 1 : range[2]
				if (count > 0) {
					separator = file in lines ? "," : ""
					lines[file] = lines[file] separator "[" range[1] "," (range[1] + count - 1) "]"
				}
			}
			END { for (file in lines) print file "\t" lines[file] }'
		git ls-files --others --exclude-standard -- src tests | sed 's/$/\t/'
	} | while IFS=$'\t' read -r file lines; do
		names "$file" | while IFS= read -r name; do
			if [ -n "$lines" ]; then echo "{\"name\":\"$name\",\"lines\":[$lines]}"; else echo "{\"name\":\"$name\"}"; fi
		done
	done | paste -s -d, -
)
# Nothing but deletions: no line to check (an empty filter would let clang-tidy report every line).
if [ ${#files[@]} -eq 0 ] || [ -z "$filter" ]; then
	echo "No changed C++ lines to check."
	exit 0
fi
echo "clang-tidy on ${#files[@]} file(s): ${files[*]}"

# One clang-tidy per file and hardware thread; each prints its findings in one piece when it's done.
printf '%s\n' "${files[@]}" | xargs -P "$(nproc)" -I{} bash -c \
	'out=$("$0" -p "$1" --quiet --warnings-as-errors="*" --line-filter="[$2]" "$3" 2>&1); status=$?
	[ $status -ne 0 ] && printf "%s\n" "$out"; exit $status' "$tidy" "$build" "$filter" {}
status=$?
[ $status -eq 0 ] && echo "No findings." && exit 0
[ $status -eq 123 ] && echo "clang-tidy found problems (above)." && exit 1
exit 2
