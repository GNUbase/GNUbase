#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests tools/install-git-hooks.sh, the hooks in tools/git-hooks and tools/keyword-scan.sh in temporary repositories
# with a made-up keyword list. Run by ctest; usage: keyword_guard_tests.sh <source-dir>
set -u
src=$(cd "${1:?usage: $0 <source-dir>}" && pwd) || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# Isolate from the user's and the system's Git configuration (hooks paths, templates, signing, line endings).
export GIT_CONFIG_GLOBAL="$tmp/gitconfig" GIT_CONFIG_NOSYSTEM=1
export GIT_AUTHOR_NAME=Test GIT_AUTHOR_EMAIL=test@example.com
export GIT_COMMITTER_NAME=Test GIT_COMMITTER_EMAIL=test@example.com
printf '[init]\n\tdefaultBranch = main\n' > "$GIT_CONFIG_GLOBAL"

failures=0
output="$tmp/output"

# check <description> <expected exit status> <command...>
check()
{
	description=$1 expected=$2
	shift 2
	"$@" > "$output" 2>&1
	status=$?
	if [ "$status" -eq "$expected" ]; then
		echo "ok: $description"
	else
		echo "FAILED: $description (exit status $status, expected $expected)"
		sed 's/^/    /' "$output"
		failures=$((failures + 1))
	fi
}

# expect <description> <grep arguments...>: the last command's output must match.
expect()
{
	description=$1
	shift
	if grep -q "$@" "$output"; then
		echo "ok: $description"
	else
		echo "FAILED: $description"
		sed 's/^/    /' "$output"
		failures=$((failures + 1))
	fi
}

# reject <description> <grep arguments...>: the last command's output must not match.
reject()
{
	description=$1
	shift
	if grep -q "$@" "$output"; then
		echo "FAILED: $description"
		failures=$((failures + 1))
	else
		echo "ok: $description"
	fi
}

# In the given repository, stages a file with the given content and commits it with the given message.
commit_file()
{
	(cd "$1" && mkdir -p "$(dirname "$2")" && printf '%s\n' "$3" > "$2" && git add "$2" && git commit -q -m "$4")
}

# Drops whatever a blocked commit left staged.
reset_index()
{
	(cd "$1" && git reset -q --hard HEAD && git clean -q -fd)
}

# Keywords: case-insensitive extended regexes, mostly CRLF as written on Windows, with an empty line. "Zorblaxer" ends
# in r on an LF line: POSIX sed doesn't define \r, and a sed that reads "s/\r$//" as "s/r$//" would cut it short.
# "bl[o0]rp" may appear in the content of files in two folders, as an SDK's namespace would.
keywords="$tmp/keywords"
printf 'wib+le\r\n\r\nquux ?frob\r\nbl[o0]rp\tvendor/sdk/ src/adapter\r\nZorblaxer\n' > "$keywords"

# A repository with the tools, and a fresh clone of it.
origin="$tmp/origin"
git init -q "$origin"
cp -R "$src/tools" "$origin/tools"
git -C "$origin" add tools
commit_file "$origin" README.md "Test repository" "Add tools"
clone="$tmp/clone"
git clone -q "$origin" "$clone"
list="$clone/.git/info/private-keywords"

echo "--- hooks in a contributor clone (no keyword list)"
check "install script runs" 0 sh -c "cd '$clone' && sh tools/install-git-hooks.sh"
check "core.hooksPath points to tools/git-hooks" 0 test "$(git -C "$clone" config core.hooksPath)" = tools/git-hooks
check "without a list, commits are not scanned" 0 commit_file "$clone" notes.txt "Zorblaxer" "Contributor commit"
# The hooks scan every tracked file, not just the changed ones, so the keyword has to go before the list arrives.
(cd "$clone" && git rm -q notes.txt && git commit -q -m "Remove notes")

echo "--- hooks in a maintainer clone"
cp "$keywords" "$list"
check "install script runs with a list" 0 sh -c "cd '$clone' && sh tools/install-git-hooks.sh"
check "a clone with a list requires it" 0 test "$(git -C "$clone" config --bool gnubase.requireKeywordList)" = true
check "a clean commit passes" 0 commit_file "$clone" clean.txt "Nothing to see" "Clean commit"
check "a keyword in staged content is blocked, in any case" 1 commit_file "$clone" a.txt "says ZORBLAXER" "Add a"
reset_index "$clone"
check "CRs are stripped without touching a trailing r" 0 commit_file "$clone" b.txt "says Zorblaxe only" "Add b"
check "keywords are regexes" 1 commit_file "$clone" c.txt "a wibbble" "Add c"
reset_index "$clone"
check "a keyword in a file name is blocked" 1 commit_file "$clone" docs/QuuxFrob.txt "harmless" "Add docs"
reset_index "$clone"
check "a keyword in the commit message is blocked" 1 commit_file "$clone" d.txt "harmless" "Mention quux frob"
reset_index "$clone"
check "a keyword passes in the folders listed with it" 0 commit_file "$clone" src/adapter/x.cpp "blorp::Thing" "Add x"
check "in each of them" 0 commit_file "$clone" vendor/sdk/y.h "BL0RP" "Add y"
check "but not outside them" 1 commit_file "$clone" src/adapterish/z.cpp "blorp" "Add z"
reset_index "$clone"
check "other keywords stay blocked in those folders" 1 commit_file "$clone" src/adapter/w.cpp "wibble" "Add w"
reset_index "$clone"
check "file names in those folders are still scanned" 1 commit_file "$clone" src/adapter/blorp.cpp "fine" "Add v"
reset_index "$clone"
check "commit messages are still scanned" 1 commit_file "$clone" src/adapter/u.cpp "fine" "About blorp"
reset_index "$clone"
mkdir -p "$clone/sub"
check "commits from a subfolder are scanned" 1 commit_file "$clone/sub" e.txt "Zorblaxer" "Add e"
reset_index "$clone"
git -C "$clone" worktree add -q "$tmp/worktree" 2> /dev/null
check "a clean commit in a worktree passes" 0 commit_file "$tmp/worktree" f.txt "fine" "Worktree commit"
check "a keyword in a worktree is blocked" 1 commit_file "$tmp/worktree" g.txt "wibble" "Worktree keyword"
reset_index "$tmp/worktree"
mv "$list" "$tmp/keywords.away"
check "a maintainer clone without its list blocks commits" 1 commit_file "$clone" h.txt "fine" "Add h"
expect "the message names the missing list" -e "keyword list .* missing"
reset_index "$clone"
# Each hook on its own, since either one blocking hides the other one passing.
printf 'Fine message\n' > "$tmp/message"
check "pre-commit alone blocks without the list" 1 sh -c "cd '$clone' && sh tools/git-hooks/pre-commit"
check "commit-msg alone blocks without the list" 1 sh -c "cd '$clone' && sh tools/git-hooks/commit-msg '$tmp/message'"
mv "$tmp/keywords.away" "$list"

echo "--- Git LFS hooks"
# Stand-ins for git and git-lfs record how the hooks call them.
sh_path=$(command -v sh)
mkdir -p "$tmp/stubs" "$tmp/no-lfs"
printf '#!/bin/sh\necho "git $*" >> "%s"\n' "$tmp/git-calls" | tee "$tmp/stubs/git" > "$tmp/no-lfs/git"
printf '#!/bin/sh\n' > "$tmp/stubs/git-lfs"
chmod +x "$tmp/stubs/git" "$tmp/stubs/git-lfs" "$tmp/no-lfs/git"
run_lfs_hooks()
{
	: > "$tmp/git-calls"
	for hook in pre-push post-checkout post-commit post-merge; do
		PATH="$tmp/stubs:$PATH" "$sh_path" "$clone/tools/git-hooks/$hook" first second || return 1
	done
	printf 'git lfs %s first second\n' pre-push post-checkout post-commit post-merge | cmp -s - "$tmp/git-calls"
}
check "each LFS hook runs its git-lfs counterpart" 0 run_lfs_hooks
check "pre-push refuses to push without git-lfs" 2 \
	env PATH="$tmp/no-lfs" "$sh_path" "$clone/tools/git-hooks/pre-push" origin url
if command -v git-lfs > /dev/null 2>&1; then
	check "the install script sets up Git LFS" 0 test -n "$(git -C "$clone" config filter.lfs.process)"
else
	echo "skipped: git-lfs is not installed, so the install script cannot set it up"
fi

echo "--- CI keyword scan"
scan()
{
	(cd "$origin" && sh tools/keyword-scan.sh "$@")
}
base=$(git -C "$origin" rev-parse HEAD)
commit_file "$origin" clean.txt "nothing here" "Clean change"
check "a clean range passes" 0 scan "$keywords" "$base" HEAD
commit_file "$origin" src/a.txt "first line
has a ZorBlaxer in it" "Add a"
check "a keyword in an added line is found" 1 scan "$keywords" "$base" HEAD
expect "the finding names file and line" -F "src/a.txt:2"
reject "the matching text is not printed" -i zorblaxer
base=$(git -C "$origin" rev-parse HEAD)
commit_file "$origin" src/a.txt "first line" "Remove the keyword"
check "removing a keyword passes" 0 scan "$keywords" "$base" HEAD
base=$(git -C "$origin" rev-parse HEAD)
commit_file "$origin" docs/quuxfrob.md "harmless" "Add docs"
check "a keyword in a file name is found" 1 scan "$keywords" "$base" HEAD
expect "the finding names the file" -F "docs/quuxfrob.md"
base=$(git -C "$origin" rev-parse HEAD)
commit_file "$origin" e.txt "harmless" "Wibble"
check "a keyword in a commit message is found" 1 scan "$keywords" "$base" HEAD
expect "the finding names the commit" -F "$(git -C "$origin" rev-parse HEAD)"
base=$(git -C "$origin" rev-parse HEAD)
printf 'Pull request about quuxfrob\n' > "$tmp/pull-request-title"
check "a keyword in an extra text file is found" 1 scan "$keywords" "$base" HEAD "$tmp/pull-request-title"
expect "the finding names the text file" -F "pull-request-title"
base=$(git -C "$origin" rev-parse HEAD)
commit_file "$origin" src/adapter/x.cpp "blorp::Thing" "Add the adapter"
check "a keyword passes in the folders listed with it" 0 scan "$keywords" "$base" HEAD
commit_file "$origin" src/adapterish/x.cpp "first line
blorp" "Add another"
check "but is found outside them" 1 scan "$keywords" "$base" HEAD
expect "the finding names file and line" -F "src/adapterish/x.cpp:2"
reject "the allowed line is not reported" -F "src/adapter/x.cpp"
base=$(git -C "$origin" rev-parse HEAD)
commit_file "$origin" src/adapter/y.cpp "wibble" "Add y"
check "other keywords are found in those folders" 1 scan "$keywords" "$base" HEAD
expect "the finding names file and line" -F "src/adapter/y.cpp:1"
base=$(git -C "$origin" rev-parse HEAD)
commit_file "$origin" src/adapter/z.cpp "fine" "About blorp"
check "commit messages are still scanned" 1 scan "$keywords" "$base" HEAD
check "a missing list is an error" 2 scan "$tmp/missing" "$base" HEAD
check "an unknown commit is an error" 2 scan "$keywords" 0123456789abcdef0123456789abcdef01234567 HEAD

[ "$failures" -eq 0 ] && echo "all keyword guard tests passed" && exit 0
echo "$failures keyword guard test(s) failed"
exit 1
