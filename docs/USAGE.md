# Usage reference (detailed)

This page keeps the detailed usage notes written during development. The overview is in the [README](../README.md).

## Run (portable package)

```sh
scripts/package.sh                       # builds dist/cwfa-portable-x86_64.run (about 2 MB)
./dist/cwfa-portable-x86_64.run writer --cd /path/to/CWFA110__01.ISO
./dist/cwfa-portable-x86_64.run artist --cd /path/to/extracted/cd/
```

* `--cd` is the CD image (`.ISO`, extracted once to `~/.local/share/cwfa/cd/` with `7z`, `bsdtar` or `isoinfo`) or a
  directory with the CD's files. `CWFA_CD` can replace the option.
* Your data (users, saved work, WIN.INI: the programs' "drive C:") lives in `./cwfa-data` in the directory you run
  the launcher from; `--data-dir DIR` (or `CWFA_HOME=DIR`, which keeps `DIR/<writer|artist>`) chooses another place.
  See "Where is my data" below.
* Window scale defaults to 2 (`CWFA_SCALE`); the window can be resized (the 4:3 picture keeps its shape, black bars)
  and `--fullscreen` starts in desktop fullscreen. **F11 or Alt+Enter** toggles fullscreen at any time (the keys are not
  sent to the program). Extra options go to `cwfa`, for example `--scale 3`, `--integer-scale`, `--fixed-size`,
  `--headless --frames 100 --screenshot out.ppm`.
* The package is one self-extracting file; it unpacks to `~/.cache/cwfa-portable/<hash>/` on first use.

Linking, stated honestly: SDL2 is linked **dynamically** (no static libSDL2 exists on the build machine). The package
bundles `libSDL2` and, because Fedora's SDL2 is the sdl2-compat shim that loads SDL3 at run time, `libSDL3`. glibc is
the host's: the package needs a glibc at least as new as the build machine's (built on Fedora 44). Video and audio
back ends (X11, Wayland, PipeWire/PulseAudio, ALSA) are loaded by SDL from the host. The window has been exercised
only with SDL's dummy driver in the tests.

Title-bar buttons (minimize, maximize, close) belong to your desktop, not to `cwfa`; the window is resizable and the
maximize button works. Fullscreen: **F11**, **Alt+Enter**, the small button that appears in the top-right corner when
you move the mouse (click it; the click does not reach the program), or `--fullscreen`. `--no-fs-button` (or
`CWFA_NO_FS_BUTTON=1`) removes the corner button.

## Program switcher

Fine Artist's and Creative Writer's switcher (the colour wheel button) lists the other program once it has run on the
same data directory. Choosing it and answering Yes starts it with WinExec: because `cwfa` runs one program per process,
it starts a second `cwfa` on the same drive C: and CD with your display options (`--child-opt ARG` adds arguments for
it). An already running copy is not found, so a second one starts.

## Icons and the application menu

For a normal install use `scripts/install.sh` (README, option A): it installs the launcher entries for every program, remembers where
the CD is and has an uninstall. The text below is what happens when you only run a program from a checkout or the portable package.

The window carries the program's own icon and an app id (`cwfa-writer`, `cwfa-artist`, `cwfa-es-writer`, `cwfa-es-artist`).
GNOME on Wayland does not take a window icon from the program: it matches the app id to a launcher of the same name, so the
first start of a program writes that launcher by itself (`scripts/play.sh` and the portable `.run` do it, quietly, for the
program being started): `~/.local/share/applications/cwfa-<program>.desktop` and the program's own pixels as PNG icons of
32, 64, 128 and 256 pixels under `~/.local/share/icons/hicolor/`. Only what changed is rewritten, so later starts cost
nothing; the entry's data directory is the one of the last start. Nothing else of your home is touched.
`CWFA_NO_DESKTOP_INTEGRATION=1` turns this off (and a `--headless` run never does it). `scripts/install-desktop.sh`
(or `./cwfa-portable-x86_64.run --install-desktop --cd /path/to/CD`, also with the Spanish disks) installs all programs at
once and `--remove-desktop` (`install-desktop.sh --remove`) deletes everything it wrote.
`cwfa ... --export-icon FILE.png [--export-icon-size 256]` writes a program's icon.

## Fonts and printing

Arial, Times New Roman and Courier New are not on the CD: the runtime draws them with the bundled Liberation faces (SIL OFL).
When the system's `libfreetype.so.6` is present (it is on every GNOME desktop) it is loaded at run time and the glyphs are
rendered hinted and 1-bit, as Windows 3.1 did, so small text keeps whole-pixel stems; without it the built-in
rasteriser is used (`CWFA_NO_FREETYPE=1` forces that). Printing goes to a virtual "PDF Printer" in both the English and
the Spanish programs. The PDFs land in `<your Documents folder>/Creative Writer` (e.g. `~/Documentos/Creative Writer`, created
if missing; the folder comes from `XDG_DOCUMENTS_DIR` in `~/.config/user-dirs.dirs`, else `~/Documents`); the full path is
printed on the terminal. `--print-dir DIR` or `CWFA_PRINT_DIR` chooses another place.

## If the program seems frozen

`cwfa --debug-live FILE` writes a snapshot (windows, timers, queues, device contexts, GDI objects, last 256 API calls)
to FILE on `kill -USR1 <pid>` or Ctrl+\\, when the built-in watchdog sees no API call for 15 s (`--watchdog SEC`), and
at exit; send that file with a report.

## Run from a checkout

```sh
make                                    # build/cwfa, needs gcc and SDL2 development files
scripts/play.sh writer|artist           # expects reference/WRITER.EXE, reference/ARTIST.EXE and reference/cd/
```

## Spanish disks (Creative Writer 1.0 / Fine Artist 1.0)

The Spanish versions come on floppies (or a CD layout) where every file is KWAJ-compressed (`WRITER.EX_`, `MAX100.AR_`...).
`cwfa` expands them itself (`src/kwaj.c`, no external library) following the media's own setup script:

```sh
scripts/play.sh es-writer                # reference/es_nrg1 (INSTALAR.LST directory)
scripts/play.sh es-artist                # reference/es_fa/Microsoft Fine Artist (the DISCO1..4 directories)
build/cwfa --es-writer DIR --install-dir ~/.local/share/cwfa/es-writer
./dist/cwfa-portable-x86_64.run writer --cd DIR      # a directory with INSTALAR.LST (or DISCOn dirs) is detected
CWFA_ES_CD=reference/es_nrg1 CWFA_ES_FA="reference/es_fa/Microsoft Fine Artist" make test
```

`scripts/headless_es.sh` drives them with `--inject`; `type:` accepts UTF-8 Spanish text (accents, n-tilde, inverted
marks) and delivers the Windows ANSI characters. `tools/kwajoracle.c` (dev only, needs libmspack) compares our
decoder with libmspack.

## Where is my data

The original programs run on a DOS/Windows machine with a drive C: (the "installed" program, your users and your
saved documents) and a CD drive D:. `cwfa` maps those letters to host directories:

| program sees | host directory |
|---|---|
| `C:\` | the data directory: `./cwfa-data` (`./cwfa-data-es` for the Spanish disks), where you ran the launcher; or `--data-dir DIR`; with `CWFA_HOME=DIR` it is `DIR/<program>`; `--install-dir DIR` / `--c-root DIR` go to `cwfa` directly |
| `D:\` | the CD you pass with `--cd` (read only in practice: nothing is written there unless you pick D: in a save list) |

Inside the data directory (the same layout as a real install, shared by Creative Writer and Fine Artist so each sees
the other in the program switcher):

* `MSKIDS/USERS/<NAME>/` your documents (`KIDSnnnn.MAX`, `*.MAG` paintings) and `MSKIDS/SHARED/USERS/` the shared samples
* `MSKIDS/WRITER`, `MSKIDS/ARTIST`, `MSKIDS/SHARED` program files copied from the CD (or the Spanish disks)
* `WINDOWS/WIN.INI`, `WINDOWS/MSKIDS.INI` settings and the user list, `WINDOWS/SYSTEM/KIDSWTCH.DLL`

The launcher prints the directory it uses (`cwfa: your data ... is in ...`). Back up or delete `cwfa-data` as you
wish: it is re-created from the CD on the next run (users and documents are lost with it).

The "where do you want to save" list is the program's own drive list: it shows the DOS letters `C` and `D`. `C` is
the data directory above, which is what you want; `D` is the CD. The English programs look for their CD on D: at run
time (stickers, sounds), so D: cannot go away; for the Spanish disks `--hide-cd-drive` (given to `cwfa`) makes the list
show only `C`, but this has only been tried in the first screens, not through a whole session. Choose `C` / "Ponerlo
aqui" when unsure.

## Test

```sh
make test                               # unit tests; the tests that need the CD skip themselves
CWFA_WRITER_EXE=reference/WRITER.EXE CWFA_ARTIST_EXE=reference/ARTIST.EXE CWFA_CD_ROOT=reference/cd \
  CWFA_ISO=/path/to/CWFA110__01.ISO make test
```

`scripts/headless.sh` runs a program headless with a scripted input (`--inject`) for exploring; screenshots of such runs
are in `docs/screenshots/`.
