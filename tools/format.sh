#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Formats the C++ files that differ from a base branch (committed, staged, unstaged and new files).
# Usage: tools/format.sh [base]          default base: origin/main
# Set CLANG_FORMAT to the clang-format 22 binary if it is not on PATH, e.g. the one bundled with Visual Studio 2026:
#   CLANG_FORMAT="/c/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-format.exe"
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

base=$(git merge-base "${1:-origin/main}" HEAD)
{
	git diff --name-only --diff-filter=ACMR "$base" -- '*.cpp' '*.h'
	git ls-files --others --exclude-standard -- '*.cpp' '*.h'
} | sort -u | while IFS= read -r file; do
	"${CLANG_FORMAT:-clang-format}" -i "$file"
	echo "formatted $file"
done
