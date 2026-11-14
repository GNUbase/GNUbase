#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs clang-tidy on the C++ files a branch changes, so that most of CI's clang-tidy findings turn up before a push:
# the changed and new .cpp files under src/ and tests/, and the .cpp file next to each changed header. It reports only
# findings on the lines the branch changed, since the rest passed CI already (and Windows' headers bring a few findings
# that CI's don't). It needs a compile database, not a build: it configures one in build/tidy, again when a changed
# file isn't in it yet. CI checks more (every line of every file that includes a changed header), so CI has the last
# word. On Linux, and so in WSL, it also compiles those files with GCC as CI's linux-x64-gcc job does (build/tidy-gcc).
# Usage: tools/tidy-changed.sh [base]    base: where the branch starts from (default origin/main)
# Linux uses a clang-tidy 22 on the PATH (CONTRIBUTING.md). CI's clang-tidy runs on Linux, where libstdc++ and the
# non-Windows #ifdef branches bring findings that Windows doesn't see (bugprone-unchecked-optional-access never fires
# with Microsoft's std::optional), so on Windows (Git Bash) the check runs in WSL when it has clang-tidy 22 (pipx, as
# for tools/ci-local.sh): on the commit at HEAD, in a clone in ~/gnubase-tidy/<checkout folder name>. Without it,
# Windows uses Visual Studio 2026's Ninja, compiler and clang-tidy. Not macOS yet: its universal build gives every file
# two -arch flags, which clang-tidy refuses.
# CLANG_TIDY overrides the clang-tidy to use, which on Windows also keeps the check out of WSL; GNUBASE_WSL names the
# WSL distribution (default Ubuntu). Exit status: 0 no findings, 1 findings, 2 error.
set -u

# Started by Windows inside WSL (below) with the repository, the commit at HEAD, where the branch starts and the
# checkout folder's name: moves the clone to that commit and checks it there. It fetches from the repository, not the
# checkout: a worktree's .git file holds a Windows path, which git in WSL can't follow.
if [ "${1:-}" = --wsl ]; then
	repository=$2 commit=$3 start=$4 mirror=$HOME/gnubase-tidy/$5
	export PATH="$HOME/.local/bin:$PATH" GIT_LFS_SKIP_SMUDGE=1 # pipx; clang-tidy needs no test audio
	mkdir -p "$HOME/gnubase-tidy" || exit 2
	if ! { { [ -d "$mirror/.git" ] || git clone -q --no-checkout "$repository" "$mirror"; } && cd "$mirror" &&
		git fetch -q --no-tags "$repository" "$commit" && git checkout -q --force --detach FETCH_HEAD &&
		git submodule sync -q && git submodule update -q --init --recursive &&
		git submodule update -q --init --checkout ext/vst3sdk &&
		git -C ext/vst3sdk submodule update -q --init base cmake pluginterfaces public.sdk; } > "$mirror.log" 2>&1; then
		echo "Preparing the clone in WSL failed, see ~/gnubase-tidy/$5.log"
		exit 2
	fi
	echo "clang-tidy runs in WSL on HEAD $(git rev-parse --short HEAD), in ~/gnubase-tidy/$5, as CI's runs on Linux."
	bash tools/tidy-changed.sh "$start" 2>&1 | sed "s|$mirror/||g"
	exit "${PIPESTATUS[0]}"
fi

root=$(git rev-parse --show-toplevel) || exit 2
cd "$root" || exit 2
start=$(git merge-base "${1:-origin/main}" HEAD) || exit 2
build=build/tidy
# One configuration: multi-config generators list every file once per configuration.
options="-DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_CONFIGURATION_TYPES=Debug"

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
	distro=${GNUBASE_WSL:-Ubuntu}
	if [ -z "${CLANG_TIDY:-}" ] && MSYS_NO_PATHCONV=1 wsl.exe -d "$distro" -e bash -c \
		'PATH=$HOME/.local/bin:$PATH clang-tidy --version' 2>/dev/null | tr -d '\0' | grep -q 'version 22\.'; then
		[ -z "$(git status --porcelain -- src tests)" ] ||
			echo "Note: uncommitted changes are not checked, only HEAD $(git rev-parse --short HEAD)."
		wslPath() { cygpath -m "$1" | sed -E 's|^([A-Za-z]):|/mnt/\L\1|'; }
		MSYS_NO_PATHCONV=1 wsl.exe -d "$distro" -e bash "$(wslPath "$root")/tools/tidy-changed.sh" --wsl \
			"$(wslPath "$(git rev-parse --path-format=absolute --git-common-dir)")" "$(git rev-parse HEAD)" "$start" \
			"$(basename "$root")"
		exit
	fi
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
	# a header's is the include folder (src or tests) with backslashes, then the #include's path as written. Each name
	# starts at a folder boundary, so that rt_tests.cpp can't stand for transport_tests.cpp.
	names()
	{
		local inFolder=${1#*/}
		printf '\\\\%s\n' "${1//\//\\\\}"
		[ "$inFolder" = "${inFolder#*/}" ] || printf '\\\\%s\n' "$inFolder"
	}
	;;
Linux)
	tidy=${CLANG_TIDY:-clang-tidy}
	# shellcheck disable=SC2086 # $options holds several arguments
	configure() { cmake --preset linux-clang -B "$build" $options; }
	names() { printf '/%s\n' "$1"; }
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
# A file new to CMake since the last configure. A file that no target of this platform builds stays missing, so only
# a CMake file newer than the database reconfigures.
cmakeChanged()
{
	[ -n "$(find . \( -path ./build -o -path ./ext \) -prune -o \( -name CMakeLists.txt -o -name '*.cmake' \) \
		-newer "${1:-$build/compile_commands.json}" -print -quit)" ]
}
for file in "${candidates[@]}"; do
	inDatabase "$file" || { cmakeChanged && reconfigure; break; }
done
files=() skipped=()
for file in "${candidates[@]}"; do
	if inDatabase "$file"; then files+=("$file"); else skipped+=("$file"); fi
done
[ ${#skipped[@]} -gt 0 ] && echo "Not checked, in no target of this platform: ${skipped[*]}"

# The changed lines as clang-tidy's line filter: {"name": file, "lines": [[first, last], ...]} per file; a new file
# without "lines" counts with all of them. Each range takes 5 lines more on both sides: some checks report a finding a
# change causes on an unchanged line next to it, such as readability-inconsistent-ifelse-braces on the unbraced first
# "if" of a chain that gained a braced "else if" (#1400).
filter=$(
	{
		changes -U0 | awk -v margin=5 '
			/^\+\+\+ b\// { file = substr($0, 7) }
			/^@@ / {
				split(substr($3, 2), range, ",")
				count = range[2] == "" ? 1 : range[2]
				if (count > 0) {
					separator = file in lines ? "," : ""
					first = range[1] > margin ? range[1] - margin : 1
					lines[file] = lines[file] separator "[" first "," (range[1] + count - 1 + margin) "]"
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
[ $status -eq 0 ] && echo "No findings."
[ $status -eq 123 ] && echo "clang-tidy found problems (above)."
[ $status -ne 0 ] && [ $status -ne 123 ] && exit 2
result=$((status == 0 ? 0 : 1))

# CI also builds with GCC 13 in Release, whose optimizer warns where Clang doesn't (-Werror=array-bounds, say), and
# clang-tidy already reports what Clang can't compile. So on Linux (CLANG_TIDY or not) the same files are compiled with
# GCC as CI builds them, whole files rather than changed lines, from a second compile database in build/tidy-gcc.
# Without LTO, as CI builds: with it, GCC's compile step stops before the optimizer and its warnings come at link time.
if [ "$(uname -s)" = Linux ]; then
	gcc=build/tidy-gcc
	if [ ! -f "$gcc/compile_commands.json" ] || cmakeChanged "$gcc/compile_commands.json" ||
		! grep -qs '^GNUBASE_LTO:BOOL=OFF$' "$gcc/CMakeCache.txt"; then
		echo "Configuring $gcc for GCC's compile commands"
		mkdir -p "$gcc" && cmake --preset linux-gcc -B "$gcc" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
			-DCMAKE_CONFIGURATION_TYPES=Release -DGNUBASE_LTO=OFF > "$gcc/configure.log" 2>&1 ||
			{ echo "Configuring failed, see $gcc/configure.log"; exit 2; }
	fi
	echo "GCC on the same file(s)"
	# Each file's compile command, to run in its folder with the object going nowhere.
	commands=$gcc/commands
	python3 - "$gcc/compile_commands.json" "${files[@]}" > "$commands" <<'EOF' || { echo "Reading $gcc's compile commands failed."; exit 2; }
import json, shlex, sys
entries = json.load(open(sys.argv[1]))
missing = []
for name in sys.argv[2:]:
	entry = next((e for e in entries if e["file"].endswith("/" + name)), None)
	if entry is None:
		missing.append(name)
		continue
	args = shlex.split(entry["command"])
	args[args.index("-o") + 1] = "/dev/null"
	sys.stdout.write("cd " + shlex.quote(entry["directory"]) + " && " + shlex.join(args) + "\0")
if missing:
	print("Not compiled with GCC, in no target of its build: " + " ".join(missing), file=sys.stderr)
EOF
	# GCC at -O3 takes up to about 2 GB for one of the larger files: no more compiles at once than WSL's free memory
	# holds, so that many changed files don't run it out of memory.
	jobs=$(awk -v cores="$(nproc)" '/^MemAvailable:/ { n = int($2 / 2097152); print (n < 1 ? 1 : n > cores ? cores : n) }' \
		/proc/meminfo)
	compiled=$(tr -cd '\0' < "$commands" | wc -c)
	[ "$compiled" -lt ${#files[@]} ] && them="the others" || them="them"
	if [ "$compiled" -gt 0 ]; then
		xargs -0 -P "${jobs:-1}" -n 1 bash -c \
			'out=$(eval "$0" 2>&1); s=$?; [ $s -ne 0 ] && printf "%s\n" "$out"; exit $s' < "$commands" &&
			echo "GCC compiles $them." || { echo "GCC failed (above)."; result=1; }
	fi
fi
exit $result
