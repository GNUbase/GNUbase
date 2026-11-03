#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests tools/clang-tidy-cache.sh with stub clang-tidy and compiler scripts that log their calls. Run by ctest; usage:
# clang_tidy_cache_tests.sh <source-dir>
set -u
src=$(cd "${1:?usage: $0 <source-dir>}" && pwd) || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# The stub clang-tidy prints the version and the configuration from files, and otherwise logs the call and exits with
# the status in $tmp/status. The stub compiler writes the source and the header for -frewrite-includes, as clang does
# with the text of what a file includes, or fails when $tmp/nokey exists.
cat > "$tmp/clang-tidy" << EOF
#!/bin/sh
case "\$1" in
--version) cat "$tmp/version" ;;
--dump-config) cat "$tmp/config" ;;
*)
	echo checked >> "$tmp/calls"
	status=\$(cat "$tmp/status")
	[ "\$status" -eq 0 ] || echo "source.cpp:1:1: error: a finding [some-check]"
	exit "\$status"
	;;
esac
EOF
cat > "$tmp/clang++" << EOF
#!/bin/sh
[ -e "$tmp/nokey" ] && exit 1
for arg in "\$@"; do
	[ "\$arg" = -frewrite-includes ] && cat "$tmp/source.cpp" "$tmp/header.h"
done
exit 0
EOF
chmod +x "$tmp/clang-tidy" "$tmp/clang++"
echo "clang-tidy version 22" > "$tmp/version"
echo "Checks: '-*,readability-*'" > "$tmp/config"
echo "int f();" > "$tmp/source.cpp"
echo "#pragma once" > "$tmp/header.h"
echo 0 > "$tmp/status"
: > "$tmp/calls"
options=--warnings-as-errors=*

failures=0
# check <description> <expected exit status> <expected clang-tidy calls so far>: runs the wrapper as CMake does.
check()
{
	bash "$src/tools/clang-tidy-cache.sh" "$tmp/cache" "$tmp/clang-tidy" "$options" "$tmp/source.cpp" -- \
		"$tmp/clang++" -c "$tmp/source.cpp" -o "$tmp/source.o" > "$tmp/output" 2>&1
	status=$?
	calls=$(wc -l < "$tmp/calls" | tr -d ' ')
	if [ "$status" -eq "$2" ] && [ "$calls" -eq "$3" ]; then
		echo "ok: $1"
	else
		echo "FAILED: $1 (exit status $status, expected $2; clang-tidy checked $calls times, expected $3)"
		sed 's/^/    /' "$tmp/output"
		failures=$((failures + 1))
	fi
}
entries() { find "$tmp/cache" -type f 2> /dev/null | wc -l | tr -d ' '; }

check "the first run checks the file" 0 1
check "a second run on the same input is reused" 0 1
[ "$(entries)" -eq 1 ] && echo "ok: a pass is stored" || { echo "FAILED: a pass is stored"; failures=$((failures + 1)); }

echo "// a comment" >> "$tmp/header.h"
check "a comment changed in an included header checks it again" 0 2
check "and the result is reused after that" 0 2
echo "CheckOptions: {}" >> "$tmp/config"
check "a changed configuration checks it again" 0 3
echo "clang-tidy version 23" > "$tmp/version"
check "another clang-tidy version checks it again" 0 4
options=--warnings-as-errors=readability-*
check "another command line checks it again" 0 5

stored=$(entries)
echo 1 > "$tmp/status"
echo "int g();" >> "$tmp/source.cpp"
check "a finding fails" 1 6
check "and fails again on the next run" 1 7
[ "$(entries)" -eq "$stored" ] && echo "ok: a failure is not stored" ||
	{ echo "FAILED: a failure is not stored"; failures=$((failures + 1)); }
if grep -q "a finding" "$tmp/output"; then
	echo "ok: the finding is printed"
else
	echo "FAILED: the finding is printed"
	failures=$((failures + 1))
fi

touch "$tmp/nokey"
echo 0 > "$tmp/status"
check "without a key clang-tidy runs" 0 8
check "and runs again, as nothing is stored" 0 9
echo 3 > "$tmp/status"
check "without a key clang-tidy's exit status is passed on" 3 10
[ "$(entries)" -eq "$stored" ] && echo "ok: nothing is stored without a key" ||
	{ echo "FAILED: nothing is stored without a key"; failures=$((failures + 1)); }

[ "$failures" -eq 0 ] && echo "all clang-tidy cache tests passed" && exit 0
echo "$failures clang-tidy cache test(s) failed"
exit 1
