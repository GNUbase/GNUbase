#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Formats the C++ files that differ from a base branch (committed, staged, unstaged and new files).
# Usage: tools/format.sh [base]          default base: origin/main
# It needs clang-format 22, the version CI pins: CLANG_FORMAT names it, else the one on the PATH, else on Windows the
# one Visual Studio 2026 bundles.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

format=${CLANG_FORMAT:-clang-format}
if [ -z "${CLANG_FORMAT:-}" ] && ! command -v clang-format >/dev/null && command -v cygpath >/dev/null; then
	vs=$("$(cygpath -u "$PROGRAMFILES") (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest \
		-property installationPath | tr -d '\r') || true
	format="$vs/VC/Tools/Llvm/x64/bin/clang-format.exe"
fi
if [[ "$("$format" --version 2>/dev/null)" != *"version 22."* ]]; then
	echo "No clang-format 22 at $format (CONTRIBUTING.md says how to install it, CLANG_FORMAT where it is)." >&2
	exit 2
fi

base=$(git merge-base "${1:-origin/main}" HEAD)
{
	git diff --name-only --diff-filter=ACMR "$base" -- '*.cpp' '*.h'
	git ls-files --others --exclude-standard -- '*.cpp' '*.h'
} | sort -u | while IFS= read -r file; do
	"$format" -i "$file"
	echo "formatted $file"
done
