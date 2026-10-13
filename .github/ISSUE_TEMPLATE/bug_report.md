---
name: Bug report
about: Something does not work (a crash, a frozen program, wrong drawing, no sound, a printing problem)
title: ""
labels: bug
---

**Do not attach Microsoft files** (ISO, EXE, DLL or program resources). Screenshots of the window are fine.

## What happened

<!-- What you did, what you expected, what you saw. -->

## Setup

- Program and edition: <!-- Creative Writer / Fine Artist; English 1.1a CD / Spanish 1.0 -->
- How you started it: <!-- scripts/play.sh es-writer, the portable .run, ... and the options -->
- Linux distribution, desktop and session: <!-- e.g. Fedora 44, GNOME, Wayland -->
- `cwfa` commit or package version:

## If it froze or misbehaved: the debug snapshot

Start it with `--debug-live snapshot.txt` (for example `scripts/play.sh es-writer --debug-live snapshot.txt`). When it
freezes, press **Ctrl+\\** (or run `kill -USR1 <pid>`) and attach `snapshot.txt`. It lists the windows, timers, message
queues, device contexts, GDI objects and the last 256 API calls.

## Terminal output

```
paste here
```
