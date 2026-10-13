# Third-party code and data

| What | Where | Licence |
|------|-------|---------|
| stb_truetype.h v1.26 (TrueType rasteriser, single header) | `stb/stb_truetype.h` | MIT or public domain (Sean Barrett), text at the end of the header |
| Liberation Sans / Serif / Mono 2.1.5, Regular and Bold (metric-compatible with Arial, Times New Roman, Courier New) | `fonts/Liberation*.ttf` | SIL Open Font License 1.1, `fonts/LICENSE-Liberation-OFL.txt` |

The Liberation faces are used unmodified, so the reserved font name is respected. Size: six files, 2.2 MB in the
repository (about 1.2 MB inside the xz-compressed portable package). No proprietary Microsoft font is shipped or read.
At run time the text engine also uses the CD's own `.TTF` files (Lucida Sans, McZee, Impact, ...) when present.
