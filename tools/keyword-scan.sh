#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Scans the commits in base..head for private keywords: lines they add, names of files they add or change, and their
# commit messages. Extra text files are scanned too; CI passes the pull request's title and body, which become the
# squash commit's message. .github/workflows/keyword-scan.yml runs this; locally, tools/git-hooks guard each commit.
# Usage: tools/keyword-scan.sh <keyword-list> <base> <head> [text-file...]
#
# The list has one keyword per line, as case-insensitive extended regexes. A keyword may be followed by a tab and
# folders, separated by spaces, whose file content may contain it, such as an SDK's namespace in its adapter; file
# names, commit messages and text files never may. Findings are printed as locations (file and line, file name,
# commit, text file), never as the matching lines, because CI logs are public. Binary files are not scanned.
# Exit status: 0 nothing found, 1 keywords found, 2 error.
set -u
[ $# -ge 3 ] || {
	echo "Usage: $0 <keyword-list> <base> <head> [text-file...]" >&2
	exit 2
}
list=$1 base=$2 head=$3
shift 3

fail()
{
	echo "Keyword scan failed: $*" >&2
	exit 2
}

tmp=$(mktemp -d) || fail "cannot create a temporary folder"
trap 'rm -rf "$tmp"' EXIT
tr -d '\r' < "$list" | sed -e '/^$/d' > "$tmp/keywords" || fail "cannot read the keyword list $list"
pattern=$(cut -f1 "$tmp/keywords" | paste -sd '|' -)
[ -n "$pattern" ] || fail "keyword list $list missing or empty"
tab=$(printf '\t')
strict=$(grep -v "$tab" "$tmp/keywords" | paste -sd '|' -)
found=0

# scan <label> <file> <pattern> [grep options]: grep's status, with errors turned into a failed scan.
scan()
{
	label=$1 file=$2 keys=$3
	shift 3
	grep -a -i -E "$@" -e "$keys" "$file"
	status=$?
	[ $status -le 1 ] || fail "cannot scan $label"
	return $status
}

# added <pattern> [pathspec...]: prints the locations of the lines that base...head adds to the paths and that contain
# a keyword from the pattern. Added lines go to one file and their locations to another, line by line, so that only
# locations get printed. Explicit prefixes and no external diff or text conversion keep user configuration out of the
# output format.
added()
{
	keys=$1
	shift
	git -c core.quotePath=false diff --no-color --no-ext-diff --no-textconv --src-prefix=a/ --dst-prefix=b/ -U0 \
		"$base...$head" -- "$@" > "$tmp/diff" || fail "git diff $base...$head"
	awk -v added="$tmp/added" -v where="$tmp/where" '
		BEGIN { printf "" > added; printf "" > where }
		/^diff --git / { header = 1; next }
		header && /^\+\+\+ / { file = substr($0, 5); sub(/^b\//, "", file); next }
		/^@@ / { header = 0; split($3, range, ","); line = substr(range[1], 2) + 0; next }
		header { next }
		/^\+/ { print substr($0, 2) > added; print file ":" line > where; line++ }
	' "$tmp/diff" || fail "cannot read the diff"
	scan "added lines" "$tmp/added" "$keys" -n > "$tmp/hits" || return 1
	cut -d: -f1 "$tmp/hits" | awk 'NR == FNR { hit[$1] = 1; next } FNR in hit { print "keyword in " $0 }' - "$tmp/where"
}

# excepted <keyword> <folders>: added only outside the folders that the list allows the keyword in.
excepted()
{
	keyword=$1 folders=$2
	set -- ':/'
	for folder in $folders; do
		set -- "$@" ":(top,exclude)$folder"
	done
	added "$keyword" "$@"
}

if [ -n "$strict" ] && added "$strict"; then
	found=1
fi
grep "$tab" "$tmp/keywords" > "$tmp/exceptions"
[ $? -le 1 ] || fail "cannot read the keyword list $list"
while IFS="$tab" read -r keyword folders; do
	excepted "$keyword" "$folders" && found=1
done < "$tmp/exceptions"

git -c core.quotePath=false diff --name-only --diff-filter=d "$base...$head" > "$tmp/names" ||
	fail "git diff --name-only"
if scan "file names" "$tmp/names" "$pattern" > "$tmp/hits"; then
	found=1
	sed 's/^/keyword in file name /' "$tmp/hits"
fi

git rev-list "$base..$head" > "$tmp/commits" || fail "git rev-list $base..$head"
while IFS= read -r commit; do
	git log -1 --format=%B "$commit" > "$tmp/message" || fail "git log $commit"
	if scan "commit message" "$tmp/message" "$pattern" -q; then
		found=1
		echo "keyword in commit message $commit"
	fi
done < "$tmp/commits"

for text in "$@"; do
	if scan "$text" "$text" "$pattern" -q; then
		found=1
		echo "keyword in $(basename "$text")"
	fi
done

# On stdout like the findings, so that the summary comes last in the CI log.
if [ $found -eq 1 ]; then
	echo "Private keywords found at the locations above. Remove them; see CLAUDE.md, \"Source of truth and naming\"."
	exit 1
fi
echo "No private keywords found (commits scanned: $(wc -l < "$tmp/commits" | tr -d ' '))."
