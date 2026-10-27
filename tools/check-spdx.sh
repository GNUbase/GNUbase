#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that every tracked file has the GNUbase license header in its first three lines, which leaves room for a
# shebang or an XML declaration: SPDX-License-Identifier: GPL-3.0-or-later (CLAUDE.md, "Code conventions").
# Exempt: submodules, Git LFS files, Markdown, JSON (no comments), .gitmodules (written by Git) and license texts.
# Usage: tools/check-spdx.sh
# Prints the files without the header. Exit status: 0 every file has it, 1 some don't, 2 error.
set -u
cd "$(git rev-parse --show-toplevel)" || exit 2
tmp=$(mktemp -d) || exit 2
trap 'rm -rf "$tmp"' EXIT
header="SPDX-License-Identifier: GPL-3.0-or-later"

# Regular files only: submodules (mode 160000) and symbolic links (120000) have no header of their own.
git -c core.quotePath=false ls-files -s > "$tmp/index" || exit 2
awk -F '\t' '$1 ~ /^100/ { print $2 }' "$tmp/index" > "$tmp/files"
git -c core.quotePath=false check-attr --stdin filter < "$tmp/files" > "$tmp/filters" || exit 2
sed -n 's/: filter: lfs$//p' "$tmp/filters" > "$tmp/lfs"

awk -v lfs="$tmp/lfs" 'BEGIN { while ((getline line < lfs) > 0) skip[line] } !($0 in skip)' "$tmp/files" |
	while IFS= read -r file; do
		case "$file" in
		*.md | *.json | .gitmodules) continue ;;
		LICENSE | LICENSE.* | */LICENSE | */LICENSE.* | LICENSES/* | */LICENSES/*) continue ;;
		COPYING | COPYING.* | */COPYING | */COPYING.* | OFL.txt | */OFL.txt) continue ;;
		esac
		[ -f "$file" ] || continue # deleted, but not staged yet
		head -n 3 "$file" | grep -q "$header" || echo "$file"
	done > "$tmp/missing"

[ -s "$tmp/missing" ] || exit 0
cat "$tmp/missing"
echo "These files lack '$header' in their first three lines; see CLAUDE.md." >&2
exit 1
