#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs CI's checks on this machine, all at once, so a pull request doesn't have to wait for CI (CONTRIBUTING.md):
# - format: license headers, clang-format and, where the private keyword list is, the keyword scan;
# - on Windows: windows-x64 (build and tests) and windows-arm64 (cross-compiled; its tests need an ARM64 machine);
# - on Linux, and on Windows in WSL: linux-gcc and linux-clang (builds and tests), asan-ubsan, tsan and clang-tidy.
# macOS is left to CI. It checks the commit at HEAD, so commit first: on Windows the Linux jobs run in a clone inside
# WSL (~/gnubase-ci/<checkout folder name>, one per checkout, so worktrees can run checks at the same time), which
# builds far faster than the Windows folder and has Linux line endings.
# Usage: tools/ci-local.sh [job...]    default: every job this machine can run
# Environment: GNUBASE_WSL names the WSL distribution (default Ubuntu), GNUBASE_CI_JOBS the compile jobs per build.
# Setup: Windows needs Visual Studio 2026; Linux (or WSL, Ubuntu 24.04) needs the packages in .github/workflows/ci.yml
# plus clang with its sanitizer runtimes (libclang-rt-18-dev), ccache, git-lfs, make (without it, GCC's link-time
# optimization runs its jobs one after another: linking the tests takes ten times as long) and, through pipx,
# clang-tidy==22.1.8 and clang-format==22.1.3.
# Windows builds through sccache when it is on the PATH, as CI does.
# Logs: build/ci-local/<job>.log. Exit status: 0 every job passed, 1 a job failed, 2 error. When every job this machine
# can run passed, build/ci-local/passed holds the commit they checked, so other tools can skip checking it again.
set -u

# Started by Windows inside WSL with the checkout, its repository and HEAD's commit: moves the clone to that commit,
# with the same submodules and test audio, runs the Linux jobs there and copies their logs back. It fetches from the
# repository, not the checkout: a worktree's .git file holds a Windows path, which git in WSL can't follow.
if [ "${1:-}" = --wsl ]; then
	source=$2 repository=$3 commit=$4
	shift 4
	mirror=$HOME/gnubase-ci/$(basename "$source")
	export GIT_LFS_SKIP_SMUDGE=1 # as in CI: only the test audio and the fonts come from Git LFS
	mkdir -p "$source/build/ci-local"
	if ! { { [ -d "$mirror/.git" ] || git clone -q --no-checkout "$repository" "$mirror"; } && cd "$mirror" &&
		git fetch -q --no-tags "$repository" "$commit" && git checkout -q --force --detach FETCH_HEAD &&
		git submodule sync -q && git submodule update -q --init --recursive &&
		git submodule update -q --init --checkout ext/vst3sdk &&
		git -C ext/vst3sdk submodule update -q --init base cmake pluginterfaces public.sdk &&
		git lfs pull --include "tests/data/**,tests/golden/**,resources/fonts/**"; } > "$source/build/ci-local/wsl.log" 2>&1; then
		echo "Preparing the clone in WSL failed, see build/ci-local/wsl.log"
		exit 1
	fi
	bash tools/ci-local.sh --here "$@"
	status=$?
	cp build/ci-local/*.log "$source/build/ci-local/"
	exit $status
fi

root=$(git rev-parse --show-toplevel) || exit 2
cd "$root" || exit 2
logs="$root/build/ci-local"
mkdir -p "$logs" || exit 2
threads=$(nproc 2>/dev/null || echo 8)
# A quarter of the hardware threads per build: several builds share them, and WSL gets only half of the memory.
export CMAKE_BUILD_PARALLEL_LEVEL=${GNUBASE_CI_JOBS:-$((threads / 4 > 2 ? threads / 4 : 2))}
export PATH="$HOME/.local/bin:$PATH" # pipx

case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*)
	os=windows
	vswhere="$(cygpath -u "$PROGRAMFILES") (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
	;;
Linux) os=linux ;;
*) os=other ;;
esac
here=false
if [ "${1:-}" = --here ]; then # the Linux half, started by Windows inside WSL
	here=true
	shift
fi
everything=true
if [ $# -gt 0 ]; then
	everything=false
	jobs=("$@")
else
	case $os in
	windows) jobs=(format windows-x64 windows-arm64 linux-gcc linux-clang asan-ubsan tsan clang-tidy) ;;
	linux) jobs=(format linux-gcc linux-clang asan-ubsan tsan clang-tidy) ;;
	*) jobs=(format) ;;
	esac
fi

# Tests run four at a time, while other jobs still build: a test that fails because of the load gets one more try on
# its own, and the log says so.
runTests()
{
	ctest --test-dir "$1" -C "$2" --output-on-failure --timeout 300 --no-tests=error -j 4 && return
	echo "=== Running the failed tests again, one at a time"
	ctest --test-dir "$1" -C "$2" --output-on-failure --timeout 300 --rerun-failed
}

launcher=""
command -v sccache > /dev/null && launcher="-DCMAKE_C_COMPILER_LAUNCHER=sccache -DCMAKE_CXX_COMPILER_LAUNCHER=sccache"

# Windows: a job runs its commands in a developer environment of the given architecture (vcvarsall's argument).
vsDev()
{
	local arch=$1 script vs
	shift
	vs=$("$vswhere" -latest -property installationPath | tr -d '\r')
	script="$logs/$BASHPID.cmd"
	printf '@call "%s\\VC\\Auxiliary\\Build\\vcvarsall.bat" %s >nul || exit /b 1\r\n' "$vs" "$arch" > "$script"
	for command in "$@"; do
		printf '%s || exit /b 1\r\n' "$command" >> "$script"
	done
	cmd //c "$(cygpath -w "$script")"
	local status=$?
	rm -f "$script"
	return $status
}

job()
{
	case $1 in
	format)
		sh tools/check-spdx.sh || return 1
		local format=${CLANG_FORMAT:-clang-format}
		if [ $os = windows ] && ! command -v "$format" >/dev/null; then
			format="$("$vswhere" -latest -property installationPath | tr -d '\r')/VC/Tools/Llvm/x64/bin/clang-format.exe"
		fi
		"$format" --version
		git ls-files '*.cpp' '*.h' | xargs "$format" --dry-run --Werror || return 1
		if [ -f .git/info/private-keywords ] && git rev-parse -q --verify origin/main >/dev/null; then
			sh tools/keyword-scan.sh .git/info/private-keywords "$(git merge-base origin/main HEAD)" HEAD || return 1
		fi
		;;
	windows-x64)
		vsDev x64 "cmake --preset windows-x64-ninja $launcher" \
			"cmake --build --preset windows-x64-ninja --config Release" &&
			runTests build/windows-x64-ninja Release
		;;
	windows-arm64)
		vsDev x64_arm64 \
			"cmake --preset windows-arm64-ninja -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=ARM64 $launcher" \
			"cmake --build --preset windows-arm64-ninja --config Release"
		;;
	linux-gcc | linux-clang)
		cmake --preset "$1" -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache &&
			cmake --build --preset "$1" --config Release && runTests "build/$1" Release
		;;
	asan-ubsan | tsan)
		local sanitizers=thread
		[ "$1" = asan-ubsan ] && sanitizers=address,undefined
		export ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
		export TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:suppressions=$root/tools/tsan-suppressions.txt
		cmake --preset linux-clang -B "build/$1" -DGNUBASE_SANITIZERS=$sanitizers -DCMAKE_C_COMPILER_LAUNCHER=ccache \
			-DCMAKE_CXX_COMPILER_LAUNCHER=ccache && cmake --build "build/$1" --target gnubase-tests &&
			runTests "build/$1" Debug
		;;
	clang-tidy)
		cmake --preset linux-clang -B build/clang-tidy -DGNUBASE_CLANG_TIDY=ON -DCMAKE_C_COMPILER_LAUNCHER=ccache \
			-DCMAKE_CXX_COMPILER_LAUNCHER=ccache && cmake --build build/clang-tidy
		;;
	*)
		echo "Unknown job $1" >&2
		return 2
		;;
	esac
}

# Runs the jobs in parallel, each into its log, and prints a line per job when it ends.
runJobs()
{
	local name pids=() start=$SECONDS failed=0
	for name in "$@"; do
		(
			began=$SECONDS
			job "$name" > "$logs/$name.log" 2>&1
			status=$?
			result=passed
			[ $status -eq 0 ] || result=FAILED
			grep -q '^=== Running the failed tests again' "$logs/$name.log" && [ $status -eq 0 ] &&
				result="passed (some tests only on their second try, see the log)"
			printf '%-14s %s after %d:%02d min\n' "$name" "$result" $(((SECONDS - began) / 60)) $(((SECONDS - began) % 60))
			exit $status
		) &
		pids+=($!)
	done
	for pid in "${pids[@]}"; do
		wait "$pid" || failed=1
	done
	return $failed
}

linuxJobs=() otherJobs=()
for name in "${jobs[@]}"; do
	case $name in
	linux-* | asan-ubsan | tsan | clang-tidy) linuxJobs+=("$name") ;;
	*) otherJobs+=("$name") ;;
	esac
done

if $here || [ $os != windows ]; then
	[ $os = linux ] && [ "$(id -u)" = 0 ] && sysctl -qw vm.mmap_rnd_bits=28 2>/dev/null # as in CI, for ASan and TSan
	export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-20G}
	export CCACHE_BASEDIR=$root # paths relative to the checkout, so clones of other checkouts share the cache
	# Vulkan only through Mesa's software rasterizer, as on CI's runners, which have no GPU: a real GPU's driver makes
	# the window tests depend on the machine, and AMD's leaks under ASan.
	lavapipe=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json 2>/dev/null | head -n 1)
	[ -n "$lavapipe" ] && export VK_DRIVER_FILES=$lavapipe
	runJobs "${jobs[@]}"
	exit
fi

# Windows: the Linux jobs go to WSL (see --wsl above). The Windows jobs build this folder, so its submodules move to
# the commits HEAD records, as in CI's checkout; after a merge they often lag behind, and a new worktree has none. The
# test audio comes from Git LFS, which a new worktree has only as pointers; WSL's clone then takes it from this
# repository.
if git submodule status --recursive | grep -q '^+' || git submodule status | grep -q '^-'; then
	echo "Moving submodules to the commits HEAD records."
	git submodule update -q --init --recursive && git submodule update -q --init --checkout ext/vst3sdk &&
		git -C ext/vst3sdk submodule update -q --init base cmake pluginterfaces public.sdk || exit 2
fi
git lfs pull --include "tests/data/**,tests/golden/**,resources/fonts/**" || exit 2
[ -z "$(git status --porcelain --untracked-files=no)" ] ||
	echo "Note: uncommitted changes are not checked, only HEAD $(git rev-parse --short HEAD)." >&2
started=$SECONDS
status=0
commit=$(git rev-parse HEAD)
rm -f "$logs/passed"
if [ ${#linuxJobs[@]} -gt 0 ]; then
	wslPath() { cygpath -m "$1" | sed -E 's|^([A-Za-z]):|/mnt/\L\1|'; }
	source=$(wslPath "$root")
	MSYS_NO_PATHCONV=1 wsl.exe -d "${GNUBASE_WSL:-Ubuntu}" -- bash "$source/tools/ci-local.sh" --wsl "$source" \
		"$(wslPath "$(git rev-parse --path-format=absolute --git-common-dir)")" "$(git rev-parse HEAD)" \
		"${linuxJobs[@]}" &
	wslPid=$!
fi
if [ ${#otherJobs[@]} -gt 0 ]; then
	runJobs "${otherJobs[@]}" || status=1
fi
if [ ${#linuxJobs[@]} -gt 0 ]; then
	wait $wslPid || status=1
fi
if [ $status -eq 0 ] && $everything; then
	echo "$commit" > "$logs/passed"
fi
echo "All jobs took $(((SECONDS - started) / 60)):$(printf %02d $(((SECONDS - started) % 60))) min; logs in build/ci-local."
exit $status
