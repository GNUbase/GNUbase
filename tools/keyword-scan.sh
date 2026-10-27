#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Scans the commits in base..head for private keywords: lines they add, names of files they add or change, and their
# commit messages. Extra text files are scanned too; CI passes the pull request's title and body, which become the
# squash commit's message. .github/workflows/keyword-scan.yml runs this; locally, tools/git-hooks guard each commit.
# Usage: tools/keyword-scan.sh <keyword-list> <base> <head> [text-file...]
#
# The list has one keyword per line, as case-insensitive extended regexes. Findings are printed as locations (file and
# line, file name, commit, text file), never as the matching lines, because CI logs are public. Binary files are not
# scanned. Exit status: 0 nothing found, 1 keywords found, 2 error.
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

pattern=$(tr -d '\r' < "$list" | sed -e '/^$/d' | paste -sd '|' -)
[ -n "$pattern" ] || fail "keyword list $list missing or empty"
tmp=$(mktemp -d) || fail "cannot create a temporary folder"
trap 'rm -rf "$tmp"' EXIT
found=0

# scan <label> <file> [grep options]: grep's status, with errors turned into a failed scan.
scan()
{
	label=$1 file=$2
	shift 2
	grep -a -i -E "$@" -e "$pattern" "$file"
	status=$?
	[ $status -le 1 ] || fail "cannot scan $label"
	return $status
}

# Added lines go to one file and their locations to another, line by line, so that only locations get printed.
# Explicit prefixes and no external diff or text conversion keep user configuration out of the output format.
git -c core.quotePath=false diff --no-color --no-ext-diff --no-textconv --src-prefix=a/ --dst-prefix=b/ -U0 \
	"$base...$head" > "$tmp/diff" || fail "git diff $base...$head"
awk -v added="$tmp/added" -v where="$tmp/where" '
	BEGIN { printf "" > added; printf "" > where }
	/^diff --git / { header = 1; next }
	header && /^\+\+\+ / { file = substr($0, 5); sub(/^b\//, "", file); next }
	/^@@ / { header = 0; split($3, range, ","); line = substr(range[1], 2) + 0; next }
	header { next }
	/^\+/ { print substr($0, 2) > added; print file ":" line > where; line++ }
' "$tmp/diff" || fail "cannot read the diff"
if scan "added lines" "$tmp/added" -n > "$tmp/hits"; then
	found=1
	cut -d: -f1 "$tmp/hits" | awk 'NR == FNR { hit[$1] = 1; next } FNR in hit { print "keyword in " $0 }' - "$tmp/where"
fi

git -c core.quotePath=false diff --name-only --diff-filter=d "$base...$head" > "$tmp/names" ||
	fail "git diff --name-only"
if scan "file names" "$tmp/names" > "$tmp/hits"; then
	found=1
	sed 's/^/keyword in file name /' "$tmp/hits"
fi

git rev-list "$base..$head" > "$tmp/commits" || fail "git rev-list $base..$head"
while IFS= read -r commit; do
	git log -1 --format=%B "$commit" > "$tmp/message" || fail "git log $commit"
	if scan "commit message" "$tmp/message" -q; then
		found=1
		echo "keyword in commit message $commit"
	fi
done < "$tmp/commits"

for text in "$@"; do
	if scan "$text" "$text" -q; then
		found=1
		echo "keyword in $(basename "$text")"
	fi
done

if [ $found -eq 1 ]; then
	echo "Private keywords found. Remove them from the changes and commit messages; see CLAUDE.md." >&2
	exit 1
fi
echo "No private keywords in $(wc -l < "$tmp/commits" | tr -d ' ') commits."
