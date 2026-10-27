#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests tools/check-spdx.sh in a temporary repository. Run by ctest; usage: spdx_check_tests.sh <source-dir>
set -u
src=$(cd "${1:?usage: $0 <source-dir>}" && pwd) || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT
export GIT_CONFIG_GLOBAL="$tmp/gitconfig" GIT_CONFIG_NOSYSTEM=1
: > "$GIT_CONFIG_GLOBAL"

failures=0
repo="$tmp/repo"
header="SPDX-License-Identifier: GPL-3.0-or-later"
git init -q "$repo"
mkdir -p "$repo/tools"
cp "$src/tools/check-spdx.sh" "$repo/tools/check-spdx.sh"
git -C "$repo" add tools

# add <path> <content>: writes a file with the given content and stages it.
add()
{
	mkdir -p "$(dirname "$repo/$1")"
	printf '%s\n' "$2" > "$repo/$1"
	git -C "$repo" add "$1"
}

# check <description> <expected exit status> <expected output file>: runs the check from a subfolder.
check()
{
	(cd "$repo/src" && sh ../tools/check-spdx.sh) > "$tmp/output" 2> "$tmp/errors"
	status=$?
	if [ "$status" -eq "$2" ] && diff "$3" "$tmp/output" > "$tmp/diff"; then
		echo "ok: $1"
	else
		echo "FAILED: $1 (exit status $status, expected $2)"
		sed 's/^/    /' "$tmp/diff" "$tmp/errors"
		failures=$((failures + 1))
	fi
}

# Files with the header, in the forms the repository uses.
add .gitattributes "# $header
*.bin filter=lfs diff=lfs merge=lfs -text"
add src/good.cpp "// $header
int good;"
add src/script.sh "#!/bin/sh
# $header"
add resources/icon.svg "<?xml version=\"1.0\"?>
<!-- $header -->
<svg/>"
printf '// %s\r\nint crlf;\r\n' "$header" > "$repo/src/crlf.h"
git -C "$repo" add src/crlf.h

# Exempt files.
add README.md "# No header in Markdown"
add CMakePresets.json "{}"
add .gitmodules "[submodule \"ext/lib\"]"
add LICENSE "License text"
add resources/fonts/Some/OFL.txt "Font license text"
add ext/json/LICENSE.MIT "License text"
add art/logo.bin "Git LFS file"
git -C "$repo" update-index --add --cacheinfo 160000,0123456789abcdef0123456789abcdef01234567,ext/lib
printf 'untracked\n' > "$repo/src/untracked.cpp"

# Files without the header.
add notes.txt "No header"
add src/late.cpp "// one
// two
// three
// $header"
add src/missing.h "#pragma once"
add src/other.cpp "// SPDX-License-Identifier: MIT"
add tools/hook "#!/bin/sh
exit 0"

printf '%s\n' notes.txt src/late.cpp src/missing.h src/other.cpp tools/hook > "$tmp/expected"
check "files without the header are listed, and only those" 1 "$tmp/expected"
if grep -q "$header" "$tmp/errors"; then
	echo "ok: the message names the header"
else
	echo "FAILED: the message names the header"
	failures=$((failures + 1))
fi

git -C "$repo" rm -q --cached notes.txt src/late.cpp src/missing.h src/other.cpp tools/hook
: > "$tmp/expected"
check "a repository where every file has the header passes" 0 "$tmp/expected"

[ "$failures" -eq 0 ] && echo "all license header check tests passed" && exit 0
echo "$failures license header check test(s) failed"
exit 1
