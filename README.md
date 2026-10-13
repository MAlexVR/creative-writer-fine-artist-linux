# win16rt: Creative Writer & Fine Artist, native on Linux

**A native Linux runtime for Microsoft's 1990s children's programs _Creative Writer_ and _Fine Artist_.** It loads the
original, unmodified Windows 3.1 executables and runs them without Wine, without a virtual machine and without Windows.

[Leer en español](README.es.md)

![License: MIT](https://img.shields.io/badge/license-MIT-blue)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-informational?logo=linux&logoColor=white)
![Tested on Fedora 44 GNOME](https://img.shields.io/badge/tested%20on-Fedora%2044%20%7C%20GNOME-51a2da?logo=fedora&logoColor=white)
![Language: C11](https://img.shields.io/badge/language-C11-555555?logo=c&logoColor=white)
![SDL2](https://img.shields.io/badge/video%20%26%20audio-SDL2-orange)
![Runs Windows 3.1 (Win16) programs](https://img.shields.io/badge/runs-Windows%203.1%20(Win16)-008080)
![No Wine](https://img.shields.io/badge/Wine-not%20needed-success)
![Tests: 66 suites passing](https://img.shields.io/badge/tests-66%20suites%20passing-brightgreen)
![Languages: English and Spanish](https://img.shields.io/badge/programs-English%20%2B%20Spanish-purple)

[![Watch the video on YouTube](https://img.youtube.com/vi/8K5z4S8xA_M/maxresdefault.jpg)](https://www.youtube.com/watch?v=8K5z4S8xA_M)

_Click the picture to watch the video on YouTube._

## Contents

1. [What this is](#what-this-is)
2. [Which programs and versions work](#which-programs-and-versions-work)
3. [A tour in order of appearance](#a-tour-in-order-of-appearance)
4. [You bring the software: where to get it](#you-bring-the-software-where-to-get-it)
5. [Quick start](#quick-start)
6. [Features](#features)
7. [What it took to run on Fedora 44](#what-it-took-to-run-on-fedora-44)
8. [Building from source](#building-from-source)
9. [Where your files go](#where-your-files-go)
10. [Packaging: portable .run, AppImage, Flatpak, deb, rpm](#packaging-portable-run-appimage-flatpak-deb-rpm)
11. [Software preservation notes](#software-preservation-notes)
12. [Known limitations](#known-limitations)
13. [Testing and contributing](#testing-and-contributing)
14. [Roadmap](#roadmap)
15. [Credits and licences](#credits-and-licences)
16. [Legal note](#legal-note)

## What this is

`cwfa` is an interpreter written in C. It reads the original 16-bit Windows (NE format) `.EXE` and `.DLL` files and runs them:

* an **x86 16-bit protected-mode CPU interpreter** (with an x87 floating-point unit and the `WIN87EM` emulator interface),
* a **C re-implementation of the Win16 API** (KERNEL, USER, GDI, MMSYSTEM, TOOLHELP, VER and the rest the programs call), a
  "high-level emulation" layer: when the program calls a Windows function, our C code answers it,
* an **SDL2 window** (a 640x480, 256-colour picture, letterboxed and scalable) and SDL2 audio.

It is **not** Wine, **not** a full-system emulator and it does **not** need Windows or a Windows licence. It only
implements what these two programs need, which keeps it small (the portable package is about 3 MB).

**This repository contains no Microsoft software.** There are no ISOs, no `.EXE`/`.DLL` files from the programs, no
sounds, pictures or documents from them. You supply your own copy of the disc (see below). The only third-party code and
data shipped are the Liberation fonts and `stb_truetype.h`, with their own free licences (see
[Credits and licences](#credits-and-licences)).

## Which programs and versions work

Both programs can be used together (as the combined CD does, switching between them) or each on its own.

| Edition | Media | Status |
|---|---|---|
| **Creative Writer + Fine Artist 1.1a, English** (combined CD, ISO label `CWFA110__01`) | CD-ROM | Both programs run end to end (see below) |
| **Creative Writer, English, on its own** | CD-ROM / floppies | Not tested: only the combined 1.1a CD was available. The runtime loads one program per process, so a stand-alone release should work if its files match; this is unverified |
| **Fine Artist, English, on its own** | CD-ROM / floppies | Not tested, same as above |
| **Creative Writer 1.0, Spanish** (*Creative Writer*) | CD layout or floppy images, files compressed with Microsoft's KWAJ format | Runs end to end |
| **Fine Artist 1.0, Spanish** (*Fine Artist*) | Four floppy disks (`DISCO1`..`DISCO4`) | Runs end to end |

"Runs end to end" means: every session below was driven headless with scripted input, with screenshots read by a person,
and covered by the automated tests: login ("who are you?" with accents typed in Spanish), the first-run story to the
Imaginopolis map, the writing studio and the painting studio (text, brush, bucket, stickers), saving a document,
quitting, reopening the saved work, and printing to PDF. Screenshots of these runs are in
[`docs/screenshots/`](docs/screenshots/). What has **not** been verified is listed in
[Known limitations](#known-limitations).

Programs from the Macintosh side of the discs (the 1.1a CD also has an m68K Macintosh version) are not used: this runs
the Windows programs.

## A tour in order of appearance

What you see, screen by screen, from the moment a program starts. They are screenshots of the original programs running on our runtime (headless runs, 640x480, with a new user called "Ana"). Both programs share the same town; each one has its own building.

### Creative Writer (English)
| **1.** Splash screen | **2.** Who are you? (McZee asks for your name) | **3.** Imaginopolis | **4.** The building (the green one is the Writer's) |
|:---:|:---:|:---:|:---:|
| ![1. Splash screen](docs/screenshots/writer-01-splash.png) | ![2. Who are you? (McZee asks for your name)](docs/screenshots/writer-02-login.png) | ![3. Imaginopolis](docs/screenshots/writer-03-imaginopolis.png) | ![4. The building (the green one is the Writer's)](docs/screenshots/writer-04-building.png) |
| **5.** Level selection: the lobby, with the list of floors at the top left | **6.** A floor: the Idea Workshop |  |  |
| ![5. Level selection: the lobby, with the list of floors at the top left](docs/screenshots/writer-05-level-select.png) | ![6. A floor: the Idea Workshop](docs/screenshots/writer-06-floor.png) |  |  |

### Fine Artist (English)
| **1.** Splash screen | **2.** Who are you? (McZee asks for your name) | **3.** Imaginopolis | **4.** The building (the pink one is the Artist's) |
|:---:|:---:|:---:|:---:|
| ![1. Splash screen](docs/screenshots/artist-01-splash.png) | ![2. Who are you? (McZee asks for your name)](docs/screenshots/artist-02-login.png) | ![3. Imaginopolis](docs/screenshots/artist-03-imaginopolis.png) | ![4. The building (the pink one is the Artist's)](docs/screenshots/artist-04-building.png) |
| **5.** Level selection: the lobby, with the list of floors at the top left | **6.** A floor: Drawing Tricks |  |  |
| ![5. Level selection: the lobby, with the list of floors at the top left](docs/screenshots/artist-05-level-select.png) | ![6. A floor: Drawing Tricks](docs/screenshots/artist-06-floor.png) |  |  |

### Creative Writer (Spanish)
| **1.** Splash screen | **2.** Who are you? | **3.** Imaginópolis | **4.** The building |
|:---:|:---:|:---:|:---:|
| ![1. Splash screen](docs/screenshots/es-writer-01-splash.png) | ![2. Who are you?](docs/screenshots/es-writer-02-login.png) | ![3. Imaginópolis](docs/screenshots/es-writer-03-imaginopolis.png) | ![4. The building](docs/screenshots/es-writer-04-building.png) |
| **5.** Level selection: the lobby, with the list of floors ("Taller de ideas", "Taller de proyectos", ...) | **6.** A floor: the Taller de ideas |  |  |
| ![5. Level selection: the lobby, with the list of floors ("Taller de ideas", "Taller de proyectos", ...)](docs/screenshots/es-writer-05-level-select.png) | ![6. A floor: the Taller de ideas](docs/screenshots/es-writer-06-floor.png) |  |  |

### Fine Artist (Spanish)
| **1.** Splash screen | **2.** Who are you? | **3.** Imaginópolis | **4.** The building |
|:---:|:---:|:---:|:---:|
| ![1. Splash screen](docs/screenshots/es-artist-01-splash.png) | ![2. Who are you?](docs/screenshots/es-artist-02-login.png) | ![3. Imaginópolis](docs/screenshots/es-artist-03-imaginopolis.png) | ![4. The building](docs/screenshots/es-artist-04-building.png) |
| **5.** Level selection: the lobby, with the list of floors ("Trucos de dibujo", "Taller de proyectos", ...) | **6.** A floor: Trucos de dibujo |  |  |
| ![5. Level selection: the lobby, with the list of floors ("Trucos de dibujo", "Taller de proyectos", ...)](docs/screenshots/es-artist-05-level-select.png) | ![6. A floor: Trucos de dibujo](docs/screenshots/es-artist-06-floor.png) |  |  |

## You bring the software: where to get it

The programs are Microsoft's and are not distributed here. Copies preserved by archivists:

* **WinWorld**, Creative Writer 1.x: <https://winworldpc.com/product/microsoft-creative-writer/1x>. It lists the 1.x
  releases, including version 1.1a, the combined Creative Writer + Fine Artist CD, and notes that Spanish and Swedish
  editions of 1.0 exist.
* **Internet Archive**, Creative Writer (CD-ROM for Windows 3.1 and Macintosh, 1994, Microsoft Kids):
  <https://archive.org/details/creative-writer>.
* **Book** about the program: [_Exploring Creative Writer_](https://archive.org/details/exploringcreativ0000kurs),
  see [the book](#the-book-exploring-creative-writer) below.

Check that you may use these files where you live: see the [legal note](#legal-note). The Spanish media used during
development were a Creative Writer 1.0 CD image and Fine Artist floppy disks.

## Quick start

You need the CD image (`.ISO`) or its contents extracted to a folder.

### Option A: install it for your user (recommended)

```sh
scripts/install.sh --cd /path/to/CWFA110__01.ISO       # builds the portable package and installs it for you
# the same from the package:  ./dist/cwfa-portable-x86_64.run --install --cd /path/to/CWFA110__01.ISO
# Spanish editions as well:    add --es-cd /path/to/spanish/creative-writer  --es-fa-cd /path/to/spanish/fine-artist
```

Creative Writer and Fine Artist then appear in your application launcher (GNOME's Activities, the app grid), each with its own
icon; the Spanish editions get their own entries when their media are given. Start them from there, or with
`cwfa-launch writer|artist|es-writer|es-artist`. Nothing needs root: everything goes under your home directory.

* **The CD:** `--cd` is optional. If you leave it out, the first start asks where the CD (or `.ISO`) is, in a dialog when `zenity`
  is installed and on the terminal otherwise, and remembers the answer in `~/.config/cwfa/media.conf`.
  `cwfa-launch --configure` changes it later (`--cd`, `--es-cd`, `--es-fa-cd`).
* **Icons:** the English CD edition draws a CD on its program icons (green basket and pencil for Creative Writer, magenta-pink
  basket and brush for Fine Artist); the Spanish editions carry the same artwork **without** the CD. The launcher entries use the
  plain Spanish artwork when Spanish media are configured, and each program's own icon otherwise (`--icons original|plain|auto`).
  The icons are made from your own media; nothing of them is shipped.
* **Spanish only:** when only Spanish media are configured (no English disc in `media.conf`), the menu lists the Spanish programs
  simply as "Creative Writer" and "Fine Artist", no English entry is made, and the English entries a package (the RPM) ships are
  hidden for you by a same-named override with `Hidden=true` in `~/.local/share/applications` (only while the English entries are
  installed; with the `cwfa-spanish` package alone there are none to hide, and a leftover override is removed). Configuring an
  English disc again removes the overrides and brings back the "(Español)" suffix.
* **Uninstall:** `cwfa-launch --uninstall` (or `scripts/install.sh --uninstall`) removes exactly what was installed, listed in
  `~/.local/share/cwfa/install.manifest`. Your users and documents stay in `~/.local/share/cwfa/data`; `--purge --yes` deletes
  them too (without `--yes` it asks, and without a terminal it refuses).
* Installing again updates in place.

### Option B: the portable package

```sh
scripts/package.sh                                           # builds dist/cwfa-portable-x86_64.run
./dist/cwfa-portable-x86_64.run writer --cd /path/to/CWFA110__01.ISO
./dist/cwfa-portable-x86_64.run artist --cd /path/to/CWFA110__01.ISO
```

`--cd` accepts the `.ISO` (extracted once to `~/.local/share/cwfa/cd/`, using `7z`, `bsdtar` or `isoinfo`) or a directory.
For the Spanish disks, pass the directory that holds `INSTALAR.LST` (Creative Writer) or the `DISCO1`..`DISCO4` folders
(Fine Artist): they are detected automatically.

### Option C: from a checkout

```sh
make                                          # needs gcc, make and the SDL2 development files
scripts/play.sh writer|artist|es-writer|es-artist [options]
```

`play.sh` expects the original files under `reference/` (git-ignored): `reference/WRITER.EXE`, `reference/ARTIST.EXE`,
`reference/cd/` (the CD's contents), and for Spanish `reference/es_nrg1` and `reference/es_fa/...`.

### Option D: an RPM (Fedora and relatives)

```sh
scripts/build-rpm.sh                                       # or: make rpm; builds from the committed tree into build/rpm/
sudo dnf install build/rpm/RPMS/x86_64/cwfa-[0-9]*.rpm build/rpm/RPMS/x86_64/cwfa-spanish-*.rpm      # the runtime and the Spanish editions in the menu
sudo dnf install build/rpm/RPMS/x86_64/cwfa-[0-9]*.rpm build/rpm/RPMS/x86_64/cwfa-english-*.rpm      # ... or the English editions (both can be installed)
```

Three packages come out of one spec:

| Package | Contains |
|---|---|
| `cwfa` | the runtime under `/usr/libexec/cwfa` and the `cwfa-launch` command in `/usr/bin`, with the documentation and licence; no menu entries |
| `cwfa-english` | the menu entries and AppStream data of Creative Writer and Fine Artist (English editions), with a generic icon |
| `cwfa-spanish` | the same for the Spanish editions, listed simply as "Creative Writer" and "Fine Artist" |

Install `cwfa` together with the edition you own; each edition package needs the runtime, and the runtime needs neither. The
packages contain no game data and no fonts: they need the Liberation font packages. Remove them from GNOME Software or with
`sudo dnf remove cwfa-spanish` (or `cwfa-english`, or `cwfa` for everything). The first start
of each program asks for the CD as in option A and makes the program's real icons in your home directory.
*Verified here:* the package builds, its contents and dependencies were inspected, and the desktop and AppStream files
validate. It was installed with `dnf` on Fedora 44 (GNOME, Wayland) and the Spanish Creative Writer was started from the
application menu, with the programs' own icons (no CD) and the plain names "Creative Writer" and "Fine Artist".
*Not verified:* seeing and removing it in GNOME Software, and starting Fine Artist from the menu.

### Window controls

The window is resizable and keeps the 4:3 picture (black bars). **F11** or **Alt+Enter** toggle fullscreen, and so does the
small button in the top-right corner (hide it with `--no-fs-button`). Other options: `--fullscreen`, `--scale N`,
`--integer-scale`, `--fixed-size`. The minimize, maximize and close buttons belong to your desktop, not to the program.

More detail (every option, the Spanish disks, the program switcher, the data directory): [`docs/USAGE.md`](docs/USAGE.md).

Fine Artist's and Creative Writer's own program switcher (the colour-wheel button) works: it starts the other program as a
second `cwfa` process on the same data directory.

## Features

* Runs the **original** NE executables and DLLs **unmodified**: 16-bit protected mode with LDT selectors, Pascal calling
  convention, a loader for NE segments and relocations, x87 emulation.
* **Win16 API in C**: KERNEL (memory, files, resources, profiles, DOS interrupts the C runtime calls), USER (windows,
  messages, menus, dialogs, controls, edit boxes, timers), GDI (bitmaps, DIBs, palettes, regions, metafiles, fonts, text,
  clipping, `ScrollDC`), MMSYSTEM (wave playback, timers), TOOLHELP, VER, SHELL.
* **Sound** through SDL2 audio (waveOut, `sndPlaySound`, ADPCM WAV decoding), with a guard against a stalled device.
* **Fonts**: the programs' own raster `.FON` fonts (Pablo Graphos) and TrueType fonts from the disc, plus Arial, Times New
  Roman and Courier New drawn with the metric-compatible Liberation fonts, hinted to 1 bit through the system FreeType
  when present.
* **Printing to PDF**: a virtual "PDF Printer" (own PDF writer, no dependencies) with the band-by-band printer protocol the
  programs expect, at 300 dpi.
* **Spanish media**: our own decoder for Microsoft's KWAJ compression (checked byte for byte against libmspack) and the
  installer logic for the Spanish CD/floppies, with accents typed and drawn correctly.
* **Data directory** that stands in for the programs' `C:` drive, and the CD as `D:`, with a faithful install layout.
* **Desktop integration**: window icon from the program's own resources, app ids, and the menu entry plus icons created
  for you on first start (GNOME on Wayland needs them).
* **Diagnostics for a frozen program**: `--debug-live FILE` (snapshot on Ctrl+\\ or `SIGUSR1`), `--watchdog SEC`, API/message
  tracing, a scripted input mode (`--inject`) used by the tests, headless screenshots.
* 66 test suites. Without any media `make test` runs 485,391 checks with 0 failures (the tests that
  need the discs skip themselves); with the English and Spanish media the same suites run 492,934 checks, 0 failures.

## What it took to run on Fedora 44

The programs are 1994-95 Win16 code, and a modern GNOME desktop is a hostile place for them. What had to be solved:

* **SDL2 on Fedora 44 is the `sdl2-compat` shim over SDL3.** We still build against the SDL2 API (confined to two files),
  and the portable package bundles `libSDL2` and `libSDL3` because the shim loads SDL3 at run time. glibc is the host's:
  the package needs a glibc at least as new as the one it was built with (Fedora 44).
* **Window icon on Wayland/GNOME.** A program cannot give GNOME its icon: the compositor pairs the window's app id
  (`cwfa-writer`, `cwfa-artist`, `cwfa-es-writer`, `cwfa-es-artist`) with a `.desktop` file and an icon in the hicolor
  theme. On first start we write both for the program being run (`~/.local/share/applications`, `~/.local/share/icons`),
  with the program's own icon scaled by whole numbers to 32, 64, 128 and 256 px. `CWFA_NO_DESKTOP_INTEGRATION=1` turns this off.
* **Text.** The programs use TrueType faces that are not on the disc. Liberation fonts stand in for them; thresholded
  outlines at 9 to 12 px turned into blobs, so the system `libfreetype` is loaded with `dlopen` (headers only at build
  time, nothing linked) and renders hinted 1-bit glyphs like Windows 3.1 did. Without FreeType a built-in rasteriser is used.
* **Printing.** Linux has no Windows printer driver for them to talk to. We provide a virtual printer that the programs find
  in `WIN.INI` and print to; the output is a PDF.
* **Spanish media.** Every file on the Spanish disks is KWAJ-compressed; the runtime expands them itself, following the media's
  own setup script.
* **Real-time behaviour.** The programs rely on message loops, `WM_TIMER` and delay loops that a modern CPU and a compositor
  expose: window-manager details (CS_OWNDC device contexts, activation messages, cursor and icon memory handles, scroll
  rectangles in logical coordinates) were found and fixed one by one, each with a test that failed first.

## Building from source

Fedora 44 (the tested system):

```sh
sudo dnf install gcc make pkgconf-pkg-config sdl2-compat-devel freetype-devel
make                       # builds build/cwfa, build/nedump and the static library
make test                  # runs the suites; the ones that need the media skip themselves
scripts/package.sh         # builds dist/cwfa-portable-x86_64.run
```

* `sdl2-compat-devel` provides the SDL2 headers (on other distributions: `libsdl2-dev` / `SDL2-devel`).
* `freetype-devel` is needed **only for its headers**: `libfreetype.so.6` is loaded at run time and is optional.
* To list the ISO's files the launcher uses one of `7z` (p7zip), `bsdtar` or `isoinfo`.
* The code is C11 and builds with `-Wall -Wextra -Werror`. Other Linux distributions have not been tried.

To run the tests that need the real programs, point them at your copy:

```sh
CWFA_WRITER_EXE=reference/WRITER.EXE CWFA_ARTIST_EXE=reference/ARTIST.EXE CWFA_CD_ROOT=reference/cd \
CWFA_ISO=/path/to/CWFA110__01.ISO make test
# Spanish: CWFA_ES_CD=reference/es_nrg1 CWFA_ES_FA="reference/es_fa/Microsoft Fine Artist"
```

The complete suite with the media takes more than ten minutes.

## Where your files go

| What | Where |
|---|---|
| The programs' drive **`C:`** (installed copy, users, saved work, `WIN.INI`) | `./cwfa-data` in the directory you launch from (`./cwfa-data-es` for the Spanish disks) |
| Choose another place | `--data-dir DIR`, or `CWFA_HOME=DIR` (then `DIR/<program>`) |
| Your documents and paintings | inside `C:`: `MSKIDS/USERS/<NAME>/` (`KIDSnnnn.MAX` texts, `*.MAG` paintings) |
| Settings and user list | `WINDOWS/WIN.INI`, `WINDOWS/MSKIDS.INI` inside the data directory |
| The programs' drive **`D:`** (the CD) | what you pass with `--cd` (read only in practice) |
| **Printed PDFs** | `<your Documents folder>/Creative Writer`, for example `~/Documentos/Creative Writer` or `~/Documents/Creative Writer`; created if missing. The folder comes from `XDG_DOCUMENTS_DIR` in `~/.config/user-dirs.dirs`. Change it with `--print-dir DIR` or `CWFA_PRINT_DIR`. The full path of every PDF is printed on the terminal |
| With the installer (option A): drive **`C:`** | `~/.local/share/cwfa/data` (`data-es` for the Spanish editions): the launcher keeps it there (`--user-data`) |
| With the installer: the program files | `~/.local/share/cwfa/app/`, and a link `~/.local/bin/cwfa-launch` |
| With the installer: where the CD is | `~/.config/cwfa/media.conf` |
| With the installer: the uninstall list | `~/.local/share/cwfa/install.manifest` |
| Extracted ISO (portable package and installer) | `~/.local/share/cwfa/cd/` |
| Package cache | `~/.cache/cwfa-portable/<hash>/` |
| Menu entry and icons | `~/.local/share/applications/cwfa-*.desktop`, `~/.local/share/icons/hicolor/` (the installer or `scripts/install-desktop.sh --remove` removes them; the RPM keeps its own in `/usr/share`) |

The "where do you want to save" list inside the programs shows the DOS drive letters `C` and `D`. Choose **C**: it is the data
directory above. Deleting `cwfa-data` is safe, but your users and documents go with it; the rest is re-created from the disc.

## Packaging: portable .run, AppImage, Flatpak, deb, rpm

**What exists today:**

* One self-extracting executable, `dist/cwfa-portable-x86_64.run` (built by `scripts/package.sh`, about 3 MB). It bundles the
  SDL libraries and the Liberation fonts. It needs a recent glibc and the host's graphics/audio stack. It contains no game data.
* A user-level installer with uninstall (`scripts/install.sh`, or `--install` / `--uninstall` on the `.run`): launcher entries, icons,
  remembered media. Tested with scripted runs in a fake home directory and with the real binary and CD.
* An RPM (`scripts/build-rpm.sh`, spec in `packaging/rpm/`; three packages: `cwfa`, `cwfa-english`, `cwfa-spanish`) with desktop entries and AppStream metadata, so it can be installed and
  removed from GNOME Software or with `dnf`. Built, installed with `dnf` on Fedora 44 and started from the application menu; its listing in GNOME Software was not checked.

**What does not exist yet** (an honest assessment, none of this has been built or tested):

| Format | What it would need | Verdict |
|---|---|---|
| **Flatpak** | A manifest on a GNOME/freedesktop runtime (SDL2 and FreeType are available there); `--filesystem` permissions so the user can pick the ISO and a Documents folder, which also fits the PDF output path | Best long-term fit for Fedora and GNOME users: sandboxed, with a proper app id and icon, which also solves the Wayland icon matching without writing into `~` |
| **COPR** | The RPM above, built by a build service for several Fedora releases | Would give `dnf copr enable` installs and automatic updates |
| **AppImage** | A `linuxdeploy` recipe; bundle SDL2/SDL3 as today; still bound to the build machine's glibc, so it should be built on an older distribution | A good portable alternative to the current `.run` |
| **deb** | A `debian/` packaging directory | For Debian/Ubuntu users; the build has only been tried on Fedora |

Recommendation: Flatpak next for the GNOME desktop, and COPR for Fedora updates, if there is interest.

## Software preservation notes

### The programs

* **Microsoft Creative Writer** is a word processor and desktop-publishing program for children, from Microsoft's kids'
  division (Microsoft Kids, later part of [Microsoft Home](https://en.wikipedia.org/wiki/Microsoft_Home)). The whole program is
  a playful town, **Imaginopolis**, led by **McZee** (a purple character), with **Max** (a writer who works in the library)
  and **Maggie** (an artist who works in the museum). Navigation is a four-storey building. Toolbar functions are objects:
  a camera for copy, glue for paste, a bee for the spell checker. It offers about 8,000 writing prompts (the "Magic
  Combobulators"), hundreds of cartoon stamps and password protection for documents. Documents are saved in a proprietary
  format (`.MAX`). It was priced at about US$65 at launch, according to [How-To Geek](https://www.howtogeek.com/723648/did-you-know-microsoft-made-a-kids-word-processor-in-the-1990s/).
* **Microsoft Fine Artist** is the paint program of the same family, developed by Turning Point Software with Microsoft Kids,
  in the same Imaginopolis. According to [Wikipedia](https://en.wikipedia.org/wiki/Fine_Artist): over 500 stickers (50 of
  them animated), 72 brush styles, 76 colours and patterns, 87 backgrounds and 300 sound effects, full-screen only, and
  "considerably more powerful than Microsoft Paint". Paintings are saved as `.MAG` files in our runs.
* Both programs can **be used together**: with both installed you can switch between them from inside the program and share
  documents and pictures. The combined **1.1a CD** puts them on one disc.

### Dates and compatibility

| | |
|---|---|
| Creative Writer, first release | December 1993 for Windows 3.1 and Macintosh ([How-To Geek](https://www.howtogeek.com/723648/did-you-know-microsoft-made-a-kids-word-processor-in-the-1990s/)) |
| Fine Artist | announced December 1993, released January 1994 for PC and Mac; discontinued 1997 ([Wikipedia](https://en.wikipedia.org/wiki/Fine_Artist)) |
| CD-ROM editions | autumn 1994 according to trade press of the time (not cross-checked); WinWorld and the Internet Archive both date the programs 1994 |
| Combined CD 1.1a | the text that accompanies the WinWorld archive says "Released in 1995 by Microsoft": the exact date was not verified |
| Creative Writer 2 | 1996, for Windows 95 (a different, windowed program; not supported here) |
| Operating system | Windows 3.1 or later (16-bit); Macintosh System 7 for the Mac versions |
| Hardware | a 386 or better, a Multimedia PC, 256-colour display ([WinWorld](https://winworldpc.com/product/microsoft-creative-writer)) |
| Languages | English; Spanish and Swedish editions of 1.0 exist; we support English and Spanish |

Contemporary reviews: [COMPUTE!, May 1994](https://atarimagazines.com/compute/issue164/88_Write_on_kids.php) and
[August 1994](https://atarimagazines.com/compute/issue167/82_Fine_Artist.php). WinWorld pages:
[Creative Writer](https://winworldpc.com/product/microsoft-creative-writer) and
[Fine Artist](https://winworldpc.com/product/microsoft-fine-artist).

### The book: Exploring Creative Writer

* **Title:** _Exploring Creative Writer: imaginative and fun computer activities_
* **Author:** Barbara L. Kurshan (as listed in the catalogue metadata)
* **Publisher:** Addison-Wesley, Reading, Massachusetts, **1994**
* **ISBN:** 0201626764 · **136 pages** (viii, 136), with a bibliography (pp. 131-134) and an index
* **What it is:** a guide for young readers to the Creative Writer software, centred on word processing and desktop-publishing
  activities. Its link to the program is direct: it teaches, step by step, projects to do **with Creative Writer**.
* **Where:** <https://archive.org/details/exploringcreativ0000kurs> (borrowable from the Internet Archive's library).

We have not read the book for this project; the description comes from the Internet Archive's catalogue record. It is useful to
anyone who wants to know how the program was meant to be used.

### What this project adds to preservation

Instead of keeping a Windows 3.1 machine, an emulator or a Wine prefix alive, the runtime re-implements only the small slice
of Windows 3.1 these programs use. The original files stay untouched and the behaviour was verified against them with scripted
sessions, screenshots and 66 test suites. The ordinal tables, call sites, file formats (NE executables, KWAJ, `.MAX`, `.MAG`,
`.FON`, metafiles) and the reasons behind each fix are written down in the commit messages and the comments of the source files,
so the knowledge does not live only in the code.

## Known limitations

Written honestly.

* Most checking was done **headless**, with screenshots. Items **not yet verified in a real GNOME window**: that the dock, title
  bar and Alt+Tab show the program icon (the application menu does show it; the very first start may show a generic icon), how the hinted fonts look on a scaled
  or fullscreen window, scrolling with a real mouse, X11/XWayland, and Fine Artist printing in Spanish.
* Not every Fine Artist tool was exercised: the brush, bucket and sticker were; the vacuum tool's colour/copy/thing suckers,
  transformers, 3D guides, undo/redo and the magnifier were opened but not used. Some Win16 functions are implemented from
  the published API without being called by these programs; they are marked `UNVERIFIED` in the notes.
* Audio was verified through the decoder and the engine, but a real stalled audio device was not reproduced.
* The program switcher starts a second process; it cannot find a copy already running in another process.
* The first-run flow of the Spanish Creative Writer needs long real-time waits (McZee's speech).
* Only Linux x86-64 on Fedora 44 was tested.

## Testing and contributing

Plain C tests with
an `assert` harness, one binary per suite in `tests/`. `make test` runs them; tests that need the media skip themselves unless
you set `CWFA_WRITER_EXE`, `CWFA_ARTIST_EXE`, `CWFA_CD_ROOT`, `CWFA_ISO`, `CWFA_ES_CD` and `CWFA_ES_FA`.

To report a problem, use the issue template. If the program freezes, run it with `--debug-live FILE`, press **Ctrl+\\** and
attach the snapshot. `scripts/headless.sh` and `--inject` let you script a session and take screenshots without a display.

## Roadmap

* Confirm the dock and Alt+Tab icons, the fonts and scrolling in a real GNOME session, and try other desktops.
* Exercise the remaining Fine Artist tools and Creative Writer features.
* Flatpak packaging, a COPR build, and checking the RPM in GNOME Software on a real session (see above).
* Continuous integration that builds and runs the tests that need no media (`make test` already ends successfully without them).
* Check the Swedish edition and any other 1.x editions, if media becomes available.

## Credits and licences

* This project's code: [MIT](LICENSE). Third-party material keeps its own licence (see `third_party/README.md`).
* **Liberation fonts 2.1.5** (SIL Open Font License 1.1), `third_party/fonts/`.
* **stb_truetype.h** by Sean Barrett (MIT or public domain), `third_party/stb/`.
* **libmspack** is used only as a development-time check of our KWAJ decoder (`tools/kwajoracle.c`); it is not linked into `cwfa`.
* SDL2/SDL3 and FreeType are used as shared libraries from the system.
* Creative Writer, Fine Artist, McZee, Imaginopolis and Windows are products and trademarks of Microsoft Corporation.

## Legal note

This project is **not affiliated with, endorsed by or sponsored by Microsoft**. It does not contain or distribute Microsoft software
or content: you need your own copy of the programs. The links above point to third-party archives that preserve old software;
laws about abandonware and archived software differ between countries, so please check what applies to you. The screenshots show
our runtime executing the original programs, for documentation. If a rights holder objects to anything in this repository, open an
issue and it will be handled.
