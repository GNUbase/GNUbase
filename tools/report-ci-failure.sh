#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Reports a failed run of a workflow that pull requests don't run (ui-macos.yml, lto.yml) in an open issue with the
# given title, and opens one when there is none: otherwise only the account that pushed or scheduled it hears of it.
# Runs in GitHub Actions with GH_TOKEN set to a token with issues: write.
# Usage: tools/report-ci-failure.sh <issue title>
set -eu
title=${1:?usage: $0 <issue title>}
body="[$GITHUB_WORKFLOW]($GITHUB_SERVER_URL/$GITHUB_REPOSITORY/actions/runs/$GITHUB_RUN_ID) failed on $GITHUB_SHA."
issue=$(gh issue list -R "$GITHUB_REPOSITORY" --state open --search "in:title $title" --json number,title \
	--jq "map(select(.title == \"$title\"))[0].number // empty")
if [ -n "$issue" ]; then
	gh issue comment "$issue" -R "$GITHUB_REPOSITORY" --body "$body"
else
	gh issue create -R "$GITHUB_REPOSITORY" --title "$title" --body "$body"
fi
