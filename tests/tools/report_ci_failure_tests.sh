#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Tests tools/report-ci-failure.sh with a stub gh that logs its calls and answers the issue search with $tmp/found.
# Run by ctest; usage: report_ci_failure_tests.sh <source-dir>
set -u
src=$(cd "${1:?usage: $0 <source-dir>}" && pwd) || exit 1
tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

mkdir "$tmp/bin"
cat > "$tmp/bin/gh" << EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$tmp/calls"
[ "\$1 \$2" = "issue list" ] && cat "$tmp/found"
exit 0
EOF
chmod +x "$tmp/bin/gh"
export GITHUB_WORKFLOW="UI tests (macOS)" GITHUB_SERVER_URL=https://github.com GITHUB_REPOSITORY=owner/repo
export GITHUB_RUN_ID=42 GITHUB_SHA=abc123

failures=0
# check <description> <expected call>: runs the script and looks for the call in the stub's log.
check()
{
	: > "$tmp/calls"
	PATH="$tmp/bin:$PATH" sh "$src/tools/report-ci-failure.sh" "UI tests fail on main" > "$tmp/output" 2>&1
	if grep -qF -- "$2" "$tmp/calls"; then
		echo "ok: $1"
	else
		echo "FAILED: $1 (expected the call: $2)"
		sed 's/^/    /' "$tmp/calls" "$tmp/output"
		failures=$((failures + 1))
	fi
}

body="[UI tests (macOS)](https://github.com/owner/repo/actions/runs/42) failed on abc123."
: > "$tmp/found"
check "it searches the open issues by title" "issue list -R owner/repo --state open --search in:title UI tests fail on main"
check "without an open issue it opens one" "issue create -R owner/repo --title UI tests fail on main --body $body"
echo 7 > "$tmp/found"
check "with an open issue it comments on it" "issue comment 7 -R owner/repo --body $body"
if grep -q "issue create" "$tmp/calls"; then
	echo "FAILED: and opens no other"
	failures=$((failures + 1))
else
	echo "ok: and opens no other"
fi

[ "$failures" -eq 0 ] && echo "all CI failure report tests passed" && exit 0
echo "$failures CI failure report test(s) failed"
exit 1
