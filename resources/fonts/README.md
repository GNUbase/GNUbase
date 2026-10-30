# GNUbase's fonts

The fonts that GNUbase bundles and loads at startup (#207). `fonts.json` lists them: the families, each with a role,
and the fallback fonts, in the order in which they supply characters that a family lacks. Themes pick families by name
in their style sheets. How the fonts are loaded and checked: [docs/features/fonts.md](../../docs/features/fonts.md).

| Folder | Family | Role | Faces | Version | Source | License |
|---|---|---|---|---|---|---|
| `Inter` | Inter | text of the user interface | Regular, Italic, Medium, SemiBold, Bold | 4.1 | [rsms/inter](https://github.com/rsms/inter/releases/tag/v4.1), `extras/ttf` | SIL Open Font License 1.1, `Inter/OFL.txt` |
| `JetBrainsMono` | JetBrains Mono | time and counter displays: its digits are all equally wide | Regular, Bold | 2.304 | [JetBrains/JetBrainsMono](https://github.com/JetBrains/JetBrainsMono/releases/tag/v2.304), `fonts/ttf` | SIL Open Font License 1.1, `JetBrainsMono/OFL.txt` |
| `NotoSansSymbols` | Noto Sans Symbols | fallback: symbols that Inter lacks, such as ♭, ♯ and ♮ at the size of text | Regular | 2.003 | [notofonts](https://github.com/notofonts/notofonts.github.io/tree/main/fonts/NotoSansSymbols/hinted/ttf) | SIL Open Font License 1.1, `NotoSansSymbols/OFL.txt` |

All are static TrueType fonts with hinting instructions, unchanged from their sources. Each folder keeps its license
next to the fonts, as the license requires; the fonts are in Git LFS.
