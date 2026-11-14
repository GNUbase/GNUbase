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
# Both databases list a.cpp and b.cpp; GCC's (Linux) compiles with "true".
for file in a b; do
	printf '{"directory": "%s", "file": "%s/src/%s.cpp", "command": "true -o x"}\n' "$repo" "$repo" "$file"
done | paste -s -d, - | sed 's/.*/[&]/' | tee "$repo/build/tidy/compile_commands.json" > "$repo/build/tidy-gcc/compile_commands.json"
echo "GNUBASE_LTO:BOOL=OFF" > "$repo/build/tidy-gcc/CMakeCache.txt"
git -C "$repo" init -q && git -C "$repo" add . && git -C "$repo" commit -q -m base || exit 1

# a.cpp: line 2 changed, and lines 16-18 added after line 15, as a braced "else if" in the middle of a file would be.
# b.cpp: lines 10-12 deleted. c.cpp: deleted.
sed -i -e '2s/.*/int changed;/' -e '15a\
else if (y)\
{\
}' "$repo/src/a.cpp"
sed -i '10,12d' "$repo/src/b.cpp"
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

check "without findings the script passes" 0
[ "$(wc -l < "$tmp/filters" | tr -d ' ')" -eq 2 ] && echo "ok: clang-tidy checks both changed files" ||
	{ echo "FAILED: clang-tidy checks both changed files"; failures=$((failures + 1)); }
expect "a change in the first lines starts at 1, a block 5 lines on both sides" -F 'a.cpp","lines":[[1,7],[11,23]]}'
expect "a deletion gets the 5 lines on both sides of it" -F 'b.cpp","lines":[[4,14]]}'
expect "a deleted file gets no entry" -v -F 'c.cpp'

touch "$tmp/finding"
check "a finding next to a change fails the script" 1
grep -q "readability-inconsistent-ifelse-braces" "$tmp/output" && echo "ok: the finding is printed" ||
	{ echo "FAILED: the finding is printed"; failures=$((failures + 1)); }

[ "$failures" -eq 0 ] && echo "all tidy-changed tests passed" && exit 0
echo "$failures tidy-changed test(s) failed"
exit 1
