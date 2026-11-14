#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests the line filter tools/tidy-changed.sh gives clang-tidy, with a stub clang-tidy that logs it, in a temporary
# repository with prepared compile databases. Run by ctest; usage: tidy_changed_tests.sh <source-dir>
set -u
src=$(cd "${1:?usage: $0 <source-dir>}" && pwd) || exit 1
case "$(uname -s)" in
Linux | MINGW* | MSYS* | CYGWIN*) ;;
*)
	echo "skipped: tidy-changed.sh runs on Windows and Linux only"
	exit 0
	;;
esac
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

export GIT_CONFIG_GLOBAL="$tmp/gitconfig" GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=Test GIT_AUTHOR_EMAIL=test@example.com
export GIT_COMMITTER_NAME=Test GIT_COMMITTER_EMAIL=test@example.com
: > "$GIT_CONFIG_GLOBAL"

# The stub logs its line filter and reports a finding, failing, when $tmp/finding exists.
cat > "$tmp/clang-tidy" << EOF
#!/bin/sh
[ "\$1" = --version ] && echo "clang-tidy version 22.1.0" && exit 0
for arg in "\$@"; do
	case "\$arg" in --line-filter=*) echo "\${arg#--line-filter=}" >> "$tmp/filters" ;; esac
done
[ -e "$tmp/finding" ] || exit 0
echo "src/a.cpp:13:2: error: statement should have braces [readability-inconsistent-ifelse-braces]"
exit 1
EOF
chmod +x "$tmp/clang-tidy"
export CLANG_TIDY="$tmp/clang-tidy"

repo=$tmp/repo
mkdir -p "$repo/src" "$repo/build/tidy" "$repo/build/tidy-gcc"
lines() { i=1; while [ $i -le 30 ]; do echo "int v$i;"; i=$((i + 1)); done; }
for file in a b c; do lines > "$repo/src/$file.cpp"; done
printf 'build/\n' > "$repo/.gitignore"
# Both databases list a.cpp and b.cpp, compiled on Linux by stubs for Clang (build/tidy) and GCC (build/tidy-gcc)
# that report an error, failing, when $tmp/<compiler>-error exists.
for compiler in clang:tidy gcc:tidy-gcc; do
	printf '#!/bin/sh\n[ -e "$0-error" ] || exit 0\necho "a.cpp:1:1: error: %s failed"\nexit 1\n' "${compiler%:*}" \
		> "$tmp/${compiler%:*}"
	chmod +x "$tmp/${compiler%:*}"
	for file in a b; do
		printf '{"directory": "%s", "file": "%s/src/%s.cpp", "command": "%s -o x"}\n' "$repo" "$repo" "$file" \
			"$tmp/${compiler%:*}"
	done | paste -s -d, - | sed 's/.*/[&]/' > "$repo/build/${compiler#*:}/compile_commands.json"
done
echo "GNUBASE_LTO:BOOL=OFF" > "$repo/build/tidy-gcc/CMakeCache.txt"
git -C "$repo" init -q && git -C "$repo" add . && git -C "$repo" commit -q -m base || exit 1

# a.cpp: line 2 changed, and lines 16-18 added after line 15, as a braced "else if" in the middle of a file would be.
# b.cpp: a line that starts with "++ " added after line 1 (it reads "+++ " in the diff, like a file header), lines
# 10-12 deleted. c.cpp: deleted.
sed -i -e '2s/.*/int changed;/' -e '15a\
else if (y)\
{\
}' "$repo/src/a.cpp"
sed -i -e '10,12d' -e '1a\
++ b/x;' "$repo/src/b.cpp"
git -C "$repo" rm -q src/c.cpp
git -C "$repo" commit -q -am change || exit 1

failures=0
# check <description> <expected exit status>: runs the script on the branch since the base commit.
check()
{
	: > "$tmp/filters"
	(cd "$repo" && bash "$src/tools/tidy-changed.sh" HEAD~1) > "$tmp/output" 2>&1
	status=$?
	if [ "$status" -eq "$2" ]; then
		echo "ok: $1"
	else
		echo "FAILED: $1 (exit status $status, expected $2)"
		sed 's/^/    /' "$tmp/output"
		failures=$((failures + 1))
	fi
}
# expect <description> <grep options and pattern...>: looks for the pattern in the logged line filter.
expect()
{
	description=$1
	shift
	if head -n 1 "$tmp/filters" | grep -q "$@"; then
		echo "ok: $description"
	else
		echo "FAILED: $description"
		sed 's/^/    /' "$tmp/filters"
		failures=$((failures + 1))
	fi
}

# printed <description> <text>: looks for the text in the script's output.
printed()
{
	if grep -q -F "$2" "$tmp/output"; then
		echo "ok: $1"
	else
		echo "FAILED: $1"
		sed 's/^/    /' "$tmp/output"
		failures=$((failures + 1))
	fi
}

check "without findings the script passes" 0
[ "$(wc -l < "$tmp/filters" | tr -d ' ')" -eq 2 ] && echo "ok: clang-tidy checks both changed files" ||
	{ echo "FAILED: clang-tidy checks both changed files"; failures=$((failures + 1)); }
expect "a change in the first lines starts at 1, a block 5 lines on both sides" -F 'a.cpp","lines":[[1,7],[11,23]]}'
expect "a deletion gets the 5 lines on both sides of it, after an added line that reads like a file header" \
	-F 'b.cpp","lines":[[1,7],[5,15]]}'
expect "a deleted file gets no entry" -v -F 'c.cpp'
if [ "$(uname -s)" = Linux ]; then
	printed "Clang compiles the changed files" "Clang compiles them."
	printed "GCC compiles the changed files" "GCC compiles them."
	for compiler in clang gcc; do
		touch "$tmp/$compiler-error"
		check "an error from $compiler fails the script" 1
		printed "the error from $compiler is printed" "error: $compiler failed"
		rm "$tmp/$compiler-error"
	done
fi

touch "$tmp/finding"
check "a finding next to a change fails the script" 1
printed "the finding is printed" "readability-inconsistent-ifelse-braces"
rm "$tmp/finding"

# A branch that only deletes a file has nothing to check.
git -C "$repo" checkout -q --detach HEAD~1 && git -C "$repo" rm -q src/c.cpp && git -C "$repo" commit -q -m delete ||
	exit 1
check "only a deleted file passes" 0
printed "only a deleted file says so" "No changed C++ lines to check."
[ -s "$tmp/filters" ] && { echo "FAILED: only a deleted file runs no clang-tidy"; failures=$((failures + 1)); } ||
	echo "ok: only a deleted file runs no clang-tidy"

[ "$failures" -eq 0 ] && echo "all tidy-changed tests passed" && exit 0
echo "$failures tidy-changed test(s) failed"
exit 1
